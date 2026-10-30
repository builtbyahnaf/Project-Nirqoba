# ABOUT NIRQOBA

## 1. Introduction

Nirqoba is a long-term programming language and compiler engineering project built by Muhammad Tahmid Ahnaf, focusing on developing a modern, statically typed, high-performance language capable of serving multiple computational domains through a unified programming model.

The project originates from a simple observation:

Programming languages often require developers to make compromises between readability, performance, speed, flexibility, and specialized computational capabilities.

High-level languages can make development productive but may require extensive external infrastructure for demanding performance-sensitive workloads.

Systems languages provide extensive control but can introduce complexity that makes everyday development more demanding.

Specialized scientific and AI frameworks offer powerful abstractions but frequently depend on separate programming models, execution environments, and interoperability layers.

Nirqoba aims to explore a different direction: a coherent language architecture that can support these requirements without treating them as unrelated extensions.

It is not intended to be merely another syntax layered over an existing language.

It is intended to become an independent language with its own semantics, compiler infrastructure, runtime, standard library, and ecosystem.

---

## 2. The Core Vision

The long-term vision is to develop a language that can comfortably express:

- Ordinary applications and utilities.
- Complex algorithms.
- Scientific simulations.
- Numerical computations.
- AI and machine-learning workloads.
- Large-scale data processing.
- Graphics and visualization.
- Systems-level operations.
- Native library integrations.
- CPU and GPU computations.

These capabilities should share a common language foundation.

A developer should not need to learn an entirely different programming language merely because their application moves from ordinary computation to numerical algorithms or heterogeneous execution.

The objective is not to eliminate specialized libraries or frameworks.

Instead, Nirqoba should provide the language-level abstractions, execution infrastructure, and interoperability necessary to use those ecosystems naturally.

---

## 3. Design Philosophy

### 3.1 Readability without unnecessary restrictions

Nirqoba is intended to be readable and approachable without abandoning the precision expected from a statically typed language.

The syntax uses:

- Explicit function declarations.
- Static type information.
- Local type inference.
- Braces for scopes.
- Semicolons for statement termination.
- Structured control flow.
- Composable expressions.

The language avoids relying on indentation as a structural requirement.

The intention is to keep source code understandable while supporting complex systems and computational logic.

### 3.2 Static typing

Static typing is a foundational design decision.

The compiler should detect type-related errors before execution whenever possible.

Type inference is supported to reduce unnecessary repetition, but it does not turn Nirqoba into a dynamically typed language.

The type system must eventually accommodate:

- Primitive values.
- Fixed and dynamic collections.
- Structured records.
- Generic abstractions.
- Algebraic data types.
- Ownership and borrowing.
- Tensors.
- Computational graphs.
- Advanced numeric representations.

The type system should remain coherent as these capabilities expand.

### 3.3 Performance as an architectural requirement

Performance is not intended to be a final optimization phase added after the language becomes usable.

The compiler architecture is planned around MLIR and LLVM from the beginning.

This allows the project to preserve higher-level information through intermediate representations and perform appropriate lowering and optimization.

The compiler should be capable of supporting:

- Native machine-code generation.
- Multiple optimization stages.
- CPU-specific optimization.
- SIMD.
- Numerical computation.
- GPU execution.
- Interoperability with native libraries.

Performance claims must be supported by actual benchmarks rather than assumptions.

### 3.4 Developer control

Convenience should not eliminate control.

Nirqoba should provide reasonable defaults while allowing developers to choose more precise behavior when their applications require it.

Examples include:

- Automatic garbage collection by default.
- Deterministic ownership for controlled lifetimes.
- Borrowing for safe temporary access.
- Explicit memory allocation.
- Custom allocator selection.
- Explicit GPU execution.
- Explicit native interoperability boundaries.

The developer should understand what the language and runtime are doing.

---

## 4. Memory Management Philosophy

Memory management is one of Nirqoba's most important architectural considerations.

The language intends to combine automatic memory management with precise control.

### 4.1 Garbage collection by default

Ordinary dynamically allocated managed objects should be automatically reclaimed when they become unreachable.

This reduces the burden of manually managing object lifetimes and helps prevent common memory-management errors.

The initial collector is intended to use precise tracing and mark-and-sweep collection, with architectural provisions for future improvements.

Potential future collector capabilities include:

- Generational collection.
- Incremental collection.
- Concurrent marking.
- Write barriers.
- Thread-aware allocation.
- Precise root tracking.

### 4.2 Deterministic resource management

Garbage collection is not appropriate as the sole cleanup mechanism for every resource.

Files, sockets, locks, GPU allocations, and native handles require predictable cleanup.

Nirqoba therefore intends to support deterministic destruction and explicit ownership.

### 4.3 Explicit memory management

Developers should be able to choose allocation strategies suitable for their workloads.

Potential strategies include:

- General-purpose allocation.
- Pool allocation.
- Arena allocation.
- Stack allocation.
- Slab allocation.
- Buddy allocation.
- Free-list allocation.
- TLSF allocation.

No allocator should be presented as universally optimal.

### 4.4 Diagnostics and profiling

Optional runtime diagnostics should provide insight into memory behavior.

These may include:

- Allocation tracking.
- Peak memory usage.
- GC statistics.
- Reference-count diagnostics.
- Memory profiling.
- Fragmentation estimates.
- Memory mapping and pinning information.

Instrumentation should remain optional where it introduces runtime overhead.

---

## 5. Computational and AI/ML Direction

AI and machine learning are primary design considerations rather than secondary use cases.

The language intends to develop native abstractions for numerical and computational workloads.

Potential capabilities include:

### 5.1 Advanced numerical representations

Nirqoba plans to support numeric representations beyond conventional integer and floating-point types.

These include:

- Standard floating-point types.
- Half precision.
- Brain floating point.
- Reduced-precision FP8 and FP4 formats.
- Packed low-bit integer representations.
- Vectorized numerical values.
- Potential ternary representations.

Exact numerical formats and semantics must be specified rather than relying on ambiguous type names.

### 5.2 Tensors

Tensors should become first-class computational abstractions.

The type system and compiler should eventually support:

- Tensor shapes.
- Multiple dimensions.
- Numerical operations.
- Broadcasting.
- Matrix multiplication.
- CPU optimization.
- GPU interoperability.

### 5.3 Computational graphs

Nirqoba intends to provide typed computational graph abstractions.

The initial concept of `Node<T>` represents a computational graph node containing or describing a typed value or operation and its dependencies.

This is distinct from ordinary variables and runtime objects.

### 5.4 Automatic differentiation

The language aims to support automatic differentiation through graph-aware representations and compiler/runtime infrastructure.

Potential capabilities include:

- Forward-mode differentiation.
- Reverse-mode differentiation.
- Gradient computation.
- Graph optimization.
- Differentiable functions.
- Gradient-control regions.

### 5.5 Semantic representations

The project also explores higher-level semantic abstractions.

Proposed concepts include:

- `SemanticVector<T, N>` for fixed-dimensional semantic representations.
- `ConceptNode<T, N>` for domain concepts associated with semantic representations and graph relationships.

These concepts are exploratory until their exact semantics and use cases are formally established.

---

## 6. Data Processing Philosophy

Nirqoba aims to make data transformation expressive without forcing developers to abandon conventional programming constructs.

The language should support:

- Conventional loops.
- Collection transformations.
- Filtering.
- Mapping.
- Reduction.
- Lazy evaluation.
- Eager evaluation.
- Parallel data processing.
- Asynchronous streams.
- Dataflow-oriented operations.

A possible future iteration construct, `flow`, is being considered alongside ordinary `for` and `while` loops.

The language must distinguish clearly between ordinary iteration, lazy pipelines, and asynchronous streams.

The intention is to support both straightforward procedural algorithms and composable data-processing workflows.

---

## 7. Concurrency and Asynchronous Programming

Modern applications often need to coordinate independent tasks, asynchronous operations, and shared resources.

Nirqoba intends to provide structured concurrency rather than relying entirely on unmanaged background threads.

Planned capabilities include:

- Async functions.
- Await expressions.
- Coroutine lowering.
- Task abstractions.
- Task cancellation.
- Structured task scopes.
- Error propagation.
- Channels.
- Asynchronous streams.
- Backpressure.
- Synchronization primitives.

Synchronous code should remain synchronous unless asynchronous behavior is explicitly requested or required by a documented API.

The concurrency architecture must integrate correctly with memory management, runtime scheduling, and native interoperability.

---

## 8. CPU and GPU Execution

Nirqoba follows a CPU-first execution philosophy.

Ordinary programs should work without requiring a GPU.

GPU execution must be explicitly requested by the developer.

The language intends to support:

- GPU availability checks.
- Device selection.
- Explicit memory transfers.
- CUDA interoperability.
- Native GPU kernel declarations.
- GPU synchronization.
- Heterogeneous CPU/GPU workloads.

The presence of a GPU must never silently change the execution model of a program.

Developers should understand where computation runs and how memory moves between devices.

---

## 9. Native Interoperability

Nirqoba should benefit from existing native software ecosystems instead of unnecessarily recreating their functionality.

The project intends to support:

### C interoperability

- Foreign function declarations.
- C-compatible structures.
- C ABI exports.
- Native library linking.
- Ownership and lifetime rules.

### C++ interoperability

- Header integration.
- Native classes and methods.
- Supported template integration.
- ABI-aware bindings.
- Interoperation with established C++ libraries.

### CUDA interoperability

- CUDA runtime integration.
- Device memory.
- Native kernels.
- Execution and synchronization.

Interoperability must be treated as an explicit architectural boundary with well-defined type, ownership, and error semantics.

---

## 10. Compiler Architecture

Nirqoba is designed as an independent language with a compiler built using C++, MLIR, and LLVM.

The compiler is expected to contain distinct stages for:

1. Lexical analysis.
2. Parsing.
3. Abstract syntax tree construction.
4. Name and symbol resolution.
5. Type and semantic analysis.
6. Typed intermediate representations.
7. Nirqoba-specific MLIR dialects.
8. Optimization.
9. Lowering.
10. LLVM code generation.
11. Linking and runtime integration.

The architecture must preserve important language-level information until the relevant compiler stage can process it correctly.

For example:

- Tensor operations should remain recognizable as tensor operations until suitable lowering.
- Computational graph operations should preserve graph semantics.
- Async operations should preserve suspension and task semantics.
- GPU operations should preserve device and execution information.
- Memory-management operations should preserve ownership and allocation information.

The project must not rely on temporary compilation strategies that undermine the intended long-term architecture.

---

## 11. Development Strategy

Nirqoba is being developed through incremental milestones.

The first five phases establish a usable general-purpose language.

The sixth phase focuses on stability and regression hardening.

Subsequent phases introduce memory management enhancements, installation tooling, advanced type-system capabilities, concurrency, interoperability, numerical abstractions, and GPU support.

This approach has two purposes:

1. Establish a dependable foundation before expanding the language.
2. Avoid introducing too many interacting features before their underlying systems are ready.

Every major feature should have a clearly defined implementation scope and should be completed within a small number of focused phases whenever practical.

---

## 12. Quality and Reliability

A compiler is infrastructure. Reliability matters.

The project intends to maintain:

- Automated unit tests.
- Integration tests.
- Regression tests.
- Negative tests.
- Language conformance tests.
- Platform-specific validation.
- Reproducible builds.
- Performance benchmarks where relevant.

Every phase must produce a detailed technical report.

Reports must describe what was actually implemented, how it was tested, and which requirements remain incomplete.

Tests must never be reported as passing without execution evidence.

---

## 13. Documentation Philosophy

Documentation is part of the implementation, not an afterthought.

The project intends to maintain two complementary documentation layers.

### Developer and contributor documentation

Explains:

- Compiler internals.
- Intermediate representations.
- Runtime architecture.
- Algorithms.
- Memory management.
- Build procedures.
- Testing procedures.
- Architectural decisions.

### Language user documentation

Explains:

- Installation.
- Syntax.
- Language fundamentals.
- Practical examples.
- How-to guides.
- Standard-library usage.
- Performance tips.
- Common mistakes.
- Advanced features.

Beginning with the end of Phase 5, user-facing documentation becomes a mandatory deliverable and must be maintained with every subsequent phase.

The ultimate objective is that developers can learn and use Nirqoba without having to understand its compiler implementation.

---

## 14. Platform Strategy

The initial development and distribution target is Windows x64.

The project includes a dedicated Windows compiler and installation milestone.

This milestone aims to provide:

- A working compiler.
- A compatible runtime.
- A Windows installer.
- PATH integration.
- Upgrade and uninstall support.
- Installation diagnostics.
- A usable command-line workflow.

Linux support follows the initial Windows distribution milestone.

Cross-platform architecture should be considered from the beginning even when a particular platform is not yet supported.

---

## 15. What Nirqoba Is Not

It is useful to establish what the project is not trying to become.

Nirqoba is not intended to be:

- A thin wrapper around Python.
- A generated-C++ transpiler.
- A language whose semantics are defined by LLVM.
- A language that silently moves computation to a GPU.
- A language that forces manual memory management for ordinary objects.
- A language that sacrifices all low-level control for automatic memory management.
- A language that introduces advanced features without a coherent underlying architecture.
- A collection of unrelated syntax features assembled without a consistent design.

It is intended to be an independent, extensible language with a clear computational identity.

---

## 16. Long-Term Objective

The long-term objective is to develop a mature programming environment consisting of:

- The Nirqoba programming language.
- A native compiler.
- A runtime system.
- A standard library.
- A package and build ecosystem.
- Development tools.
- Documentation.
- Testing and benchmarking infrastructure.
- Native interoperability.
- CPU and GPU execution capabilities.

The project should eventually allow developers to build ordinary software, computational applications, and performance-sensitive systems using the same language foundation.

This is a long-term engineering objective, not a claim that all these capabilities already exist.

---

## 17. Closing Statement

Nirqoba is an attempt to approach programming-language design from a broader computational perspective.

It aims to combine expressive programming, static correctness, native performance, flexible memory management, advanced numerical capabilities, and explicit execution control.

The project will be developed incrementally, tested rigorously, and documented continuously.

**Nirqoba is designed to compute, built to evolve, and intended to make complex programming feel coherent.**