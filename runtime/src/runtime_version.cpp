// runtime/src/runtime_version.cpp
// Returns the runtime version string.
//
#include "nirqoba/runtime/version.h"
#include "nirqoba/Version.h"

extern "C" {

const char* nrq_runtime_version(void) {
    return NIRQOBA_VERSION_STRING "-phase" NIRQOBA_PHASE;
}

}  // extern "C"
