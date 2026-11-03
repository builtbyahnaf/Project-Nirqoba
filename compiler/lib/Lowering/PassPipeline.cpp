#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"

#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinDialect.h"
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

// Adds the semantics-preserving MLIR cleanup shared by the pre- and
// post-lowering stages.
//
// The canonicalizer folds constants, removes trivially dead operations whose
// results are unused, and simplifies control flow. CSE then merges structurally
// identical operations that are proven side-effect-free. Both passes rely on
// MLIR's side-effect interfaces, so calls, runtime operations, GC-root
// operations, ownership transfers, and cleanup calls are retained: an operation
// without a proven memory-effect model is never treated as removable or
// deduplicable. Trivial DCE then removes side-effect-free operations that only
// became dead during CSE and drops blocks that are unreachable from their
// region entry, which cannot remove reachable runtime or cleanup work.
//
// Order matters: canonicalization exposes dead operations and simpler CFG, CSE
// deduplicates the remaining pure operations, and trivial DCE runs last so it
// also drops operations and blocks exposed by the preceding simplifications.
void add_cleanup_passes(mlir::PassManager& pm) {
  pm.addPass(mlir::createCanonicalizerPass());
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
