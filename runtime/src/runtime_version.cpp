// runtime/src/runtime_version.cpp
// Returns the runtime version string.
//
// The version string is hard-coded rather than generated from a header because
// the runtime is a standalone library that CMake links into the compiled
// program — it does not have access to the compiler's generated Version.h.
// Both version strings must be kept in sync manually; the phase report records
// the expected value.

#include "nirqoba/runtime/version.h"

extern "C" {

const char* nrq_runtime_version(void) {
    // "major.minor.patch — Phase N" format used by `nirqoba version`.
    return "0.3.0-phase3";
}

}  // extern "C"
