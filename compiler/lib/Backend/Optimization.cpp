#include "nirqoba/Backend/Optimization.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "llvm/IR/Attributes.h"
#include "llvm/IR/DiagnosticHandler.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/Attributor.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/DeadStoreElimination.h"

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Diagnostics/DiagnosticEngine.h"

namespace nirq {
using namespace std;

string_view optimization_level_name(OptimizationLevel level) {
  switch (level) {
    case OptimizationLevel::O0: return "-O0";
    case OptimizationLevel::O1: return "-O1";
    case OptimizationLevel::O2: return "-O2";
    case OptimizationLevel::O3: return "-O3";
    case OptimizationLevel::Os: return "-Os";
    case OptimizationLevel::Oz: return "-Oz";
  }
  return "-O2";
}

bool parse_optimization_level(string_view text, OptimizationLevel& level) {
  // Accept the command-line spelling with or without the "-O" prefix so the
  // same parser serves `-O2`, `-O 2`-style aliases, and `--opt-level=2`.
  if (text.size() >= 2 && text[0] == '-' &&
      (text[1] == 'O' || text[1] == 'o')) {
    text.remove_prefix(2);
  } else if (text.size() >= 1 && (text[0] == 'O' || text[0] == 'o')) {
    text.remove_prefix(1);
  }

  if (text == "0") { level = OptimizationLevel::O0; return true; }
  if (text == "1") { level = OptimizationLevel::O1; return true; }
  if (text == "2") { level = OptimizationLevel::O2; return true; }
  if (text == "3") { level = OptimizationLevel::O3; return true; }
  if (text == "s" || text == "S") { level = OptimizationLevel::Os; return true; }
  if (text == "z" || text == "Z") { level = OptimizationLevel::Oz; return true; }
  return false;
}

string_view ipo_mode_name(IpoMode mode) {
  switch (mode) {
    case IpoMode::Standard: return "standard";
    case IpoMode::Aggressive: return "aggressive";
    case IpoMode::Minimal: return "minimal";
  }
  return "standard";
}

bool parse_ipo_mode(string_view text, IpoMode& mode) {
  if (text == "standard") { mode = IpoMode::Standard; return true; }
  if (text == "aggressive") { mode = IpoMode::Aggressive; return true; }
  if (text == "minimal") { mode = IpoMode::Minimal; return true; }
  return false;
}

namespace {

llvm::OptimizationLevel llvm_level_for(OptimizationLevel level) {
  switch (level) {
    case OptimizationLevel::O0: return llvm::OptimizationLevel::O0;
    case OptimizationLevel::O1: return llvm::OptimizationLevel::O1;
    case OptimizationLevel::O2: return llvm::OptimizationLevel::O2;
    case OptimizationLevel::O3: return llvm::OptimizationLevel::O3;
    // LLVM's PassBuilder uses function attributes to choose size-oriented
    // decisions at the O2 pipeline, which is also how clang represents -Os/-Oz.
    case OptimizationLevel::Os:
    case OptimizationLevel::Oz:
      return llvm::OptimizationLevel::O2;
  }
  return llvm::OptimizationLevel::O2;
}

// Enables LLVM's optimization remarks through the context's diagnostic handler.
//
// Passes consult these predicates (through OptimizationRemarkEmitter) before
// formatting a remark, so overriding them is what makes `--remarks` work. The
// handler deliberately does not intercept `handleDiagnostics`: returning false
// from it lets LLVMContext print the remark with its normal "remark: <location>:"
// formatting to stderr, which keeps the compiler's own diagnostics untouched.
class RemarkDiagnosticHandler final : public llvm::DiagnosticHandler {
 public:
  RemarkDiagnosticHandler(RemarkMode mode, llvm::StringRef filter)
      : mode_(mode), filter_(filter) {}

  bool isAnalysisRemarkEnabled(llvm::StringRef pass) const override {
    return enabled(pass);
  }
  bool isMissedOptRemarkEnabled(llvm::StringRef pass) const override {
    return enabled(pass);
  }
  bool isPassedOptRemarkEnabled(llvm::StringRef pass) const override {
    return mode_ == RemarkMode::All && matches(pass);
  }
  bool isAnyRemarkEnabled() const override { return mode_ != RemarkMode::None; }

 private:
  bool matches(llvm::StringRef pass) const {
    return filter_.empty() || pass.contains(filter_);
  }
  bool enabled(llvm::StringRef pass) const {
    return mode_ != RemarkMode::None && matches(pass);
  }

  RemarkMode mode_;
  llvm::StringRef filter_;
};

void apply_size_attributes(llvm::Module& module, OptimizationLevel level) {
  if (level != OptimizationLevel::Os && level != OptimizationLevel::Oz) return;
  const llvm::Attribute::AttrKind kind = level == OptimizationLevel::Os
      ? llvm::Attribute::OptimizeForSize
      : llvm::Attribute::MinSize;
  for (llvm::Function& function : module) {
    if (function.isDeclaration()) continue;
    function.addFnAttr(kind);
  }
}

// Translates the user-facing IPO policy into the knobs LLVM's PassBuilder
// exposes. The default (standard) keeps LLVM's own level-dependent cost model;
// aggressive raises the inliner budget and enables speculative devirtualization;
// minimal suppresses inlining so a very large merged program does not pay for
// cross-function cloning. IPSCCP, GlobalOpt, and GlobalDCE still run in every
// mode, so constant propagation, internalization, and dead-function removal are
// never silently lost.
llvm::PipelineTuningOptions pipeline_tuning_for(IpoMode mode) {
  llvm::PipelineTuningOptions tuning;
  switch (mode) {
    case IpoMode::Standard:
      break;
    case IpoMode::Aggressive:
      tuning.InlinerThreshold = std::max(tuning.InlinerThreshold, 400);
      tuning.DevirtualizeSpeculatively = true;
      break;
    case IpoMode::Minimal:
      tuning.InlinerThreshold = 0;
      break;
  }
  return tuning;
}

// The front end resolves every imported module into one TIR/MLIR/LLVM module,
// so the backend always performs whole-program LTO. Recording that fact (and
// the policy that governed it) keeps an optimized artifact auditable: a build
// made with --ipo=minimal cannot be mistaken for an aggressive one.
void add_ipo_module_flags(llvm::Module& module, IpoMode mode) {
  llvm::LLVMContext& context = module.getContext();
  module.addModuleFlag(llvm::Module::Error, "nrqoba.ipo.mode",
                       llvm::MDString::get(context, ipo_mode_name(mode)));
  module.addModuleFlag(llvm::Module::Error, "nrqoba.ipo.strategy",
                       llvm::MDString::get(context, "whole-program"));
}

}  // namespace

bool optimize_llvm_module(llvm::Module& module,
                          llvm::TargetMachine& target_machine,
                          const OptimizationOptions& options,
                          DiagnosticEngine& diagnostics) {
  const OptimizationLevel level = options.level;
  if (level < OptimizationLevel::O0 || level > OptimizationLevel::Oz) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "internal: invalid optimization level");
    return false;
  }

  // Catch malformed IR before the pass pipeline so a frontend bug produces a
  // compiler diagnostic instead of an optimizer crash or a silent miscompile.
  if (llvm::verifyModule(module, &llvm::errs())) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "LLVM IR failed verification before optimization");
    return false;
  }

  apply_size_attributes(module, level);
  add_ipo_module_flags(module, options.ipo);

  // The context outlives this call (the driver creates one per compilation), so
  // installing the handler here needs no restore.
  if (options.remarks != RemarkMode::None) {
    module.getContext().setDiagnosticHandler(
        std::make_unique<RemarkDiagnosticHandler>(options.remarks,
                                                  options.remark_filter));
  }

  llvm::PassBuilder pass_builder(&target_machine,
                                 pipeline_tuning_for(options.ipo));
  llvm::LoopAnalysisManager loop_analyses;
  llvm::FunctionAnalysisManager function_analyses;
  llvm::CGSCCAnalysisManager cgscc_analyses;
  llvm::ModuleAnalysisManager module_analyses;

  pass_builder.registerModuleAnalyses(module_analyses);
  pass_builder.registerCGSCCAnalyses(cgscc_analyses);
  pass_builder.registerFunctionAnalyses(function_analyses);
  pass_builder.registerLoopAnalyses(loop_analyses);
  pass_builder.crossRegisterProxies(loop_analyses, function_analyses,
                                    cgscc_analyses, module_analyses);

  const llvm::OptimizationLevel llvm_level = llvm_level_for(level);
  llvm::ModulePassManager pipeline = level == OptimizationLevel::O0
      ? pass_builder.buildO0DefaultPipeline(llvm_level)
      : pass_builder.buildPerModuleDefaultPipeline(llvm_level);

  // Aggressive mode asks LLVM's Attributor for a deeper interprocedural
  // deduction over the final (already inlined) program. Attributor is the
  // canonical implementation of interprocedural capture, alias, and memory
  // reasoning; running it only on request keeps the default compile-time
  // profile unchanged.
  if (level != OptimizationLevel::O0 &&
      options.ipo == IpoMode::Aggressive) {
    pipeline.addPass(llvm::AttributorPass());
  }

  // Guarantee dead-code and dead-store elimination at every optimized level.
  //
  // LLVM's per-module pipeline already contains DCE and DSE, but module-level
  // transforms (inlining, global simplification, constant propagation) can
  // expose computations and overwritten stores after the per-function
  // simplification stage has run. This final function stage performs the two
  // Section 3 requirements explicitly, so their presence does not depend on the
  // upstream pipeline ordering. Both passes are idempotent when nothing is
  // dead. -O0 intentionally skips the stage so the direct, source-shaped IR
  // remains inspectable.
  if (level != OptimizationLevel::O0) {
    llvm::FunctionPassManager final_cleanup;
    final_cleanup.addPass(llvm::DSEPass());
    final_cleanup.addPass(llvm::ADCEPass());
    pipeline.addPass(
        llvm::createModuleToFunctionPassAdaptor(std::move(final_cleanup)));

    // GlobalDCE removes functions and globals that became unreachable after the
    // final inlining and cleanup round. Internalization already marked every
    // generated helper except `main` as module-private, which is what makes
    // this removal legal; the pass is not a no-op on programs with unused
    // cross-module helpers.
    pipeline.addPass(llvm::GlobalDCEPass());
  }

  // The new pass manager reports analysis preservation, not failure. Verify the
  // transformed module before it reaches code generation so a bad pass or an
  // invalid source construct cannot silently become a broken object file.
  pipeline.run(module, module_analyses);

  if (llvm::verifyModule(module, &llvm::errs())) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "optimized LLVM IR failed verification");
    return false;
  }
  return true;
}

}  // namespace nirq
