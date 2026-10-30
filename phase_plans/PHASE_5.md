# Phase 5 — Modules, Files, Errors, and Usable-Language Milestone

**Status:** Implementation plan  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Plan version:** 1.0  
**Scope sources:** `project_context/ROADMAP.md` §Phase 5, `project_context/SYNTAX.md` §Phase 5, `project_context/AGENTS.md`  
**Target report:** `reports/phase-05.md`  

---

## 1. Decisions this plan locks

| Decision | Choice | Rationale |
|---|---|---|
| Module declaration syntax | `module <name>;` (top of file, optional) | Explicitly names a module unit. If omitted, module name defaults to the file stem. |
| Import statements | `import <mod>;`, `import <mod>.<item>;`, `import <mod> as <alias>;` | Unambiguous module consumption supporting whole-module, selective, and aliased imports. |
| Visibility modifiers | `public` and `private` keywords preceding `function` and `struct` | `public` declarations can be imported across modules; `private` declarations are module-scoped only. Default is `public`. |
| Dependency graph & resolution | Topological dependency sort with cycle detection | Deterministic compilation order; cycles reported with `NRQ3029`; missing modules reported with `NRQ3028`. |
| Multi-file compilation | Direct CLI invocation with multiple files (`nirqoba build a.nrqba b.nrqba`) or recursive import resolution | Enables modular software development and separate compilation units. |
| File I/O builtins | `file.read_text`, `file.write_text`, `file.append_text`, `file.exists` | Core systems programming capability implemented natively with Win32 APIs without CRT stdio. |
| Standard library namespaces | `io`, `file`, `math`, `time`, `system` | Initial standard library hierarchy providing foundational OS and computational primitives. |
| Command-line arguments | `int function main(string[] args)` and `void function main(string[] args)` | Standard CLI program entry point; runtime passes arguments converted into Nirqoba dynamic array of strings. |
| Error handling foundation | Explicit runtime error reporting via fatal trap with message & code | Predictable, safe execution preventing silent failures or undefined behavior. |

---

## 2. Prerequisites and Environment Baseline

- **OS:** Windows 11 / Windows 10 x64.
- **Toolchain:** MSVC (v143+ toolset), Ninja, CMake >= 3.28.
- **LLVM / MLIR:** 23.1.4 SDK (`toolchain/llvm-23.1.4/`).
- **Python:** 3.10+ (for integration test runner).
- **Previous Phases:**
  - Phase 1: Native compilation pipeline & executable slice.
  - Phase 2: All scalar types, numeric suffixes, Pratt expressions, explicit casts.
  - Phase 3: Functions, parameters, recursion, scopes, `if`/`while`/`for`/`break`/`continue`.
  - Phase 4: Arrays, tuples, structs, indexing, destructuring, member access, console input.

---

## 3. Repository Layout and Changes

Files created or modified in Phase 5:

```text
compiler/
  include/nirqoba/
    Diagnostics/
      Diagnostic.h                          # Added: NRQ3028 (module not found), NRQ3029 (cyclic dependency), NRQ3030 (private access violation), NRQ3031 (duplicate module)
    Frontend/
      Ast.h                                 # Added: ModuleDecl, ImportDecl, Visibility enum, updated FunctionDecl & StructDecl & Program
      Parser.h                              # Added: parse_module_decl, parse_import_decl, visibility parsing
    Driver/
      ModuleResolver.h, ModuleResolver.cpp  # Added: multi-file dependency discovery, DAG builder, cycle detection, topological sorting
    Sema/
      TypeChecker.h, TypeChecker.cpp        # Added: cross-module symbol resolution, visibility checks, file/time/system builtin checking, main(string[] args)
    TIR/
      Builder.h, Builder.cpp                # Lowers multi-file modules, imports, and new builtins
    Lowering/
      TirToMlir.h, TirToMlir.cpp            # Lowers multi-module MLIR modules and runtime function calls
runtime/
  include/nirqoba/runtime/
    file.h                                  # Added: nrq_file_read_text, nrq_file_write_text, nrq_file_append_text, nrq_file_exists
    system.h                                # Added: nrq_time_now_ms, nrq_system_exit
  src/
    file_windows.cpp                        # Win32 implementation of file I/O using CreateFileA, ReadFile, WriteFile, GetFileSizeEx
    system_windows.cpp                      # Win32 implementation of system primitives using GetSystemTimeAsFileTime, ExitProcess
examples/
  multi_file/                               # Multi-module Nirqoba project example
    geometry.nrqba                          # Module with calculate_area
    main.nrqba                              # Entry point importing geometry and file I/O
reports/
  phase-05.md                               # Complete Phase 5 report
```

---

## 4. Architecture and Stage Details

### 4.1 Modules and Imports

1. **Module Declaration:**
   ```nirqoba
   module geometry;
   ```
   Stored in `Program::module_name()`. If omitted, defaults to the base name of the source file.

2. **Import Statements:**
   ```nirqoba
   import geometry;                     // Module import: access via geometry.item
   import geometry.calculate_area;      // Selective import: access via calculate_area
   import linear_algebra as la;         // Aliased import: access via la.item
   ```

3. **Visibility Modifiers:**
   ```nirqoba
   public int function calculate_area(int w, int h) { return w * h; }
   private int function internal_helper() { return 0; }
   ```
   - Functions and structs can be marked `public` or `private`. Default is `public`.
   - Accessing a `private` member from outside its declaring module produces diagnostic `NRQ3030`.

### 4.2 Multi-File Driver & Dependency Resolution

1. **`ModuleResolver`**:
   - Discovers dependencies by parsing imports across source files.
   - Builds directed dependency graph: `Module A -> Module B`.
   - Cycle detection: DFS with three-color cycle detection algorithm. Emits `NRQ3029` with dependency cycle path if detected.
   - Missing module: emits `NRQ3028` if an imported module cannot be found in search paths.
   - Computes topological sort of modules so that dependencies are compiled and type-checked before consumers.

### 4.3 Native File I/O & System Primitives

1. **Win32 File I/O (`runtime/src/file_windows.cpp`)**:
   - `nrq_file_read_text(const char* path)`: Opens file with `GENERIC_READ`, queries size via `GetFileSizeEx`, allocates buffer, reads with `ReadFile`, null-terminates, and returns pointer.
   - `nrq_file_write_text(const char* path, const char* text)`: Opens file with `CREATE_ALWAYS` / `GENERIC_WRITE`, writes text with `WriteFile`, closes handle. Returns 0 or error code.
   - `nrq_file_append_text(const char* path, const char* text)`: Opens file with `OPEN_ALWAYS`, seeks to end, writes text. Returns 0 or error code.
   - `nrq_file_exists(const char* path)`: Uses `GetFileAttributesA` returning 1 if valid and not directory, 0 otherwise.

2. **System & Time Primitives (`runtime/src/system_windows.cpp`)**:
   - `nrq_time_now_ms()`: Milliseconds since Unix epoch using `GetSystemTimeAsFileTime`.
   - `nrq_system_exit(int32_t code)`: Invokes `ExitProcess`.

### 4.4 Entry Point with Arguments

- Supports both:
  - `int function main()`
  - `int function main(string[] args)`
  - `void function main()`
  - `void function main(string[] args)`
- For `main(string[] args)`, the entry block or launcher marshals native `argc`/`argv` into a Nirqoba dynamic array of strings.

---

## 5. Milestones & Execution Strategy

- **Milestone 1:** AST, Diagnostics, and Parser for Modules, Imports, and Visibility.
- **Milestone 2:** Multi-File Dependency Resolver, Topological Sorter & Cross-Module Sema.
- **Milestone 3:** Native Windows File I/O, System & Time Runtime Functions, and Standard Library Builtins.
- **Milestone 4:** Command-Line Arguments Marshaling (`main(string[] args)`) & Lowering.
- **Milestone 5:** CLI Multi-File Project Workflows, Project Templates & Examples.
- **Milestone 6:** Unit & Integration Tests.
- **Milestone 7:** Documentation & Phase 5 Usable-Language Report (`reports/phase-05.md`).
