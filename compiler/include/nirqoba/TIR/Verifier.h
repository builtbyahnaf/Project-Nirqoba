// Structural and type checks for TIR before it is translated to MLIR.
//
// The verifier exists so a lowering bug surfaces as a diagnostic pointing at
// the source line instead of as an opaque MLIR/LLVM failure or a crashing
// executable. It is always run in debug and release builds.
#pragma once

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/TIR/IR.h"

namespace nirq {

// Returns true when the module verifies. Every violation is reported as
// NRQ4003 (structure) or NRQ4004 (types) with the offending source location.
bool verify_tir_module(const tir::Module& module, DiagnosticEngine& diagnostics);

}  // namespace nirq
