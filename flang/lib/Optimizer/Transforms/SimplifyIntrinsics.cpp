//===- SimplifyIntrinsics.cpp -- replace intrinsics with simpler form -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
/// \file
/// This pass looks for suitable calls to runtime library for intrinsics that
/// can be simplified/specialized and replaces with a specialized function.
///
/// For example, SUM(arr) can be specialized as a simple function with one loop,
/// compared to the three arguments (plus file & line info) that the runtime
/// call has - when the argument is a 1D-array (multiple loops may be needed
//  for higher dimension arrays, of course)
///
/// The general idea is that besides making the call simpler, it can also be
/// inlined by other passes that run after this pass, which further improves
/// performance, particularly when the work done in the function is trivial
/// and small in size.
//===----------------------------------------------------------------------===//

#include "PassDetail.h"
#include "flang/Optimizer/Builder/BoxValue.h"
#include "flang/Optimizer/Builder/FIRBuilder.h"
#include "flang/Optimizer/Builder/Todo.h"
#include "flang/Optimizer/Dialect/FIROps.h"
#include "flang/Optimizer/Dialect/FIRType.h"
#include "flang/Optimizer/Support/FIRContext.h"
#include "flang/Optimizer/Transforms/Passes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/RegionUtils.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "flang-simplify-intrinsics"

namespace {

class SimplifyIntrinsicsPass
    : public fir::SimplifyIntrinsicsBase<SimplifyIntrinsicsPass> {
  using FunctionTypeGeneratorTy =
      std::function<mlir::FunctionType(fir::FirOpBuilder &)>;
  using FunctionBodyGeneratorTy =
      std::function<void(fir::FirOpBuilder &, mlir::func::FuncOp &)>;

public:
  /// Generate a new function implementing a simplified version
  /// of a Fortran runtime function defined by \p basename name.
  /// \p typeGenerator is a callback that generates the new function's type.
  /// \p bodyGenerator is a callback that generates the new function's body.
  /// The new function is created in the \p builder's Module.
  mlir::func::FuncOp getOrCreateFunction(fir::FirOpBuilder &builder,
                                         const mlir::StringRef &basename,
                                         FunctionTypeGeneratorTy typeGenerator,
                                         FunctionBodyGeneratorTy bodyGenerator);
  void runOnOperation() override;
  void getDependentDialects(mlir::DialectRegistry &registry) const override;
};

} // namespace

/// Generate function type for the simplified version of FortranASum
/// operating on the given \p elementType.
static mlir::FunctionType genFortranASumType(fir::FirOpBuilder &builder,
                                             const mlir::Type &elementType) {
  mlir::Type boxType = fir::BoxType::get(builder.getNoneType());
  return mlir::FunctionType::get(builder.getContext(), {boxType},
                                 {elementType});
}

/// Generate function body of the simplified version of FortranASum
/// with signature provided by \p funcOp. The caller is responsible
/// for saving/restoring the original insertion point of \p builder.
/// \p funcOp is expected to be empty on entry to this function.
static void genFortranASumBody(fir::FirOpBuilder &builder,
                               mlir::func::FuncOp &funcOp) {
  // function FortranASum<T>_simplified(arr)
  //   T, dimension(:) :: arr
  //   T sum = 0
  //   integer iter
  //   do iter = 0, extent(arr)
  //     sum = sum + arr[iter]
  //   end do
  //   FortranASum<T>_simplified = sum
  // end function FortranASum<T>_simplified
  auto loc = mlir::UnknownLoc::get(builder.getContext());
  mlir::Type elementType = funcOp.getResultTypes()[0];
  builder.setInsertionPointToEnd(funcOp.addEntryBlock());

  mlir::IndexType idxTy = builder.getIndexType();

  mlir::Value zero = elementType.isa<mlir::FloatType>()
                         ? builder.createRealConstant(loc, elementType, 0.0)
                         : builder.createIntegerConstant(loc, elementType, 0);
  mlir::Value sum = builder.create<fir::AllocaOp>(loc, elementType);
  builder.create<fir::StoreOp>(loc, zero, sum);

  mlir::Block::BlockArgListType args = funcOp.front().getArguments();
  mlir::Value arg = args[0];

  mlir::Value zeroIdx = builder.createIntegerConstant(loc, idxTy, 0);

  fir::SequenceType::Shape flatShape = {fir::SequenceType::getUnknownExtent()};
  mlir::Type arrTy = fir::SequenceType::get(flatShape, elementType);
  mlir::Type boxArrTy = fir::BoxType::get(arrTy);
  mlir::Value array = builder.create<fir::ConvertOp>(loc, boxArrTy, arg);
  auto dims =
      builder.create<fir::BoxDimsOp>(loc, idxTy, idxTy, idxTy, array, zeroIdx);
  mlir::Value len = dims.getResult(1);
  mlir::Value one = builder.createIntegerConstant(loc, idxTy, 1);
  mlir::Value step = one;

  // We use C indexing here, so len-1 as loopcount
  mlir::Value loopCount = builder.create<mlir::arith::SubIOp>(loc, len, one);
  auto loop = builder.create<fir::DoLoopOp>(loc, zeroIdx, loopCount, step);

  // Begin loop code
  mlir::OpBuilder::InsertPoint loopEndPt = builder.saveInsertionPoint();
  builder.setInsertionPointToStart(loop.getBody());

  mlir::Type eleRefTy = builder.getRefType(elementType);
  mlir::Value index = loop.getInductionVar();
  mlir::Value addr =
      builder.create<fir::CoordinateOp>(loc, eleRefTy, array, index);
  mlir::Value elem = builder.create<fir::LoadOp>(loc, addr);
  mlir::Value sumVal = builder.create<fir::LoadOp>(loc, sum);

  mlir::Value res;
  if (elementType.isa<mlir::FloatType>())
    res = builder.create<mlir::arith::AddFOp>(loc, elem, sumVal);
  else if (elementType.isa<mlir::IntegerType>())
    res = builder.create<mlir::arith::AddIOp>(loc, elem, sumVal);
  else
    TODO(loc, "Unsupported type");

  builder.create<fir::StoreOp>(loc, res, sum);
  // End of loop.
  builder.restoreInsertionPoint(loopEndPt);

  mlir::Value resultVal = builder.create<fir::LoadOp>(loc, sum);
  builder.create<mlir::func::ReturnOp>(loc, resultVal);
}

/// Generate function type for the simplified version of FortranADotProduct
/// operating on the given \p elementType.
static mlir::FunctionType genFortranADotType(fir::FirOpBuilder &builder,
                                             const mlir::Type &elementType) {
  mlir::Type boxType = fir::BoxType::get(builder.getNoneType());
  return mlir::FunctionType::get(builder.getContext(), {boxType, boxType},
                                 {elementType});
}

/// Generate function body of the simplified version of FortranADotProduct
/// with signature provided by \p funcOp. The caller is responsible
/// for saving/restoring the original insertion point of \p builder.
/// \p funcOp is expected to be empty on entry to this function.
/// \p arg1ElementTy and \p arg2ElementTy specify elements types
/// of the underlying array objects - they are used to generate proper
/// element accesses.
static void genFortranADotBody(fir::FirOpBuilder &builder,
                               mlir::func::FuncOp &funcOp,
                               mlir::Type arg1ElementTy,
                               mlir::Type arg2ElementTy) {
  // function FortranADotProduct<T>_simplified(arr1, arr2)
  //   T, dimension(:) :: arr1, arr2
  //   T product = 0
  //   integer iter
  //   do iter = 0, extent(arr1)
  //     product = product + arr1[iter] * arr2[iter]
  //   end do
  //   FortranADotProduct<T>_simplified = product
  // end function FortranADotProduct<T>_simplified
  auto loc = mlir::UnknownLoc::get(builder.getContext());
  mlir::Type resultElementType = funcOp.getResultTypes()[0];
  builder.setInsertionPointToEnd(funcOp.addEntryBlock());

  mlir::IndexType idxTy = builder.getIndexType();

  mlir::Value zero =
      resultElementType.isa<mlir::FloatType>()
          ? builder.createRealConstant(loc, resultElementType, 0.0)
          : builder.createIntegerConstant(loc, resultElementType, 0);

  mlir::Block::BlockArgListType args = funcOp.front().getArguments();
  mlir::Value arg1 = args[0];
  mlir::Value arg2 = args[1];

  mlir::Value zeroIdx = builder.createIntegerConstant(loc, idxTy, 0);

  fir::SequenceType::Shape flatShape = {fir::SequenceType::getUnknownExtent()};
  mlir::Type arrTy1 = fir::SequenceType::get(flatShape, arg1ElementTy);
  mlir::Type boxArrTy1 = fir::BoxType::get(arrTy1);
  mlir::Value array1 = builder.create<fir::ConvertOp>(loc, boxArrTy1, arg1);
  mlir::Type arrTy2 = fir::SequenceType::get(flatShape, arg2ElementTy);
  mlir::Type boxArrTy2 = fir::BoxType::get(arrTy2);
  mlir::Value array2 = builder.create<fir::ConvertOp>(loc, boxArrTy2, arg2);
  // This version takes the loop trip count from the first argument.
  // If the first argument's box has unknown (at compilation time)
  // extent, then it may be better to take the extent from the second
  // argument - so that after inlining the loop may be better optimized, e.g.
  // fully unrolled. This requires generating two versions of the simplified
  // function and some analysis at the call site to choose which version
  // is more profitable to call.
  // Note that we can assume that both arguments have the same extent.
  auto dims =
      builder.create<fir::BoxDimsOp>(loc, idxTy, idxTy, idxTy, array1, zeroIdx);
  mlir::Value len = dims.getResult(1);
  mlir::Value one = builder.createIntegerConstant(loc, idxTy, 1);
  mlir::Value step = one;

  // We use C indexing here, so len-1 as loopcount
  mlir::Value loopCount = builder.create<mlir::arith::SubIOp>(loc, len, one);
  auto loop = builder.create<fir::DoLoopOp>(loc, zeroIdx, loopCount, step,
                                            /*unordered=*/false,
                                            /*finalCountValue=*/false, zero);
  mlir::Value sumVal = loop.getRegionIterArgs()[0];

  // Begin loop code
  mlir::OpBuilder::InsertPoint loopEndPt = builder.saveInsertionPoint();
  builder.setInsertionPointToStart(loop.getBody());

  mlir::Type eleRef1Ty = builder.getRefType(arg1ElementTy);
  mlir::Value index = loop.getInductionVar();
  mlir::Value addr1 =
      builder.create<fir::CoordinateOp>(loc, eleRef1Ty, array1, index);
  mlir::Value elem1 = builder.create<fir::LoadOp>(loc, addr1);
  // Convert to the result type.
  elem1 = builder.create<fir::ConvertOp>(loc, resultElementType, elem1);

  mlir::Type eleRef2Ty = builder.getRefType(arg2ElementTy);
  mlir::Value addr2 =
      builder.create<fir::CoordinateOp>(loc, eleRef2Ty, array2, index);
  mlir::Value elem2 = builder.create<fir::LoadOp>(loc, addr2);
  // Convert to the result type.
  elem2 = builder.create<fir::ConvertOp>(loc, resultElementType, elem2);

  if (resultElementType.isa<mlir::FloatType>())
    sumVal = builder.create<mlir::arith::AddFOp>(
        loc, builder.create<mlir::arith::MulFOp>(loc, elem1, elem2), sumVal);
  else if (resultElementType.isa<mlir::IntegerType>())
    sumVal = builder.create<mlir::arith::AddIOp>(
        loc, builder.create<mlir::arith::MulIOp>(loc, elem1, elem2), sumVal);
  else
    llvm_unreachable("unsupported type");

  builder.create<fir::ResultOp>(loc, sumVal);
  // End of loop.
  builder.restoreInsertionPoint(loopEndPt);

  mlir::Value resultVal = loop.getResult(0);
  builder.create<mlir::func::ReturnOp>(loc, resultVal);
}

mlir::func::FuncOp SimplifyIntrinsicsPass::getOrCreateFunction(
    fir::FirOpBuilder &builder, const mlir::StringRef &baseName,
    FunctionTypeGeneratorTy typeGenerator,
    FunctionBodyGeneratorTy bodyGenerator) {
  // WARNING: if the function generated here changes its signature
  //          or behavior (the body code), we should probably embed some
  //          versioning information into its name, otherwise libraries
  //          statically linked with older versions of Flang may stop
  //          working with object files created with newer Flang.
  //          We can also avoid this by using internal linkage, but
  //          this may increase the size of final executable/shared library.
  std::string replacementName = mlir::Twine{baseName, "_simplified"}.str();
  mlir::ModuleOp module = builder.getModule();
  // If we already have a function, just return it.
  mlir::func::FuncOp newFunc =
      fir::FirOpBuilder::getNamedFunction(module, replacementName);
  mlir::FunctionType fType = typeGenerator(builder);
  if (newFunc) {
    assert(newFunc.getFunctionType() == fType &&
           "type mismatch for simplified function");
    return newFunc;
  }

  // Need to build the function!
  auto loc = mlir::UnknownLoc::get(builder.getContext());
  newFunc =
      fir::FirOpBuilder::createFunction(loc, module, replacementName, fType);
  auto inlineLinkage = mlir::LLVM::linkage::Linkage::LinkonceODR;
  auto linkage =
      mlir::LLVM::LinkageAttr::get(builder.getContext(), inlineLinkage);
  newFunc->setAttr("llvm.linkage", linkage);

  // Save the position of the original call.
  mlir::OpBuilder::InsertPoint insertPt = builder.saveInsertionPoint();

  bodyGenerator(builder, newFunc);

  // Now back to where we were adding code earlier...
  builder.restoreInsertionPoint(insertPt);

  return newFunc;
}

fir::ConvertOp expectConvertOp(mlir::Value val) {
  if (fir::ConvertOp op =
          mlir::dyn_cast_or_null<fir::ConvertOp>(val.getDefiningOp()))
    return op;
  LLVM_DEBUG(llvm::dbgs() << "Didn't find expected fir::ConvertOp\n");
  return nullptr;
}

static bool isOperandAbsent(mlir::Value val) {
  if (auto op = expectConvertOp(val)) {
    assert(op->getOperands().size() != 0);
    return mlir::isa_and_nonnull<fir::AbsentOp>(
        op->getOperand(0).getDefiningOp());
  }
  return false;
}

static bool isZero(mlir::Value val) {
  if (auto op = expectConvertOp(val)) {
    assert(op->getOperands().size() != 0);
    if (mlir::Operation *defOp = op->getOperand(0).getDefiningOp())
      return mlir::matchPattern(defOp, mlir::m_Zero());
  }
  return false;
}

static mlir::Value findShape(mlir::Value val) {
  if (auto op = expectConvertOp(val)) {
    assert(op->getOperands().size() != 0);
    if (auto box = mlir::dyn_cast_or_null<fir::EmboxOp>(
            op->getOperand(0).getDefiningOp()))
      return box.getShape();
  }
  return {};
}

static unsigned getDimCount(mlir::Value val) {
  if (mlir::Value shapeVal = findShape(val)) {
    mlir::Type resType = shapeVal.getDefiningOp()->getResultTypes()[0];
    return fir::getRankOfShapeType(resType);
  }
  return 0;
}

/// Given the call operation's box argument \p val, discover
/// the element type of the underlying array object.
/// \returns the element type or llvm::None if the type cannot
/// be reliably found.
/// We expect that the argument is a result of fir.convert
/// with the destination type of !fir.box<none>.
static llvm::Optional<mlir::Type> getArgElementType(mlir::Value val) {
  mlir::Operation *defOp;
  do {
    defOp = val.getDefiningOp();
    // Analyze only sequences of convert operations.
    if (!mlir::isa<fir::ConvertOp>(defOp))
      return llvm::None;
    val = defOp->getOperand(0);
    // The convert operation is expected to convert from one
    // box type to another box type.
    auto boxType = val.getType().cast<fir::BoxType>();
    auto elementType = fir::unwrapSeqOrBoxedSeqType(boxType);
    if (!elementType.isa<mlir::NoneType>())
      return elementType;
  } while (true);
}

void SimplifyIntrinsicsPass::runOnOperation() {
  LLVM_DEBUG(llvm::dbgs() << "=== Begin " DEBUG_TYPE " ===\n");
  mlir::ModuleOp module = getOperation();
  fir::KindMapping kindMap = fir::getKindMapping(module);
  module.walk([&](mlir::Operation *op) {
    if (auto call = mlir::dyn_cast<fir::CallOp>(op)) {
      if (mlir::SymbolRefAttr callee = call.getCalleeAttr()) {
        mlir::StringRef funcName = callee.getLeafReference().getValue();
        // Replace call to runtime function for SUM when it has single
        // argument (no dim or mask argument) for 1D arrays with either
        // Integer4 or Real8 types. Other forms are ignored.
        // The new function is added to the module.
        //
        // Prototype for runtime call (from sum.cpp):
        // RTNAME(Sum<T>)(const Descriptor &x, const char *source, int line,
        //                int dim, const Descriptor *mask)
        //
        // Disable SUM inlining by default, because it fatally fails on some
        // FIR yet.
        if (enableExperimental && funcName.startswith("_FortranASum")) {
          mlir::Operation::operand_range args = call.getArgs();
          // args[1] and args[2] are source filename and line number, ignored.
          const mlir::Value &dim = args[3];
          const mlir::Value &mask = args[4];
          // dim is zero when it is absent, which is an implementation
          // detail in the runtime library.
          bool dimAndMaskAbsent = isZero(dim) && isOperandAbsent(mask);
          unsigned rank = getDimCount(args[0]);
          if (dimAndMaskAbsent && rank == 1) {
            mlir::Location loc = call.getLoc();
            mlir::Type type;
            fir::FirOpBuilder builder(op, kindMap);
            if (funcName.endswith("Integer4")) {
              type = mlir::IntegerType::get(builder.getContext(), 32);
            } else if (funcName.endswith("Real8")) {
              type = mlir::FloatType::getF64(builder.getContext());
            } else {
              return;
            }
            auto typeGenerator = [&type](fir::FirOpBuilder &builder) {
              return genFortranASumType(builder, type);
            };
            mlir::func::FuncOp newFunc = getOrCreateFunction(
                builder, funcName, typeGenerator, genFortranASumBody);
            auto newCall = builder.create<fir::CallOp>(
                loc, newFunc, mlir::ValueRange{args[0]});
            call->replaceAllUsesWith(newCall.getResults());
            call->dropAllReferences();
            call->erase();
          }

          return;
        }
        if (funcName.startswith("_FortranADotProduct")) {
          LLVM_DEBUG(llvm::dbgs() << "Handling " << funcName << "\n");
          LLVM_DEBUG(llvm::dbgs() << "Call operation:\n"; op->dump();
                     llvm::dbgs() << "\n");
          mlir::Operation::operand_range args = call.getArgs();
          const mlir::Value &v1 = args[0];
          const mlir::Value &v2 = args[1];
          mlir::Location loc = call.getLoc();
          fir::FirOpBuilder builder(op, kindMap);
          mlir::Type type = call.getResult(0).getType();
          if (!type.isa<mlir::FloatType>() && !type.isa<mlir::IntegerType>())
            return;

          // Try to find the element types of the boxed arguments.
          auto arg1Type = getArgElementType(v1);
          auto arg2Type = getArgElementType(v2);

          if (!arg1Type || !arg2Type)
            return;

          // Support only floating point and integer arguments
          // now (e.g. logical is skipped here).
          if (!arg1Type->isa<mlir::FloatType>() &&
              !arg1Type->isa<mlir::IntegerType>())
            return;
          if (!arg2Type->isa<mlir::FloatType>() &&
              !arg2Type->isa<mlir::IntegerType>())
            return;

          auto typeGenerator = [&type](fir::FirOpBuilder &builder) {
            return genFortranADotType(builder, type);
          };
          auto bodyGenerator = [&arg1Type,
                                &arg2Type](fir::FirOpBuilder &builder,
                                           mlir::func::FuncOp &funcOp) {
            genFortranADotBody(builder, funcOp, *arg1Type, *arg2Type);
          };

          // Suffix the function name with the element types
          // of the arguments.
          std::string typedFuncName(funcName);
          llvm::raw_string_ostream nameOS(typedFuncName);
          nameOS << "_";
          arg1Type->print(nameOS);
          nameOS << "_";
          arg2Type->print(nameOS);

          mlir::func::FuncOp newFunc = getOrCreateFunction(
              builder, typedFuncName, typeGenerator, bodyGenerator);
          auto newCall = builder.create<fir::CallOp>(loc, newFunc,
                                                     mlir::ValueRange{v1, v2});
          call->replaceAllUsesWith(newCall.getResults());
          call->dropAllReferences();
          call->erase();

          LLVM_DEBUG(llvm::dbgs() << "Replaced with:\n"; newCall.dump();
                     llvm::dbgs() << "\n");
          return;
        }
      }
    }
  });
  LLVM_DEBUG(llvm::dbgs() << "=== End " DEBUG_TYPE " ===\n");
}

void SimplifyIntrinsicsPass::getDependentDialects(
    mlir::DialectRegistry &registry) const {
  // LLVM::LinkageAttr creation requires that LLVM dialect is loaded.
  registry.insert<mlir::LLVM::LLVMDialect>();
}
std::unique_ptr<mlir::Pass> fir::createSimplifyIntrinsicsPass() {
  return std::make_unique<SimplifyIntrinsicsPass>();
}
