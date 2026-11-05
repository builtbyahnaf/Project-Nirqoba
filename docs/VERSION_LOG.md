# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

---

## Version 0.127-beta (dev)

### Summary of Changes (since `0.123-beta`):

*Development in progress. This version aggregates ongoing work after the 0.123-beta release.*

---

## Version 0.123-beta

### Summary of Changes (since `0.120-beta`):

#### 1. Profile-Guided Optimization (PO-19)
- **PGO CLI Controls (`--pgo`):** Added compiler flags `--pgo=generate` and `--pgo=use` to instrument binaries with profile counters and consume merged `.profdata` during optimization. Instrumentation is driven by LLVM's PGO pipeline (`InstrProf`) and linked via `clang_rt.profile` on Windows.
- **Profile Generation & Linking:** Binaries built with `--pgo=generate` emit raw profile data (`default.profraw`) at exit. The `llvm-profdata merge` step produces a combined `.profdata` consumed by `--pgo=use` to guide inlining, branch weighting, and hot/cold partitioning.
- **Sample Profile Round-Trip Verification:** Added integration tests verifying profile generation, merging, and consumption produce stable optimization decisions across rebuilds (`pgo_generate`, `pgo_use`, `pgo_roundtrip`).
- **Workload Coverage:** Profile quality validated across compute kernels (matmul, fib), allocator stress, and string processing workloads. Profile-guided inlining reduces code size in cold paths while accelerating hot loops.
- **Documentation:** Added `reports/performance/po-19-pgo.md` with IR evidence, build instructions, and correctness verification methodology.

---

## Version 0.120-beta

### Summary of Changes (since `0.114-beta`):

#### 1. Function Call & Linkage Optimization (PO-16 / P1 3.7)
- **Internal Linkage for Generated Helpers:** Set `internal` linkage on all lowered function definitions except the C entry point `main`. Enables LLVM to perform whole-module reasoning, eliminate unused helpers, and select optimal internal calling conventions (`fastcc`).
- **Tail Call Marking (`MarkTailCallsPass`):** Introduced an MLIR pass that detects proven tail-position return sequences and attaches the `tail` call marker. Enables the LLVM x86 backend to emit sibling-call jumps and perform tail-recursion elimination at `-O0` and `-O2`, preventing stack exhaustion under deep recursion.
- **Single-Assignment Stack Slot Promotion:** Enhanced lowering to detect local variables, parameters, fixed arrays, views, and structs that are initialized once and only read. Bypasses redundant entry-block `alloca`, `store`, and `load` sequences by propagating the SSA value directly, cutting unnecessary memory traffic.

#### 2. Target-Aware Code Generation & Architecture Profiles (PO-17 / P1 3.8)
- **Target Profiles (`portable`, `native`, `explicit`):** Introduced structured target configuration resolving `--target-profile`, `--target <triple>`, `--target-cpu`, and `--target-features` once prior to optimization and code generation.
- **Strict CPU & Feature Validation:** Integrated subtarget feature validation (`isCPUStringValid`, `getAllProcessorFeatures`) that rejects invalid target options with diagnostic `NRQ5002` instead of silently ignoring invalid options.
- **Target Metadata Recording:** Lowered modules now embed target configuration module flags (`nrqoba.target.profile`, `triple`, `cpu`, `features`) ensuring transparent artifact profiling and verifiable host vs. portable distinction.
- **Assembly Verification Harness:** Added `assembly_check` mode to `tests/run_tests.py` using `llvm-objdump` to inspect emitted machine instructions and verify SSE2 baseline vs. AVX2 (`ymm`) vs. AVX-512 (`zmm`) code generation.

#### 3. Interprocedural Optimization & Whole-Program LTO (PO-18)
- **IPO Policy Controls (`--ipo`):** Added CLI flag `--ipo=standard|aggressive|minimal` to tune whole-program interprocedural optimization depth. Controls inliner budget, speculative devirtualization, and Attributor analysis runs.
- **Call-Graph Memory Effects Fixpoint:** Implemented a call-graph worklist solver that propagates `llvm.memory_effects` across the module, enabling LLVM to infer precise read/write/capture semantics for all functions including helpers.
- **Whole-Program LTO Configuration:** Backend now configures LLVM's LTO pass pipeline (`ThinLTO` or `FullLTO`) from the resolved IPO policy, enabling cross-module inlining, interprocedural constant propagation, function specialization, and global dead-code elimination without a separate link-time step.
- **IPO Metadata Recording:** Optimized modules embed `nrqoba.ipo.mode` and `nrqoba.ipo.strategy` module flags for transparent artifact inspection.
- **Integration Coverage:** Added test suites for multi-module inlining, symbol visibility, and function specialization under each IPO policy.

---

## Version 0.114-beta

### Summary of Changes (since `0.111-beta`):

#### 1. Optimization Diagnostics & Remarks Reporting (`--remarks`)
- Added compiler CLI flag `--remarks` (with `--remarks=missed` and pass-name filtering like `--remarks=loop-vectorize`).
- Plugs a custom diagnostic handler into LLVM's `DiagnosticInfoOptimizationBase` to report loop vectorization, inlining, and unrolling decisions to stderr without affecting emitted object code.

#### 2. Vector Alignment for Stack Allocations
- Promoted stack buffers for owned arrays and raw memory allocations previously defaulted to scalar natural alignment (`align 8`), forcing LLVM to emit unaligned SIMD instructions (`movups`).
- Implemented a 16-byte alignment floor for promoted stack slots meeting or exceeding vector register widths, generating `align 16` in LLVM IR and enabling selection of aligned SIMD moves (`movaps`). Stricter explicit alignments requested via `owned_array_aligned` continue to take precedence.

#### 3. Floating-Point Reduction Vectorization (`--fast-math`)
- Added an opt-in `--fast-math` flag enabling `reassoc` and `contract` fast-math flags on lowered floating-point operations.
- Relaxes strict IEEE 754 reordering constraints that previously prevented LLVM from vectorizing `f64` loop accumulations, allowing vector reduction synthesis (`llvm.vector.reduce.fadd`) and fused multiply-add (FMA) instruction selection while preserving strict IEEE semantics by default.

#### 4. Vectorized Loop Correctness & Integration Coverage
- Added dedicated test coverage verifying execution semantics under loop vectorization:
  - Runtime non-multiple trip counts (`vectorized_reduction_tail`) exercising scalar loop epilogues.
  - Overflow wrapping semantics (`vectorized_reduction_wrap`) across SIMD reduction trees.
  - Array bounds checking traps (`vectorized_bounds_trap`) ensuring runtime violation detection and exit code 70 remain intact in vectorized loop bodies.
  - Verification of 16-byte alignment and emitted `--remarks` diagnostics.

---

## Version 0.111-beta

### Summary of Changes (since `0.108-beta`):

#### 1. GC Root-Liveness Optimization (PO-11 / P0 3.3)
- Replaced eager activation-wide shadow-stack root registrations with fine-grained backward liveness analysis over TIR values.
- Managed stack slots, write barriers, and root frames are now emitted strictly across collection points where values are actually live, eliminating heavy shadow-stack metadata churn, mutex contention, and unnecessary tracing overhead for short-lived locals.
- Root summary information at collection boundaries is propagated across the call graph.

#### 2. Runtime Function Attributes & Memory Effects Inference (PO-12 / P0 3.4)
- Annotated all `nrq_gc_*` runtime entry points with accurate attributes (`nounwind`, `willreturn`, `nocapture`), allowing LLVM to treat GC bookkeeping and allocation helpers as non-throwing operations.
- Implemented whole-module memory effect inference: lowered functions now publish precise `llvm.memory_effects` (via fixpoint analysis over pointer provenance, stack spills, and external calls) rather than assuming every helper can read and write all memory.

#### 3. Entry-Block Stack Allocation & SSA Promotion (PO-13 / P0 3.5)
- Ensured all temporary stack allocations (aggregate barrier copies, `array.allocate` reference offsets, mapped memory access buffers, profiler records) are allocated exclusively in the function entry block via `alloca_at_entry`.
- Resolves unbounded stack growth inside loops and exposes all stack-allocated temporaries directly to LLVM's `mem2reg` and `SROA` passes.

#### 4. Review Hardening & Optimizations (PO-14 / F2–F6)
- **Parameter Attribute Derivation (F2):** `nocapture` and `readonly` attributes are now derived directly from escape fixpoint analysis and store-destination scanning instead of being blindly assigned from ownership classes.
- **Escape Analysis Store Soundness (F3):** Refused pointer-derived stores through borrowed views, ensuring publishing pointers/references into caller-accessible memory is properly treated as an escape.
- **Linear Stack-Allocation Analysis (F4):** Re-architected candidate descriptor lookups in stack promotion analysis using indexed hash tables, reducing escape analysis time on large view sets from cubic ($O(N^3)$) to linear ($O(N)$).
- **Cold Bounds-Failure Block Sharing (F5):** Deduplicated array bounds check traps so each function shares a single cold, noreturn failure block, drastically cutting redundant basic blocks and instruction bloat.
- **Loop Safepoint Polling Throttling (F6):** Throttled GC safepoint polling on loop back edges to execute once every 64 iterations via a register-promoted bitmask counter, eliminating per-iteration runtime call overhead in tight, allocation-free loops.

---

## Version 0.108-beta

### Changes since 0.105-beta:
- **`eb858e7`** — `feat(opt): implement escape analysis and stack promotion for pointer allocations`
- **`2a96640`** — `docs(report): complete alias analysis report and integration test expectation`
- **`6fec2d7`** — `feat(compiler): alias architecture upgrade`
- **`4bbaf93`** — `feat(compiler): Accurate Runtime Function Attributes`
- **`a5c7a28`** — `benchmark: rerun stress test 2`
- **`1caa5e8`** — `perf(gc): upgraded the internal mechanisms (pt 3)`
- **`0df9174`** — `perf(gc): upgraded the internal mechanisms (pt 2)`
- **`fdd9066`** — `perf(gc): upgraded the internal mechanisms`

---

## Version 0.105-beta

### Changes since 0.102-beta:
- **`ede0f26`** — `feat: gc improvement and new benchmark tests`
- **`678d7c1`** — `perf: add constant propagation and sccp to mlir cleanup`