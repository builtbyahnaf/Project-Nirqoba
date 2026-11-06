# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

---

## Version 0.206-beta (dev)

### Summary of Changes (since `0.201-beta`):

*Development in progress. This version aggregates ongoing work after the 0.201-beta release.*

---

## Version 0.201-beta

### Summary of Changes (since `0.195-beta`):

#### 1. Mutator Regions Follow the Call Graph (PO-27 �2.2)
- **Propagation:** a function needs a region when it holds a managed value or calls one that does, resolved to a fixed point over direct calls. A driver loop that only calls an allocating helper keeps one region instead of entering and leaving per iteration; runtime entry points are not propagated, so querying or toggling collection still needs no region.
- **Recursion:** region elision now resolves from the optimistic direction and narrows, so a direct or mutually recursive group entered once from `main` shares the outer region instead of duplicating it on every activation.
- **Entry wrapper:** the wrapper the C runtime calls opens and closes the region, and the elision pass sees func-level as well as LLVM calls, so everything under `main` runs in one region.
- **Evidence:** `tests/integration/positive/gc_region_transitive`, `gc_region_recursive` and `gc_region_mutual` assert one enter and one leave for the whole program, with the safepoint poll still present in the loops that may allocate.

---

## Version 0.195-beta

### Summary of Changes (since `0.192-beta`):

#### 1. Transfer-Pin Delta Lifecycle (PO-27 �2.1)
- **Retirement:** a delta that cancels out clears its slot instead of leaving an entry that describes nothing but still matches later handoffs.
- **Reuse:** empty slots are reused before the high-water count grows, so returning and releasing distinct objects no longer pushes every handoff past the thirty-second onto the collector mutex; `nrq_gc_transfer_pin_fallbacks()` reports the genuine full-buffer cases.
- **Ordering:** a fallback settles the mutator's buffered deltas under the collector mutex before applying its own operation, so a release can no longer erase an entry a buffered pin had not reached yet and leave the object pinned for the life of the process.

---

## Version 0.192-beta

### Summary of Changes (since `0.186-beta`):

#### 1. Safepoint Poll Bound
- **Comparison:** the countdown is tested at or below zero instead of for equality, so a counter that reaches zero off the polled path or wraps past it can no longer stop a loop from observing collection requests.
- **Evidence:** `tests/integration/positive/gc_safepoint_bound` asserts one polled back edge and the comparison in the optimized module while the program allocates past the poll interval.

#### 2. Root Publication Edges
- **Never dropped:** a type the lowering reports as managed always publishes an offset; a layout walk that returned nothing now registers offset 0 instead of silently skipping the root, with a debug assertion covering the disagreement.
- **Complete elision:** `tests/integration/positive/gc_root_elision_owned` requires no descriptor, no mutator entry and no poll for a program with no managed storage.
- **Every level:** `tests/integration/positive/gc_root_at_O0` requires the frame call at `-O0`.

#### 3. Documented Invariants
- **Runtime ABI:** `docs/architecture/06-runtime.md` now states the three root-completeness invariants and the poll bound, each naming the artifact that fails when it is broken.

---

## Version 0.186-beta

### Summary of Changes (since `0.181-beta`):

#### 1. Root Registration Completeness
- **One layout walk:** the frame descriptor's words and its validity check come from a single derivation, so the table an activation publishes always matches the roots it was built from.
- **Self-describing tables:** a descriptor now carries its own word count, and the runtime rejects a truncated table before publishing anything, including on the marking path where the roots are marked from the table itself.
- **Late and nested slots:** `tests/integration/positive/gc_root_completeness` collects between declarations and asserts one descriptor holding a slot declared after a collection plus a nested aggregate layout.

---

## Version 0.181-beta

### Summary of Changes (since `0.176-beta`):

#### 1. Optimizer Evidence Matrix Recorded
- **Report:** `reports/performance/po-26-optimizer-evidence.md` maps every requested transform to its owner, its artifact and its verdict, including the one deferred item (SLP) and the two IR properties that limit LLVM.
- **Roadmap:** the optimization roadmap now names the owner and evidence artifact for each transform it lists, so a future cycle does not re-implement a pass the pipeline already runs.

---

## Version 0.176-beta

### Summary of Changes (since `0.171-beta`):

#### 1. Optimizer Evidence Matrix
- **Inlining:** `tests/integration/positive/opt_inlining` proves a scalar helper and a GC-registering helper both disappear into their caller while a real loop survives.
- **Loop-Invariant Hoisting:** `tests/integration/positive/opt_licm` asserts LLVM's hoisting remark for arithmetic that the source computes inside the loop.
- **Loop Vectorization:** `tests/integration/positive/opt_vectorize_owned` requires a vectorized body and vector trip count for an owned-array loop.
- **Justified Absence:** `tests/integration/positive/opt_vectorize_managed_blocked` records that a managed loop keeps its safepoint poll and is therefore not vectorized.
- **Rationale:** PO-7 asks whether the generated IR gives LLVM enough information, not for hand-written replacements of LLVM's own passes. Each claim is now backed by an artifact on the optimized module.

---

## Version 0.171-beta

### Summary of Changes (since `0.168-beta`):

#### 1. Batched Root Publication
- **Frame Root Descriptors:** A function's root set is a compile-time fact, so the lowering emits one read-only descriptor per root set and publishes every root of an activation with a single `nrq_gc_root_register_frame` call instead of one call per slot. Descriptors are interned per module, so functions that register the same layouts share one table in `.rodata`.
- **One Barrier Region Per Activation:** Registration now reserves the frame's roots once and takes a single barrier region, with the whole descriptor validated before any slot is published. Per-slot duplicate counts, in-flight snapshot marking and root frames are unchanged, and a lone root keeps the direct call.
- **Evidence:** `tests/integration/positive/gc_frame_roots` asserts one call and one shared descriptor for four slots of three layouts; `GcTest.FrameRootRegistrationHandlesWideMixedLayouts` covers frames that cross the inline layout storage.

#### 2. Shared Layout Descriptors Across Array Paths
- **One Descriptor Per Element Layout:** Managed array allocation, array root registration and array reference clearing each materialised the element layout in a stack array, one store per offset, on every call. All three now pass the interned read-only descriptor the scalar root paths use, so a managed array allocation site carries no layout stores at all.

#### 3. Optimized-Module Test Assertions
- **Assertions on LLVM Output:** Integration fixtures can now assert on the module that leaves the LLVM pipeline (`expect_optimized_ir_*`), which is the only way to prove that a transform LLVM owns actually ran rather than merely being reachable.
- **Unreachable Code Evidence:** `tests/integration/positive/opt_unreachable_code` proves a folded-away branch and the helper reachable only through it are both absent from the optimized module.

---

## Version 0.168-beta

### Summary of Changes (since `0.162-beta`):

#### 1. Write Barrier and Codegen Protocol Optimization (PO-24)
- **Forward References Through Write Barrier:** References stored through the write barrier are now forwarded directly to the remembered set without an intermediate root-store round trip, eliminating a redundant store and a cache-line bounce per mutator store.
- **Single Read-Only Descriptor Per Reference Layout:** The backend emits one shared descriptor per distinct reference layout instead of one per store site, reducing metadata size and enabling the collector to switch on the descriptor rather than recomputing layout.
- **Elide Redundant Mutator Regions:** A call that does not change the active mutator region now reuses the caller's established region instead of emitting a fresh `mutator_begin`/`mutator_end` pair, cutting protocol overhead in call chains.
- **Integration Assertions for GC Protocol:** Added integration test assertions that verify the exact sequence of emitted GC protocol operations (`mutator_begin`, `mutator_end`, `root_store`, `root_load`, `write_barrier`), so regressions in protocol emission are caught at test time rather than through runtime misbehavior.
- **Documentation:** Added `reports/performance/po-24-write-barrier-codegen.md` with the transformation description, IR evidence, and remaining opportunities.

#### 2. Compiler-Assisted Runtime Optimizations (PO-25)
- **Publish Managed References Directly to Roots:** The lowering path for managed references now emits a root-store at the definition site and skips the intermediate buffer, so the collector sees the reference immediately without a second store.
- **Initialize Parameter Roots with the Barrier Itself:** A reference parameter is rooted by the incoming barrier write rather than by a separate `root_store`, eliminating a redundant operation at function entry.
- **Unpin the Just-Published Reference:** After a managed reference is published to its root slot, the lowering immediately emits a matching `root_unpin` for the temporary, keeping the pin table accurate without a dedicated cleanup pass.
- **Throttle Safepoint Polls with a Countdown:** The safepoint poll on loop back-edges now runs once per 64 iterations via a register-promoted countdown instead of a branch on every iteration, removing the poll overhead from hot loops.
- **Documentation:** Added `reports/performance/po-25-compiler-assisted-runtime.md` with the lowering transformations, IR evidence, and remaining opportunities.

---

## Version 0.162-beta

### Summary of Changes (since `0.158-beta`):

#### 1. Write Barrier and Codegen Protocol Optimization (PO-24)
- **Forward References Through Write Barrier:** References stored through the write barrier are now forwarded directly to the remembered set without an intermediate root-store round trip, eliminating a redundant store and a cache-line bounce per mutator store.
- **Single Read-Only Descriptor Per Reference Layout:** The backend emits one shared descriptor per distinct reference layout instead of one per store site, reducing metadata size and enabling the collector to switch on the descriptor rather than recomputing layout.
- **Elide Redundant Mutator Regions:** A call that does not change the active mutator region now reuses the caller's established region instead of emitting a fresh `mutator_begin`/`mutator_end` pair, cutting protocol overhead in call chains.
- **Integration Assertions for GC Protocol:** Added integration test assertions that verify the exact sequence of emitted GC protocol operations (`mutator_begin`, `mutator_end`, `root_store`, `root_load`, `write_barrier`), so regressions in protocol emission are caught at test time rather than through runtime misbehavior.
- **Documentation:** Added `reports/performance/po-24-write-barrier-codegen.md` with the transformation description, IR evidence, and remaining opportunities.

---

## Version 0.158-beta

### Summary of Changes (since `0.153-beta`):

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

## Version 0.153-beta

### Summary of Changes (since `0.147-beta`):

#### 1. Runtime Synchronization and GC Protocol Optimization (PO-23)
- **Per-Mutator Barrier Counters:** Each mutator publishes its own barrier depth on its own cache line, so a store barrier never invalidates another thread's counter and the collector polls registered mutators directly instead of maintaining a shared reader count.
- **Flat Root-Slot Index:** Active root slots live in one open-addressing table per thread instead of a hash node per slot, so a registration or a frame-end removal touches a single cache line and never allocates.
- **Mutex-Free Root Fast Paths:** Registration and frame end skip the shadow-stack mutex because the collector can only read a stack while its owner is parked, and reference diagnostics is the one other reader, so those paths take the mutex only while diagnostics are enabled.
- **Buffered Handoff Pins:** A managed return records a signed transfer-pin delta in the returning thread's own buffer, folded into the pin table when a cycle is prepared, so helper-heavy call chains no longer serialize on the collector mutex.
- **Single Published Store-Barrier Switch:** Generational tracking publishes one derived switch with conservative ordering, and aggregate stores read it once per store instead of twice per machine word.
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