#pragma once

#include <string>
#include <vector>

#include "nirqoba/Driver/CompilationPipeline.h"

namespace nirq {

enum class CommandKind {
  Build,
  Run,
  Check,
  Version,
  Help,
  Unknown
};

struct CommandLineOptions {
  CommandKind command = CommandKind::Unknown;
  // Root source files named on the command line. Imported modules are found
  // through `import` declarations, not listed here.
  std::vector<std::string> source_paths;
  std::vector<std::string> module_paths;
  std::string output_path;
  std::string out_dir = ".";
  std::string help_topic;
  EmitKind emit = EmitKind::Exe;
  bool emit_specified = false;
  bool verbose = false;
  std::vector<std::string> trailing_args;  // For `nirqoba run ... -- args`
};

}  // namespace nirq
