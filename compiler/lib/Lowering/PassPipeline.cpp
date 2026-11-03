#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"

#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinDialect.h"
#include "llvm/ADT/APInt.h"
#include "mlir/Pass/Pass.h"  // completes mlir::Pass for the unique_ptr created below
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Transforms/Passes.h"

#include "nirqoba/Lowering/NrqDialect.h"

// The NrqToLLVM pass is declared here; its definition is in NrqToLLVMPass.cpp.
namespace nirq {
using namespace std;

unique_ptr<mlir::Pass> createNrqToLLVMPass();
}

namespace nirq {

void register_all_dialects(mlir::DialectRegistry& registry) {
  registry.insert<
    mlir::BuiltinDialect,
    mlir::func::FuncDialect,
    mlir::LLVM::LLVMDialect,
    nirq::nrq::NrqDialect
  >();
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
}

void register_dialects(mlir::DialectRegistry& registry) {
  register_all_dialects(registry);
}

namespace {

// MLIR's canonicalizer has no folders for LLVM-dialect integer arithmetic, so
// constants introduced by expression lowering survive until LLVM. Folding them
// here exposes simpler SSA values to SCCP and reduces the IR that reaches the
// backend. The pass is deliberately limited to integer operations with no
// exceptional behavior: division and remainder are skipped because a division
// by zero is not foldable.
class FoldLlvmIntegerConstantsPass
    : public mlir::PassWrapper<FoldLlvmIntegerConstantsPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FoldLlvmIntegerConstantsPass)

  llvm::StringRef getArgument() const final {
    return "fold-llvm-integer-constants";
  }
  llvm::StringRef getDescription() const final {
    return "Fold LLVM integer arithmetic and comparisons with constant operands";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::Operation* operation) {
      if (operation->getNumResults() != 1) return;
      auto result_type =
          mlir::dyn_cast<mlir::IntegerType>(operation->getResult(0).getType());
      if (!result_type) return;

      if (auto compare = mlir::dyn_cast<mlir::LLVM::ICmpOp>(operation)) {
        llvm::APInt lhs;
        llvm::APInt rhs;
        if (!constant_integer(compare.getLhs(), lhs) ||
            !constant_integer(compare.getRhs(), rhs)) return;
        bool result = false;
        switch (compare.getPredicate()) {
          case mlir::LLVM::ICmpPredicate::eq: result = lhs == rhs; break;
          case mlir::LLVM::ICmpPredicate::ne: result = lhs != rhs; break;
          case mlir::LLVM::ICmpPredicate::slt: result = lhs.slt(rhs); break;
          case mlir::LLVM::ICmpPredicate::sle: result = lhs.sle(rhs); break;
          case mlir::LLVM::ICmpPredicate::sgt: result = lhs.sgt(rhs); break;
          case mlir::LLVM::ICmpPredicate::sge: result = lhs.sge(rhs); break;
          case mlir::LLVM::ICmpPredicate::ult: result = lhs.ult(rhs); break;
          case mlir::LLVM::ICmpPredicate::ule: result = lhs.ule(rhs); break;
          case mlir::LLVM::ICmpPredicate::ugt: result = lhs.ugt(rhs); break;
          case mlir::LLVM::ICmpPredicate::uge: result = lhs.uge(rhs); break;
        }
        replace_with_constant(operation, result_type,
                              llvm::APInt(1, result ? 1 : 0));
        return;
      }

      if (operation->getNumOperands() != 2) return;
      llvm::APInt lhs;
      llvm::APInt rhs;
      if (!constant_integer(operation->getOperand(0), lhs) ||
          !constant_integer(operation->getOperand(1), rhs)) return;

      llvm::StringRef name = operation->getName().getStringRef();
      llvm::APInt result;
      if (name == "llvm.add") {
        result = lhs + rhs;
      } else if (name == "llvm.sub") {
        result = lhs - rhs;
      } else if (name == "llvm.mul") {
        result = lhs * rhs;
      } else if (name == "llvm.and") {
        result = lhs & rhs;
      } else if (name == "llvm.or") {
        result = lhs | rhs;
      } else if (name == "llvm.xor") {
        result = lhs ^ rhs;
      } else if (name == "llvm.shl") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.shl(rhs);
      } else if (name == "llvm.lshr") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.lshr(rhs);
      } else if (name == "llvm.ashr") {
        if (rhs.uge(lhs.getBitWidth())) return;
        result = lhs.ashr(rhs);
      } else {
        return;
      }
      replace_with_constant(operation, result_type, result);
    });
  }

 private:
  static bool constant_integer(mlir::Value value, llvm::APInt& result) {
    auto constant = value.getDefiningOp<mlir::LLVM::ConstantOp>();
    if (!constant) return false;
    auto integer = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
    if (!integer) return false;
    result = integer.getValue();
    return true;
  }

  static void replace_with_constant(mlir::Operation* operation,
                                    mlir::Type type,
                                    const llvm::APInt& value) {
    mlir::OpBuilder builder(operation);
    auto constant = builder.create<mlir::LLVM::ConstantOp>(
        operation->getLoc(), type,
        mlir::IntegerAttr::get(type, value));
    operation->getResult(0).replaceAllUsesWith(constant.getResult());
    operation->erase();
  }
};

// LLVM's `cond_br` does not currently canonicalize a constant condition into
// an unconditional branch. SCCP proves the condition constant, but the
// unreachable edge remains until the terminator itself is rewritten. Doing that
// here lets the following canonicalizer and trivial-DCE remove the now-dead
// block and its constants. The branch operands are carried over unchanged, so
// block-argument types and values are preserved.
class FoldConstantLlvmCondBrPass
    : public mlir::PassWrapper<FoldConstantLlvmCondBrPass,
                               mlir::OperationPass<>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(FoldConstantLlvmCondBrPass)

  llvm::StringRef getArgument() const final {
    return "fold-constant-llvm-condbr";
  }
  llvm::StringRef getDescription() const final {
    return "Fold llvm.cond_br with a constant i1 condition";
  }

  void runOnOperation() override {
    getOperation()->walk([&](mlir::LLVM::CondBrOp branch) {
      auto constant =
          branch.getCondition().getDefiningOp<mlir::LLVM::ConstantOp>();
      if (!constant || !constant.getType().isInteger(1)) return;
      auto value = mlir::dyn_cast<mlir::IntegerAttr>(constant.getValue());
      if (!value) return;

      mlir::OpBuilder builder(branch);
      const bool taken = value.getInt() != 0;
      auto unconditional = builder.create<mlir::LLVM::BrOp>(
          branch.getLoc(),
          taken ? branch.getTrueDestOperands()
                : branch.getFalseDestOperands(),
          taken ? branch.getTrueDest() : branch.getFalseDest());
      // Keep loop metadata attached to the surviving edge.
      unconditional.setLoopAnnotationAttr(branch.getLoopAnnotationAttr());
      branch.erase();
    });
  }
};

// Adds the semantics-preserving MLIR cleanup shared by the pre- and
// post-lowering stages.
//
// The canonicalizer folds constants, removes trivially dead operations whose
// results are unused, and simplifies control flow. SCCP then propagates known
// constants through SSA values, block arguments, and conditional control flow,
// proving edges and blocks unreachable. A second canonicalization folds the
// operations exposed by SCCP before CSE merges structurally identical pure
// operations. Trivial DCE runs last and drops the operations and blocks made
// dead by those simplifications.
//
// Every cleanup pass relies on MLIR's side-effect interfaces, so calls, runtime
// operations, GC-root operations, ownership transfers, and cleanup calls are
// retained: an operation without a proven memory-effect model is never treated
// as removable or deduplicable.
//
// Order matters: folding and propagation simplify the CFG before CSE looks for
// equal expressions, and the final canonicalization plus trivial DCE removes
// the now-dead operands and blocks without leaving a second cleanup round.
void add_cleanup_passes(mlir::PassManager& pm) {
  pm.addPass(mlir::createCanonicalizerPass());   // structural folding
  pm.addPass(std::make_unique<FoldLlvmIntegerConstantsPass>());
  pm.addPass(mlir::createSCCPPass());            // propagation + reachability
  pm.addPass(std::make_unique<FoldLlvmIntegerConstantsPass>());
  pm.addPass(std::make_unique<FoldConstantLlvmCondBrPass>());
  pm.addPass(mlir::createCanonicalizerPass());   // fold SCCP results
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::createTrivialDeadCodeEliminationPass());
}

}  // namespace

void build_lowering_pipeline(mlir::PassManager& pm, bool optimize) {
  // MLIR-level cleanup catches duplicated constants, unreachable blocks, and
  // dead alias-forming operations before they reach LLVM. At -O0 the direct
  // lowering is preserved for inspection and debugging.
  if (optimize) {
    add_cleanup_passes(pm);
  }

  // Step 1: Lower nrq dialect ops to LLVM dialect calls.
  pm.addPass(createNrqToLLVMPass());

  // Step 2: Lower func.func to llvm.func (standard MLIR conversion).
  pm.addPass(mlir::createConvertFuncToLLVMPass());

  // Step 3: Resolve any remaining unrealized cast ops left by the two
  // conversions above; this is required before LLVM IR export.
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());

  if (optimize) {
    // Canonicalization and CSE are legal on LLVM-dialect IR too. Running the
    // same cleanup again removes duplicate constants and casts introduced by
    // the conversion passes while leaving every call and runtime operation
    // intact.
    add_cleanup_passes(pm);
  }
}

}  // namespace nirq
