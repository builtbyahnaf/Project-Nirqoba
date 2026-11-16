// CFG-aware borrow and lifetime verification for TIR.
//
// Phase 3's semantic checks are path-sensitive over the AST, but the verified
// IR also has to prove the same rules after lowering. This pass builds the
// control-flow graph from the flat label/branch operation stream and computes
// the set of borrows that may still be active at every block entry. That makes
// branch merges, loop back-edges and early returns part of the borrow contract
// instead of relying on a straight-line scan.
#pragma once

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/TIR/IR.h"

namespace nirq {

// Returns true when every block in `function` preserves the borrow contract.
// Violations are reported as NRQ4007 so they stay distinguishable from
// structural, type and destruction failures.
bool verify_borrow_flow(const tir::Function& function,
                        const tir::Module& module,
                        DiagnosticEngine& diagnostics);

}  // namespace nirq
