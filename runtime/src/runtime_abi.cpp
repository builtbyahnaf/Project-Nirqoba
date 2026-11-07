// runtime_abi.cpp — the link-time identity of a runtime configuration.
//
// A program compiled for one configuration is only correct against the runtime
// library built from that configuration: a development object calls entry points
// the production library does not define, while nearly every other symbol the
// two libraries export has the same name. A mistaken link would therefore fail
// late, or not at all, and the second case is the dangerous one.
//
// Three symbols make a mismatch fail at link time instead.
//
//   nrq_runtime_abi_v1_production    defined only by the production library
//   nrq_runtime_abi_v1_development   defined only by the development library
//
// The compiler emits a call to the one matching its own configuration into the
// entry wrapper, so every program references exactly one of them, including a
// program that never touches managed memory. Linking that object against the
// other library leaves the symbol unresolved.
//
//   nrq_runtime_abi_configuration_marker
//
// is defined with external linkage by both libraries. Pulling an object from
// each configuration into one link — the only way to mix instrumented and
// uninstrumented code — is then a duplicate definition. Without it, the linker
// would quietly take whichever definition it saw first, and half the program
// would run against a runtime that does not agree with the other half about how
// the heap is observed.
//
// The marker is never read. It exists to collide.

#include <stdint.h>

#include "runtime_config.h"

extern "C" {

#if NRQ_DEBUG_ENABLED
void nrq_runtime_abi_v1_development(void) {}
#else
void nrq_runtime_abi_v1_production(void) {}
#endif

int nrq_runtime_abi_configuration_marker = NRQ_DEBUG_ENABLED ? 2 : 1;

}  // extern "C"
