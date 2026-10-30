# Phase 1 Technical Report — Permanent Architecture and Executable Vertical Slice

## 1. Phase Overview

- **Phase Identifier:** Phase 1
- **Phase Title:** Permanent Architecture and Executable Vertical Slice
- **Implementation Date:** 2026-10-30
- **Status:** Complete
- **Objective:** Establish the permanent multi-stage compiler architecture (C++, MLIR, LLVM), reproducible build system, runtime boundary, CLI driver, and an end-to-end executable vertical slice compiling `.nrqba` source to native Windows x64 binaries.
- **Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

---

## 2. Scope

### 2.1 Features Required
- Pinned toolchain (LLVM/MLIR 23.1.4, MSVC, Ninja, CMake) with automated verification.
- Lexer recognizing Phase 1 tokens and reserving Phase 2 operators/keywords.
- Parser with error recovery and explicit `NRQ2100` diagnostics for future-phase constructs.
- Semantic analyzer validating `main` entry point, return types, binding immutability, and `print` constraints.
- Target-independent Typed Intermediate Representation (TIR) with verification invariants and printer.
- Nirqoba MLIR dialect (`nrq`) defined with TableGen and custom lowering pass `nrq-to-llvm`.
- MLIR to LLVM IR lowering pass pipeline and native x86-64 COFF object emission.
- Runtime library (`nrqoba_runtime.lib`) implementing direct Win32 I/O.
- Full CLI driver supporting `build`, `run`, `check`, `version`, `help`, and `--emit={ast,tir,mlir,llvm,obj}`.
- Automated unit test suite and Python integration test runner.
- Architecture documentation, ADRs, CLI references, and CI workflow.

### 2.2 Features Implemented
- All required features above have been fully implemented and integrated.

### 2.3 Features Intentionally Excluded
- Phase 2 operators (`+`, `-`, `*`, `/`, `%` in full expression contexts beyond literals; logical/bitwise ops; comparisons).
- Control flow (`if`, `else`, `while`, `for`, `break`, `continue`).
- User-defined function calls and parameters (scheduled for Phase 3).
- Structured records and collections (Phase 4).
- Modules and imports (Phase 5).
- Garbage collection and explicit memory allocators (Phase 6.5+).
- Async / await (Phase 10).
- Tensors, autograd, and GPU execution (Phase 19–24).

---

## 3. Architecture and Design

The compiler pipeline operates through strictly defined boundaries:

```text
Source (.nrqba) -> Lexer -> AST -> Sema -> TIR -> nrq Dialect -> LLVM Dialect -> LLVM IR -> COFF Object -> Native Exe
```

### Key Design Decisions
1. **Toolchain Pinning (ADR 0001):** Pinning LLVM/MLIR to 23.1.4 via locked archive hashes guarantees reproducible compilation across environments and CI runners.
2. **Canonical Extension (ADR 0002):** `.nrqba` is locked as the official file extension; non-matching extensions are rejected with `NRQ5007`.
3. **Runtime ABI Boundary (ADR 0003):** `print` is lowered through `nrq.print_string` into `nrqoba_runtime.lib`, using Win32 `WriteFile` directly rather than C stdio to facilitate future GC memory ownership.
4. **Extension Points:** Dialects register via `register_all_dialects()` and passes via `build_lowering_pipeline()`, providing permanent extension points without touching core infrastructure.

---

## 4. Implementation Details

- **Frontend (`compiler/lib/Frontend/`):** Hand-written recursive descent parser, line/column tracking in `SourceFile`, maximal-munch lexer with escape sequence resolution.
- **Sema (`compiler/lib/Sema/`):** Scope-based symbol resolution in `SymbolTable`, entry point validation in `TypeChecker`.
- **TIR (`compiler/lib/TIR/`):** Explicit `Alloca`, `Store`, `Load`, `ConstantInt`, `ConstantString`, `PrintString`, `Return`. Full structural invariant checking in `Verifier`.
- **Lowering (`compiler/lib/Lowering/`):** TableGen dialect definition (`NrqOps.td`), `TirToMlir` translation, `NrqToLLVMPass` materializing string globals and runtime calls.
- **Backend (`compiler/lib/Backend/`):** `ObjectEmitter` instantiates LLVM `TargetMachine` for `x86_64-pc-windows-msvc` and emits native COFF objects.
- **Driver & CLI (`compiler/lib/Driver/`, `compiler/lib/CLI/`):** Process management with MSVC command-line argument quoting via `CreateProcessW`, dynamic runtime library discovery, complete CLI driver.
- **Runtime (`runtime/`):** Win32 console/pipe write routines in `io_windows.cpp`, fatal abort handler in `fatal_windows.cpp`.

---

## 5. Modified Files

| File | Role |
|---|---|
| `CMakeLists.txt` | Root build configuration locating LLVM/MLIR and defining targets |
| `CMakePresets.json` | Configure, build, and test presets for Windows MSVC (Release & RelWithDebInfo) |
| `cmake/NirqobaOptions.cmake` | Warning flags, permissive mode, and project options |
| `cmake/NirqobaLLVMTargets.cmake` | LLVM library mapping wrappers and default target triple |
| `compiler/CMakeLists.txt` | Defines TableGen generation, `nirqoba_compiler_lib`, and `nirqoba` executable |
| `compiler/include/nirqoba/Version.h.in` | Version header template populated by CMake |
| `compiler/include/nirqoba/Diagnostics/DiagnosticRenderer.h` | Diagnostics rendering interface |
| `compiler/lib/Diagnostics/DiagnosticRenderer.cpp` | Formats diagnostics with line/column/caret output |
| `compiler/include/nirqoba/Lowering/PassPipeline.h` | Lowering pass pipeline declarations |
| `compiler/include/nirqoba/Lowering/RegisterDialects.h` | Dialect registration declarations |
| `compiler/lib/Lowering/PassPipeline.cpp` | Implements dialect registration and pass pipeline |
| `compiler/lib/Lowering/NrqToLLVMPass.cpp` | Custom MLIR pass lowering `nrq.print_string` to LLVM calls |
| `compiler/include/nirqoba/Lowering/TirToMlir.h` | TIR to MLIR translation declarations |
| `compiler/lib/Lowering/TirToMlir.cpp` | Lowers TIR operations to MLIR operations |
| `compiler/include/nirqoba/Backend/TargetInfo.h` | Target triple helpers |
| `compiler/include/nirqoba/Backend/ObjectEmitter.h` | Object file emission declarations |
| `compiler/lib/Backend/ObjectEmitter.cpp` | LLVM TargetMachine COFF emission |
| `compiler/include/nirqoba/Driver/LinkerEnvironment.h` | Linker and SDK path discovery declarations |
| `compiler/lib/Driver/LinkerEnvironment.cpp` | Loads `toolchain.json` and locates linkers/libraries |
| `compiler/include/nirqoba/Driver/ProcessRunner.h` | Process execution helper declarations |
| `compiler/lib/Driver/ProcessRunner.cpp` | Win32 `CreateProcessW` runner with argument quoting |
| `compiler/include/nirqoba/Driver/CompilationPipeline.h` | End-to-end compilation orchestration declarations |
| `compiler/lib/Driver/CompilationPipeline.cpp` | Coordinates all compiler stages, backend, and linker |
| `compiler/include/nirqoba/CLI/CommandLine.h` | CLI argument data structures |
| `compiler/include/nirqoba/CLI/Driver.h` | CLI driver entry point declarations |
| `compiler/lib/CLI/Driver.cpp` | CLI command parser, options handler, and subcommands |
| `compiler/tools/nirqoba/main.cpp` | Executable entry point for `nirqoba.exe` |
| `runtime/CMakeLists.txt` | Build configuration for `nrqoba_runtime` static library |
| `runtime/include/nirqoba/runtime/*.h` | C ABI runtime headers (`io.h`, `fatal.h`, `version.h`) |
| `runtime/src/*.cpp` | Win32 runtime implementations (`io_windows.cpp`, `fatal_windows.cpp`, `runtime_version.cpp`) |
| `tests/CMakeLists.txt` | Test suite CMake target configuration |
| `tests/support/*` | In-repo unit test harness (`test_harness.h`, `unit_test_main.cpp`) |
| `tests/unit/*_test.cpp` | Unit tests for Lexer, Parser, Sema, Diagnostics, TIR, Dialect, and CLI |
| `tests/run_tests.py` | Python standard library integration test runner |
| `tests/README.md` | Test suite documentation |
| `tests/integration/**` | Integration test cases (positive, negative, cli, mlir) |
| `examples/hello.nrqba` | Nirqoba first program example |
| `.github/workflows/ci.yml` | GitHub Actions CI workflow |
| `docs/architecture/*.md` | Comprehensive architecture specifications (00 through 08) |
| `docs/architecture/decisions/*.md` | Architecture Decision Records (ADR 0001 to 0003) |
| `docs/compiler/cli.md` | CLI command and diagnostic code documentation |
| `docs/getting-started/build-from-source.md` | Step-by-step build and run instructions |

---

## 6. Language/API Changes

- Implemented syntax:
  - `type function IDENT() { ... }`
  - `let IDENT [: type] = expr;`
  - `var IDENT [: type] = expr;`
  - `return [expr];`
  - `print("literal");`
  - Types: `int` (i64), `bool` (i1), `string`, `void`.
- Diagnostic codes established: `NRQ1xxx` (lexical), `NRQ2xxx` (syntax), `NRQ3xxx` (semantic), `NRQ4xxx` (TIR/MLIR), `NRQ5xxx` (driver/linker).

---

## 7. Testing

### 7.1 Unit Tests
- `nirqoba_unit_tests` executed via `ctest`.
- Covers: Lexer (tokens, lines/columns, escapes, comments, future keywords), Parser (recovery, expected tokens, phase diagnostics), Sema (main signatures, duplicate symbols, print restrictions), Diagnostics (rendering, spans), TIR (builder, verifier, printing), MLIR (nrq dialect op round-trip), CLI (argument parsing, flags).

### 7.2 Integration Tests
- `tests/run_tests.py` runs end-to-end test cases:
  - `positive/hello`: Verifies execution, stdout `"Hello, Nirqoba!\n"`, exit code 0, and COFF header check.
  - `positive/return42`: Verifies exit code propagation (42).
  - `negative/syntax_error`: Verifies exit code 1 and `[NRQ2002]`.
  - `negative/type_error`: Verifies exit code 1 and `[NRQ3015]`.
  - `negative/print_42`: Verifies exit code 1 and `[NRQ3010]`.
  - `negative/duplicate_main`: Verifies exit code 1 and `[NRQ3002]`.
  - `negative/phase2_operator`: Verifies exit code 1 and `[NRQ2100]`.
  - `cli/missing_file`: Verifies exit code 3 and `[NRQ5001]`.
  - `cli/unknown_flag`: Verifies exit code 2 and `[NRQ5002]`.
  - `mlir/print_lowering`: Verifies `--emit=mlir` contains `nrq.print_string` before lowering and `llvm.call @nrq_print_string` after lowering.

---

## 8. Performance

Not applicable for Phase 1. Benchmarks will be introduced alongside arithmetic optimizations and collection data structures in Phase 2/4.

---

## 9. Known Issues & Limitations

- Identifiers and keywords are case-sensitive in Phase 1 (case-insensitivity resolution deferred to Phase 2 per SYNTAX Appendix C).
- Source column numbering counts UTF-8 bytes rather than grapheme clusters (Unicode support planned for Phase 4).
- Target platform is currently Windows x64; Linux platform support will follow in the dedicated platform milestone.

---

## 10. Documentation Changes

- Created `docs/architecture/00-overview.md` through `08-extension-points.md`.
- Created ADRs 0001, 0002, 0003 in `docs/architecture/decisions/`.
- Created `docs/compiler/cli.md` with complete diagnostic code reference table.
- Created `docs/getting-started/build-from-source.md`.

---

## 11. Acceptance Criteria

| Requirement | Status |
|---|---|
| Repository, CMake configuration, CI baseline | PASS |
| Pinned C++ toolchain and MLIR/LLVM dependencies | PASS |
| Frontend, TIR, MLIR, lowering, backend, linker, and runtime boundaries documented | PASS |
| Memory, type, effect, async, and execution-target models specified | PASS |
| Extensibility interfaces for future tensor, graph, async, GPU operations | PASS |
| Minimum lexer, parser, and semantic analysis for the first program | PASS |
| Native code generated through MLIR/LLVM (COFF header verified) | PASS |
| Compiler executable and diagnostic infrastructure | PASS |
| `build`, `run`, `check`, `version`, `help` commands | PASS |
| First program runs with correct process exit code | PASS |
| Invalid source produces meaningful diagnostics | PASS |
| Missing files and invalid options fail clearly (exit codes 3 and 2) | PASS |
| Generated executables run without a GPU | PASS |
| No generated C++ transpiler intermediate backend used | PASS |
| Phase 1 report complete | PASS |

---

## 12. Final Verdict

**PASS**

Phase 1 permanent architecture, complete compiler pipeline, runtime library, test suite, and documentation are fully implemented, verified, and ready.
