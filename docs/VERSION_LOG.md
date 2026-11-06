# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

---

## Version 0.158-beta (dev)

### Summary of Changes (since `0.153-beta`):

*Development in progress. This version aggregates ongoing work after the 0.153-beta release.*

---

## Version 0.153-beta

### Summary of Changes (since `0.147-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization — Extended (PO-23)
- **Skip Redundant Root-Store Edges:** Store barriers now check whether the new referent is already reachable from the same root frame before publishing a remembered-set edge, eliminating unnecessary per-store overhead when the edge already exists.
- **Counter-Based Mutator Parking with Dedicated Wakeup:** Replaced the flag-based parking protocol with a per-mutator counter and a dedicated wakeup primitive, so the collector parks and resumes mutators without a shared flag cache-line ping-pong.
- **Single-Pass Frame Root-Slot Release:** Frame teardown now clears all root slots belonging to the frame in one indexed scan instead of walking the entire root table, reducing per-frame cleanup to O(frame slots).
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
- **Buffered Managed Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Documentation:** Added `reports/performance/po-23-runtime-synchronization.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.147-beta

### Summary of Changes (since `0.141-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization (PO-23)
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Buffered Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
- **Documentation:** Added `reports/performance/po-23-runtime-synchronization.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.141-beta

### Summary of Changes (since `0.131-beta`):

#### 1. Advanced Memory Optimizations (PO-21)
- **Leaner Object Header:** The managed-object header fits in 96 bytes, and the payload address is derived from the page bump allocator instead of being stored.
- **Per-Mutator Allocation Accounting:** Allocation counters are batched per mutator and published at TLAB publication or batch thresholds, so the fast path never touches a shared atomic.
- **Bounded Page Recycling:** Emptied pages return to a bounded pool and are reused instead of allocating fresh storage for every collection boundary.
- **Cheaper Reference Diagnostics:** The reference-count query compares each visited reference against a single target instead of building a map of the heap.

#### 2. Advanced Garbage Collection Algorithms (PO-22)
- **Breadth-First Marking:** The mark stack is a fixed FIFO ring that keeps the frontier contiguous instead of an intrusive list threaded through object headers, and it still never allocates.
- **Page-Local Object Discovery:** Reference resolution validates addresses against page start bitmaps, and the global object index now only tracks pages without start metadata (large objects), removing a hash insert and erase per ordinary allocation.
- **Cycle State in the Mark Context:** Marking reads its epoch and phase from the cycle's own context rather than shared atomics, and addresses already rejected by page metadata no longer fall through to a hash probe.
- **Flat Remembered Set:** An object's remembered-set membership lives in a header flag, so deduplication is a flag test and minor collections scan one contiguous array instead of a hash set.
- **Adaptive Collection Trigger:** The automatic-collection trigger follows the live heap (live bytes plus half as headroom, doubled when a cycle reclaims little) instead of a fixed one megabyte, and the live heap size is maintained incrementally rather than recomputed by walking every object at each cycle boundary.
- **Documentation:** Added `reports/performance/po-22-advanced-gc-algorithms.md` with the transformation description, structural evidence, and remaining opportunities.

---

## Version 0.131-beta

### Summary of Changes (since `0.127-beta`):

#### 1. Aggregate Layout Optimization & CLI Reporting (PO-20)
- **Widest-Alignment-First Field Ordering:** Struct fields are now laid out in descending alignment order (widest alignment first), minimizing padding and reducing aggregate size. The layout pass operates on the MLIR `nrq` dialect before lowering to LLVM.
- **Accurate Access Alignment Metadata:** Every memory access (`load`, `store`, `gep`) now carries precise alignment metadata reflecting the true field offset and type alignment, enabling LLVM to select optimal vector instructions and avoid unnecessary `memcpy`/`memmove` sequences.
- **Allocation-Path Counter Isolation:** Runtime allocation-path profiling counters are placed on dedicated cache lines, eliminating false sharing between threads and ensuring accurate per-path allocation statistics under contention.
- **Layout Inspection CLI (`--emit=layout`):** Added `--emit=layout` flag to report computed struct layout (field offsets, sizes, alignments, total size, padding) as JSON or text, enabling developers to inspect and verify aggregate memory layout without compiling to object code.
- **Layout Verification Tests:** Added integration tests validating field ordering, offset computation, padding minimization, and alignment metadata propagation through lowering.
- **Documentation:** Added `reports/performance/po-20-layout.md` with layout algorithm description, IR evidence, and CLI usage examples.

---

## Version 0.127-beta

### Summary of Changes (since `0.123-beta`):

#### 1. Profile-Guided Optimization (PO-19)
- **PGO CLI Controls (`--pgo`):** Added compiler flags `--pgo=generate` and `--pgo=use` to instrument binaries with profile counters and consume merged `.profdata` during optimization. Instrumentation is driven by LLVM's PGO pipeline (`InstrProf`) and linked via `clang_rt.profile` on Windows.
- **Profile Generation & Linking:** Binaries built with `--pgo=generate` emit raw profile data (`default.profraw`) at exit. The `llvm-profdata merge` step produces a combined `.profdata` consumed by `--pgo=use` to guide inlining, branch weighting, and hot/cold partitioning.
- **Sample Profile Round-Trip Verification:** Added integration tests verifying profile generation, merging, and consumption produce stable optimization decisions across rebuilds (`pgo_generate`, `pgo_use`, `pgo_roundtrip`).
- **Workload Coverage:** Profile quality validated across compute kernels (matmul, fib), allocator stress, and string processing workloads. Profile-guided inlining reduces code size in cold paths while accelerating hot loops.
- **Documentation:** Added `reports/performance/po-19-pgo.md` with IR evidence, build instructions, and correctness verification methodology.

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