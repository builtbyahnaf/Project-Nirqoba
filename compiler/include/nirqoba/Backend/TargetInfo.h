// Backend/TargetInfo.h — target triple and data layout for the Nirqoba backend.
//
// Keeping target information in one place means the ObjectEmitter and the
// version string both read the same value; there is no risk of a mismatch
// between the triple the TargetMachine uses and the triple printed by
// `nirqoba version`.
#pragma once

#include <string>

namespace nirq {

// The default target triple for Phase 1 builds. Set at CMake time via
// NIRQOBA_DEFAULT_TARGET_TRIPLE (see cmake/NirqobaLLVMTargets.cmake) and
// compiled into the binary through Version.h.
//
// A function rather than a constant so tests can inspect it without linking
// the generated Version.h that depends on cmake configuration.
std::string default_target_triple();

}  // namespace nirq
