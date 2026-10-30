// Driver/CompilationPipeline.h — orchestrates the full build pipeline.
//
// The pipeline runs: lex → parse → sema → TIR → MLIR → object → link.
// Each stage can stop early by returning false; diagnostics accumulate in
// the engine and are rendered by the caller after the pipeline returns.
#pragma once

#include <string>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/SourceFile.h"

namespace nirq {

// What the pipeline should produce; used by `--emit` to stop early.
enum class EmitKind {
  Ast,    // print AST text, exit
  Tir,    // print TIR text, exit
  Mlir,   // print MLIR (before and after lowering), exit
  Llvm,   // print LLVM IR text, exit
  Obj,    // write object file, exit
  Exe,    // link to executable (default)
};

struct PipelineOptions {
  std::string source_path;
  std::string output_path;      // path for the final artifact (.obj or .exe)
  std::string obj_path;         // intermediate .obj (used when emit == Exe)
  std::string toolchain_json;   // empty = use default location
  EmitKind emit = EmitKind::Exe;
  bool verbose = false;
};

// Runs the compilation pipeline as directed by `options`. Returns 0 on
// success, 1 on compile/link failure, or another non-zero code for
// environment errors. Diagnostics are rendered to stderr before returning.
//
// The `source` output parameter (when not null) receives the loaded SourceFile
// so the caller can render diagnostics with source-line underlines.
int run_pipeline(const PipelineOptions& options,
                 DiagnosticEngine& diagnostics);

}  // namespace nirq
