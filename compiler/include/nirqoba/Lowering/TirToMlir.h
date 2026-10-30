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
mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics);

}  // namespace nirq
