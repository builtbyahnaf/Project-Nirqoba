// Lowering/RegisterDialects.h — convenience header for dialect registration.
//
// Included by any translation unit that builds an MLIRContext and needs the
// full Nirqoba dialect set. Keeping the list here instead of duplicating it
// in every test and in the pipeline ensures additions are made in one place.
#pragma once

#include "mlir/IR/DialectRegistry.h"

namespace nirq {

// Appends all Phase 1 dialects to `registry`. Idempotent; safe to call more
// than once on the same registry (MLIR deduplicates registrations).
void register_all_dialects(mlir::DialectRegistry& registry);

}  // namespace nirq
