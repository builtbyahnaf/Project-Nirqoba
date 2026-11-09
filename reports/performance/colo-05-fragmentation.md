# ColO-05 --- Fragmentation and reuse workload

**Stage:** 6 of the ColO Roadmap 2 sequence
**Base / result:** new diagnostic mode only; production runtime unchanged
(`d23a2b4`).
**Date:** 2026-11-09

## 1. Why a new workload

`[SOURCE]` Stress Test 2 / 02 keeps every node alive for the whole measurement and
03 frees whole pages at once. Neither leaves *some* objects alive and some dead in
the same page across cycles, which is the shape that makes
`release_span_locked` / `take_free_run_locked` / TLAB refill do their work. The
existing harness modes therefore could not show whether the free-run path is
exercised at all.

The canonical stress tests were not modified.

## 2. Workload (`gc_harness` mode `frag`)

`benchmarks/bottleneck_discovery/gc_harness/main.cpp`, `run_fragmentation`:

* **Phase A (8 rounds x 40,000 allocations):** 16-byte nodes are allocated;
  every 8th is linked into a rooted list and every other node dies at the round's
  collection. Long-lived objects are therefore interleaved with churn across the
  same pages. Steady state after Phase A: 40,000 live, 169 pages, 70,505 span
  reuses already recorded.
* **Phase B (16 rounds x 40,000 allocations):** varied object sizes (16, 32, 64,
  128 bytes) are allocated; half the rounds keep a **contiguous cluster** of 500
  objects alive until the next round, the other half keep a **pseudo-random spread**
  (xorshift, 1 in 32). Everything kept in round N is released in round N+1's
  prologue, so release patterns are clustered and scattered across cycles.
* Allocations use the real trace callback (the single reference at offset 0), so
  marking and remembered-set work is exercised, not just the allocator.
* The final list and the last round's kept objects are released and a final
  collection must bring the live count back to zero (`LIVE_AFTER 0`).

`[MEASURED]` Instrumentation through public diagnostics only:
`nrq_gc_span_reuse_count`, `nrq_gc_page_count`, `nrq_gc_recycled_page_count`,
`nrq_gc_get_live_objects`, `nrq_gc_get_collection_count`.

## 3. Results (8 process runs, production flags)

Binary: `gc_harness_d23a2b4.exe`. Raw: `build/bottleneck_discovery/frag/run{1..8}.txt`.

| metric | min | median | max |
| --- | --- | --- | --- |
| `ALLOC` per 40,000-object churn round (ms) | 12.30 | **12.87** | 13.35 |
| `COLLECT` per churn round (ms) | 4.57 | **4.92** | 5.67 |

Per-round state (all runs agree within one page):

* Phase A ends at 40,000 live objects, **169 pages**.
* Phase B pages: **237--240** and flat across rounds 8--16; the page set does not
  keep growing after the live set stabilises at ~40,500--41,300 objects.
* **Recycled pages** (empty pages retained for reuse): 64--69 during churn --- the
  retirement/recycling path executes.
* **Span reuse** grows by **215,610** over the 16 churn rounds; every round's
  allocations are served from reclaimed runs, so `take_free_run_locked` and TLAB
  refill-from-run execute on every round. `[MEASURED]`
* Final collection returns `LIVE_AFTER 0`; the 41,000-object live set and all
  churn are reclaimed with no leak. `[MEASURED]`

Allocation cost per object in this workload is ~322 ns, versus ~7 ns for the
16-byte-only growth phase measured earlier while the page set was still growing
(`[MEASURED]`, same machine, preliminary run of the same mode before the steady-
state redesign). The difference is the fragmented refill path: each TLAB refill
searches the free-run index and the page set for a run large enough, instead of
bumping a fresh page. This is a diagnostic number, not a target: the workload is
designed to be hostile, not representative.

## 4. What is *not* measured, and why

* **Refill scans and free-run count/distribution:** the runtime exposes span-reuse
  and page counters but no free-run-list diagnostics, and adding them would be a
  production ABI change the roadmap does not request. The benchmark proves the path
  executes (215,610 reuses, 64--69 recycled pages) but not how many candidates a
  refill rejected. `[UNVERIFIED]`
* **Coalescing work:** not counted; the bridge test (`ColO-03`) proves correctness
  of the merge, and this workload exercises it, but no counter exists to attribute
  time to it.
* **Chunk lock-hold time:** `NRQ_GC_EVENT_SWEEP_CHUNK_NS` exists in the debug
  runtime; this harness is built with production configuration, so the event is
  compiled out. A debug-build measurement was not taken. `[UNVERIFIED]`
* **Peak heap:** not reported separately; `STEADY_PAGES 169` / `FINAL_PAGES 238`
  bound it (238 x 61,440 bytes of page storage, plus headers).

## 5. Commands

```
powershell -ExecutionPolicy Bypass -File benchmarks/bottleneck_discovery/gc_harness/build_and_run.ps1 -Rev HEAD
for r in 1..8: gc_harness_d23a2b4.exe frag > build/bottleneck_discovery/frag/run$r.txt
```

## 6. Regression analysis

No production code changed in this stage. The workload shows the reclaim path is
alive and self-limiting: page growth stops once the live set stops growing, and
the churn is served from reclaimed runs (reuse grows monotonically). It does not
show, and does not claim, that refill cost is optimal.

## 7. Recommendation

**Accept as a diagnostic benchmark.** It answers the roadmap's central question
("does the page index and free-run path actually execute?") with a measured yes,
records the numbers honestly, and explicitly lists the counters that do not exist
rather than inventing proxies.
