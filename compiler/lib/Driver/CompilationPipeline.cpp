#include "nirqoba/Driver/CompilationPipeline.h"

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

#include "nirqoba/Backend/ObjectEmitter.h"
#include "nirqoba/Diagnostics/Diagnostic.h"
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
namespace {

namespace fs = std::filesystem;

bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

std::string get_executable_dir() {
#ifdef _WIN32
  wchar_t buf[4096] = {};
  if (GetModuleFileNameW(nullptr, buf, 4096) != 0) {
    std::string path;
    for (int i = 0; buf[i]; ++i) path.push_back(static_cast<char>(buf[i]));
    auto sep = path.find_last_of("/\\");
    if (sep != std::string::npos) return path.substr(0, sep);
  }
#endif
  return ".";
}

std::string locate_runtime_lib() {
  const std::string exe_dir = get_executable_dir();
  const std::vector<std::string> candidates = {
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

bool ends_with(std::string_view str, std::string_view suffix) {
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
              const std::vector<const SourceFile*>& sources) {
  render_diagnostics(diagnostics, sources);
  return 1;
}

}  // namespace

int run_pipeline(const PipelineOptions& options,
                 DiagnosticEngine& diagnostics) {
  const std::vector<const SourceFile*> no_sources;

  if (options.source_paths.empty()) {
    diagnostics.error(diag::kMissingSourceFile, SourceLocation{},
                      "no source file was given to the compiler");
    render_diagnostics(diagnostics, no_sources);
    return 2;
  }

  // Step 1: validate the extension and readability of every root file before
  // resolution starts, so a typo is reported as an input error (exit 3) rather
  // than as a mystery about a module that "cannot be found".
  for (const std::string& path : options.source_paths) {
    if (!ends_with(path, ".nrqba")) {
      diagnostics.error(diag::kInvalidSourceExtension, SourceLocation{},
                        "source file '" + path +
                            "' must have extension '.nrqba' (ADR 0002)");
      render_diagnostics(diagnostics, no_sources);
      return 1;
    }
    std::ifstream probe(path);
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
  std::vector<fs::path> root_paths;
  root_paths.reserve(options.source_paths.size());
  for (const std::string& path : options.source_paths) {
    root_paths.emplace_back(path);
  }

  std::vector<fs::path> search_paths;
  search_paths.reserve(options.module_search_paths.size());
  for (const std::string& path : options.module_search_paths) {
    search_paths.emplace_back(path);
  }

  ModuleResolver resolver(diagnostics, std::move(search_paths));
  std::vector<std::unique_ptr<ModuleUnit>> units = resolver.resolve(root_paths);

  std::vector<const SourceFile*> sources;
  sources.reserve(units.size());
  for (const auto& unit : units) {
    if (unit && unit->source_file) sources.push_back(unit->source_file.get());
  }

  if (units.empty() || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Ast) {
    for (const auto& unit : units) {
      std::cout << "// module " << unit->name << " (" << unit->file_path.string() << ")\n";
      std::cout << print_program(*unit->ast);
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
  auto tir_module = std::make_unique<tir::Module>();
  const ModuleUnit* entry_unit = nullptr;
  for (const auto& unit : units) {
    if (defines_main(*unit)) {
      entry_unit = unit.get();
      break;
    }
  }
  for (const auto& unit : units) {
    tir_builder.lower_module(unit.get() == entry_unit ? std::string() : unit->name,
                             *unit->ast, *tir_module);
  }
  if (!verify_tir_module(*tir_module, diagnostics) || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Tir) {
    std::cout << print_tir(*tir_module);
    return 0;
  }

  // Step 5: MLIR translation.
  mlir::DialectRegistry registry;
  register_all_dialects(registry);
  mlir::MLIRContext mlir_context(registry);
  mlir_context.loadAllAvailableDialects();

  mlir::OwningOpRef<mlir::ModuleOp> mlir_module =
      translate_tir_to_mlir(*tir_module, mlir_context, diagnostics);
  if (!mlir_module || diagnostics.has_errors()) {
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Mlir) {
    llvm::outs() << "// === Unlowered MLIR (nrq dialect) ===\n";
    mlir_module->print(llvm::outs());
    llvm::outs() << "\n\n";

    mlir::PassManager pm(&mlir_context);
    build_lowering_pipeline(pm);
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
  build_lowering_pipeline(pm);
  if (mlir::failed(pm.run(*mlir_module))) {
    diagnostics.error(diag::kInternalError, SourceLocation{}, "MLIR lowering failed");
    return fail_with(diagnostics, sources);
  }

  // Step 7: MLIR to LLVM IR.
  llvm::LLVMContext llvm_context;
  std::unique_ptr<llvm::Module> llvm_module =
      mlir::translateModuleToLLVMIR(*mlir_module, llvm_context);
  if (!llvm_module) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to translate MLIR to LLVM IR");
    return fail_with(diagnostics, sources);
  }

  if (options.emit == EmitKind::Llvm) {
    llvm_module->print(llvm::outs(), nullptr);
    return 0;
  }

  // Step 8: emit the object file.
  if (options.emit == EmitKind::Obj) {
    if (!emit_object_file(*llvm_module, options.output_path, diagnostics)) {
      return fail_with(diagnostics, sources);
    }
    render_diagnostics(diagnostics, sources);
    return diagnostics.has_errors() ? 1 : 0;
  }

  std::string obj_file = options.obj_path;
  if (obj_file.empty()) {
    obj_file = options.output_path + ".obj";
  }

  if (!emit_object_file(*llvm_module, obj_file, diagnostics)) {
    return fail_with(diagnostics, sources);
  }

  // Step 9: link against the Nirqoba runtime.
  LinkerEnvironment linker_env = load_linker_environment(options.toolchain_json, diagnostics);
  if (!linker_env.is_valid) {
    render_diagnostics(diagnostics, sources);
    return 4;  // Exit code 4: environment/toolchain error
  }

  std::string runtime_lib = locate_runtime_lib();
  if (runtime_lib.empty()) {
    diagnostics.error(diag::kLinkFailed, SourceLocation{},
                      "cannot find nrqoba_runtime.lib; please build the runtime target first");
    return fail_with(diagnostics, sources);
  }

  std::vector<std::string> link_args = {
    "/NOLOGO",
    "/SUBSYSTEM:CONSOLE",
    "/OUT:" + options.output_path,
    obj_file,
    runtime_lib,
    "msvcrt.lib",
    "ucrt.lib",
    "vcruntime.lib",
    "kernel32.lib",
  };

  for (const auto& lib_dir : linker_env.lib_dirs) {
    link_args.push_back("/LIBPATH:" + lib_dir);
  }

  ProcessResult link_res = run_process(linker_env.linker_path, link_args);
  if (!link_res.launched || link_res.exit_code != 0) {
    diagnostics.error(diag::kLinkFailed, SourceLocation{},
                      "linking failed with exit code " +
                          std::to_string(link_res.exit_code) + ":\n" + link_res.output);
    return fail_with(diagnostics, sources);
  }

  render_diagnostics(diagnostics, sources);
  return diagnostics.has_errors() ? 1 : 0;
}

}  // namespace nirq
