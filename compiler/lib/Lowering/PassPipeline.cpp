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

#include "nirqoba/Lowering/NrqDialect.h"

// The NrqToLLVM pass is declared here; its definition is in NrqToLLVMPass.cpp.
namespace nirq {
std::unique_ptr<mlir::Pass> createNrqToLLVMPass();
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

void build_lowering_pipeline(mlir::PassManager& pm) {
  // Step 1: Lower nrq dialect ops to LLVM dialect calls.
  pm.addPass(createNrqToLLVMPass());

  // Step 2: Lower func.func to llvm.func (standard MLIR conversion).
  pm.addPass(mlir::createConvertFuncToLLVMPass());

  // Step 3: Resolve any remaining unrealized cast ops left by the two
  // conversions above; this is required before LLVM IR export.
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());
}

}  // namespace nirq
