#include "nirqoba/CLI/Driver.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

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
  std::cout << "Usage: nirqoba <command> <file.nrqba>... [options]\n\n"
            << "Commands:\n"
            << "  build <files...>     Compile Nirqoba source code\n"
            << "  run   <files...>     Compile and run Nirqoba program\n"
            << "  check <files...>     Check source code for syntax and semantic errors\n"
            << "  new   <dir>          Create a new multi-module project from the template\n"
            << "  version              Print compiler version and target information\n"
            << "  help [command]       Show help for a command\n\n"
            << "Global options:\n"
            << "  -h, --help           Show help message\n"
            << "  --version            Show version information\n";
}

static void print_command_help(std::string_view cmd) {
  if (cmd == "build") {
    std::cout << "Usage: nirqoba build <file.nrqba>... [options]\n\n"
              << "Options:\n"
              << "  -o <path>            Output executable or object file path\n"
              << "  --out-dir <dir>      Directory to place output artifacts\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  --emit=<kind>        Emit intermediate stage and stop\n"
              << "                       Kinds: ast, tir, mlir, llvm, obj\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "run") {
    std::cout << "Usage: nirqoba run <file.nrqba>... [options] [-- <args>...]\n\n"
              << "Options:\n"
              << "  --out-dir <dir>      Directory for temporary build artifacts\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "check") {
    std::cout << "Usage: nirqoba check <file.nrqba>... [options]\n\n"
              << "Options:\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  --emit=<kind>        Emit checked stage (ast or tir)\n";
  } else if (cmd == "new") {
    std::cout << "Usage: nirqoba new <dir>\n\n"
              << "Creates <dir> with src/main.nrqba, lib/greeting.nrqba and a README.\n"
              << "The directory must be new or empty.\n";
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

// --- `nirqoba new` project template -----------------------------------------

static constexpr const char* kProjectMainTemplate = R"(// Entry point of the project.
//
// Build: nirqoba build src/main.nrqba --module-path lib -o app.exe
// Run:   nirqoba run src/main.nrqba --module-path lib -- <arguments>

import greeting;

int function main(string[] args) {
    greeting.greet(args.length);
    return 0;
}
)";

static constexpr const char* kProjectGreetingTemplate = R"(module greeting;

// Called from main with the number of command-line arguments.
public void function greet(int argument_count) {
    if (argument_count == 0) {
        print("Hello from Nirqoba!");
        return;
    }
    print("Hello from Nirqoba, with arguments!");
}
)";

static bool write_scaffold_file(const fs::path& path, const std::string& contents,
                                std::string& error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot create '" + path.string() + "'";
    return false;
  }
  out << contents;
  if (!out) {
    error = "cannot write '" + path.string() + "'";
    return false;
  }
  return true;
}

static int create_project(const std::string& directory) {
  const fs::path root(directory);
  std::error_code ec;

  if (fs::exists(root, ec) && !fs::is_empty(root, ec)) {
    DiagnosticEngine diag;
    diag.error(diag::kProjectScaffoldFailed, SourceLocation{},
               "directory '" + directory + "' already exists and is not empty");
    render_diagnostics(diag, nullptr);
    return 1;
  }

  fs::create_directories(root / "src", ec);
  fs::create_directories(root / "lib", ec);
  if (ec) {
    DiagnosticEngine diag;
    diag.error(diag::kProjectScaffoldFailed, SourceLocation{},
               "cannot create project directories in '" + directory + "': " +
                   ec.message());
    render_diagnostics(diag, nullptr);
    return 1;
  }

  const std::string name = root.filename().empty() ? std::string("project")
                                                   : root.filename().string();
  std::string error;
  if (!write_scaffold_file(root / "src" / "main.nrqba", kProjectMainTemplate, error) ||
      !write_scaffold_file(root / "lib" / "greeting.nrqba",
                           kProjectGreetingTemplate, error)) {
    DiagnosticEngine diag;
    diag.error(diag::kProjectScaffoldFailed, SourceLocation{}, error);
    render_diagnostics(diag, nullptr);
    return 1;
  }

  std::string readme = "# " + name + "\n\n"
      "A Nirqoba project.\n\n"
      "## Layout\n\n"
      "- `src/main.nrqba` - entry point; `main` receives the command-line arguments.\n"
      "- `lib/greeting.nrqba` - a module imported by the entry point.\n\n"
      "## Build and run\n\n"
      "```text\n"
      "nirqoba run src/main.nrqba --module-path lib\n"
      "nirqoba run src/main.nrqba --module-path lib -- one two\n"
      "nirqoba build src/main.nrqba --module-path lib -o app.exe\n"
      "```\n";
  if (!write_scaffold_file(root / "README.md", readme, error)) {
    DiagnosticEngine diag;
    diag.error(diag::kProjectScaffoldFailed, SourceLocation{}, error);
    render_diagnostics(diag, nullptr);
    return 1;
  }

  std::cout << "created project '" << name << "' in " << directory << "\n"
            << "  src/main.nrqba\n"
            << "  lib/greeting.nrqba\n"
            << "  README.md\n\n"
            << "next:\n"
            << "  cd " << directory << "\n"
            << "  nirqoba run src/main.nrqba --module-path lib\n";
  return 0;
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

  if (first == "new") {
    options.command = CommandKind::New;
    while (idx < argc) {
      std::string arg = argv[idx++];
      if (arg == "--help" || arg == "-h") {
        options.command = CommandKind::Help;
        options.help_topic = "new";
        return true;
      }
      if (arg.rfind("-", 0) == 0) {
        error_message = "unknown option '" + arg + "' for 'new'";
        return false;
      }
      if (!options.project_dir.empty()) {
        error_message = "'new' takes exactly one project directory";
        return false;
      }
      options.project_dir = std::move(arg);
    }
    if (options.project_dir.empty()) {
      error_message = "no project directory specified for 'new'";
      return false;
    }
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
    if (arg == "--module-path") {
      if (idx >= argc) {
        error_message = "missing directory after '--module-path'";
        return false;
      }
      options.module_paths.push_back(argv[idx++]);
      continue;
    }
    if (arg.rfind("--module-path=", 0) == 0) {
      options.module_paths.push_back(arg.substr(14));
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

    options.source_paths.push_back(std::move(arg));
  }

  if (options.source_paths.empty()) {
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

  if (options.command == CommandKind::New) {
    return create_project(options.project_dir);
  }

  if (options.command == CommandKind::Help) {
    if (options.help_topic.empty()) {
      print_general_help();
    } else {
      print_command_help(options.help_topic);
    }
    return 0;
  }

  // Determine artifact paths. The first source file on the command line names
  // the artifact; imported modules do not produce separate outputs.
  fs::path src_path(options.source_paths.front());
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
  pipe_opts.source_paths = options.source_paths;
  pipe_opts.module_search_paths = options.module_paths;
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
