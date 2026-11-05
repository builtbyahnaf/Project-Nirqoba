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
using namespace std;
namespace {

namespace fs = filesystem;

static void print_version() {
  cout << "nirqoba compiler version " << NIRQOBA_VERSION_LABEL
            << " (Phase " << NIRQOBA_PHASE << ")\n"
            << "LLVM/MLIR: " << NIRQOBA_LLVM_VERSION << "\n"
            << "Target: " << NIRQOBA_DEFAULT_TARGET_TRIPLE << "\n"
            << "Build: " << NIRQOBA_BUILD_TYPE << "\n";
}

static void print_general_help() {
  cout << "Usage: nirqoba <command> <file.nrqba>... [options]\n\n"
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

static void print_command_help(string_view cmd) {
  if (cmd == "build") {
    cout << "Usage: nirqoba build <file.nrqba>... [options]\n\n"
              << "Options:\n"
              << "  -o <path>            Output executable or object file path\n"
              << "  --out-dir <dir>      Directory to place output artifacts\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  --emit=<kind>        Emit intermediate stage and stop\n"
              << "                       Kinds: ast, layout, tir, mlir, llvm, llvm-raw, obj\n"
              << "  -O<level>            Optimization level: -O0, -O1, -O2, -O3,\n"
              << "                       -Os, -Oz (default: -O2)\n"
              << "  --opt-level=<level>  Long form of -O<level> (0-3, s, z)\n"
              << "  --ipo=<mode>         Interprocedural optimization: standard,\n"
              << "                       aggressive, minimal (default: standard)\n"
              << "  --pgo-generate       Instrument for profile generation\n"
              << "  --pgo-use=<file>     Optimize with a merged instrumentation profile\n"
              << "  --pgo-sample=<file>  Optimize with an LLVM sample profile\n"
              << "  --pgo-cold=<mode>    Cold-function treatment: default, optsize,\n"
              << "                       minsize, optnone (default: default)\n"
              << "  --target-profile=<p> Target profile: portable, native, explicit\n"
              << "                       (default: inferred; portable without target flags)\n"
              << "  --target <triple>    Explicit target triple (e.g. x86_64-pc-windows-msvc)\n"
              << "  --target-cpu <cpu>   Target CPU model, or 'native' for the host\n"
              << "  --target-features <f> LLVM target feature string (e.g. +avx2)\n"
              << "  --native             Shorthand for --target-profile=native\n"
              << "  --remarks[=mode]     Print LLVM optimization remarks to stderr;\n"
              << "                       mode is 'all', 'missed', or a pass name\n"
              << "                       (e.g. loop-vectorize)\n"
              << "  --fast-math          Allow FP reassociation and FMA contraction\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "run") {
    cout << "Usage: nirqoba run <file.nrqba>... [options] [-- <args>...]\n\n"
              << "Options:\n"
              << "  --out-dir <dir>      Directory for temporary build artifacts\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  -O<level>            Optimization level (default: -O2)\n"
              << "  --ipo=<mode>         Interprocedural optimization: standard,\n"
              << "                       aggressive, minimal (default: standard)\n"
              << "  --pgo-generate       Instrument for profile generation\n"
              << "  --pgo-use=<file>     Optimize with a merged instrumentation profile\n"
              << "  --pgo-sample=<file>  Optimize with an LLVM sample profile\n"
              << "  --pgo-cold=<mode>    Cold-function treatment: default, optsize,\n"
              << "                       minsize, optnone (default: default)\n"
              << "  --target-profile=<p> Target profile: portable, native, explicit\n"
              << "  --target <triple>    Explicit target triple\n"
              << "  --target-cpu <cpu>   Target CPU model, or 'native'\n"
              << "  --target-features <f> LLVM target feature string\n"
              << "  --native             Shorthand for --target-profile=native\n"
              << "  --remarks[=mode]     Print LLVM optimization remarks to stderr;\n"
              << "                       mode is 'all', 'missed', or a pass name\n"
              << "                       (e.g. loop-vectorize)\n"
              << "  --fast-math          Allow FP reassociation and FMA contraction\n"
              << "  --verbose            Enable verbose output\n";
  } else if (cmd == "check") {
    cout << "Usage: nirqoba check <file.nrqba>... [options]\n\n"
              << "Options:\n"
              << "  --module-path <dir>  Extra directory searched for imported modules\n"
              << "  --emit=<kind>        Emit checked stage (ast or tir)\n";
  } else if (cmd == "new") {
    cout << "Usage: nirqoba new <dir>\n\n"
              << "Creates <dir> with src/main.nrqba, lib/greeting.nrqba and a README.\n"
              << "The directory must be new or empty.\n";
  } else {
    print_general_help();
  }
}

static bool parse_emit_kind(string_view str, EmitKind& kind) {
  if (str == "ast")  { kind = EmitKind::Ast; return true; }
  if (str == "layout") { kind = EmitKind::Layout; return true; }
  if (str == "tir")  { kind = EmitKind::Tir; return true; }
  if (str == "mlir") { kind = EmitKind::Mlir; return true; }
  if (str == "llvm") { kind = EmitKind::Llvm; return true; }
  if (str == "llvm-raw") { kind = EmitKind::LlvmRaw; return true; }
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

static bool write_scaffold_file(const fs::path& path, const string& contents,
                                string& error) {
  ofstream out(path, ios::binary | ios::trunc);
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

static int create_project(const string& directory) {
  const fs::path root(directory);
  error_code ec;

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

  const string name = root.filename().empty() ? string("project")
                                              : root.filename().string();
  string error;
  if (!write_scaffold_file(root / "src" / "main.nrqba", kProjectMainTemplate, error) ||
      !write_scaffold_file(root / "lib" / "greeting.nrqba",
                           kProjectGreetingTemplate, error)) {
    DiagnosticEngine diag;
    diag.error(diag::kProjectScaffoldFailed, SourceLocation{}, error);
    render_diagnostics(diag, nullptr);
    return 1;
  }

  string readme = "# " + name + "\n\n"
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

  cout << "created project '" << name << "' in " << directory << "\n"
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
                        string& error_message) {
  if (argc <= 1) {
    options.command = CommandKind::Help;
    return true;
  }

  int idx = 1;
  string first = argv[idx++];

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
      string arg = argv[idx++];
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
      options.project_dir = move(arg);
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
    string arg = argv[idx++];
    if (seen_dash_dash) {
      options.trailing_args.push_back(move(arg));
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
    if (arg.rfind("-O", 0) == 0) {
      OptimizationLevel level = options.optimization_level;
      if (!parse_optimization_level(string_view(arg).substr(2), level)) {
        error_message = "invalid optimization level '" + arg +
                        "'; expected -O0, -O1, -O2, -O3, -Os, or -Oz";
        return false;
      }
      options.optimization_level = level;
      continue;
    }
    if (arg == "--opt-level") {
      if (idx >= argc) {
        error_message = "missing level after '--opt-level'";
        return false;
      }
      OptimizationLevel level = options.optimization_level;
      if (!parse_optimization_level(argv[idx++], level)) {
        error_message = "invalid optimization level; expected 0, 1, 2, 3, s, or z";
        return false;
      }
      options.optimization_level = level;
      continue;
    }
    if (arg.rfind("--opt-level=", 0) == 0) {
      OptimizationLevel level = options.optimization_level;
      if (!parse_optimization_level(string_view(arg).substr(12), level)) {
        error_message = "invalid optimization level '" +
                        arg.substr(12) + "'; expected 0, 1, 2, 3, s, or z";
        return false;
      }
      options.optimization_level = level;
      continue;
    }
    if (arg == "--ipo") {
      if (idx >= argc) {
        error_message = "missing mode after '--ipo'";
        return false;
      }
      if (!parse_ipo_mode(argv[idx++], options.ipo_mode)) {
        error_message = "invalid interprocedural mode; expected standard, "
                        "aggressive, or minimal";
        return false;
      }
      continue;
    }
    if (arg.rfind("--ipo=", 0) == 0) {
      IpoMode mode = options.ipo_mode;
      if (!parse_ipo_mode(string_view(arg).substr(6), mode)) {
        error_message = "invalid interprocedural mode '" + arg.substr(6) +
                        "'; expected standard, aggressive, or minimal";
        return false;
      }
      options.ipo_mode = mode;
      continue;
    }
    if (arg == "--pgo-generate") {
      if (options.pgo_mode != PgoMode::None &&
          options.pgo_mode != PgoMode::Generate) {
        error_message = "conflicting profile-guided optimization options";
        return false;
      }
      options.pgo_mode = PgoMode::Generate;
      continue;
    }
    if (arg == "--pgo-use" || arg.rfind("--pgo-use=", 0) == 0) {
      string profile_path;
      if (arg == "--pgo-use") {
        if (idx >= argc) {
          error_message = "missing profile after '--pgo-use'";
          return false;
        }
        profile_path = argv[idx++];
      } else {
        profile_path = arg.substr(10);
      }
      if (profile_path.empty()) {
        error_message = "empty profile path for '--pgo-use'";
        return false;
      }
      if (options.pgo_mode != PgoMode::None &&
          options.pgo_mode != PgoMode::Use) {
        error_message = "conflicting profile-guided optimization options";
        return false;
      }
      options.pgo_mode = PgoMode::Use;
      options.pgo_profile_path = move(profile_path);
      continue;
    }
    if (arg == "--pgo-sample" || arg.rfind("--pgo-sample=", 0) == 0) {
      string profile_path;
      if (arg == "--pgo-sample") {
        if (idx >= argc) {
          error_message = "missing profile after '--pgo-sample'";
          return false;
        }
        profile_path = argv[idx++];
      } else {
        profile_path = arg.substr(13);
      }
      if (profile_path.empty()) {
        error_message = "empty profile path for '--pgo-sample'";
        return false;
      }
      if (options.pgo_mode != PgoMode::None &&
          options.pgo_mode != PgoMode::Sample) {
        error_message = "conflicting profile-guided optimization options";
        return false;
      }
      options.pgo_mode = PgoMode::Sample;
      options.pgo_profile_path = move(profile_path);
      continue;
    }
    if (arg == "--pgo-cold" || arg.rfind("--pgo-cold=", 0) == 0) {
      string_view mode;
      if (arg == "--pgo-cold") {
        if (idx >= argc) {
          error_message = "missing mode after '--pgo-cold'";
          return false;
        }
        mode = argv[idx++];
      } else {
        mode = string_view(arg).substr(11);
      }
      if (!parse_pgo_cold_mode(mode, options.pgo_cold_mode)) {
        error_message = "invalid cold-function mode '" + string(mode) +
                        "'; expected default, optsize, minsize, or optnone";
        return false;
      }
      continue;
    }
    if (arg == "--native") {
      options.target_profile = TargetProfile::Native;
      options.target_profile_specified = true;
      options.native_profile_requested = true;
      continue;
    }
    if (arg == "--target-profile") {
      if (idx >= argc) {
        error_message = "missing profile after '--target-profile'";
        return false;
      }
      if (!parse_target_profile(argv[idx++], options.target_profile)) {
        error_message = "invalid target profile; expected portable, native, or "
                        "explicit";
        return false;
      }
      options.target_profile_specified = true;
      continue;
    }
    if (arg.rfind("--target-profile=", 0) == 0) {
      const string_view profile = string_view(arg).substr(17);
      if (!parse_target_profile(profile, options.target_profile)) {
        error_message = "invalid target profile '" + string(profile) +
                        "'; expected portable, native, or explicit";
        return false;
      }
      options.target_profile_specified = true;
      continue;
    }
    if (arg == "--target" || arg == "--target-triple") {
      if (idx >= argc) {
        error_message = "missing triple after '" + arg + "'";
        return false;
      }
      options.target_triple = argv[idx++];
      continue;
    }
    if (arg.rfind("--target=", 0) == 0) {
      options.target_triple = arg.substr(9);
      continue;
    }
    if (arg.rfind("--target-triple=", 0) == 0) {
      options.target_triple = arg.substr(16);
      continue;
    }
    if (arg == "--target-cpu") {
      if (idx >= argc) {
        error_message = "missing CPU model after '--target-cpu'";
        return false;
      }
      string cpu = argv[idx++];
      if (cpu == "native") {
        options.target_profile = TargetProfile::Native;
        options.target_profile_specified = true;
        options.native_profile_requested = true;
      } else {
        options.target_cpu = move(cpu);
      }
      continue;
    }
    if (arg.rfind("--target-cpu=", 0) == 0) {
      string cpu = arg.substr(13);
      if (cpu == "native") {
        options.target_profile = TargetProfile::Native;
        options.target_profile_specified = true;
        options.native_profile_requested = true;
      } else {
        options.target_cpu = move(cpu);
      }
      continue;
    }
    if (arg == "--target-features") {
      if (idx >= argc) {
        error_message = "missing feature string after '--target-features'";
        return false;
      }
      options.target_features = argv[idx++];
      continue;
    }
    if (arg.rfind("--target-features=", 0) == 0) {
      options.target_features = arg.substr(18);
      continue;
    }
    if (arg == "--remarks") {
      options.emit_remarks = true;
      continue;
    }
    if (arg == "--fast-math") {
      options.fast_math = true;
      continue;
    }
    if (arg.rfind("--remarks=", 0) == 0) {
      string_view mode = string_view(arg).substr(10);
      options.emit_remarks = true;
      if (mode == "all") {
        options.remarks_missed_only = false;
        options.remark_filter.clear();
      } else if (mode == "missed") {
        options.remarks_missed_only = true;
        options.remark_filter.clear();
      } else if (!mode.empty()) {
        // Any other value narrows reporting to passes whose name contains it,
        // which is how `--remarks=loop-vectorize` asks a single question.
        options.remarks_missed_only = false;
        options.remark_filter = string(mode);
      } else {
        error_message = "empty remark mode; expected 'all', 'missed', or a "
                        "pass name";
        return false;
      }
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
      string_view kind_str = string_view(arg).substr(7);
      if (!parse_emit_kind(kind_str, options.emit)) {
        error_message = "invalid emit kind '" + string(kind_str) +
                        "'; expected ast, layout, tir, mlir, llvm, llvm-raw, or obj";
        return false;
      }
      options.emit_specified = true;
      continue;
    }
    if (arg.rfind("-", 0) == 0) {
      error_message = "unknown option '" + arg + "'";
      return false;
    }

    options.source_paths.push_back(move(arg));
  }

  // Infer the profile when the user did not name one. Any explicit target
  // input means the build is tied to a known deployment target; otherwise the
  // output stays on the portable x86-64 baseline.
  if (!options.target_profile_specified) {
    const bool has_explicit_target =
        !options.target_triple.empty() ||
        (!options.target_cpu.empty() && options.target_cpu != "generic") ||
        !options.target_features.empty();
    options.target_profile = has_explicit_target ? TargetProfile::Explicit
                                                 : TargetProfile::Portable;
  }

  if (options.pgo_mode == PgoMode::Use &&
      options.pgo_profile_path.empty()) {
    error_message = "'--pgo-use' requires a merged profile file";
    return false;
  }
  if (options.pgo_mode == PgoMode::Sample &&
      options.pgo_profile_path.empty()) {
    error_message = "'--pgo-sample' requires a sample profile file";
    return false;
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
  string error_message;
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
  string stem = src_path.stem().string();
  fs::path out_directory = fs::path(options.out_dir);

  string output_file = options.output_path;
  if (output_file.empty()) {
    if (options.emit == EmitKind::Obj) {
      output_file = (out_directory / (stem + ".obj")).string();
    } else {
      output_file = (out_directory / (stem + ".exe")).string();
    }
  }

  string obj_file = (out_directory / (stem + ".obj")).string();

  PipelineOptions pipe_opts;
  pipe_opts.source_paths = options.source_paths;
  pipe_opts.module_search_paths = options.module_paths;
  pipe_opts.output_path = output_file;
  pipe_opts.obj_path = obj_file;
  pipe_opts.verbose = options.verbose;
  pipe_opts.optimization_level = options.optimization_level;
  pipe_opts.ipo_mode = options.ipo_mode;
  pipe_opts.pgo_mode = options.pgo_mode;
  pipe_opts.pgo_profile_path = options.pgo_profile_path;
  pipe_opts.pgo_cold_mode = options.pgo_cold_mode;
  pipe_opts.target_profile = options.target_profile;
  pipe_opts.target_triple = options.target_triple;
  pipe_opts.target_cpu = options.target_cpu;
  pipe_opts.target_features = options.target_features;
  pipe_opts.emit_remarks = options.emit_remarks;
  pipe_opts.remarks_missed_only = options.remarks_missed_only;
  pipe_opts.remark_filter = options.remark_filter;
  pipe_opts.fast_math = options.fast_math;

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
      cout << run_res.output;
    }
    return run_res.exit_code;
  }

  return 0;
}

}  // namespace nirq
