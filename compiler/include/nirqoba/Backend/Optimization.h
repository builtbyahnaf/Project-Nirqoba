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

// How much hardware-specific knowledge the backend may use.
//
// Portable keeps the published x86-64 baseline so the object runs on any
// x86-64 Windows machine. Native replaces the CPU and feature set with the
// host's, which is faster but must not be shipped as a universal binary.
// Explicit uses the CPU/features/triple named by the developer, for a known
// deployment target. The profile is resolved and validated once, before the
// target machine and optimization pipeline are created.
enum class TargetProfile {
  Portable,
  Native,
  Explicit,
};

// Returns the spelling accepted on the command line ("portable", "native",
// "explicit").
std::string_view target_profile_name(TargetProfile profile);

// Parses a target profile spelling. Returns false for an unknown name.
bool parse_target_profile(std::string_view text, TargetProfile& profile);

// Returns the spelling accepted on the command line (for example "-O2").
std::string_view optimization_level_name(OptimizationLevel level);

// Parses "0", "1", "2", "3", "s", or "z" (case-insensitive). The accepted
// spelling omits the leading "-O" so callers can share the parser between
// `-O<level>` and `--opt-level=<level>`.
bool parse_optimization_level(std::string_view text, OptimizationLevel& level);

// Which LLVM optimization remarks to print on stderr.
//
// Remarks are LLVM's structured account of what a pass did, or why it declined
// to transform a loop (for example "loop not vectorized: ..."). They are
// diagnostic-only: they never influence the generated code, so enabling them
// cannot change an object file.
enum class RemarkMode {
  None,    // silent (default)
  All,     // transformations performed plus missed/analysis explanations
  Missed,  // missed/analysis explanations only
};

// How much interprocedural optimization the backend should run.
//
// Nirqoba lowers every resolved language module into one LLVM module, so the
// optimizer already has whole-program visibility: the default "standard" mode
// is therefore full LTO over the merged program. "Aggressive" adds LLVM's
// Attributor and a larger inliner budget for builds that can afford more
// compile time. "Minimal" keeps cross-function work to the inexpensive passes
// (IPSCCP, GlobalDCE) and disables inlining, which bounds compile time and
// code growth for very large programs.
enum class IpoMode {
  Standard,
  Aggressive,
  Minimal,
};

// Returns the spelling accepted on the command line ("standard", "aggressive",
// "minimal").
std::string_view ipo_mode_name(IpoMode mode);

// Parses an IPO mode spelling. Returns false for an unknown name.
bool parse_ipo_mode(std::string_view text, IpoMode& mode);

// Target and optimization settings for one compiled module.
struct OptimizationOptions {
  OptimizationLevel level = OptimizationLevel::O2;

  // How much interprocedural work the LLVM pipeline may perform.
  IpoMode ipo = IpoMode::Standard;

  // Which target profile the backend should honour. `Portable` ignores the
  // CPU/features fields, `Native` derives them from the host, and `Explicit`
  // uses them exactly as given.
  TargetProfile target_profile = TargetProfile::Portable;

  // Empty means the platform default triple (x86_64-pc-windows-msvc).
  std::string target_triple;

  // "generic" keeps the object portable. A named CPU selects that model, and
  // the `Native` profile replaces both fields with the host CPU and its
  // feature set.
  std::string target_cpu = "generic";
  std::string target_features;

  // Optimization-remark reporting, selected by `--remarks`.
  RemarkMode remarks = RemarkMode::None;
  // When non-empty, remarks are only emitted by passes whose name contains
  // this text (for example "loop-vectorize"), mirroring clang's `-Rpass=<regex>`
  // narrowing without pulling a regular-expression engine into the driver.
  std::string remark_filter;
};

// Runs the LLVM PassBuilder pipeline for `level` on `module`.
//
// The target machine supplies the target IR analyses used by inlining,
// vectorization, and cost modelling. The module must already have its data
// layout and target triple set from the same target machine. Returns false for
// an invalid level or when the input/output module fails LLVM verification.
//
// When `options.remarks` is not `RemarkMode::None`, LLVM's optimization
// remarks for this run are printed to stderr. Remarks are requested through the
// module's LLVMContext, so they live as long as that context; callers create
// one context per compilation.
bool optimize_llvm_module(llvm::Module& module,
                          llvm::TargetMachine& target_machine,
                          const OptimizationOptions& options,
                          DiagnosticEngine& diagnostics);

}  // namespace nirq
