// Lowering/PassPipeline.h — manages the MLIR dialect registry and pass pipeline.
//
// The registry and pipeline are the two extensibility points ROADMAP §3.3
// requires to be in place from Phase 1: later phases add dialects and passes
// here rather than touching the core lowering code.
#pragma once

#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"

namespace nirq {

// Registers all dialects that the Phase 1 compiler uses: BuiltinDialect,
// func dialect, llvm dialect, and the Nirqoba nrq dialect. Call once per
// MLIRContext before any lowering.
void register_dialects(mlir::DialectRegistry& registry);

// Populates `pm` with the Phase 1 lowering pipeline:
//   1. nrq-to-llvm         (our custom pass — lowers nrq.print_string)
//   2. convert-func-to-llvm
//   3. reconcile-unrealized-casts
//
// These three passes are the minimum needed to produce an llvm.module that
// mlir::translateModuleToLLVMIR can consume. The pipeline is intentionally
// left as a list so Phase 2 can prepend an arithmetic-lowering pass without
// restructuring this function.
void build_lowering_pipeline(mlir::PassManager& pm);

}  // namespace nirq
