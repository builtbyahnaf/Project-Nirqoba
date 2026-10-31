#include "nirqoba/Lowering/NrqDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "nirqoba/Lowering/NrqOps.h"

// Dialect definition: name, namespace and the attribute printer/parser hooks
// selected in NrqOps.td.
#include "nirqoba/Lowering/NrqDialect.cpp.inc"

void nirq::nrq::NrqDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "nirqoba/Lowering/NrqOps.cpp.inc"
      >();
}

// Operation definitions (builders, verifiers, parsers).
#define GET_OP_CLASSES
#include "nirqoba/Lowering/NrqOps.cpp.inc"
