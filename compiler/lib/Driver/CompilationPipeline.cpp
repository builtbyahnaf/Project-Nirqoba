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
#include "nirqoba/Driver/ProcessRunner.h"
#include "nirqoba/Frontend/AstPrinter.h"
#include "nirqoba/Frontend/Lexer.h"
#include "nirqoba/Frontend/Parser.h"
#include "nirqoba/Frontend/SourceFile.h"
#include "nirqoba/Lowering/PassPipeline.h"
#include "nirqoba/Lowering/RegisterDialects.h"
#include "nirqoba/Lowering/TirToMlir.h"
#include "nirqoba/Sema/TypeChecker.h"
#include "nirqoba/TIR/Builder.h"
#include "nirqoba/TIR/Printing.h"
#include "nirqoba/TIR/Verifier.h"

namespace nirq {
namespace {

static bool file_exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

static std::string get_executable_dir() {
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

static std::string locate_runtime_lib() {
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

static bool ends_with(std::string_view str, std::string_view suffix) {
  return str.size() >= suffix.size() &&
         str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

int run_pipeline(const PipelineOptions& options,
                 DiagnosticEngine& diagnostics) {
  // Step 1: Validate file extension (.nrqba)
  if (!ends_with(options.source_path, ".nrqba")) {
    diagnostics.error(diag::kInvalidSourceExtension, SourceLocation{},
                      "source file '" + options.source_path +
                          "' must have extension '.nrqba' (ADR 0002)");
    render_diagnostics(diagnostics, nullptr);
    return 1;
  }

  // Step 2: Read source file
  std::ifstream file(options.source_path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    diagnostics.error(diag::kMissingSourceFile, SourceLocation{},
                      "cannot open source file '" + options.source_path + "'");
    render_diagnostics(diagnostics, nullptr);
    return 3;  // Exit code 3: input I/O error
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  std::string source_text = buffer.str();
  file.close();

  SourceFile source(options.source_path, std::move(source_text));

  // Step 3: Lex
  Lexer lexer(source, diagnostics);
  std::vector<Token> tokens = lexer.tokenize();
  if (diagnostics.has_errors()) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  // Step 4: Parse
  Parser parser(std::move(tokens), diagnostics);
  std::unique_ptr<Program> program = parser.parse_program();
  if (diagnostics.has_errors() || !program) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  if (options.emit == EmitKind::Ast) {
    std::cout << print_program(*program);
    return 0;
  }

  // Step 5: Sema
  TypeChecker checker(diagnostics);
  if (!checker.check(*program) || diagnostics.has_errors()) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  // Step 6: TIR
  TirBuilder tir_builder(diagnostics);
  std::unique_ptr<tir::Module> tir_module = tir_builder.lower(*program);
  if (!tir_module || !verify_tir_module(*tir_module, diagnostics) || diagnostics.has_errors()) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  if (options.emit == EmitKind::Tir) {
    std::cout << print_tir(*tir_module);
    return 0;
  }

  // Step 7: MLIR Translation
  mlir::DialectRegistry registry;
  register_all_dialects(registry);
  mlir::MLIRContext mlir_context(registry);
  mlir_context.loadAllAvailableDialects();

  mlir::OwningOpRef<mlir::ModuleOp> mlir_module =
      translate_tir_to_mlir(*tir_module, mlir_context, diagnostics);
  if (!mlir_module || diagnostics.has_errors()) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  if (options.emit == EmitKind::Mlir) {
    llvm::outs() << "// === Unlowered MLIR (nrq dialect) ===\n";
    mlir_module->print(llvm::outs());
    llvm::outs() << "\n\n";

    mlir::PassManager pm(&mlir_context);
    build_lowering_pipeline(pm);
    if (mlir::failed(pm.run(*mlir_module))) {
      diagnostics.error(diag::kInternalError, SourceLocation{}, "MLIR pass pipeline failed");
      render_diagnostics(diagnostics, &source);
      return 1;
    }

    llvm::outs() << "// === Lowered MLIR (LLVM dialect) ===\n";
    mlir_module->print(llvm::outs());
    llvm::outs() << "\n";
    return 0;
  }

  // Step 8: Lower MLIR to LLVM dialect
  mlir::PassManager pm(&mlir_context);
  build_lowering_pipeline(pm);
  if (mlir::failed(pm.run(*mlir_module))) {
    diagnostics.error(diag::kInternalError, SourceLocation{}, "MLIR lowering failed");
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  // Step 9: MLIR to LLVM IR
  llvm::LLVMContext llvm_context;
  std::unique_ptr<llvm::Module> llvm_module =
      mlir::translateModuleToLLVMIR(*mlir_module, llvm_context);
  if (!llvm_module) {
    diagnostics.error(diag::kInternalError, SourceLocation{},
                      "failed to translate MLIR to LLVM IR");
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  if (options.emit == EmitKind::Llvm) {
    llvm_module->print(llvm::outs(), nullptr);
    return 0;
  }

  // Step 10: Emit Object file
  if (options.emit == EmitKind::Obj) {
    if (!emit_object_file(*llvm_module, options.output_path, diagnostics)) {
      render_diagnostics(diagnostics, &source);
      return 1;
    }
    render_diagnostics(diagnostics, &source);
    return diagnostics.has_errors() ? 1 : 0;
  }

  // Step 11: Emit temporary/intermediate object file for linking
  std::string obj_file = options.obj_path;
  if (obj_file.empty()) {
    obj_file = options.output_path + ".obj";
  }

  if (!emit_object_file(*llvm_module, obj_file, diagnostics)) {
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  // Step 12: Link to executable
  LinkerEnvironment linker_env = load_linker_environment(options.toolchain_json, diagnostics);
  if (!linker_env.is_valid) {
    render_diagnostics(diagnostics, &source);
    return 4;  // Exit code 4: environment/toolchain error
  }

  std::string runtime_lib = locate_runtime_lib();
  if (runtime_lib.empty()) {
    diagnostics.error(diag::kLinkFailed, SourceLocation{},
                      "cannot find nrqoba_runtime.lib; please build the runtime target first");
    render_diagnostics(diagnostics, &source);
    return 1;
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
    render_diagnostics(diagnostics, &source);
    return 1;
  }

  render_diagnostics(diagnostics, &source);
  return diagnostics.has_errors() ? 1 : 0;
}

}  // namespace nirq
