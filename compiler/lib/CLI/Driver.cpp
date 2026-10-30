#include "nirqoba/CLI/Driver.h"

#include <iostream>
#include <string_view>
#include <filesystem>

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Diagnostics/DiagnosticRenderer.h"
#include "nirqoba/Driver/ProcessRunner.h"
#include "nirqoba/Version.h"

namespace nirq {
namespace {

namespace fs = std::filesystem;

static void print_version() {
  std::cout << "nirqoba compiler version " << NIRQOBA_VERSION_STRING
            << " (Phase " << NIRQOBA_PHASE << ")\n"
            << "LLVM/MLIR: " << NIRQOBA_LLVM_VERSION << "\n"
            << "Target: " << NIRQOBA_DEFAULT_TARGET_TRIPLE << "\n"
            << "Build: " << NIRQOBA_BUILD_TYPE << "\n";
}

static void print_general_help() {
  std::cout << "Usage: nirqoba <command> [options]\n\n"
            << "Commands:\n"
            << "  build <file.nrqba>   Compile Nirqoba source code\n"
            << "  run   <file.nrqba>   Compile and run Nirqoba program\n"
            << "  check <file.nrqba>   Check source code for syntax and semantic errors\n"
            << "  version              Print compiler version and target information\n"
            << "  help [command]       Show help for a command\n\n"
            << "Global options:\n"
            << "  -h, --help           Show help message\n"
            << "  --version            Show version information\n";
}

static void print_command_help(std::string_view cmd) {
  if (cmd == "build") {
    std::cout << "Usage: nirqoba build <file.nrqba> [options]\n\n"
              << "Options:\n"
              << "  -o <path>            Output executable or object file path\n"
              << "  --out-dir <dir>      Directory to place output artifacts\n"
              << "  --emit=<kind>        Emit intermediate stage and stop\n"
              << "                       Kinds: ast, tir, mlir, llvm, obj\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "run") {
    std::cout << "Usage: nirqoba run <file.nrqba> [options] [-- <args>...]\n\n"
              << "Options:\n"
              << "  --out-dir <dir>      Directory for temporary build artifacts\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "check") {
    std::cout << "Usage: nirqoba check <file.nrqba> [options]\n\n"
              << "Options:\n"
              << "  --emit=<kind>        Emit checked stage (ast or tir)\n";
  } else {
    print_general_help();
  }
}

static bool parse_emit_kind(std::string_view str, EmitKind& kind) {
  if (str == "ast")  { kind = EmitKind::Ast; return true; }
  if (str == "tir")  { kind = EmitKind::Tir; return true; }
  if (str == "mlir") { kind = EmitKind::Mlir; return true; }
  if (str == "llvm") { kind = EmitKind::Llvm; return true; }
  if (str == "obj")  { kind = EmitKind::Obj; return true; }
  return false;
}

}  // namespace

bool parse_command_line(int argc, const char* const* argv,
                        CommandLineOptions& options,
                        std::string& error_message) {
  if (argc <= 1) {
    options.command = CommandKind::Help;
    return true;
  }

  int idx = 1;
  std::string first = argv[idx++];

  if (first == "--version" || first == "-v") {
    options.command = CommandKind::Version;
    return true;
  }
  if (first == "--help" || first == "-h") {
    options.command = CommandKind::Help;
    if (idx < argc) options.help_topic = argv[idx++];
    return true;
  }

  if (first == "version") {
    options.command = CommandKind::Version;
    return true;
  }
  if (first == "help") {
    options.command = CommandKind::Help;
    if (idx < argc) options.help_topic = argv[idx++];
    return true;
  }

  if (first == "build") {
    options.command = CommandKind::Build;
  } else if (first == "run") {
    options.command = CommandKind::Run;
  } else if (first == "check") {
    options.command = CommandKind::Check;
  } else {
    error_message = "unknown command '" + first + "'";
    return false;
  }

  // Parse options and source path for build/run/check
  bool seen_dash_dash = false;
  while (idx < argc) {
    std::string arg = argv[idx++];
    if (seen_dash_dash) {
      options.trailing_args.push_back(std::move(arg));
      continue;
    }
    if (arg == "--") {
      seen_dash_dash = true;
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      options.command = CommandKind::Help;
      options.help_topic = first;
      return true;
    }
    if (arg == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (arg == "-o") {
      if (idx >= argc) {
        error_message = "missing path after '-o'";
        return false;
      }
      options.output_path = argv[idx++];
      continue;
    }
    if (arg.rfind("-o", 0) == 0 && arg.size() > 2) {
      options.output_path = arg.substr(2);
      continue;
    }
    if (arg == "--out-dir") {
      if (idx >= argc) {
        error_message = "missing directory after '--out-dir'";
        return false;
      }
      options.out_dir = argv[idx++];
      continue;
    }
    if (arg.rfind("--out-dir=", 0) == 0) {
      options.out_dir = arg.substr(10);
      continue;
    }
    if (arg.rfind("--emit=", 0) == 0) {
      std::string_view kind_str = std::string_view(arg).substr(7);
      if (!parse_emit_kind(kind_str, options.emit)) {
        error_message = "invalid emit kind '" + std::string(kind_str) +
                        "'; expected ast, tir, mlir, llvm, or obj";
        return false;
      }
      options.emit_specified = true;
      continue;
    }
    if (arg.rfind("-", 0) == 0) {
      error_message = "unknown option '" + arg + "'";
      return false;
    }

    if (options.source_path.empty()) {
      options.source_path = std::move(arg);
    } else {
      error_message = "unexpected argument '" + arg + "'; only one source file is accepted";
      return false;
    }
  }

  if (options.source_path.empty()) {
    error_message = "no source file specified for '" + first + "'";
    return false;
  }

  if (options.command == CommandKind::Check) {
    if (options.emit_specified &&
        options.emit != EmitKind::Ast &&
        options.emit != EmitKind::Tir) {
      error_message = "'check' only supports --emit=ast or --emit=tir";
      return false;
    }
  }

  return true;
}

int run_driver(int argc, const char* const* argv) {
  CommandLineOptions options;
  std::string error_message;
  if (!parse_command_line(argc, argv, options, error_message)) {
    DiagnosticEngine diag;
    diag.error(diag::kInvalidCommandLine, SourceLocation{}, error_message);
    render_diagnostics(diag, nullptr);
    return 2;  // Exit code 2: invalid usage
  }

  if (options.command == CommandKind::Version) {
    print_version();
    return 0;
  }

  if (options.command == CommandKind::Help) {
    if (options.help_topic.empty()) {
      print_general_help();
    } else {
      print_command_help(options.help_topic);
    }
    return 0;
  }

  // Determine artifact paths.
  fs::path src_path(options.source_path);
  std::string stem = src_path.stem().string();
  fs::path out_directory = fs::path(options.out_dir);

  std::string output_file = options.output_path;
  if (output_file.empty()) {
    if (options.emit == EmitKind::Obj) {
      output_file = (out_directory / (stem + ".obj")).string();
    } else {
      output_file = (out_directory / (stem + ".exe")).string();
    }
  }

  std::string obj_file = (out_directory / (stem + ".obj")).string();

  PipelineOptions pipe_opts;
  pipe_opts.source_path = options.source_path;
  pipe_opts.output_path = output_file;
  pipe_opts.obj_path = obj_file;
  pipe_opts.verbose = options.verbose;

  if (options.command == CommandKind::Check) {
    pipe_opts.emit = options.emit_specified ? options.emit : EmitKind::Tir;
    DiagnosticEngine diag;
    // For check without emit, run_pipeline up to TIR then stop
    int ret = run_pipeline(pipe_opts, diag);
    return ret;
  }

  if (options.command == CommandKind::Build) {
    pipe_opts.emit = options.emit;
    DiagnosticEngine diag;
    int ret = run_pipeline(pipe_opts, diag);
    return ret;
  }

  if (options.command == CommandKind::Run) {
    pipe_opts.emit = EmitKind::Exe;
    DiagnosticEngine diag;
    int ret = run_pipeline(pipe_opts, diag);
    if (ret != 0) return ret;

    // Run the generated executable.
    ProcessResult run_res = run_process(output_file, options.trailing_args);
    if (!run_res.launched) {
      diag.error(diag::kProgramFailedToStart, SourceLocation{},
                 "failed to execute '" + output_file + "': " + run_res.output);
      render_diagnostics(diag, nullptr);
      return 1;
    }

    if (!run_res.output.empty()) {
      std::cout << run_res.output;
    }
    return run_res.exit_code;
  }

  return 0;
}

}  // namespace nirq
