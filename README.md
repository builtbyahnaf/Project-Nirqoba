# Project Nirqoba

<p align="center">
  <strong>A modern programming language built for computation, creativity, and control.</strong>
</p>

<p align="center">
  <em>One language. Multiple domains. No unnecessary compromises.</em>
</p>

---

## Overview

**Nirqoba** is an ambitious, statically typed, high-performance systems and scientific programming language built by Muhammad Tahmid Ahnaf (@builtbyahnaf). It is designed to unify general-purpose software development, numerical computing, artificial intelligence, graphics, and systems programming within a single coherent compiler and runtime ecosystem.

Rather than forcing developers to compromise between productive high-level syntax and low-level mechanical sympathy, Nirqoba pairs a clear, structured syntax with native machine code generation via MLIR and LLVM. It provides a unique hybrid memory model: **deterministic ownership and borrowing by default**, coupled with an **opt-in, generational, incremental garbage collector** for complex cyclic graphs and **explicit, configurable allocators** for systems-critical paths.

The long-term goal is simple:

**Build a language that is comfortable for everyday programming without sacrificing the capabilities required for demanding computational workloads.**

## Vision

Nirqoba is developed around five fundamental principles:

- **Readability:** Expressive, predictable syntax featuring explicit types, statement semicolons, lexical scopes, and local type inference without indentation-sensitive pitfalls.
- **Performance:** Native compilation via MLIR dialects and LLVM PassBuilder optimization pipelines (`-O0` through `-O3`, `-Os`, `-Oz`), hardware-targeted SIMD vectorization, fast-math reduction synthesis, and escape analysis with stack promotion.
- **Control:** Deterministic ownership by default, compile-time borrow checking with mutable view exclusivity, selectable custom allocators (arena, pool, slab, stack, region), memory pinning/mapping, and an opt-in tracing GC.
- **Extensibility:** Architectural foundations designed to support advanced numerical abstractions, computational graphs, tensors, asynchronous concurrency, and explicit GPU execution.
- **Consistency:** A unified language architecture where high-level operations lower into transparent, zero-cost abstractions rather than disconnected feature silos.

## Current Architectural Reality & Key Design Goals

| Area | Current Status / Architectural Direction |
|---|---|
| **Language Design** | Statically typed, structured syntax, local type inference (`let` / `var`), records/structs, fixed arrays |
| **Compiler Infrastructure** | Multi-stage pipeline: Lexer -> Parser -> AST -> TypeChecker (Sema) -> TIR -> MLIR -> LLVM IR |
| **Optimization Backend** | LLVM PassBuilder pipeline (`-O0` to `-O3`, `-Os`, `-Oz`, `--ipo=standard\|aggressive\|minimal`), MLIR canonicalization, CSE, SCCP, DCE/DSE |
| **Memory Model** | Hybrid: **Ownership & borrowing by default**; opt-in generational & incremental GC; explicit custom allocators |
| **Escape Analysis** | Interprocedural pointer & array escape analysis; automatic stack promotion (`alloca_at_entry`) with 16-byte SIMD alignment floors |
| **Vectorization & SIMD** | Loop vectorization, aligned SIMD codegen (`movaps`), opt-in `--fast-math` reductions (`llvm.vector.reduce.fadd`), and `--remarks` diagnostics |
| **Target Architecture** | Windows x64 MSVC primary; portable/native/explicit target profiles (`--target-profile`, `--target`, `--target-cpu`, `--target-features`, `--native`) |
| **Runtime ABI** | Lightweight native runtime with direct Win32 I/O, TLABs, safepoints, shadow stack with liveness-based root registration |
| **Concurrency (Planned)** | Structured concurrency, coroutines, low-overhead thread scheduler, thread-safe memory models |
| **Numerical & AI (Planned)** | First-class tensors, computational graphs, automatic differentiation, reduced precision formats (FP16, BF16, FP8) |
| **GPU Execution (Planned)** | Explicit CPU-first execution model: developer-managed device selection, memory transfers, and kernel dispatch |

## What Makes Nirqoba Different?

### 1. Hybrid Memory Architecture: Ownership by Default, Tracing GC by Choice

In contrast to pure garbage-collected languages or strictly borrow-checked systems, Nirqoba provides a tiered, developer-controlled memory model:

1. **Deterministic Ownership & Borrowing:** Uniquely owned arrays and allocations (`memory.owned_array<T>(len)`) transfer explicitly on move and deallocate immediately upon leaving lexical scope. Borrowed views (`borrow array_view<T>` and `borrow mut source array_view<T>`) enforce borrow semantics with zero runtime overhead.
2. **Opt-in Generational & Incremental GC:** Automatic garbage collection starts disabled by default and can be engaged explicitly for dynamic object graphs. The runtime collector features young and old generations, thread-local allocation buffers (TLABs), remembered sets, write barriers, and sliced incremental marking/sweeping overlapping mutator execution.
3. **Liveness-Aware Shadow Roots:** The compiler performs backward liveness analysis over managed references so GC root frames and write barriers are emitted exclusively across safepoints where values are live.
4. **Pluggable Explicit Allocators:** Applications can construct and bind custom allocators per-thread or per-operation (`general`, `arena`, `pool`, `slab`, `stack`, `region`, `buddy`, `tlsf`) with fine-grained alignment and profiling options.

### 2. Predictable, High-Performance Code Generation

Nirqoba compiles directly to machine code through LLVM and MLIR:
- **Escape Analysis & Stack Promotion:** Local owned arrays and heap allocations that do not escape their scope are automatically promoted to stack slots in the function entry block, completely eliminating dynamic allocation traffic.
- **SIMD Vectorization & Optimization Remarks:** Loops over contiguous memory are vectorized with aligned 16-byte stack floors. Developers can observe compiler optimization decisions in real time via `--remarks` or `--remarks=missed`.
- **Accurate Function Attributes:** The compiler infers whole-module memory effects (`llvm.memory_effects`) and derives `nocapture` and `readonly` attributes from escape fixpoints, enabling LLVM to aggressively hoist loads, eliminate dead stores, and optimize calls.

### 3. Readable Syntax Without Hidden Costs

Nirqoba uses explicit, brace-delimited declarations with clear control flow and typing:

```nirqoba
// Contiguous owned array processed via borrowed views and loop vectorization
int function sum_positive(borrow array_view<int> slice) {
    var total = 0;
    var i = 0;
    while (i < slice.length) {
        if (slice[i] > 0) {
            total += slice[i];
        }
        i += 1;
    }
    return total;
}

int function main() {
    var numbers = memory.owned_array<int>(64);
    var i = 0;
    while (i < 64) {
        numbers[i] = i - 10;
        i += 1;
    }

    let view = memory.array_view<int>(numbers, 0, 64);
    let result = sum_positive(view);
    print(result);
    return 0;
}
```

### 4. Explicit Systems & Heterogeneous Computing Philosophy

Nirqoba follows a predictable, CPU-first model:
- No hidden background runtime threads unless explicitly created.
- GPU availability does not automatically trigger GPU execution—device selection, buffer transfers, and kernel launches are developer-directed.
- Direct native interoperability with C/C++ ABIs, DirectX/DXGI device estimation, and memory-mapped file handles without heavy foreign function interfaces.

---

## Compiler Architecture

Nirqoba uses a multi-tier compilation pipeline designed to preserve semantic and ownership information through lowering:

```text
       Nirqoba Source (.nrqba)
                  │
                  ▼
          Lexer & Parser
                  │
                  ▼
             AST & CST
                  │
                  ▼
       Semantic Analysis & Types (Sema)
  (Ownership validation, borrow checking, types)
                  │
                  ▼
      Typed Intermediate Representation (TIR)
  (Liveness analysis, escape analysis, root summaries)
                  │
                  ▼
             MLIR Dialects
  (Canonicalization, CSE, SCCP, bounds guards, cleanup)
                  │
                  ▼
                LLVM IR
  (Function attributes, memory effects, PassBuilder O0-O3)
                  │
                  ▼
         Native Machine Code (.obj / .exe)
                  │
                  ▼
          Nirqoba Native Runtime
  (TLABs, generational GC, custom allocators, Win32 I/O)
```

---

## Development Status & Roadmap

The language is actively progressing through its architectural milestones. Current active version: **`0.127-beta`** (Phase 6 / Compiler Optimization and Hardening).

| Phase | Description | Status |
|---|---|---|
| **Phase 1** | Compiler architecture and executable vertical slice | Completed |
| **Phase 2** | Primitive types, expressions, and operators | Completed |
| **Phase 3** | Functions, lexical scoping, and control flow | Completed |
| **Phase 4** | Collections, records/structs, and direct Win32 I/O | Completed |
| **Phase 5** | Modules, multi-file builds, and error reporting | Completed |
| **Phase 6** | Allocator-aware owned arrays, mutable views, and stabilization | Completed |
| **PO 3.1–3.6** | PassBuilder pipeline, alias analysis, escape analysis, root liveness, SIMD vectorization | Completed |
| **Phase 6.5–6.9** | Generational incremental GC, explicit allocators, memory profiling | Completed |
| **Phase 7–9** | Generics, algebraic data types, advanced pattern matching | Planned |
| **Phase 10–12** | Structured concurrency, async/await, task scheduling | Planned |
| **Phase 13–15** | C/C++ FFI interoperability and graphics ecosystem | Planned |
| **Phase 16–18** | Advanced numeric representations (FP16, BF16, FP8), matrix primitives | Planned |
| **Phase 19–22** | First-class tensors, computational graphs, autodiff | Planned |
| **Phase 23–24** | GPU runtime and compute kernels | Planned |
| **Phase 25** | Production distribution tooling and ecosystem | Planned |

---

## Repository Structure

```text
Project-Nirqoba/
├── CMakeLists.txt        # Root build configuration
├── compiler/             # Frontend, AST, Sema, TIR, MLIR lowering, LLVM codegen, Driver
│   ├── include/nirqoba/  # Public compiler headers
│   └── lib/              # AST, Lexer, Parser, Sema, TIR, Lowering, Backend, CLI
├── runtime/              # Native runtime library
│   ├── include/          # Public runtime interfaces (gc.h, allocator.h, version.h)
│   └── src/              # Windows I/O, allocators, generational GC, thread buffers
├── tests/                # Test suites
│   ├── unit/             # C++ GoogleTest unit test suite
│   ├── integration/      # End-to-end positive, negative, and MLIR expectation tests
│   └── readiness/        # Language readiness audit programs and benchmarks
├── benchmarks/           # Standardized performance and stress test suites
├── docs/                 # Architecture, language specifications, and VERSION_LOG.md
├── reports/              # Phase verification and performance engineering reports
└── scripts/              # Bootstrap, build (build.ps1), and test (test.ps1) automation
```

---

## Getting Started

### Prerequisites (Windows x64)

- **Visual Studio 2022** (MSVC C++20 toolchain)
- **CMake 3.28+** and **Ninja**
- **LLVM & MLIR 23.1.4 SDK** (prebuilt tarball or local SDK)
- **Python 3.10+** (for test running and verification)

### Building and Testing

Run the automated PowerShell build and test scripts from the repository root:

```powershell
# Bootstrap toolchain configuration (first time)
.\scripts\bootstrap.ps1

# Build compiler and runtime in RelWithDebInfo
.\scripts\build.ps1 -Configuration RelWithDebInfo

# Run unit tests and integration test suite
.\scripts\test.ps1 -Configuration RelWithDebInfo
```

### Compiler CLI Usage

```powershell
# Show version and target configuration
nirqoba --version

# Compile and run a source file directly
nirqoba run main.nrqba

# Compile to a native executable with O3 optimizations and vector remarks
nirqoba build main.nrqba -O3 --remarks=loop-vectorize -o program.exe

# Tune for this machine (do not ship the result as a universal binary)
nirqoba build main.nrqba -O2 --native -o program.exe

# Target a known deployment CPU or triple instead
nirqoba build main.nrqba -O2 --target-profile=explicit --target-cpu=x86-64-v3 -o program.exe
nirqoba build main.nrqba -O2 --target-profile=explicit --target x86_64-unknown-linux-gnu --emit=obj

# Enable fast-math for floating-point loop reductions
nirqoba build main.nrqba -O2 --fast-math -o program.exe

# Tune whole-program interprocedural optimization
nirqoba build main.nrqba -O2 --ipo=standard -o program.exe    # default
nirqoba build main.nrqba -O2 --ipo=aggressive -o program.exe  # deeper analysis
nirqoba build main.nrqba -O2 --ipo=minimal -o program.exe     # bounded compile

# Profile-guided optimization: instrument, run, merge, then rebuild
nirqoba build main.nrqba -O2 --pgo-generate -o app_instrumented.exe
.\app_instrumented.exe            # writes default.profraw in the current directory
llvm-profdata merge -o app.profdata default.profraw
nirqoba build main.nrqba -O2 --pgo-use=app.profdata -o program.exe

# Consume a sample profile captured by an external profiler
nirqoba build main.nrqba -O2 --pgo-sample=app.sample -o program.exe

# Inspect lowered LLVM IR or MLIR representations
nirqoba build main.nrqba --emit=llvm
nirqoba build main.nrqba --emit=mlir
```

Target profiles are resolved once before optimization and recorded as
`nrqoba.target.*` module flags in `--emit=llvm` output:

- `portable` (default): the published x86-64 baseline (`generic` CPU), safe to
  distribute to any compatible machine.
- `native` (`--native` or `--target-cpu=native`): the host triple, CPU model,
  and detected feature set; tuned for this machine only.
- `explicit`: the CPU, features, and triple you name, for a known deployment
  target. Unknown CPUs or features are rejected instead of silently ignored.

### Interprocedural Optimization

Nirqoba resolves every `import` before lowering and produces one LLVM module,
so the backend always optimizes the complete program. That merged module is the
compiler's equivalent of a scalable whole-program LTO unit: cross-module
inlining, interprocedural constant propagation, function specialization, and
global dead-code elimination all see every source module without a separate
link-time step.

`--ipo` controls how much work the backend spends on that unit:

- `standard` (default): LLVM's normal level-dependent inlining and analysis.
- `aggressive`: raises the inliner budget, enables speculative
  devirtualization, and runs LLVM's Attributor after inlining for deeper
  capture, alias, and memory-effect facts. Trades compile time and code size
  for analysis depth.
- `minimal`: disables inlining to bound compile time and code growth on very
  large programs. IPSCCP, global optimization, and global DCE still run.

Every optimized module records its policy and strategy as `nrqoba.ipo.mode` and
`nrqoba.ipo.strategy` module flags, visible in `--emit=llvm` output. `--verbose`
prints the active policy before optimization.

### Profile-Guided Optimization

Static heuristics estimate which functions, branches, and loops matter. A
profile replaces those estimates with measurements from a representative run.
Nirqoba supports both LLVM approaches:

- **Instrumentation-based:** build with `--pgo-generate`, run the program, merge
  the raw counters with `llvm-profdata merge`, then rebuild with
  `--pgo-use=<profile.profdata>`. Instrumented executables link the LLVM profile
  runtime from the packaged clang toolchain and write `default.profraw` when
  they exit.
- **Sample-based:** capture samples with an external profiler (for example ETW,
  `perf`, or Instruments) in LLVM sample-profile format, then rebuild with
  `--pgo-sample=<profile>`.

The loaded profile informs function entry counts and hot/cold identification,
branch probabilities, inlining, loop trip counts, code layout, function
specialization, and hot/cold path separation. Ordinary transformations remain
LLVM's; Nirqoba supplies the policy, validates the input, and records the mode.

`--pgo-cold=<mode>` controls how functions the profile marks cold are optimized:
`default` (LLVM's normal handling), `optsize`, `minsize`, or `optnone`. Every
profile-guided module records `nrqoba.pgo.mode` and `nrqoba.pgo.cold` module
flags in `--emit=llvm` output; the profile path itself is not embedded. A
missing, empty, or directory profile is rejected with `NRQ5009` instead of
silently producing an unprofiled build. Instrumented builds are slower and
larger by design, and the profile is only as representative as the workload used
to produce it.

---

## License & Ownership

- **Author & Technical Authority:** Muhammad Tahmid Ahnaf ([@builtbyahnaf](https://github.com/builtbyahnaf))
- **License:** See [LICENSE.md](LICENSE.md) for licensing terms.

---

<p align="center">
  <strong>Nirqoba</strong><br>
  <em>Designed to compute. Built to evolve.</em>
</p>
