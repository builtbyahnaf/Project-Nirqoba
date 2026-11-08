# Why Nirqoba Still Cannot Beat Java On Stress Test 2

**Status:** analysis of the committed artifact; no benchmark was run for this report
**Source:** `benchmarks/stress_test_2/benchmark_results.json` (2026-11-08 03:13, commit `c384cee`)
**Domain:** construction path (allocation + frame protocol + barriers), not collection
**Companion:** `project_context/ColO_ROADMAP.md`, `reports/performance/bo-02-builder-status.md`

---

## 1. The numbers

Medians of 15 round samples, same machine, same session.

| Test | Phase | nirqoba | rust | cpp | c | java |
|---|---|---:|---:|---:|---:|---:|
| 01 managed_object_allocation | ALLOC | **137.0 ms** | 32.5 | 29.3 | 27.8 | **2.48** |
| 01 | TRAVERSE | **9.0** | 6.08 | 5.98 | 5.93 | 1.88 |
| 01 | COLLECT | **25.0** | 19.1 | 17.6 | 17.6 | 6.83 |
| 02 mark_and_sweep | BUILD | **478.0** | 24.3 | 25.7 | 24.2 | **16.0** |
| 02 | TRAVERSE | **15.0** | 7.53 | 12.8 | 13.0 | 5.52 |
| 02 | COLLECT | **85.0** | 36.1 | 33.9 | 35.7 | **73.1** |
| 03 cyclic_graph_reclamation | BUILD | **102.0** | 6.18 | 6.73 | 6.84 | **5.72** |
| 03 | COLLECT | **25.0** | 5.32 | 6.99 | 7.08 | 42.8 |

Three facts decide the question:

1. **Collection is not the problem.** On cyclic garbage nirqoba collects 1.7× faster
   than HotSpot. On a fully live heap it is 1.16× slower. ColO-01 (C1–C6) already
   addressed the parts of that gap that are structural.
2. **Construction is 30× off, and it is the whole suite.** `BUILD`/`ALLOC` is where the
   four other implementations are 4–55× faster.
3. **`managed_object_allocation` against java is not a pure allocation comparison.**
   Java's 2.48 ms for 1 M nodes is 2.5 ns per node *including* zeroing. A 24-byte
   contiguous object stream at 2.5 ns is ~10 GB/s of store traffic, which is what a
   JIT-issued TLAB bump plus the field stores can do with escape analysis still keeping
   the chain alive (`sink`). It is a fair number for HotSpot, but it is a best case.

The comparison that actually has to be won is `mark_and_sweep` BUILD: 478 ms for
2 097 151 nodes = **228 ns ≈ 700 cycles per node**, against 8 ns for java and 12 ns for C.

---

## 2. Where those 700 cycles go

Emitted code for `build_tree` at `-O3` (from `--emit=llvm`, current tree):

```llvm
%6  = tail call i64 @nrq_gc_root_frame_begin()
store ptr null, ptr %5
store ptr null, ptr %4
store ptr %5, ptr %3 ; store ptr %4, ptr %3+8      ; the frame's slot table
call void @nrq_gc_root_register_frame_static(ptr %3, i64 2, ptr @nrq_gc_roots_...)
%8  = call ptr @nrq_gc_allocate_array_typed_rooted(ptr %5, i64 1, i64 24, ptr @nrq_gc_layout_0_8, i64 2)
call void @nrq_gc_root_store(ptr %4, ptr %8)
store i64 %1, ptr %8+16
...
%14 = call fastcc ptr @build_tree(i64 %12, i64 %13)
call void @nrq_gc_unpin(ptr %14)
%15 = load ptr, ptr %4
call void @nrq_gc_root_store(ptr %15, ptr %14)
...
%22 = call ptr @nrq_gc_pin_transfer(ptr %21)
call void @nrq_gc_root_frame_end(i64 %6)
```

**Nine runtime calls per node**, every one across a translation-unit boundary
(the runtime is a static MSVC library, the caller is LLVM-emitted object code, and the
link step runs no cross-module optimization). None of them can inline, so each pays a
call, no shrink-wrapping, its own prologue and a full barrier region's atomics.

Estimated cost per call site from the code paths, labelled as estimates:

| Call | Per node | What it does | Estimate |
|---|---:|---|---:|
| `root_frame_begin` | 1 | TLS shadow-stack lookup, `frames` vector push | 25–40 cyc |
| `root_register_frame_static` | 1 (2 slots) | per slot: 2 open-addressing inserts into thread-local indexes | 80–140 cyc |
| `allocate_array_typed_rooted` | 1 | pointer-root index lookup, barrier region, 96 B header init, payload `memset`, accounting, TLAB publish check | 150–220 cyc |
| `root_store` | 3 | phase load, plain store, remembered-set test | 15–30 cyc each |
| `pin_transfer` / `unpin` | 3 | barrier region + 32-entry delta buffer scan | 25–40 cyc each |
| `root_frame_end` | 1 | per slot hash release + `pointer_roots` erase, vector pop | 60–100 cyc |

That is 350–470 cycles of runtime plus the memory traffic of a **128-byte object**
(96 B header + 24 B payload, 16-byte aligned): 2 M nodes is 256 MB against java's
48 MB and C's 48 MB. Everything HotSpot and the C versions spend is memory traffic and
field stores; everything nirqoba spends on top of that is protocol.

### Why Java wins this comparison

- `new Node()` is a **bump of a thread-local pointer** that the JIT inlines; there is no
  call, no header write beyond the 16-byte object header, and no barrier.
- **No per-call root protocol.** HotSpot's GC reads a stack map at safepoints; it does
  not register and release a root slot per function activation.
- **Headers are 12–16 bytes**, so the object stream is 4–5× smaller and fits in cache.
- Escape analysis and scalar replacement remove work the C versions do not remove, which
  is why java beats C on test 01.

### Why the C versions win

- A pool/arena bump: one add per object, an 8–16-byte header, no barrier, no roots.
- Their `COLLECT` is a real mark-and-sweep over the same objects, so the comparison is
  honest: they pay 17–36 ms to sweep 2 M objects, nirqoba pays 85 ms.

---

## 3. What is left, ranked

Ordered by (expected effect on BUILD) / risk. No timing is claimed; the ordering is
from the call structure above.

| ID | Change | Effect on the 700 cycles | Risk |
|---|---|---|---|
| **J1** | Inline the TLAB bump in emitted code: TLS cursor/end check, bump, header init, zero-fill, slow-path call on refill | removes the largest call (~150–220) and lets LLVM schedule the header stores | high (compiler ABI) |
| **J2** | One entry opens a frame *and* publishes its slots | removes a call, a barrier region and a mutex check per activation | medium |
| **J3** | Fuse `unpin` + `root_store` into one handoff call | removes 2 of 9 calls per node in recursion-heavy code | low |
| **J4** | Runtime as LLVM bitcode instead of a static library, so the linker can inline the GC entry points | would collapse most of the remaining calls | medium-high (build system) |
| **J5** | 96 → 48 B descriptor-pointer header (AO Stage 4) | halves object traffic; 12 header stores become ~6 | medium-high (heap format) |
| **J6** | Do not register a slot index entry for a compiler-trusted static frame; answer the pointer-root question at the allocation site | removes 4 hash operations per slot per activation (~80–140) | medium |
| **J7** | Reuse one root frame for a loop instead of one per iteration (where the language allows it) | removes the whole frame protocol from the loop body | high (semantics) |
| **J8** | Drop the null preamble and the redundant `root_store` where the slot is known null | 2–3 stores and 1 call per iteration | medium (definite-initialisation proof) |
| **J9** | Skip the zero-fill for definitely-initialised objects | one store per word of payload | medium |
| **J10** | Branchless TLAB refill and larger reservations per thread | amortises the slow path | low |

### What "beating java" actually requires

At 2.9 GHz, java's 8 ns per node is ~23 cycles; C's 12 ns is ~35 cycles. A 128-byte
object stream cannot reach 23 cycles: 256 MB at a realistic 15 GB/s is ~17 ms, i.e.
8 ns per node before any header store. So:

- **With the current 96-byte header, beating java on test 02 is not achievable by
  tuning.** The object footprint alone is at java's total time.
- **With the 48-byte header** the stream is 72 bytes → 144 MB → ~9.6 ms of traffic, and
  the remaining budget is ~30 cycles of protocol per node. J1 + J2 + J3 + J5 landing
  together makes that plausible. Any one of them alone does not.
- The honest target order is: **close to C first** (35 cycles), then java (23).

This is why the remaining steps of this track are construction changes, not further
collection tuning: ColO-01 took the collector to parity while the suite gap is emitted
call sequences and the header size.

---

## 4. Implemented from this report

| Step | State |
|---|---|
| J3 — fused return handoff (`unpin` + `root_store`) | see `0.266-beta` steps in `docs/VERSION_LOG.md` |
| J2 — one-call frame open and publish | see `0.266-beta` steps in `docs/VERSION_LOG.md` |
| J1, J4, J5, J6 | planned; each needs its own decision (J5 is AO Stage 4) |
