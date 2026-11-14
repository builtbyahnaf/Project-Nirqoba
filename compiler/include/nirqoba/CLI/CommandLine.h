#pragma once

#include <string>
#include <vector>

#include "nirqoba/Backend/Optimization.h"
#include "nirqoba/Driver/CompilationPipeline.h"

namespace nirq {

enum class CommandKind {
  Build,
  Run,
  Check,
  New,
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
  std::string project_dir;  // For `nirqoba new <dir>`
  std::string output_path;
  std::string out_dir = ".";
  std::string help_topic;
  EmitKind emit = EmitKind::Exe;
  bool emit_specified = false;
  bool verbose = false;
  // Compiler optimization and target selection. The defaults preserve a
  // portable release build while allowing debug builds and host tuning.
  OptimizationLevel optimization_level = OptimizationLevel::O2;
  IpoMode ipo_mode = IpoMode::Standard;
  PgoMode pgo_mode = PgoMode::None;
  std::string pgo_profile_path;
  PgoColdMode pgo_cold_mode = PgoColdMode::Default;
  TargetProfile target_profile = TargetProfile::Portable;
  std::string target_triple;
  std::string target_cpu = "generic";
  std::string target_features;
  // True when the user named a profile (including through --native). The
  // parser uses this to distinguish an inferred profile from a requested one
  // when rejecting contradictory flags.
  bool target_profile_specified = false;
  bool native_profile_requested = false;
  // `--remarks` prints LLVM optimization remarks (vectorization, unrolling,
  // inlining) to stderr; `--remarks=missed` prints only declined/analysis ones.
  bool emit_remarks = false;
  bool remarks_missed_only = false;
  std::string remark_filter;
  // `--fast-math` allows reassociation and FMA contraction of floating-point
  // arithmetic so reductions can vectorize; the default keeps strict semantics.
  bool fast_math = false;
  // `--debug-enabled` selects the development configuration: the compiler links
  // the debug runtime library and emits the instrumentation its extra entry
  // points consume (allocation-site tags, event emission). The default is the
  // production path, which contains none of it.
  bool debug_enabled = false;
  // `run` normally removes the executable and object it generated; this keeps
  // them at their final paths for inspection. `build` already keeps them.
  bool keep_artifacts = false;
  std::vector<std::string> trailing_args;  // For `nirqoba run ... -- args`
};

}  // namespace nirq
