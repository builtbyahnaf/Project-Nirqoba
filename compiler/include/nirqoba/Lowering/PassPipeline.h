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

// Populates `pm` with the MLIR lowering and cleanup pipeline:
//   1. canonicalize + fold integer constants + SCCP + fold again + fold
//      constant branches + canonicalize + CSE + trivial-DCE (`optimize` only)
//   2. nrq-to-llvm         (our custom pass — lowers nrq.print_string)
//   3. convert-func-to-llvm
//   4. reconcile-unrealized-casts
//   5. repeat the optimized cleanup on the LLVM dialect IR
//
// The cleanup passes are semantics-preserving: they never erase an operation
// that has not proven itself free of side effects, and they never destroy a
// reachable runtime, GC-root, ownership, or cleanup call. SCCP only replaces
// values it can prove constant and removes edges it can prove unreachable.
//
// `optimize` is false for -O0 so debugging sees the direct lowering result.
// The pipeline is intentionally a list so later passes can be inserted without
// restructuring the driver.
void build_lowering_pipeline(mlir::PassManager& pm, bool optimize = true);

}  // namespace nirq
