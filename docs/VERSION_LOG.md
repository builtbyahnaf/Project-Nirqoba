# Nirqoba Version Log

**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

> Note: The changes before `0.102-beta` were massive, so they were intentionally left out here.

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
