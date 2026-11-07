# Nirqoba : Compiler & Performance Upgrade Specification

**Document type:** Authoritative Engineering Specification  
**Project:** Nirqoba Programming Language  
**Author & Final Decision Authority:** **Muhammad Tahmid Ahnaf (@builtbyahnaf)**  
**Status:** In Progress (Phase 6 Performance Optimization — compiler version `0.186-beta`)  
**Target:** Stable Version 1.0 and subsequent Version 2.0 development

---

## 1. Authority, Ownership & Change Control

This document is an authoritative engineering specification for Nirqoba's compiler, runtime, memory management, execution model, and performance objectives.

**The final authority over every architectural decision, implementation priority, scope change, and completion approval belongs exclusively to the project author: Muhammad Tahmid Ahnaf (@builtbyahnaf).**

### Mandatory rules for coding agents

Any AI coding agent, automated development tool, or human contributor working from this specification MUST follow these rules:

1. **Do not overwrite this document.** No agent is authorized to modify, replace, restructure, or remove its requirements independently.
2. **Do not redefine architectural decisions.** Existing decisions must not be silently replaced with an agent's preferred implementation.
3. **Do not claim unimplemented work is complete.** A feature is not complete merely because code was generated, a function was declared, or a placeholder was added.
4. **Do not fabricate test results.** Every claimed test, benchmark, and verification must have actually been executed.
5. **Do not silently skip requirements.** If an item is infeasible, incomplete, blocked, or incompatible with the existing architecture, report it explicitly.
6. **Do not weaken acceptance criteria.** Agents cannot lower performance targets, remove tests, or redefine completion conditions to make their work appear successful.
7. **Do not make unauthorized scope changes.** New architectural proposals must be presented to the author for approval before implementation.
8. **Preserve existing functionality.** An upgrade must not silently break previously supported language features.
9. **Report actual implementation status.** Distinguish between implemented, partially implemented, tested, benchmarked, and verified.
10. **Never modify this specification without explicit authorization from the author.**

### Required implementation reports

At the end of every substantial implementation task, the coding agent must report:

- Files created, modified, or removed.
- Architectural changes and their reasoning.
- Requirements completed and their corresponding evidence.
- Requirements not completed and why.
- Tests actually executed and their results.
- Benchmarks actually executed, including reproduction instructions.
- Known limitations, regressions, and technical debt.
- Remaining work and dependencies.

**A successful code-generation operation is not equivalent to a successful engineering implementation.**

This specification takes precedence over assumptions made by an individual coding agent. If an implementation instruction conflicts with this document, the agent must stop and request clarification rather than silently changing the specification.

---

## 2. Overall Objective

Nirqoba aims to become a high-performance, general-purpose native programming language capable of outperforming established languages such as C++ and Rust in carefully defined workloads.

The objective is not merely to generate native machine code. It is to minimize unnecessary work at every level:

- Compile-time analysis and optimization.
- Machine-code generation.
- Memory allocation and management.
- Ownership and borrowing.
- Parallel execution and concurrency.
- Numerical computation.
- Runtime initialization and execution overhead.

Nirqoba must pursue performance without sacrificing correctness, reliability, memory safety guarantees, or maintainability.

### Performance philosophy

1. Zero-cost abstractions wherever practical.
2. Compile-time guarantees instead of unnecessary runtime checks.
3. Efficient memory layouts and cache locality.
4. Predictable allocation and deallocation.
5. Aggressive but semantically correct compiler optimization.
6. Efficient parallel execution.
7. Hardware-aware code generation.
8. Reproducible performance measurement.

Universal superiority over C++ and Rust is not assumed. Performance claims must be established independently for each workload.

---

# PART I — VERSION 1.0

The following sections define the performance-related engineering requirements for Nirqoba's stable Version 1.0.

## 3. Compiler Optimization Infrastructure

**Priority:** Critical  
**Release:** Version 1.0

The current native compilation architecture must evolve into a proper optimizing compiler.

### Requirements

- [x] Implement a proper LLVM IR optimization pipeline using LLVM PassBuilder.
- [x] Integrate MLIR canonicalization and common subexpression elimination (CSE).
- [x] Implement dead-code elimination.
- [x] Implement dead-store elimination.
- [x] Implement constant folding.
- [x] Implement constant propagation.
- [x] Implement sparse conditional constant propagation.
- [x] Implement control-flow simplification.
- [x] Implement unreachable-block elimination. _(PO-26: LLVM SCCP, SimplifyCFG, GlobalDCE + MLIR trivial-DCE; tested in `opt_unreachable_code`)_
- [x] Implement function inlining with an appropriate cost model. _(PO-26: LLVM per-module default pipeline inliner; tested in `opt_inlining`)_
- [x] Implement interprocedural optimization. _(PO-9/PO-18: `--ipo=standard|aggressive|minimal`; Attributor behind `--ipo=aggressive`; final GlobalDCE; whole-program unit)_
- [x] Implement loop-invariant code motion. _(PO-26: LLVM LICM; tested in `opt_licm` with hoisting remark)_
- [x] Implement loop unrolling with heuristics. _(PO-26: LLVM loop unroller with cost-model; observed on fixed-trip-count loops)_
- [x] Implement loop strength reduction. _(PO-26: LLVM LSR; exercised through `opt_vectorize_owned` trip-count loop)_
- [x] Implement induction-variable simplification. _(PO-26: LLVM IndVarSimplify; exercised through `opt_vectorize_owned`)_
- [x] Implement loop vectorization.
- [ ] Implement SLP vectorization. _(Deferred: no stable straight-line artifact; straight-line probes fold to constants or are unrolled first; failure mode is a missed optimization, not a miscompile — see PO-26)_
- [x] Implement tail-call optimization where valid.
- [x] Implement escape analysis and allocation elimination.
- [x] Implement scalar replacement of aggregates.
- [ ] Implement devirtualization and indirect-call elimination where possible. _(Not applicable: Nirqoba has no virtual dispatch, traits, interfaces, or first-class function values — see PO-9)_
- [x] Introduce optional ThinLTO or equivalent link-time optimization. _(PO-9/PO-18: whole-program LTO — imports are merged before LLVM runs, equivalent to ThinLTO for this compilation model)_
- [x] Support optimization profiles: `-O0`, `-O1`, `-O2`, `-O3`, `-Os`, and `-Oz`. _(Entry #001: PassBuilder per-module default pipeline; invalid levels rejected with NRQ5002)_
- [x] Ensure Nirqoba-specific semantic guarantees are preserved across optimization passes.
- [x] Provide LLVM IR inspection.
- [x] Provide optimized IR inspection. _(PO-26: `expect_optimized_ir_contains` / `expect_optimized_ir_not_contains` / `expect_optimized_ir_counts` runner keys; separate `--emit=llvm` vs `--emit=llvm-raw` channels)_
- [x] Provide optimization remarks and pass diagnostics. _(Entry #006/PO-15: `--remarks`, `--remarks=missed`, substring filters e.g. `--remarks=loop-vectorize`)_

### Acceptance criteria

- Release builds must execute the actual LLVM optimization pipeline.
- Optimization levels must produce meaningfully different optimization behavior where appropriate.
- Tests must verify that optimizations preserve program semantics.
- Representative workloads must demonstrate measurable improvements over unoptimized builds.

---

## 4. CPU Architecture & Machine-Code Optimization

**Priority:** Critical  
**Release:** Version 1.0

Nirqoba must generate machine code that takes advantage of the target processor rather than relying exclusively on generic CPU configurations.

### Requirements

- [x] Support configurable target CPU architectures and models.
- [x] Support CPU feature detection and explicit feature selection.
- [x] Provide a portable target profile.
- [x] Provide an optional host-tuned target profile.
- [x] Support available SIMD instruction sets, including SSE, AVX2, AVX-512, and ARM NEON where supported. _(Entry #008/PO-17: portable/native/explicit profiles; SSE2/AVX2/AVX-512 assembly selection tested; portable baseline asserts xmm only; explicit AVX2 asserts ymm; explicit AVX-512 asserts zmm)_
- [x] Integrate target-specific instruction selection through LLVM. _(Entry #008/PO-17: `TargetProfile` + `TargetConfiguration`; `resolve_target_configuration()` with CPU/feature validation; instruction selection driven by resolved triple/CPU/features)_
- [x] Improve alignment and aliasing information available to the optimizer.
- [x] Optimize calling conventions and ABI handling. _(Entry #007/PO-16: `InternalizeFunctionsPass` assigns internal linkage → `fastcc` for GC-frame-free functions; calling convention pinned in `recursive_numeric_no_gc_frame`)_
- [x] Reduce unnecessary stack spills and register pressure. _(Entry #007/PO-16: single-assignment slot promotion removes write-once alloca/store/load triples; IR slots 818→646, instructions 5882→5297 on measured corpus)_
- [x] Reduce avoidable function-call overhead. _(Entry #007/PO-16: `InternalizeFunctionsPass` + inliner eliminates small helpers; `add_one`/`twice` vanish at -O2; call-heavy -O2 object 638→427 bytes)_
- [x] Inspect generated assembly in performance tests. _(Entry #008/PO-17: `assembly_check` mode in `run_tests.py` — object-emits, runs `llvm-objdump`, asserts required/forbidden instruction markers)_
- [ ] Benchmark across supported architectures.

### Acceptance criteria

- Generated code must respect the selected target architecture.
- Portable builds must not accidentally require unsupported instructions.
- Host-tuned builds must demonstrate measurable improvements where hardware-specific optimization is beneficial.

---

## 5. Parallel Execution, Concurrency & Asynchronous Runtime

**Priority:** Critical  
**Release:** Version 1.0

Parallel execution is a fundamental performance capability of Nirqoba, not an optional ecosystem feature.

Nirqoba must support efficient utilization of multiple CPU cores while minimizing scheduling, synchronization, allocation, and communication overhead.

**Syntax authority:** The concrete language syntax and user-facing constructs for parallel execution will be defined separately by the author in `SYNTAX.md`.

Coding agents must not independently invent, finalize, or modify Nirqoba's parallel execution syntax. Runtime and compiler implementation must follow the approved syntax specification.

### Requirements

- [ ] Design a low-overhead threading model.
- [ ] Implement efficient thread creation and management.
- [ ] Implement a task scheduler.
- [ ] Implement work-stealing task scheduling where appropriate.
- [ ] Minimize synchronization overhead.
- [ ] Reduce lock contention in runtime components.
- [ ] Provide efficient atomic operations.
- [ ] Implement appropriate memory-ordering primitives.
- [ ] Optimize mutexes, condition variables, and synchronization primitives.
- [ ] Implement structured concurrency.
- [ ] Optimize asynchronous state-machine generation.
- [ ] Avoid unnecessary heap allocations for asynchronous tasks.
- [ ] Implement efficient coroutine-frame allocation.
- [ ] Support parallel loops and reductions through the approved language design.
- [ ] Support efficient task distribution across CPU cores.
- [ ] Implement cache-line-aware data layouts and padding where necessary.
- [ ] Evaluate NUMA-aware execution for supported platforms.
- [ ] Ensure deterministic cleanup and resource management across concurrent execution.
- [ ] Integrate parallel execution with Nirqoba's ownership and memory model.
- [ ] Ensure the garbage collector can safely coexist with multithreaded execution.
- [ ] Add race-detection and concurrency stress tests.

### Acceptance criteria

- Parallel workloads must demonstrate measurable scaling against their single-threaded equivalents.
- Runtime overhead must be measured independently from useful computation.
- Concurrency must not compromise memory safety or program correctness.
- Scheduling must remain efficient for both small and large tasks.
- Thread synchronization and memory management must have documented correctness guarantees.

### Important architectural requirement

Parallel execution must be designed into the compiler and runtime rather than implemented as a superficial wrapper around operating-system threads.

The compiler should eventually be capable of recognizing and optimizing parallel execution opportunities where the language's semantics permit it.

---

## 6. Numerical Computing & SIMD

**Priority:** High  
**Release:** Version 1.0

Numerical computing must be a first-class performance consideration.

Nirqoba aims to support scientific computation, mathematical workloads, data processing, and machine-learning foundations.

### Requirements

- [ ] Define precise floating-point semantics.
- [x] Support fused multiply-add when permitted by the selected semantics.
- [ ] Optimize integer and floating-point arithmetic.
- [ ] Provide efficient vector and matrix primitives.
- [x] Implement SIMD-friendly memory layouts. _(PO-20: field reordering to minimize padding; 16-byte alignment floor for owned-array stack buffers; natural alignment emitted for all accesses; Entry #006: `align 16` in LLVM IR enables `movaps`)_
- [ ] Implement optimized numerical operations.
- [x] Optimize reductions and common numerical kernels.
- [x] Provide explicitly controlled fast-math compilation options. _(Entry #006/PO-15: `--fast-math` attaches `reassoc` + `contract` fast-math flags; enables vectorized reductions `llvm.vector.reduce.fadd` and FMA)_
- [ ] Support f16 and other appropriate reduced-precision formats.
- [ ] Establish correct semantics for future f8 and f4 types.
- [ ] Integrate numerical optimization with the CPU target configuration.
- [x] Ensure numerical operations can benefit from vectorization.
- [ ] Benchmark numerical kernels against optimized C++ and Rust implementations.
- [ ] Validate numerical accuracy alongside execution speed.

### Acceptance criteria

- Common numerical operations must generate efficient native code.
- SIMD execution must be available where supported by the target.
- Fast-math optimizations must be explicitly controlled and documented.
- Numerical correctness must be tested independently from performance.

---

## 7. Ownership, Borrowing & Zero-Cost Abstractions

**Priority:** Critical  
**Release:** Version 1.0

Nirqoba must support efficient memory ownership without introducing unnecessary runtime overhead.

### Requirements

- [x] Complete static ownership analysis.
- [x] Complete move semantics and ownership transfer.
- [x] Implement field-level ownership tracking.
- [x] Implement lifetime and escape analysis.
- [ ] Implement interprocedural lifetime analysis where practical.
- [x] Support borrowing without runtime borrow-checking overhead when statically provable.
- [x] Support mutable borrowing with compile-time exclusivity guarantees.
- [x] Optimize compiler-generated cleanup and destruction. _(Entry #007/PO-16: single-assignment slot promotion removes alloca/store/load for write-once locals; PO-24 elides nested mutator region enter/leave pairs)_
- [ ] Ensure early returns and exception paths perform correct cleanup.
- [ ] Define ownership rules for closures and captured variables.
- [ ] Eliminate unnecessary reference counting for uniquely owned objects.
- [ ] Keep shared ownership explicit and measurable.
- [ ] Optimize reference counting, including safe batching or elision opportunities.
- [ ] Add compile-time checks for use-after-free, double-free, and invalid aliasing.
- [ ] Benchmark ownership abstractions against equivalent handwritten C++ and Rust implementations.

### Acceptance criteria

- Common uniquely owned and borrowed operations must not require unnecessary runtime bookkeeping.
- Ownership transfer must not introduce avoidable copies.
- Borrowing guarantees must be enforced correctly.
- Shared ownership must have clearly documented runtime costs.

---

## 8. Garbage Collector Performance & Correctness

**Priority:** Critical  
**Release:** Version 1.0

Nirqoba's garbage collector must remain a reliable memory-management option without imposing excessive overhead on performance-sensitive programs.

### Requirements

- [ ] Resolve the inconsistency between default-enabled and opt-in GC semantics across implementation and documentation.
- [x] Replace per-object dynamic reference-offset vectors with shared immutable type descriptors. _(PO-24: `reference_layout_descriptor()` interns the offset/size array as a `private constant [N x i64]` global per module; multiple call sites share one descriptor; stack alloca eliminated)_
- [x] Reduce global GC mutex contention. _(PO-23: per-mutator `barrier_depth` removes the shared `barrier_readers` RMW; `SlotIndex` open-addressing table replaces `unordered_map`; shadow-stack mutex gated behind diagnostics flag; transfer-pin deltas buffered per-mutator)_
- [x] Introduce thread-local allocation buffers (TLABs).
- [x] Implement size-class allocation. _(PO-22: per-page free runs with coalescing; short-lived objects reclaimed without bumping into a fresh page)_
- [x] Implement free-slot reuse. _(PO-22: per-page free runs with coalescing and TLAB reuse; `nrq_gc_span_reuse_count` accessor)_
- [x] Reuse empty pages and partially occupied page regions.
- [x] Optimize root-frame registration and cleanup.
- [x] Avoid root-frame machinery for functions without managed references.
- [x] Implement liveness-aware root maps where practical.
- [x] Establish explicit safepoints and stop-the-world coordination.
- [x] Implement write barriers and remembered sets.
- [x] Implement generational collection with promotion policies.
- [ ] Measure allocation latency.
- [ ] Measure collection pause times.
- [ ] Add heap fragmentation and object-retention diagnostics.
- [x] Test cyclic object graphs and deeply nested references.
- [x] Add multithreaded GC stress tests.
- [x] Evaluate incremental or concurrent collection only after safepoints and write barriers are correct.
- [x] Implement incremental marking and sliced sweeping that overlap mutator execution.
- [x] Keep GC-managed and explicitly managed allocation paths distinct.

### Acceptance criteria

- Collection must be correct under supported execution models.
- Allocation and collection overhead must be independently measurable.
- The collector must not interfere with safe concurrent execution.
- Memory retention and fragmentation must be tested.
- GC must not impose unnecessary costs on programs that do not use it.

---

## 9. Explicit Allocators & Memory Runtime Optimization

**Priority:** High  
**Release:** Version 1.0

Explicit allocation must provide predictable and efficient memory management for systems programming and performance-sensitive workloads.

### Requirements

- [ ] Optimize allocator fast paths.
- [x] Avoid unnecessary allocator-registry lookups.
- [x] Reduce synchronization on allocation and deallocation hot paths.
- [x] Add thread-local allocation caches. _(PO-23: per-mutator transfer-pin delta buffer; allocation accounting moved into MutatorState; TLAB refill off the global lock hot path)_
- [x] Optimize arena and pool allocation.
- [ ] Implement efficient size-class free lists.
- [ ] Improve slab allocator reuse.
- [ ] Reduce fragmentation.
- [ ] Optimize reallocation paths.
- [ ] Reduce allocator metadata overhead.
- [x] Introduce allocator-aware compiler optimizations.
- [ ] Allow allocation strategies to be selected at compile time where practical.
- [ ] Benchmark each allocator against platform allocators and equivalent C++/Rust implementations.
- [ ] Retain appropriate debug safety checks while minimizing unnecessary Release-build overhead.

### Acceptance criteria

- Common allocation paths must be efficient and predictable.
- Allocator selection must not introduce avoidable runtime overhead.
- Fragmentation, allocation throughput, and deallocation throughput must be measured.
- Allocator behavior must remain correct under concurrent workloads.

---

## 10. Pre-Release Verification & Stable Version 1.0

**Priority:** Mandatory release gate  
**Release:** Version 1.0 pre-release

This section is the final engineering verification phase before declaring Nirqoba stable Version 1.0.

No feature is considered production-ready merely because it compiles or passes a basic demonstration.

### 10.1 Correctness and semantic verification

- [ ] Define and verify integer overflow behavior.
- [ ] Define and verify floating-point behavior.
- [ ] Verify aliasing guarantees.
- [ ] Verify ownership and borrowing semantics.
- [ ] Verify memory safety guarantees.
- [ ] Verify exception handling and cleanup.
- [ ] Verify concurrent execution correctness.
- [ ] Verify GC behavior under stress.
- [ ] Verify allocator correctness.
- [ ] Test debug and Release builds for behavioral equivalence.
- [ ] Verify optimization passes do not silently change program semantics.
- [ ] Maintain a compiler regression test corpus.

### 10.2 Compiler testing

- [ ] Implement compiler fuzzing.
- [ ] Implement differential testing where applicable.
- [ ] Run sanitizers on compiler and runtime components.
- [ ] Test optimized and unoptimized compilation.
- [ ] Test supported target architectures.
- [ ] Test error handling and diagnostic quality.
- [ ] Test large source files and complex compilation units.

### 10.3 Benchmark verification

- [ ] Establish a permanent C++/Rust/Nirqoba benchmark suite.
- [ ] Use identical algorithms and data sets.
- [ ] Use comparable optimization levels and fair compiler flags.
- [ ] Execute all benchmarks on the same machine and under controlled conditions.
- [ ] Use Release builds.
- [ ] Record compiler versions, LLVM version, CPU model, OS, and flags.
- [ ] Perform warmups and multiple repetitions.
- [ ] Report median, variance, and outliers.
- [ ] Separate compilation time from execution time.
- [ ] Measure throughput, latency, memory consumption, and allocation rate.
- [ ] Measure GC pause times and total GC overhead.
- [ ] Include single-threaded and multithreaded workloads.
- [ ] Include arithmetic, recursion, allocation, arrays, strings, hash maps, sorting, and numerical workloads.
- [ ] Include large-input and cache-stressing workloads.
- [ ] Inspect generated assembly for unexpected regressions.
- [ ] Publish benchmark source code and exact reproduction instructions.
- [ ] Establish performance regression thresholds.

### 10.4 Release requirements

- [ ] All mandatory Version 1.0 requirements must be implemented or explicitly approved as deferred by the author.
- [ ] No known critical correctness or memory-safety defects may remain unresolved.
- [ ] Compiler and runtime behavior must be documented.
- [ ] Benchmark results must be reproducible.
- [ ] Known limitations must be disclosed.
- [ ] Release binaries must be tested independently of development builds.
- [ ] Version 1.0 release readiness must be reviewed and approved by Muhammad Tahmid Ahnaf (@builtbyahnaf).

**Only the author can authorize the stable Version 1.0 release.**

---

# PART II — VERSION 2.0

The following sections are explicitly reserved for Version 2.0 development.

They are not prerequisites for declaring Version 1.0 stable unless the author subsequently changes the release scope.

## 11. Standard Library & High-Performance Data Structures

**Priority:** High  
**Release:** Version 2.0

### Requirements

- [ ] Develop performance-focused contiguous arrays and containers.
- [ ] Implement efficient strings and string builders.
- [ ] Optimize hash maps and hash sets.
- [ ] Provide cache-friendly tree and graph structures.
- [ ] Implement efficient sorting and searching algorithms.
- [ ] Provide SIMD-friendly numerical containers.
- [ ] Optimize iteration and slicing to avoid unnecessary copies.
- [ ] Support views and spans for zero-copy access.
- [ ] Provide allocator-aware containers.
- [ ] Benchmark against mature C++ and Rust standard-library alternatives.
- [ ] Document complexity, allocation behavior, and invalidation rules.

---

## 12. GPU & Accelerator Execution

**Priority:** Strategic  
**Release:** Version 2.0

### Requirements

- [ ] Define a stable CPU/GPU execution model.
- [ ] Implement GPU availability and capability detection.
- [ ] Build a GPU runtime and memory-transfer layer.
- [ ] Implement GPU kernel compilation and launch.
- [ ] Optimize host-to-device and device-to-host transfers.
- [ ] Minimize synchronization and launch overhead.
- [ ] Support memory coalescing and shared-memory optimization.
- [ ] Implement GPU-specific compiler optimizations.
- [ ] Support appropriate GPU backends and hardware targets.
- [ ] Benchmark complete workloads, including transfer costs.
- [ ] Compare against optimized CUDA implementations.

---

## 13. Build System, Linker & Runtime Overhead

**Priority:** High  
**Release:** Version 2.0

### Requirements

- [ ] Minimize runtime initialization cost.
- [ ] Remove unnecessary runtime dependencies from programs that do not need them.
- [ ] Support dead-stripping of unused runtime components.
- [ ] Optimize static and dynamic linking configurations.
- [ ] Implement efficient startup and shutdown paths.
- [ ] Ensure small programs do not pay for unused GC or concurrency machinery.
- [ ] Measure executable size and cold-start latency.
- [ ] Support reproducible Release builds.
- [ ] Keep debug instrumentation out of performance-critical Release paths.
- [ ] Validate ABI and runtime compatibility across compiler versions.

---

## 14. Extended Performance Engineering & Developer Observability

**Priority:** Medium  
**Release:** Version 2.0

### Requirements

- [ ] Provide compiler flags to explain optimization decisions.
- [ ] Provide warnings for missed vectorization and missed inlining.
- [ ] Provide allocation and GC profiling tools.
- [ ] Provide runtime memory statistics.
- [ ] Provide assembly and LLVM IR output options.
- [ ] Provide optimization remarks and pass timing.
- [ ] Document performance characteristics of language features.
- [ ] Create a comprehensive performance guide.
- [ ] Provide reproducible performance diagnostics through simple compiler commands.

---

# 15. Implementation Governance

### 15.1 Development workflow

Every major implementation should follow this sequence:

1. Inspect the existing implementation.
2. Identify architectural dependencies.
3. Present any necessary design changes for approval.
4. Implement the feature.
5. Add relevant tests.
6. Execute the tests.
7. Benchmark where performance is involved.
8. Inspect generated code or runtime behavior where relevant.
9. Document the implementation.
10. Submit an evidence-based completion report.

### 15.2 Completion classifications

Every checklist item must use one of these statuses:

| Status | Meaning |
|---|---|
| NOT STARTED | No implementation work has been completed. |
| IN PROGRESS | Implementation is underway. |
| IMPLEMENTED | Code exists, but full verification is pending. |
| TESTED | Relevant tests have actually passed. |
| BENCHMARKED | Performance has been measured. |
| VERIFIED | Implementation, correctness, and applicable performance requirements have been independently checked. |
| BLOCKED | Work cannot proceed due to a documented dependency or technical issue. |
| DEFERRED | Explicitly postponed with author approval. |

An item must never be marked VERIFIED based solely on code inspection or an agent's assertion.

### 15.3 Architectural change proposals

Any proposed modification to this specification must be submitted separately with:

- The original requirement.
- The proposed change.
- The technical justification.
- The expected benefits.
- The potential disadvantages.
- The affected components.
- Migration implications.
- Benchmark or test evidence, if available.

The author must explicitly approve the change before it becomes part of the authoritative specification.

---

# 16. Final Performance Objective

Nirqoba is not being developed merely to become another language that compiles to native code.

The long-term objective is to build a language whose compiler, memory model, runtime, concurrency architecture, and numerical execution model work together to produce exceptionally efficient programs.

**The standard is not whether Nirqoba can compete with C++ and Rust in theory. The standard is whether it can demonstrate that competitiveness through reproducible engineering evidence.**

Performance must be earned, measured, verified, and maintained.

---

**Document ownership:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Final architectural authority:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Modification policy:** Explicit author authorization required.