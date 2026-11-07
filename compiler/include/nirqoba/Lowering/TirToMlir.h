// Lowering/TirToMlir.h — translates a TIR module into an MLIR module.
//
// This is the bridge between the Nirqoba-specific TIR and the generic MLIR
// world. It is invoked once per compilation after TIR verification succeeds.
#pragma once

#include <memory>

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/TIR/IR.h"

namespace nirq {

// Translates `tir_module` into a fresh MLIR ModuleOp owned by `context`.
// Returns nullptr and records an NRQ4xxx diagnostic if translation fails.
//
// Preconditions:
//   - `tir_module` has passed verify_tir_module() with no errors.
//   - The nrq dialect and func dialect are registered in `context`.
//
// Work done by the GC root-liveness analysis, counted so its cost can be checked
// without timing anything. The analysis is the one place where the lowering walks
// the whole function per root candidate, so these numbers are what a complexity
// change has to move.
struct GcRootAnalysisStats {
  // Functions the analysis ran on.
  std::size_t functions = 0;
  // Operations visited, summed over those functions.
  std::size_t operations = 0;
  // Stack addresses attributed to a managed slot, including derived ones.
  std::size_t slot_addresses = 0;
  // Get-element-pointer operations resolved to a slot.
  std::size_t address_derivations = 0;
  // Loads recorded against the slot they read.
  std::size_t loads = 0;
  // Slot candidates looked up in liveness at a collection point.
  std::size_t liveness_probes = 0;
  // Get-element-pointer operations revisited by the address fixpoint.
  std::size_t address_scans = 0;
};

// Totals since the last reset, for the compilation that is running now.
GcRootAnalysisStats gc_root_analysis_stats();
void reset_gc_root_analysis_stats();

// `fast_math` opts floating-point arithmetic into reassociation and FMA
// contraction; the default leaves IEEE semantics strict.
mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics,
    bool fast_math = false);

}  // namespace nirq
