// Driver/LinkerEnvironment.h — discovers the linker and system libraries.
//
// Reads toolchain/toolchain.json written by scripts/bootstrap.ps1 and
// exposes the paths needed to invoke lld-link (or MSVC link.exe as fallback)
// and the MSVC/UCRT library directories.
#pragma once

#include <string>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"

namespace nirq {

struct LinkerEnvironment {
  // Absolute path to the linker executable.
  std::string linker_path;
  // Library directories passed as /LIBPATH: to the linker.
  std::vector<std::string> lib_dirs;

  // True when this environment was successfully loaded.
  bool is_valid = false;
};

// Loads the toolchain configuration from `toolchain_json_path` (or the
// default location `<repo_root>/toolchain/toolchain.json` if empty).
// Records NRQ5003 and returns an invalid environment on failure.
LinkerEnvironment load_linker_environment(const std::string& toolchain_json_path,
                                          DiagnosticEngine& diagnostics);

}  // namespace nirq
