# Nirqoba — Comprehensive Performance Investigation

**Task:** `tasks/CPInBD.md`
**Investigated tree:** `fa799e8` (investigation commit) on top of `904aee0`;
working version 0.266-beta
**Date:** 2026-11-08/09
**Configuration:** production runtime, MSVC x64, LLVM/MLIR 23.1.4, `-O3`
**Companion documents:**
`reports/bottleneck_discovery/00…07`,
`reports/performance/performance_cost_model.md`

Evidence labels are used exactly as defined in `00-method-and-provenance.md`.

---

## A. Executive Summary

1. **The reported collection regression is real and is one commit.** `e830b65`
   ("sweep pages in address order") replaced a heap-object list walk with a
   per-bit walk of the object-start bitmap. On a 2.1 M-node graph the sweep tests
   28.65 M zero bits per collection; sweep rose 25.1 → 87.5 ms and total
   collection 86 → 197 ms. [MEASURED]
2. **The task's version framing is wrong.** The "0.257" numbers were produced
   before the 0.260 commits, and the 0.260 release artifact is a stale copy of
   the 0.257 run. No 0.260 release was ever benchmarked. [SOURCE]
3. **Same source, two runs: 146 ms and 85 ms.** `98a1099` and `c384cee` are
   identical code; the committed test-02 COLLECT medians differ by 1.7×. Only
   interleaved repeats can settle version claims. [MEASURED]
4. **The regression has a ~20-line fix**, not an architectural cause: iterate
   set bits with `std::countr_zero` (the idiom already in `gc.cpp:687-710`) and
   charge bitmap words to the slice budget. Projected 225 → ~130 ms on test 02
   and 60 → ~33 ms on test 03. [PROJECTION]
5. **The header is 96 bytes, not 128.** A 24-byte node has a 120-byte span,
   80 % metadata, 3,841 pages / 240 MiB for 2.1 M nodes. 64 bytes is low-risk;
   40 bytes gives exactly one cache line per object and is the best total
   design. [MEASURED/PROJECTION]
6. **Allocation is not the main gap in these workloads.** Warm bump is 23.2 ns;
   the collection in test 02 costs 110 ns per live object *per cycle*, and the
   harness runs five cycles. Cold allocation adds 30.7 ns of page faults.
   [MEASURED]
7. **Builder protocol is the largest construction component:** 63 ns per
   two-slot frame, 33 % of the ~193 ns/node best-case build. The shipped J3
   fusion is worth ~2–3 ns. [MEASURED/PROJECTION]
8. **Hidden ceiling:** the runtime is a non-LTO archive whose declarations have
   no memory effects, so every runtime call is an optimisation barrier and a
   call. Eleven of twelve cores are idle during a serial 66 ms mark. [SOURCE]

---

## B. Repository and Version Verification

| Fact | Verified value |
|---|---|
| Branch / HEAD | `fa799e8` (investigation) on `904aee0` |
| Working version | 0.266-beta (`CMakeLists.txt:10`) |
| Release 0.257 | `98a1099`, 02:57 |
| Pre-ColO tree | `c384cee` = `98a1099` code (benchmark-only commit), 03:25 |
| ColO commits | `f1d2b42` 10:23, `1ca3c37` 10:26, `e830b65` 10:33, `1646b93` 10:36, `0708f75` 10:37 |
| Release 0.260 | `86cfc43` 10:40 — carries `benchmark_results.json` generated **03:13** |
| Post-release | `5d62abf` 10:43 (narrow marking), `904aee0` 11:16 (docs) |
| Compiler binary | `build/msvc-dev/bin/nirqoba.exe`, built 11:14 |
| Runtime configs | `nirqoba_runtime` / `nirqoba_runtime_debug`, one source list |
| Linker | `lld-link`, no `/LTCG` |
| Benchmark metadata | all `compilers: unknown`, `memory_bytes: null` |

The full provenance chain, including the stale-artifact proof, is in
`00-method-and-provenance.md` §2.

---

## C. Current Performance Architecture

```
source .nrqba
  └─ TIR → MLIR (TirToMlir) → LLVM IR → LLVM -O3 → COFF
       every managed object: frame register, allocate_rooted, root_store,
       pin_transfer, unpin, frame end
  └─ lld-link + nirqoba_runtime.lib (prebuilt, no LTO, no /GL)
       └─ GC heap: 64 KiB aligned pages, 64 KiB TLABs, 96 B header,
          start bitmap (1 bit / 8 B), free-span list per page,
          span_pages index, remembered set, buffered transfer pins
       └─ collector: serial, incremental mark (64 KiB slices) +
          sweep (256 KiB slices), page-ordered bitmap sweep,
          full and young-only cycles
```

A full description with line references is in `05-hidden-bottlenecks.md` and
`03-collector-colo.md` §1.

---

## D. Benchmark Analysis

Committed medians (ms). Test 02 = 2.097 M live nodes; 03 = 1 M reclaimed; 01 = 1 M retained.

| Test | Metric | 0.257 `98a1099` | pre-ColO `c384cee` | 0.266 `fa799e8` | C++ at 0.266 |
|---|---|---:|---:|---:|---:|
| 02 | BUILD | 665 | 478 | 558 | 33.2 |
| 02 | COLLECT | 146 | 85 | **234** | 36.8 |
| 02 | TRAVERSE | 17 | 15 | 16 | 15.0 |
| 03 | BUILD | 167 | 102 | 135 | 6.5 |
| 03 | COLLECT | 27 | 25 | **88** | 5.9 |
| 01 | ALLOC | 248 | 137 | 140 | 31.3 |
| 01 | COLLECT | 27 | 25 | **77** | 18.6 |

Harness defects (1 ms clock, single build sample, no metadata, no memory
measurement, 5 collections per round) are analysed in
`06-comparative-baseline.md` §1–2 and `05-hidden-bottlenecks.md` H9. The
66 %-of-runtime collection share in test 02 is a property of the harness's five
identical collections, not of a representative program.

---

## E. AO Investigation

* Only the GC heap is on the benchmark path; `allocator_windows.cpp` is not
  (`01-allocator-ao.md` §1).
* Warm allocation 23.2 ns, cold 53.9 ns; the 30.7 ns delta is page faults.
* TLAB = 64 KiB, refill every 546 objects, free-run refill, 2,048-page recycle
  pool — all present.
* The reuse/fragmentation path is **never executed** by tests 01–03.
* AO-01's "128-byte" arithmetic is wrong; the span is 120.
* Biggest AO lever is footprint (header), not instructions.
* Ranked candidates A1–A6 in `01-allocator-ao.md` §7.

---

## F. BO Investigation

* 10 runtime calls per internal node; 68-instruction `build_tree`.
* Frame protocol 63 ns = 33 % of the best-case 193 ns build.
* `root_store` 1.6 ns, pin+unpin 5.1 ns: the J3 fusion saves ~2–3 ns, not the
  claimed barrier merge.
* Dead null store in every activation because the runtime declaration has no
  memory effects.
* Static descriptor validation is removed in production; the descriptor memo is
  correspondingly dead.
* Ranked candidates B1–B6 in `02-builder-bo.md` §7.

---

## G. ColO Investigation

* Stage split per collection (02): begin 0.7, mark 66.6, sweep 96.6 ms; (03):
  begin 11.4, mark 0.0, sweep 31.8 ms. Stages account for 77–103 % of the
  measured collection (median 85 %).
* Regression commit `e830b65`; mechanism is 15 bitmap tests per object, 2.18 ns
  each.
* Byte slicing cut lock rounds 34× with no time change.
* Narrow marking had no measurable effect.
* Minor-collection and page-index changes are unexercised.
* Fix and acceptance gates: `03-collector-colo.md` §5/§8; implementation
  roadmap: `project_context/ColO_ROADMAP_2.md`.

---

## H. Object Header Investigation

Measured layout (96 B, align 8, span 120), field-by-field necessity, six
compiled candidate layouts (96/64/48/40/32/24), what each requires, and the
conclusion that **40 bytes is the fastest total architecture** are in
`04-object-header.md`. Key numbers: 2.1 M nodes → 3,841 pages / 240 MiB at 96 B;
2,048 pages / 128 MiB at 40 B; 29.4 M vs 15.3 M bitmap positions per page set.

---

## I. Hidden Bottleneck Findings

Twelve items in `05-hidden-bottlenecks.md`. The five that matter most:

1. Non-LTO runtime archive makes every runtime operation a call (H1).
2. Runtime declarations lack memory effects → spills, reloads, dead stores (H2).
3. `begin_cycle` carries 11.4 ns/object of deferred construction (H3).
4. Mark is 32.5 ns/object and serial on 12 threads (H4).
5. Benchmark harness cannot resolve < 25 % and records no provenance (H9).

---

## J. Unfinished Work Audit

`07-unfinished-work-audit.md`: five ColO items, three of them unexercised; two
BO claims unverified; the J3 fusion shipped untested inside the investigation
commit; seven earlier-report claims now wrong; nine missing test paths; five
benchmark-artifact defects.

---

## K. Root Cause Analysis

**Why did collection regress?**

* Proximate: `sweep_chunk_locked` tests each of 8,192 bitmap bits per page.
* Design: page-ordered sweep replaced the heap list; the start bitmap became the
  only enumeration source, and it was walked as a bit array rather than a set.
* Process: 0.260 shipped without a benchmark of its own code, so a 2.3–3.9×
  regression passed the release gate. The committed artifact was seven hours
  older than the change.
* Contributing: the suite has no sweep-cursor test and no harness that can see a
  62 ms change as anything other than "valid".

**Why is build slow?** Per-activation root frame protocol (63 ns) + call-based
runtime (no LTO) + 3× larger objects than the comparison tracks.

**Why is mark 32 ns/object?** Two `find_object_locked` hash+bitmap probes per
object, each a dependent cache miss, on a single thread. [HYPOTHESIS, no PMCs]

---

## L. Recommended Optimization Roadmap

**Phase 0 — correctness and regression (this round, `ColO_ROADMAP_2.md`):**

1. ColO-00 baseline and source audit.
2. ColO-01 set-bit bitmap sweep (expected: removes the regression).
3. ColO-02 explicit bitmap-word work budget (bounded lock hold).
4. ColO-03 free-run/reclamation tests.
5. ColO-04 minor/generational workload and correctness.
6. ColO-05 fragmentation/reuse benchmark.
7. ColO-06 multithreaded stress.

**Phase 1 — cheap footprint and protocol wins (evidence already collected):**

8. `ReferenceLayout` 32 → 8 bytes (biggest single field).
9. Header 96 → 64, then evaluate 40 (one cache line per object).
10. Per-TLAB accounting to remove the 11.4 ns/object cycle-start publish.
11. Runtime memory-effect attributes on declarations.

**Phase 2 — architectural (needs its own design and budget):**

12. Inlined allocation fast path / runtime ABI split.
13. Parallel marking with per-thread worklists.
14. Frame fast path without per-slot `RootInfo` copies.
15. Page-run allocation to remove over-aligned `operator new` waste.

**Phase 3 — measurement integrity:**

16. Microsecond clock, repeated builds, min/median/max, memory sampling,
    recorded compiler versions and binary hashes; interleaved A/B runs.

Do not start Phase 1 item 9 before item 8: the layout field is the
largest and least risky part of the header, and it changes the arithmetic the
rest of the header work depends on.

---

## M. Performance Acceptance Criteria

A change is accepted only if all hold:

| Gate | Threshold |
|---|---|
| Correctness | full unit + integration suite passes in **both** runtime configurations; output byte-identical on the corpus |
| Collection (test 02) | median ≤ 140 ms over 8 interleaved runs, and no run > baseline median |
| Collection (test 03) | median ≤ 40 ms over 8 interleaved runs |
| Build | no regression beyond run-to-run spread (currently ±25 %); if a change targets build, ≥ 5 % median improvement on 8 runs |
| Traversal | within 1 ms (clock resolution) |
| Memory | no increase in peak working set without a stated trade-off |
| Lock hold | max sweep chunk hold not increased; bounded on sparse pages |
| Evidence | commit hashes, flags, binary hashes, raw samples, min/median/max recorded |
| Claims | every performance statement labelled and tied to a run |

A neutral result is acceptable and must be reported as neutral. A benchmark
result produced by changing the workload, flags, or warmup is not a result.

---

## N. Unresolved Questions

1. **Where do the 32.5 ns/object of mark actually go?** Needs hardware counters
   or a targeted probe (hash vs bitmap vs worklist vs cache miss).
2. **How much memory does over-aligned `operator new` really commit?** Working
   set was never sampled.
3. **Is the free-run reuse path correct and fast under fragmentation?** No
   workload exists.
4. **Does young-only collection behave correctly and help?** No workload, no
   test evidence.
5. **What is the real cost of `ReferenceLayout` overflow** (>2 refs)?
6. **How much of build is cache miss vs protocol?** The measured components sum
   to ~126 of 266 ns/node; ~140 ns is unattributed.
7. **What is the multithreaded behaviour** of TLAB publication, barrier wait,
   and `gc_mutex` refill contention?
8. **Is Java's 2.6 ns allocation reproducible on this machine with a committed
   heap?** It is a JIT/commit artefact as much as an allocator result.
9. **Would PGO/aggressive IPO move the build numbers?** Implemented, never
   measured on these workloads.
10. **Why did the same code measure 146 ms and 85 ms?** Thermal state, heap
    layout, or binary difference; no run recorded enough environment data to
    tell.
