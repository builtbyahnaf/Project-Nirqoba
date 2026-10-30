# Project Nirqoba

<p align="center">
  <strong>A modern programming language built for computation, creativity, and control.</strong>
</p>

<p align="center">
  <em>One language. Multiple domains. No unnecessary compromises.</em>
</p>

---

## Overview

**Nirqoba** is an ambitious, statically typed, high-performance programming language built by Muhammad Tahmid Ahnaf. It is designed to bring general-purpose programming, artificial intelligence, numerical computing, graphics, and systems development into one coherent ecosystem.

The project aims to combine the readability and developer-friendliness of modern high-level languages with the performance, memory control, and native interoperability expected from systems languages.

Rather than treating AI/ML and scientific computing as external additions, Nirqoba is being designed with these workloads in mind from its foundational architecture.

The long-term goal is simple:

**Build a language that is comfortable for everyday programming without sacrificing the capabilities required for demanding computational workloads.**

## Vision

Nirqoba is being developed around five fundamental principles:

- **Readability:** Expressive syntax that is easy to understand without sacrificing precision.
- **Performance:** Native compilation and an architecture designed for optimization.
- **Control:** Flexible memory management, explicit execution policies, and low-level interoperability.
- **Extensibility:** A foundation capable of supporting advanced numerical, graphical, asynchronous, and GPU-oriented features.
- **Consistency:** A unified language experience rather than disconnected collections of specialized features.

## Key Design Goals

| Area | Direction |
|---|---|
| Language design | Statically typed, modern, expressive |
| Compiler infrastructure | MLIR and LLVM |
| Compilation | Native machine-code generation |
| Primary platform | Windows x64 initially |
| Additional platform | Linux |
| Memory management | Automatic GC by default, with precise alternatives |
| Concurrency | Async/await, coroutines, structured concurrency |
| Interoperability | C, C++, and CUDA |
| Numerical computing | Native tensor and advanced numeric abstractions |
| AI/ML | Computational graphs, automatic differentiation, data pipelines |
| Graphics | Native graphics ecosystem integration |
| GPU execution | Explicit developer-controlled execution |

## What Makes Nirqoba Different?

### 1. Designed for more than general-purpose programming

Most languages have a primary domain and rely on libraries or external frameworks to expand beyond it.

Nirqoba aims to make several domains first-class architectural considerations:

- Artificial intelligence and machine learning.
- Scientific and numerical computing.
- Large-scale data processing.
- Graphics and visual applications.
- General-purpose software development.
- Systems and low-level programming.

### 2. Readable without being restrictive

Nirqoba uses a familiar programming structure with explicit types, braces, semicolons, and readable declarations.

Example:

```nirqoba
f64 function calculate_mean(f64[] values) {
    if (values.length == 0) {
        return 0.0;
    }

    let total = values.reduce((a, b) => a + b);

    return total / values.length;
}
```

The intended experience is readable, structured, and predictable.

### 3. Automatic memory management with developer control

Nirqoba uses garbage collection by default for ordinary managed objects.

However, developers can choose more precise mechanisms when required:

- Deterministic ownership.
- Borrowing and scoped references.
- Explicit dynamic allocation.
- Custom allocator strategies.
- Memory mapping and pinning.
- Optional memory profiling and diagnostics.

The objective is to combine developer convenience with low-level control.

### 4. Explicit GPU execution

Nirqoba follows a CPU-first execution model.

GPU availability does not automatically mean GPU execution.

Developers must explicitly request GPU operations, select devices, and manage relevant memory transfers.

This provides predictable execution behavior and avoids unexpected hardware dependencies.

### 5. Native ecosystem interoperability

Nirqoba is not intended to reinvent every existing library.

Its long-term interoperability goals include:

- C libraries and APIs.
- C++ libraries and APIs.
- CUDA infrastructure.
- Native graphics libraries.
- Existing numerical and scientific computing ecosystems.

## Architecture

The compiler is intended to use a multi-stage architecture built around MLIR and LLVM.

Conceptual compilation pipeline:

```text
Nirqoba Source Code
        |
        v
      Lexer
        |
        v
      Parser
        |
        v
       AST
        |
        v
Semantic and Type Analysis
        |
        v
Typed Nirqoba Representation
        |
        v
   Nirqoba MLIR Dialects
        |
        v
Optimization and Lowering
        |
        v
       LLVM
        |
        v
 Native Machine Code
        |
        v
 Runtime and Standard Library
```

The compiler architecture must preserve high-level information long enough to support advanced features such as tensors, computational graphs, asynchronous execution, memory management, and GPU operations.

MLIR and LLVM serve as compiler infrastructure. They do not define Nirqoba's language semantics.

## Development Roadmap

Nirqoba is being developed through 25 major phases, preceded by a dedicated Windows distribution milestone.

| Milestone | Focus |
|---|---|
| Phase 1 | Compiler architecture and executable vertical slice |
| Phase 2 | Primitive types and expressions |
| Phase 3 | Functions and control flow |
| Phase 4 | Collections, structured data, and I/O |
| Phase 5 | Modules, errors, and usable general-purpose language |
| Phase 6 | Stabilization and regression hardening |
| Phase 6.5 | Default garbage collection and memory management |
| Phase 6.7 | Explicit allocation and selectable allocators |
| Phase 6.9 | Advanced memory utilities |
| Pre-Phase 7 | Windows compiler, installer, and PATH integration |
| Phase 7–9 | Algebraic types, generics, ownership, and low-level memory |
| Phase 10–12 | Async, coroutines, structured concurrency, and streams |
| Phase 13–15 | C/C++ interoperability and graphics ecosystem |
| Phase 16–18 | Optimization, SIMD, and advanced numeric types |
| Phase 19–22 | Tensors, computational graphs, autodiff, and data analysis |
| Phase 23–24 | GPU runtime and native GPU kernels |
| Phase 25 | Production tooling and ecosystem |

The first five phases focus on building a usable general-purpose language before introducing the more ambitious domain-specific capabilities.

## Project Structure

The repository is expected to evolve into a structure similar to:

```text
Project-Nirqoba/
│
├── README.md
├── ABOUT.md
├── LICENSE
├── CONTRIBUTING.md
│
├── docs/
│   ├── language/
│   ├── compiler/
│   ├── runtime/
│   ├── memory/
│   ├── standard-library/
│   ├── tutorials/
│   ├── how-to/
│   ├── reference/
│   ├── architecture/
│   └── installation/
│
├── reports/
│   ├── phase-01.md
│   ├── phase-02.md
│   └── ...
│
├── compiler/
├── runtime/
├── standard-library/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── regression/
│   ├── conformance/
│   └── examples/
│
├── tools/
│
├── installer/
│
├── examples/
│
└── scripts/
```

This is a proposed organization. Actual directories should be created as their corresponding components are implemented.

## Getting Started

Nirqoba is under development.

Installation instructions and executable releases will be published when the corresponding compiler and distribution milestones are complete.

Until then, the repository should be treated as a language and compiler development project rather than a production-ready programming environment.

Once a usable compiler is available, the intended workflow will look like:

```powershell
nirqoba --version

nirqoba check main.nrqba

nirqoba build main.nrqba

nirqoba run main.nrqba
```

## Documentation

The project documentation will cover:

- Language syntax and semantics.
- Installation and environment setup.
- Compiler architecture.
- Runtime architecture.
- Memory management.
- Standard-library usage.
- Practical programming tutorials.
- Advanced numerical and GPU programming.
- Troubleshooting and optimization.

Beginning with the usable-language milestone, documentation will include practical instructions, examples, usage patterns, and tips—not just API descriptions.

## Development Philosophy

Nirqoba is being developed incrementally, but its architecture is planned with the long-term language in mind.

A feature should not require a fundamental redesign of the compiler merely because it introduces a more advanced capability.

Every development phase must include:

- Defined implementation scope.
- Architecture and specification updates.
- Relevant automated tests.
- Regression testing.
- Technical documentation.
- A detailed implementation report.

Features must be tested before they are declared complete.

## Contributing

Contribution guidelines will be published as the project structure and development workflow mature.

Until then, all development should follow the repository's `AGENTS.md` instructions and the applicable phase specification.

## License

License information will be specified in the repository's `LICENSE` file.

---

<p align="center">
  <strong>Nirqoba</strong>
  <br>
  <em>Designed to compute. Built to evolve.</em>
</p>
