// Backend/Optimization.h — optimization-level and target-selection options
// shared by the driver and the object emitter.
//
// The compiler exposes the same optimization levels as LLVM. `O0` keeps the
// generated IR close to the source, while `O1`–`O3` run the corresponding
// LLVM PassBuilder pipelines. `Os` and `Oz` reuse the `O2` pipeline with the
// function-level size attributes LLVM's pass builder uses to select its
// size-oriented decisions.
#pragma once

#include <string>
#include <string_view>

namespace llvm {
class Module;
class TargetMachine;
}  // namespace llvm

namespace nirq {

class DiagnosticEngine;

enum class OptimizationLevel {
  O0,
  O1,
  O2,
  O3,
  Os,
  Oz,
};

// Returns the spelling accepted on the command line (for example "-O2").
std::string_view optimization_level_name(OptimizationLevel level);

// Parses "0", "1", "2", "3", "s", or "z" (case-insensitive). The accepted
// spelling omits the leading "-O" so callers can share the parser between
// `-O<level>` and `--opt-level=<level>`.
bool parse_optimization_level(std::string_view text, OptimizationLevel& level);

// Target and optimization settings for one compiled module.
struct OptimizationOptions {
  OptimizationLevel level = OptimizationLevel::O2;

  // Empty means the platform default triple (x86_64-pc-windows-msvc).
  std::string target_triple;

  // "generic" keeps the object portable. A named CPU selects that model, and
  // `host_cpu` replaces both fields with the host CPU and its feature set.
  std::string target_cpu = "generic";
  std::string target_features;
  bool host_cpu = false;
};

// Runs the LLVM PassBuilder pipeline for `level` on `module`.
//
// The target machine supplies the target IR analyses used by inlining,
// vectorization, and cost modelling. The module must already have its data
// layout and target triple set from the same target machine. Returns false
// only if the requested level is invalid.
bool optimize_llvm_module(llvm::Module& module,
                          llvm::TargetMachine& target_machine,
                          OptimizationLevel level,
                          DiagnosticEngine& diagnostics);

}  // namespace nirq
