#include "nirqoba/Driver/CompilationPipeline.h"
#include "nirqoba/Lowering/TirToMlir.h"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include "nirqoba/Backend/ObjectEmitter.h"
#include "nirqoba/Backend/TargetInfo.h"
#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Lowering/DataLayout.h"
#include "nirqoba/Diagnostics/DiagnosticRenderer.h"
#include "nirqoba/Driver/LinkerEnvironment.h"
#include "nirqoba/Driver/ModuleResolver.h"
#include "nirqoba/Driver/ProcessRunner.h"
#include "nirqoba/Frontend/AstPrinter.h"
#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"
#include "nirqoba/Lowering/TirToMlir.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/Printing.h"
#include "nirqoba/TIR/Verifier.h"

namespace nirq {
using namespace std;
namespace {

namespace fs = filesystem;

bool file_exists(const string& path) {
  ifstream f(path);
  return f.good();
}

string get_executable_dir() {
#ifdef _WIN32
  wchar_t buf[4096] = {};
  if (GetModuleFileNameW(nullptr, buf, 4096) != 0) {
    string path;
    for (int i = 0; buf[i]; ++i) path.push_back(static_cast<char>(buf[i]));
    auto sep = path.find_last_of("/\\");
    if (sep != string::npos) return path.substr(0, sep);
  }
#endif
  return ".";
}

string locate_runtime_lib() {
  const string exe_dir = get_executable_dir();
  const vector<string> candidates = {
    exe_dir + "\\nrqoba_runtime.lib",
    exe_dir + "\\..\\lib\\nrqoba_runtime.lib",
    exe_dir + "\\..\\runtime\\nrqoba_runtime.lib",
    exe_dir + "\\..\\..\\build\\msvc-dev\\lib\\nrqoba_runtime.lib",
    exe_dir + "\\..\\..\\build\\msvc-release\\lib\\nrqoba_runtime.lib",
    "lib\\nrqoba_runtime.lib",
    "build\\msvc-dev\\lib\\nrqoba_runtime.lib",
    "build\\msvc-release\\lib\\nrqoba_runtime.lib",
    "nrqoba_runtime.lib",
  };

  for (const auto& path : candidates) {
    if (file_exists(path)) return path;
  }
  return {};
}

// Finds the LLVM profile runtime. The main LLVM SDK ships compiler headers and
// libraries but not compiler-rt, so PGO builds look in the packaged clang
// toolchain as well. The search is limited to the expected
// <toolchain>/<name>/lib/clang/<version>/lib/windows/ layout; iterating the
// whole toolchain would make every PGO link scan thousands of headers.
string locate_profile_runtime_lib() {
  vector<fs::path> roots;
  const fs::path exe_dir = get_executable_dir();
  roots.push_back(exe_dir / ".." / ".." / "toolchain");
  roots.push_back(fs::path("toolchain"));

  for (const fs::path& root : roots) {
    std::error_code error;
    if (!fs::exists(root, error)) continue;
    for (const fs::directory_entry& toolchain :
         fs::directory_iterator(root, error)) {
      if (!toolchain.is_directory()) continue;
      const fs::path clang_root = toolchain.path() / "lib" / "clang";
      if (!fs::exists(clang_root, error)) continue;
      for (const fs::directory_entry& version :
           fs::directory_iterator(clang_root, error)) {
        const fs::path library = version.path() / "lib" / "windows" /
                                 "clang_rt.profile-x86_64.lib";
        if (fs::exists(library, error)) return library.string();
      }
    }
  }
  return {};
}

bool ends_with(string_view str, string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The entry module is the one that defines `main`. Its functions keep bare
// symbol names; every other module qualifies them as `module.name`.
bool defines_main(const ModuleUnit& unit) {
  if (!unit.ast) return false;
  for (const auto& function : unit.ast->functions()) {
    if (function->name() == "main") return true;
  }
  return false;
}

// Renders every diagnostic with the source file it belongs to and returns the
// exit code the driver should report for a compilation failure.
int fail_with(const DiagnosticEngine& diagnostics,
              const vector<const SourceFile*>& sources) {
  render_diagnostics(diagnostics, sources);
  return 1;
}

}  // namespace

int run_pipeline(const PipelineOptions& options,
                 DiagnosticEngine& diagnostics) {
  const vector<const SourceFile*> no_sources;

  if (options.source_paths.empty()) {
    diagnostics.error(diag::kMissingSourceFile, SourceLocation{},
                      "no source file was given to the compiler");
    render_diagnostics(diagnostics, no_sources);
    return 2;
  }

  // Step 1: validate the extension and readability of every root file before
  // resolution starts, so a typo is reported as an input error (exit 3) rather
  // than as a mystery about a module that "cannot be found".
  for (const string& path : options.source_paths) {
    if (!ends_with(path, ".nrqba")) {
      diagnostics.error(diag::kInvalidSourceExtension, SourceLocation{},
                        "source file '" + path +
                            "' must have extension '.nrqba' (ADR 0002)");
      render_diagnostics(diagnostics, no_sources);
      return 1;
    }
    ifstream probe(path);
    if (!probe.good()) {
      diagnostics.error(diag::kMissingSourceFile, SourceLocation{},
                        "cannot open source file '" + path + "'");
      render_diagnostics(diagnostics, no_sources);
      return 3;  // Exit code 3: input I/O error
    }
  }

  // Step 2: build the module graph. The resolver parses every reachable file,
  // reports cycles and missing modules, and returns the modules in dependency
  // order so a consumer is always checked after the module it imports.
  vector<fs::path> root_paths;
  root_paths.reserve(options.source_paths.size());
  for (const string& path : options.source_paths) {
    root_paths.emplace_back(path);
  }

  vector<fs::path> search_paths;
  search_paths.reserve(options.module_search_paths.size());
  for (const string& path : options.module_search_paths) {
    search_paths.emplace_back(path);
  }

  ModuleResolver resolver(diagnostics, move(search_paths));
  vector<unique_ptr<ModuleUnit>> units = resolver.resolve(root_paths);

  vector<const SourceFile*> sources;
  sources.reserve(units.size());
  for (const auto& unit : units) {
    if (unit && unit->source_file) sources.push_back(unit->source_file.get());
  }

  if (units.empty() || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Ast) {
    for (const auto& unit : units) {
      cout << "// module " << unit->name << " (" << unit->file_path.string() << ")\n";
      cout << print_program(*unit->ast);
    }
    return 0;
  }

  // Step 3: semantic analysis across every module.
  TypeChecker checker(diagnostics);
  if (!checker.check_modules(units) || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  // Step 4: lower each module into one TIR module. Non-root modules qualify
  // their symbols so two modules may define the same private helper name.
  TirBuilder tir_builder(diagnostics);
  auto tir_module = make_unique<tir::Module>();
  const ModuleUnit* entry_unit = nullptr;
  for (const auto& unit : units) {
    if (defines_main(*unit)) {
      entry_unit = unit.get();
      break;
    }
  }
  for (const auto& unit : units) {
    tir_builder.lower_module(unit.get() == entry_unit ? string() : unit->name,
                             *unit->ast, *tir_module);
  }
  if (!verify_tir_module(*tir_module, diagnostics) || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Tir) {
    cout << print_tir(*tir_module);
    return 0;
  }

  // The layout report needs the fully typed TIR module but nothing after it.
  if (options.emit == EmitKind::Layout) {
    cout << print_data_layout(*tir_module);
    return 0;
  }

  // Step 5: MLIR translation.
  mlir::DialectRegistry registry;
  register_all_dialects(registry);
  mlir::MLIRContext mlir_context(registry);
  mlir_context.loadAllAvailableDialects();

  mlir::OwningOpRef<mlir::ModuleOp> mlir_module =
      translate_tir_to_mlir(*tir_module, mlir_context, diagnostics,
                            options.fast_math);
  if (!mlir_module || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.verbose) {
    const GcRootAnalysisStats stats = gc_root_analysis_stats();
    cerr << "gc roots: " << stats.functions << " function(s), "
         << stats.slot_addresses << " slot address(es), "
         << stats.loads << " load(s), "
         << stats.liveness_probes << " liveness probe(s), "
         << stats.address_scans << " address scan(s)" << "\n";
  }

  if (options.emit == EmitKind::Mlir) {
    llvm::outs() << "// === Unlowered MLIR (nrq dialect) ===\n";
    mlir_module->print(llvm::outs());
    llvm::outs() << "\n\n";

    mlir::PassManager pm(&mlir_context);
    build_lowering_pipeline(pm, options.optimization_level != OptimizationLevel::O0);
    if (mlir::failed(pm.run(*mlir_module))) {
      diagnostics.error(diag::kInternalError, SourceLocation{}, "MLIR pass pipeline failed");
      return fail_with(diagnostics, sources);
    }

    llvm::outs() << "// === Lowered MLIR (LLVM dialect) ===\n";
    mlir_module->print(llvm::outs());
    llvm::outs() << "\n";
    return 0;
  }

  // Step 6: lower MLIR to the LLVM dialect.
  mlir::PassManager pm(&mlir_context);
  build_lowering_pipeline(pm, options.optimization_level != OptimizationLevel::O0);
  if (mlir::failed(pm.run(*mlir_module))) {
    diagnostics.error(diag::kInternalError, SourceLocation{}, "MLIR lowering failed");
    return fail_with(diagnostics, sources);
  }

  // Step 7: MLIR to LLVM IR.
  llvm::LLVMContext llvm_context;
  unique_ptr<llvm::Module> llvm_module =
      mlir::translateModuleToLLVMIR(*mlir_module, llvm_context);
  if (!llvm_module) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to translate MLIR to LLVM IR");
    return fail_with(diagnostics, sources);
  }

  // Configure the target before optimizing: target IR analyses drive inline
  // costs, vector-width selection, and ABI lowering.
  OptimizationOptions backend_options;
  backend_options.level = options.optimization_level;
  backend_options.ipo = options.ipo_mode;
  backend_options.pgo = options.pgo_mode;
  backend_options.pgo_profile_path = options.pgo_profile_path;
  backend_options.pgo_cold = options.pgo_cold_mode;
  backend_options.target_profile = options.target_profile;
  backend_options.target_triple = options.target_triple;
  backend_options.target_cpu = options.target_cpu;
  backend_options.target_features = options.target_features;
  backend_options.remarks = !options.emit_remarks
      ? RemarkMode::None
      : (options.remarks_missed_only ? RemarkMode::Missed
                                     : RemarkMode::All);
  backend_options.remark_filter = options.remark_filter;

  TargetConfiguration target_config;
  if (!resolve_target_configuration(backend_options, target_config,
                                    diagnostics)) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::LlvmRaw) {
    llvm_module->print(llvm::outs(), nullptr);
    return 0;
  }

  unique_ptr<llvm::TargetMachine> target_machine =
      create_target_machine(*llvm_module, backend_options, target_config,
                            diagnostics);
  if (!target_machine) return fail_with(diagnostics, sources);

  if (options.verbose) {
    cerr << "optimization: " << optimization_level_name(options.optimization_level)
         << "\n"
         << "ipo: " << ipo_mode_name(options.ipo_mode)
         << " (whole-program lto)\n";
    if (options.pgo_mode != PgoMode::None) {
      cerr << "pgo: " << pgo_mode_name(options.pgo_mode);
      if (!options.pgo_profile_path.empty()) {
        cerr << " (profile: " << options.pgo_profile_path << ")";
      }
      cerr << " (cold: " << pgo_cold_mode_name(options.pgo_cold_mode) << ")\n";
    }
    cerr << "target: " << describe_target_configuration(target_config) << "\n";
  }

  if (!optimize_llvm_module(*llvm_module, *target_machine, backend_options,
                            diagnostics)) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Llvm) {
    llvm_module->print(llvm::outs(), nullptr);
    return 0;
  }

  // Step 8: emit the object file.
  if (options.emit == EmitKind::Obj) {
    if (!emit_object_file(*llvm_module, options.output_path, *target_machine,
                          diagnostics)) {
      return fail_with(diagnostics, sources);
    }
    render_diagnostics(diagnostics, sources);
    return diagnostics.has_errors() ? 1 : 0;
  }

  string obj_file = options.obj_path;
  if (obj_file.empty()) {
    obj_file = options.output_path + ".obj";
  }

  if (!emit_object_file(*llvm_module, obj_file, *target_machine, diagnostics)) {
    return fail_with(diagnostics, sources);
  }

  // Step 9: link against the Nirqoba runtime.
  LinkerEnvironment linker_env = load_linker_environment(options.toolchain_json, diagnostics);
  if (!linker_env.is_valid) {
    render_diagnostics(diagnostics, sources);
    return 4;  // Exit code 4: environment/toolchain error
  }

  string runtime_lib = locate_runtime_lib();
  if (runtime_lib.empty()) {
    diagnostics.error(diag::kLinkFailed, SourceLocation{},
                      "cannot find nrqoba_runtime.lib; please build the runtime target first");
    return fail_with(diagnostics, sources);
  }

  vector<string> link_args = {
    "/NOLOGO",
    "/SUBSYSTEM:CONSOLE",
    "/OUT:" + options.output_path,
    obj_file,
    runtime_lib,
    "msvcrt.lib",
    "ucrt.lib",
    "vcruntime.lib",
    "kernel32.lib",
    "dxgi.lib",
  };

  // Instrumented objects reference __llvm_profile_* and the profile runtime
  // owns the raw-counter buffer and its atexit writer. The compiler-rt archive
  // must be linked explicitly; unlike clang, Nirqoba invokes the linker
  // directly and has no driver to add it.
  if (options.pgo_mode == PgoMode::Generate) {
    const string profile_runtime = locate_profile_runtime_lib();
    if (profile_runtime.empty()) {
      diagnostics.error(
          diag::kToolchainUnavailable, SourceLocation{},
          "cannot find clang_rt.profile-x86_64.lib for --pgo-generate; "
          "re-run scripts/bootstrap.ps1 so the packaged LLVM toolchain "
          "contains compiler-rt (NRQ5003)");
      return fail_with(diagnostics, sources);
    }
    link_args.push_back(profile_runtime);
  }

  for (const auto& lib_dir : linker_env.lib_dirs) {
    link_args.push_back("/LIBPATH:" + lib_dir);
  }

  ProcessResult link_res = run_process(linker_env.linker_path, link_args);
  if (!link_res.launched || link_res.exit_code != 0) {
    diagnostics.error(diag::kLinkFailed, SourceLocation{},
                      "linking failed with exit code " +
                          to_string(link_res.exit_code) + ":\n" + link_res.output);
    return fail_with(diagnostics, sources);
  }

  render_diagnostics(diagnostics, sources);
  return diagnostics.has_errors() ? 1 : 0;
}

}  // namespace nirq
