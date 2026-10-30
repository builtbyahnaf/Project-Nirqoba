# Phase 1 — Permanent Architecture and Executable Vertical Slice

**Status:** Implementation plan
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)
**Plan version:** 1.0
**Scope sources:** `project_context/ROADMAP.md` §Phase 1, `project_context/SYNTAX.md` §Phase 1, `project_context/AGENTS.md`
**Target report:** `reports/phase-01.md`

---

## 1. Decisions this plan locks

| Decision | Choice | Rationale |
|---|---|---|
| Source extension | `.nrqba` | ROADMAP and README use it throughout; `.nq` appears once, in SYNTAX §Pre-Phase 7. Recorded as ADR 0002 and as a spec discrepancy in the report. |
| MLIR/LLVM | **23.1.4** (released 20 Oct 2026), `clang+llvm-23.1.4-x86_64-pc-windows-msvc.tar.xz` | That archive is produced with `LLVM_INSTALL_TOOLCHAIN_ONLY=OFF`, so it contains `lib/cmake/mlir/MLIRConfig.cmake`, MLIR headers and static libraries, `mlir-tblgen` and `lld-link`. The `.msi`/`.exe` installer is toolchain-only and ships **no** MLIR CMake package. |
| Toolchain | MSVC (Visual Studio Build Tools) + Ninja + CMake >= 3.28 | The prebuilt MLIR SDK uses the MSVC ABI and the dynamic Release CRT; MinGW g++ cannot link it, and LLVM does not support MinGW builds on Windows. |
| `print` | Language builtin `nrq.print_string`, lowered to a call into a real `runtime/` static library | Establishes the compiler/runtime ABI and the future GC-root boundary in Phase 1 instead of a throwaway `printf` shim. |
| CI baseline | `.github/workflows/ci.yml` on `windows-latest` | ROADMAP Phase 1 task 1; remote is `builtbyahnaf/Project-Nirqoba`. |
| `int` | Fixed 64-bit (`i64`) | SYNTAX Appendix C high-priority item; a fixed width is predictable across platforms. |
| Identifier case | Keywords and identifiers case-sensitive in Phase 1, compared through one lexer helper | SYNTAX Appendix C proposes case-insensitive identifiers but marks it unresolved; Phase 1 records the choice and the migration path instead of inventing the final rule. |

### Verified environment baseline

Windows 11 (build 26200), MSYS2/MinGW64 shell, g++ 16.2.0, Python 3.12, `winget` available,
CMake 4.5.0-rc2 at `C:\Program Files\CMake`, **no Ninja**, **no MSVC**, **no LLVM/MLIR**, 12 cores, 103 GB free on `D:`.
Git: 2 commits, remote `https://github.com/builtbyahnaf/Project-Nirqoba`.
The repository currently contains documentation only; Phase 1 creates the implementation.

---

## 2. Prerequisites installed during implementation (one elevation request)

1. **Visual Studio Build Tools** with *Desktop development with C++* (MSVC v143+, Windows 11 SDK):
   `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
   Requires administrator approval. If declined, implementation stops at that step and the phase is reported
   `BLOCKED` with the exact missing tool. No MinGW workaround is attempted.
2. **Ninja**: `py -m pip install --user ninja` (no admin rights).
3. **LLVM 23.1.4 SDK**: downloaded by `scripts/bootstrap.ps1` into `toolchain/llvm-23.1.4/`. The SHA-256 is taken from
   the official GitHub release asset digest at fetch time and recorded in `toolchain/versions.lock.json`. A digest is
   never invented; if the published digest cannot be obtained, the download is treated as unverified and the report
   says so.
4. **CMake**: reuse the installed 4.5.0-rc2 only if it satisfies `>= 3.28`; bootstrap warns that it is a release
   candidate and the lock file records the exact version used.

Disk budget: roughly 2 GB compressed and 6 GB extracted for the LLVM SDK, plus about 4 GB for VS Build Tools.

---

## 3. Repository layout created in this phase

```text
CMakeLists.txt                     # root: locates MLIR/LLVM, adds components, C++20
CMakePresets.json                  # windows-msvc-release, windows-msvc-dev + build/test presets
cmake/NirqobaOptions.cmake         # warnings, Werror switch, install layout, version header
cmake/NirqobaLLVMTargets.cmake     # llvm_map_components_to_libnames helper
toolchain/versions.lock.json       # llvm 23.1.4 + sha256, cmake, ninja, python, msvc minimum
toolchain/                         # gitignored: extracted SDK + generated toolchain.json
compiler/
  include/nirqoba/
    Common/{StringRefCompat,Result}.h
    Diagnostics/{SourceLocation,Diagnostic,DiagnosticEngine,DiagnosticRenderer}.h
    Frontend/{Token,TokenKind,Lexer,Ast,Parser,AstPrinter}.h
    Sema/{SymbolTable,TypeResolver,TypeChecker}.h
    TIR/{Types,Operations,Block,Function,Module,Builder,Verifier,Printing}.h
    Lowering/{RegisterDialects,PassPipeline}.h
    Backend/{TargetInfo,ObjectEmitter}.h
    Driver/{CompilationPipeline,LinkerEnvironment,ProcessRunner}.h
    CLI/{CommandLine,Driver}.h
    Version.h.in
  lib/
    Diagnostics/{DiagnosticEngine,DiagnosticRenderer}.cpp
    Frontend/{Lexer,Parser,AstPrinter}.cpp
    Sema/{SymbolTable,TypeChecker}.cpp
    TIR/{Builder,Verifier,Printing}.cpp
    Lowering/PassPipeline.cpp
    Backend/ObjectEmitter.cpp
    Driver/{CompilationPipeline,LinkerEnvironment,ProcessRunner}.cpp
    CLI/Driver.cpp
  dialects/nrq/
    include/nrq/{NrqDialect.td,NrqOps.td,NrqDialect.h,NrqOps.h,Passes.h}
    lib/{NrqDialect.cpp,NrqOps.cpp,NrqToLLVMPass.cpp}
  tools/nirqoba/main.cpp
runtime/
  include/nirqoba/runtime/{io.h,fatal.h,version.h}
  src/{io_windows.cpp,fatal_windows.cpp,runtime_version.cpp}
  CMakeLists.txt                   # static library nrqoba_runtime
tests/
  support/{test_harness.h,unit_test_main.cpp}
  unit/{lexer,parser,sema,diagnostics,tir,nrq_dialect,cli}_test.cpp
  integration/{positive,negative,cli,mlir}/<case>/{main.nrqba,expect.json[,expect.mlir]}
  run_tests.py                     # CLI/integration runner, Python standard library only
  README.md                        # layout, how to add a case, how to run
examples/hello.nrqba
scripts/{bootstrap,build,test}.ps1
docs/architecture/{00-overview,01-frontend,02-typed-ir,03-dialects,04-lowering,05-backend-and-linker,06-runtime,07-models,08-extension-points}.md
docs/architecture/decisions/{0001-toolchain-pinning,0002-source-extension,0003-print-runtime-boundary}.md
docs/compiler/cli.md
docs/getting-started/build-from-source.md
.github/workflows/ci.yml
reports/phase-01.md
```

`.gitignore` receives `build/`, `toolchain/`, `out/`, `*.obj`, `*.exe`, `__pycache__/` **appended** to the existing
uncommitted edit. No existing line is modified or removed.

---

## 4. Architecture and stage boundaries

Every stage below is implemented in this phase; nothing is documentation-only.

```text
source .nrqba
  -> SourceManager        owns the buffer, maps offsets to line/column
  -> Lexer                tokens (full Phase-2 operator and keyword set recognized)
  -> Parser               AST: function decl, block, let/var decl, return, call/print, literals
  -> Sema                 typed AST (int=i64, string=UTF-8, bool, void), symbol table, diagnostics
  -> TIR                  nirq::tir::Module, SSA-lite with explicit local slots and source locations
  -> nrq dialect (MLIR)   nrq.print_string, nrq.entry_point
  -> nrq-to-llvm, convert-func-to-llvm, reconcile-unrealized-casts
  -> mlir::translateModuleToLLVMIR -> llvm::Module
  -> TargetMachine (x86_64-pc-windows-msvc) -> COFF object
  -> lld-link + nrqoba_runtime.lib + MSVC/UCRT libraries -> .exe
```

### 4.1 Diagnostics

`DiagnosticEngine` collects `Diagnostic{severity, code, SourceLocation, message, optional fixit}`. The renderer prints
`file:line:col: severity[NRQxxxx]: message`, the offending source line, and a caret span. Code blocks, tabulated in
`docs/compiler/cli.md`:

- `NRQ1xxx` lexical, `NRQ2xxx` syntax, `NRQ3xxx` semantic/type,
- `NRQ4xxx` TIR/MLIR/lowering, `NRQ5xxx` driver/linker/runtime.

Severities: `note`, `warning`, `error`, `fatal`. Warnings do not fail a build unless `NIRQOBA_WERROR` is enabled.

### 4.2 Lexer

Maximally-munching scanner over a `std::string_view`, tracking offset/line/column (1-based; columns count UTF-8 bytes,
grapheme handling deferred to Phase 4). Supports `//`, `///` and block comments, string literals with escape sequences
for newline, tab, backslash, double quote and NUL, an unknown escape reported as `NRQ1006`, `NRQ1007` for an
unterminated string, and `NRQ1008` for an illegal character.

The lexer recognizes the **entire** Phase-2 operator/delimiter set
(`+ - * / % ** == != < <= > >= && || ! & | ^ ~ << >> = += -= *= /= |= -> : . ,`) and reserves future keywords
(`if else while for break continue struct module import public private async await protected fn`), so Phase 2 is
additive rather than a lexer rewrite, as ROADMAP §3.3 requires.

### 4.3 Parser

Hand-written recursive descent. Expression parsing uses a precedence-climbing skeleton whose table contains only
primary expressions in this phase. When a binary or unary operator is encountered, the parser emits a deliberate
diagnostic — `NRQ2100: binary operator '+' is specified for Phase 2 and is not implemented in Phase 1` — instead of a
generic "unexpected token". This satisfies AGENTS §4.5 (no silent skipping) and §3 (no premature Phase 2 work).

Grammar implemented in Phase 1:

```text
program        := function_decl* EOF
function_decl  := type 'function' IDENT '(' ')' block          # zero parameters in Phase 1
type           := 'int' | 'string' | 'bool' | 'void'
block          := '{' statement* '}'
statement      := var_decl | return_stmt | expr_stmt | block
var_decl       := ('let' | 'var') IDENT (':' type)? '=' expression ';'
return_stmt    := 'return' expression? ';'
expr_stmt      := expression ';'
expression     := primary
primary        := INT_LITERAL | STRING_LITERAL | 'true' | 'false' | call_expr | '(' expression ')'
call_expr      := IDENT '(' (expression (',' expression)*)? ')'
```

### 4.4 Semantic analysis

- Exactly one `main` per executable (`NRQ3001` none, `NRQ3002` duplicates).
- `main` return type must be `int` or `void` (`NRQ3003`); parameters rejected with `NRQ3004` until Phase 3.
- `void` functions may not return a value (`NRQ3005`); non-`void` functions must return (`NRQ3006`).
- `let` bindings are immutable; no assignment grammar exists yet, so the rule is enforced at declaration level and
  documented as partially enforced until Phase 2.
- `print` accepts exactly one `string` argument (`NRQ3010`); `print(42)` is a type error, not an implicit conversion.
- Undefined name `NRQ3011`, duplicate declaration in the same scope `NRQ3012`.

### 4.5 Typed IR (TIR)

`nirq::tir::{Module, Function, Block, Op}` with ops `LiteralInt`, `LiteralString`, `PrintString`, `LocalAlloc`,
`LocalStore`, `LocalLoad`, `Return`. Every op carries a `SourceLocation`: the hook Phase 3 task 9 needs for debuggable
source mappings. `Printing` produces the stable textual form used by `--emit=tir` and golden tests. `Verifier`
enforces that every block ends in `Return`, that local slots are allocated before use, and that `PrintString`'s
operand is a string literal.

The type system is a closed `TypeKind{Ptr, Void, I64, Bool, String}` with documented reserved kinds. Beyond the
`DialectRegistry`/`PassPipeline` registries ROADMAP §3.3 requires, no speculative abstraction is introduced
(AGENTS §2).

### 4.6 MLIR dialect `nrq`

TableGen-defined out-of-tree dialect, built with the SDK's `mlir-tblgen`.

| Op | Signature | Semantics |
|---|---|---|
| `nrq.print_string` | `(StringAttr value) -> ()` | Writes the value followed by a newline to stdout through the runtime |
| `nrq.entry_point` | unit attribute on `func.func` | Marks the executable entry point |

- Registered types in Phase 1: **none**. `!nrq.tensor`, `!nrq.node` and `!nrq.device` are named in
  `docs/architecture/03-dialects.md` as future work and are not stubbed in code.
- `NrqToLLVMPass` lowers `nrq.print_string` to an `llvm.mlir.global` string constant plus
  `llvm.call @nrq_print_string`; the standard `convert-func-to-llvm` and `reconcile-unrealized-casts` pipeline
  follows; then `mlir::translateModuleToLLVMIR`.
- `initialize()` and `verifyInvariants()` are implemented. No `nrq-opt` tool is built; inspection happens through
  `nirqoba --emit=mlir`.

### 4.7 Backend and linker

`ObjectEmitter` builds an LLVM `TargetMachine` for `x86_64-pc-windows-msvc` (`Reloc::PIC_`,
`CodeGenOptLevel::Default`), takes triple and data layout from `LLVM::LLVMDialect` so MLIR and the object always
agree, and emits a COFF object through `addPassesToEmitFile(ObjectFile)`.

`LinkerEnvironment` reads `toolchain/toolchain.json` (written by bootstrap from `vswhere`) and exposes
`lld-link.exe`, the MSVC x64 library directory, and the Windows SDK ucrt and um x64 library directories. A missing
file produces `NRQ5003: toolchain configuration not found; run scripts/bootstrap.ps1`.

`ProcessRunner` uses `CreateProcessW` with correct argument quoting for paths containing spaces, captures stdout and
stderr, and returns the child exit code. It serves both linking and `nirqoba run`.

### 4.8 Runtime boundary

C ABI, headers under `runtime/include/nirqoba/runtime/`:

```c
int32_t     nrq_print_string(const char* utf8, size_t byte_len); /* bytes then newline; 0 = success */
void        nrq_runtime_init(void);
void        nrq_runtime_shutdown(void);
void        nrq_runtime_fatal(const char* message);              /* stderr then ExitProcess(70) */
const char* nrq_runtime_version(void);
```

The implementation uses `WriteFile` on the standard output handle with a partial-write loop, deliberately avoiding CRT
`stdio` so the boundary stays replaceable and the future collector can own it. Non-Windows compilation hits an
`#error` naming the platform milestone, never a silent stub. `docs/architecture/06-runtime.md` reserves the GC
root-registration and `nrq_alloc` hooks for Phase 6.5 **without** implementing them.

---

## 5. Command-line interface

```text
nirqoba build <file.nrqba> [--out-dir DIR] [-o PATH] [--emit=ast|tir|mlir|llvm|obj] [--verbose]
nirqoba run   <file.nrqba> [--out-dir DIR] [--verbose]
nirqoba check <file.nrqba> [--emit=ast|tir]
nirqoba version | --version
nirqoba help [command] | --help
```

- Default output `<stem>.exe` inside `--out-dir` (default: current directory); intermediate `<stem>.obj`.
- `--emit` prints the requested stage to stdout and stops. `--emit=mlir` prints both the unlowered and the lowered
  form, which is the test proving a genuine MLIR pipeline.
- Exit codes: `0` success, `1` compile/link failure (diagnostics on stderr), `2` invalid usage, `3` input I/O error,
  `4` environment/toolchain error. `run` propagates the program's exit code on success. The report documents that a
  program returning `1` is indistinguishable from a compilation failure by exit code alone, and that diagnostics
  always carry the `severity[NRQxxxx]` prefix so scripts can distinguish them.
- `version` prints compiler version, phase, pinned LLVM version, target triple and build configuration.
- Deliberately absent: `--debug`/`--release`, `doctor`, `fmt`, `new` and package commands — no functional backend
  exists for them yet (ROADMAP §Pre-Phase 7.2: do not implement commands without backend support).

---

## 6. Build system and reproducibility

- Root `CMakeLists.txt`: `cmake_minimum_required(VERSION 3.28)`,
  `project(nirqoba VERSION 0.1.0 LANGUAGES C CXX)`, C++20, `find_package(LLVM 23.1 REQUIRED CONFIG)`,
  `find_package(MLIR REQUIRED CONFIG)`, `include(AddLLVM)`, `include(AddMLIR)`, `include(TableGen)`,
  `/W4 /permissive- /EHsc`, warnings-as-errors behind `NIRQOBA_WERROR`.
- `CMakePresets.json`: `windows-msvc-release` (Ninja, `Release`, dynamic Release CRT) and `windows-msvc-dev`
  (`RelWithDebInfo`, dynamic Release CRT). **No Debug preset**: the SDK is a Release build, so a debug-CRT compiler
  build would fail at link. Our own assertions remain available through `NIRQOBA_ENABLE_ASSERTS`, documented as
  distinct from MLIR-internal assertions, which this SDK does not provide.
- `toolchain/versions.lock.json` pins the LLVM version and SHA-256, CMake minimum and pinned version, Ninja, Python and
  the minimum MSVC toolset. No third-party library beyond MLIR/LLVM is introduced: unit tests use a small in-repo
  harness instead of GoogleTest, keeping the dependency surface exactly MLIR+LLVM.
- `scripts/bootstrap.ps1` is idempotent (`-Force` to redownload), verifies every prerequisite, prints actionable
  errors for missing items, checks that `lib/cmake/mlir/MLIRConfig.cmake` and `bin/mlir-tblgen.exe` exist, and writes
  `toolchain/toolchain.json`.
- `scripts/build.ps1 [-Configuration Release|RelWithDebInfo] [-Clean]` enters the developer environment through
  `vcvars64.bat` located by `vswhere` (no hard-coded installation paths), then configures and builds the preset.
- `scripts/test.ps1` builds, then runs `ctest` and `python tests/run_tests.py`, writing logs to `build/logs/`.

---

## 7. Testing plan

### 7.1 Unit tests (`nirqoba_unit_tests`, driven by ctest)

- Lexer: token matrix, line/column accuracy, escape handling, unterminated string, illegal character, comment forms.
- Parser: accepts the Phase 1 first program; rejects malformed functions, blocks and declarations with the expected
  code.
- Sema: accepts `int` and `void` `main`; rejects missing or duplicate `main`, wrong return type, parameters,
  `print(42)`, undefined and duplicate names.
- Diagnostics: rendering snapshot with file, line, column and caret.
- TIR: builder, printer round-trip, verifier failures.
- `nrq` dialect: operation parse/print round-trip and verifier rejection of a malformed `nrq.print_string`.
- CLI: argument parsing for valid and invalid invocations, `--help`, `--version`, `--` handling.

### 7.2 Integration tests (`tests/run_tests.py`, Python standard library only)

Each case directory holds `main.nrqba` and `expect.json` with `mode`, `args`, `expect_stdout`,
`expect_stderr_contains`, `expect_exit`.

- Positive: `examples/hello.nrqba` prints `Hello, Nirqoba!` and exits 0 (the ROADMAP first program).
- Exit code: a program with `return 42`, run through `nirqoba run`, yields 42.
- Negative: syntax error, type error, `print(42)`, duplicate `main`, use of a Phase 2 operator — each exits 1 with the
  documented code and never crashes.
- CLI: missing file exits 3; unknown flag or command exits 2; `check` succeeds and fails correctly; paths containing
  spaces; relative and absolute paths; `--out-dir`.
- MLIR proof: golden `expect.mlir` shows `nrq.print_string` before lowering and `llvm.call @nrq_print_string` after.
- Native-code proof: the emitted `.obj` begins with the AMD64 COFF machine header (`0x64 0x86`), evidence that the
  artifact came from LLVM code generation rather than generated C++.
- No-GPU: the produced `.exe` runs on this machine; `dumpbin /dependents` shows only system libraries (kernel32, ucrt,
  vcruntime).
- Regression baseline: every Phase 1 case stays registered under `tests/integration/**` for later phases.

### 7.3 CI

`.github/workflows/ci.yml`: `windows-latest`, cache keyed on the `toolchain/versions.lock.json` hash for the LLVM SDK,
run bootstrap (skipping the VS install step because the runner image already provides MSVC), build, run `ctest` and
the Python suite, upload test logs. If the workflow cannot be executed in this phase because nothing has been pushed,
the report states exactly that instead of claiming a CI pass.

### 7.4 Commands recorded verbatim in the report

```powershell
.\scripts\bootstrap.ps1
.\scripts\build.ps1 -Configuration RelWithDebInfo
.\scripts\test.ps1
build\msvc-dev\compiler\nirqoba.exe run examples\hello.nrqba
```

---

## 8. Documentation deliverables

`docs/architecture/`:

- `00-overview.md` — pipeline, stage ownership, what each stage may and may not know.
- `01-frontend.md` — source mapping, token model, AST design, error-recovery limits.
- `02-typed-ir.md` — why TIR exists between AST and MLIR, op set, verification invariants.
- `03-dialects.md` — the `nrq` dialect, plus reserved async/tensor/graph/dataflow/GPU dialect responsibilities and
  their lowering boundaries.
- `04-lowering.md` — op-by-op mapping and which information must never be discarded early.
- `05-backend-and-linker.md` — COFF emission, `lld-link` invocation, MSVC/SDK library discovery.
- `06-runtime.md` — runtime ABI table, ownership, thread-safety, what Phase 6.5 will add.
- `07-models.md` — memory (GC by default, roots reserved, ownership deferred to Phase 9), type model (`int` = i64
  fixed), effect model (none yet, reserved), async model, execution-target model (CPU-only, GPU never implicit).
- `08-extension-points.md` — how Phase 2+ and future dialects, targets and runtime services plug in.
- `decisions/0001-toolchain-pinning.md`, `0002-source-extension.md`, `0003-print-runtime-boundary.md`.

Plus `docs/compiler/cli.md` (Phase 1 CLI reference and diagnostic-code table) and
`docs/getting-started/build-from-source.md` (bootstrap, build, run, clean rebuild). No empty placeholder documents.

`reports/phase-01.md` follows the mandated 12 sections: overview (with author line), scope, architecture and design,
implementation details, modified files with roles, language/API changes, testing (categories, commands, environment,
expected versus actual, PASS/FAIL, skips), performance (N/A with justification — no benchmarks exist yet), known
issues, documentation changes, requirement-by-requirement acceptance criteria, and a single verdict `PASS`, `FAIL` or
`BLOCKED`.

---

## 9. Implementation milestones and commit checkpoints

AGENTS §1.9 requires prompting for commits as features complete. A commit is suggested at each checkpoint; nothing is
pushed without an explicit request.

| # | Milestone | Suggested commit message |
|---|---|---|
| 0 | Save this plan; append `.gitignore` entries | `docs: add Phase 1 implementation plan` |
| 1 | Toolchain lock, bootstrap/build/test scripts, `toolchain.json` generation, install prerequisites (elevation for VS Build Tools) | `build: pin LLVM 23.1.4 and add reproducible Windows bootstrap` |
| 2 | Root CMake, presets, version header, driver skeleton (`version`, `help`, exit codes 2 and 3); verify MLIR/LLVM target names against `MLIRTargets.cmake` | `build: add CMake presets and compiler driver skeleton` |
| 3 | Source manager, diagnostics engine and renderer, lexer, unit tests | `feat(frontend): add source manager, diagnostics and lexer` |
| 4 | AST, parser including Phase-2 operator diagnostics, AST printer, unit tests | `feat(frontend): parse functions, declarations, returns and calls` |
| 5 | Symbol table, type checker, TIR builder/printer/verifier, unit tests | `feat(sema): type-check the vertical slice and build typed IR` |
| 6 | `nrq` dialect (TableGen), `nrq-to-llvm` pass, object emitter, linker environment, `run` | `feat(mlir): add nrq dialect and LLVM native backend` |
| 7 | Runtime library and `print` end-to-end; integration suite and runner; CI workflow | `feat(runtime): link print through the Nirqoba runtime` |
| 8 | Architecture docs, ADRs, CLI docs, `reports/phase-01.md` with real command output | `docs: document Phase 1 architecture and report` |

---

## 10. Explicit exclusions

Operators and expressions beyond literals; control flow (`if`/`while`/`for`); user-defined function calls and
parameters; arrays, tuples, structs; modules and imports; string operations beyond literals and `print`; I/O beyond
`print`; optimization passes beyond those required for lowering; GC, ownership and borrowing; async and coroutines;
tensors, computational graphs, automatic differentiation; GPU/CUDA; package manager; `nirqoba doctor`; formatter;
language server; Linux support; case-insensitive identifiers; `f_string`.

Phase 2 syntax is **recognized by the lexer** but every use is rejected with a diagnostic naming the phase that
implements it. Recognition without implementation is not silent support.

---

## 11. Risks and mitigations

| # | Risk | Mitigation |
|---|---|---|
| 1 | No MSVC installed; the install needs elevation | One explicit `request_elevation`; if declined, report `BLOCKED` at that step. No MinGW fallback, since LLVM does not support it on Windows. |
| 2 | SDK is a Release build without MLIR assertions | Presets limited to Release/RelWithDebInfo; limitation documented; a source-built LLVM with assertions becomes a Phase 6 option if MLIR debugging is needed. |
| 3 | Tarball likely lacks `FileCheck`/`llvm-lit` (`LLVM_INSTALL_UTILS` off) | Custom Python runner plus in-repo unit harness; no `lit` dependency invented. |
| 4 | MLIR/LLVM library and target names may differ in 23.1.4 (e.g. the LLVM-IR export target) | Verified against `$MLIR_DIR/MLIRTargets.cmake` in milestone 2 before dependent code is written. |
| 5 | `lld-link` may be absent from the archive | `toolchain.json` records the resolved linker; documented fallback to MSVC `link.exe` with the same library-path set. |
| 6 | Local CMake is a release candidate (4.5.0-rc2) | Lock requires >= 3.28 and records the exact version; bootstrap warns on RC builds. |
| 7 | Windows path and quoting edge cases | Dedicated tests for spaces, absolute and relative paths; `CreateProcessW` quoting is tested rather than assumed. |
| 8 | Prebuilt SDK has a restricted backend list (`AArch64;ARM;X86;BPF;WebAssembly;RISCV;NVPTX`) | X86 is present, which covers Phase 1 and Phase 17 on the primary platform; noted for future cross-target work. |
| 9 | Scope creep from SYNTAX §1.3 (operators, booleans) into Phase 2 | Phase 1 implements literals and `bool` typing only; operators produce the explicit `NRQ2100` diagnostic. |

---

## 12. Acceptance criteria

| ROADMAP Phase 1 requirement | Evidence produced |
|---|---|
| Repository, CMake configuration, CI baseline | `CMakeLists.txt`, `CMakePresets.json`, `ci.yml`; CI run log or an explicit statement that it has not run yet |
| Pinned C++ toolchain and MLIR/LLVM dependencies | `toolchain/versions.lock.json` plus `nirqoba version` output |
| Frontend, typed representation, MLIR dialect, lowering, backend, linker and runtime boundaries documented | `docs/architecture/01`–`06` |
| Memory, type, effect, async and execution-target models specified architecturally | `docs/architecture/07-models.md` |
| Interfaces for future tensor, graph, async, dataflow and GPU operations | `DialectRegistry` and `PassPipeline` plus `docs/architecture/08-extension-points.md` |
| Minimum lexer, parser and semantic analysis for the first program | unit tests and the `examples/hello.nrqba` integration test |
| Native code generated through MLIR/LLVM | `--emit=mlir` golden and the COFF header check on the object file |
| Compiler executable and diagnostic infrastructure | `nirqoba.exe`, `NRQxxxx` codes with `file:line:col` |
| `build`, `run`, `check`, `version` and `help` commands | CLI integration tests |
| First program runs with the correct process exit code | `examples/hello.nrqba` test plus the `return 42` test |
| Invalid source produces a diagnostic | negative test set |
| Missing files and invalid options fail clearly | exit code 3 and exit code 2 tests |
| Generated executables run without a GPU | dependency listing and execution test |
| No generated C++ program used as an intermediate backend | stage inspection, `--emit` outputs, object-file provenance, architecture review |
| `reports/phase-01.md` complete | 12-section report with executed evidence and a single verdict |

---

## 13. Definition of done

Phase 1 is complete only when: every milestone above is implemented; `ctest` and the Python suite have actually run
with their output pasted into the report; the first program compiles through MLIR/LLVM and prints the expected text;
diagnostics carry file, line, column and a stable code; the architecture documents describe the interfaces that exist
in code; every exclusion in §10 is still excluded; and `reports/phase-01.md` ends with `PASS`, `FAIL` or `BLOCKED`
justified by evidence rather than assertion.
