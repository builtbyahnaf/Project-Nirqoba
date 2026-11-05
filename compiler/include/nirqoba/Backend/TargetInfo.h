// Backend/TargetInfo.h — target triple and data layout for the Nirqoba backend.
//
// Keeping target information in one place means the ObjectEmitter and the
// version string both read the same value; there is no risk of a mismatch
// between the triple the TargetMachine uses and the triple printed by
// `nirqoba version`.
#pragma once

#include <string>

#include "nirqoba/Backend/Optimization.h"

namespace nirq {

class DiagnosticEngine;

// The concrete triple, CPU, and feature set the backend will use after the
// requested profile has been resolved. `Portable` and `Native` ignore most of
// the raw options; `Explicit` uses them verbatim.
struct TargetConfiguration {
  TargetProfile profile = TargetProfile::Portable;
  std::string triple;
  std::string cpu = "generic";
  std::string features;
  // True when CPU and features came from the host rather than the command
  // line. Verbose output and tests use it to prove native tuning happened.
  bool from_host = false;
};

// The default target triple for Phase 1 builds. Set at CMake time via
// NIRQOBA_DEFAULT_TARGET_TRIPLE (see cmake/NirqobaLLVMTargets.cmake) and
// compiled into the binary through Version.h.
//
// A function rather than a constant so tests can inspect it without linking
// the generated Version.h that depends on cmake configuration.
std::string default_target_triple();

// The triple of the machine running the compiler. The Native profile uses it
// so host CPU name and host feature set can never be paired with a different
// architecture.
std::string host_target_triple();

// Turns the profile plus raw CPU/features/triple options into the values code
// generation will use. Rejects contradictory combinations (for example
// `portable` with a named CPU) and validates named CPUs and features against
// the selected target. Records NRQ5002 diagnostics and returns false on error.
bool resolve_target_configuration(const OptimizationOptions& options,
                                  TargetConfiguration& resolved,
                                  DiagnosticEngine& diagnostics);

// One-line summary for `--verbose` (for example
// "portable (x86_64-pc-windows-msvc, generic)"). Never empty.
std::string describe_target_configuration(const TargetConfiguration& config);

}  // namespace nirq
