# ColO-03 --- Preserve and validate page-ordered reclamation

**Stage:** 4 of the ColO Roadmap 2 sequence
**Base / result:** tests only, on top of `9d5b6ac`; the stage commit adds two
tests (`FreedNeighbourSpansBridgeIntoOneRun` in this stage,
`MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage` in ColO-04) and no production
change.
**Date:** 2026-11-09

## 1. Audit of the reclamation path

`[SOURCE]` The path is: `release_span_locked(page, offset, span)` records a free
span, merges it with the span that ends exactly at `offset` (left) and the span
that starts exactly at `offset + span` (right), and deletes both neighbours when
both exist; `take_free_run_locked(desired, minimum, ...)` first-fits over the
free spans in page order and takes the whole run when it is no larger than
`desired`, so a TLAB reservation can consume a reclaimed page region instead of
carving a fixed slice; `reserve_tlab_locked` tries `take_free_run_locked` *before*
it considers the bump tail or a new page, so a recycled run is preferred to
growth.

Findings that matter for preserving the design:

* Page ordering is preserved on the free side: every release is anchored at the
  page that owns the span, and pages are swept in ascending `gc_pages` order, so
  the free-run index never needs a heap-wide object list. `[SOURCE]`
* Bridging is order-independent: releasing the middle of three spans after both
  neighbours were released must join all three. This was not covered by a test
  before this stage. `[SOURCE]` (code) + new test (below).
* `total_free_spans` is maintained on both merge and take, so a bridge reduces the
  count by two and a take removes one; the free-run index cannot grow without
  bound on a heap that keeps releasing adjacent objects. `[SOURCE]`
* Page retirement and recycling (`retired_page_count`, `nrq_gc_recycled_page_count`)
  are driven from `prepare_cycle_locked`, not from `release_span_locked`; a page
  that becomes empty during a sweep is retired at the next cycle boundary and its
  storage is reused by `acquire_page`. `[SOURCE]`
* Rollback / out-of-order release is still supported: `rollback_reservation` in
  `allocate_locked` calls `release_span_locked` for a span that was just taken
  from a free run, i.e. a release that is not the reverse of a sweep. It is
  exercised by the typed-layout rejection and size-overflow tests. `[SOURCE]` +
  existing tests.

No replacement proof was needed, and none is claimed.

## 2. Coverage against the roadmap's list

| Required case | Test | Status |
| --- | --- | --- |
| Adjacent dead objects merge into one run | `MergedFreeRunsServeThreadLocalReserves` (a whole cohort dies) | existing, passing |
| Newly freed run bridges left and right neighbours | **`FreedNeighbourSpansBridgeIntoOneRun`** (new) | new, passing |
| Mixed object sizes and alignment | `BumpRegionsAlignMixedObjectSizesAndReclaimEmptyPages` | existing, passing |
| Allocation from a previously freed run | `MergedFreeRunsServeThreadLocalReserves`, `ReclaimedSpansAreReusedBeforePagesGrow`, `ReclaimedRunsCoalesceAndAreReusedWithoutGrowingThePageSet` | existing, passing |
| Page retirement and recycling | `EmptiedPagesAreRecycledAcrossCollections` | existing, passing |
| Rollback / out-of-order release | `RejectsInvalidTypedLayoutsWithoutPublishingAnObject`, `RejectsAllocationSizeOverflowWithoutChangingHeapStatistics` (rollback path in `allocate_locked`) | existing, passing |

## 3. The new bridge test

`tests/unit/gc_test.cpp`, `GcTest.FreedNeighbourSpansBridgeIntoOneRun`:

1. A 60,000-byte rooted filler occupies a page up to 60,096 bytes, leaving 2,080
   bytes of bump tail --- too little for the probe.
2. Three 1,024-byte objects are allocated after it: `first`, `middle` (rooted),
   `last`. Each span is 1,120 bytes, so all three fit in the same 65,536-byte page.
3. A collection frees `first` and `last`; the page now holds two 1,120-byte runs
   separated by the live `middle`.
4. `middle` is unrooted and a second collection frees it. A correct implementation
   joins all three spans into one 3,360-byte run.
5. A 3,000-byte probe (span 3,096) is allocated. It fits only in the bridged run:
   the page tail is 2,080 bytes, and no single pre-existing span is large enough.
   The test asserts the page count does not grow and `nrq_gc_span_reuse_count`
   increases, i.e. the probe was served from reclaimed space on the same page
   rather than from a new page. `[MEASURED]`

Failure mode if bridging were broken: the probe would take the bump tail of
another page or grow the page set, and `nrq_gc_page_count()` would increase --- the
assertion catches exactly that.

## 4. Commands and results

```
./build269.bat
./build/msvc-dev/tests/nirqoba_unit_tests.exe
```

* With the new test added and ColO-04's test not yet present: **191 passed**.
* With both: **192 passed**, 0 failed (`build/tmp/ut_final.txt`).
* The full suite takes ~4 minutes on this machine; no test was skipped or filtered.

## 5. Performance

No production code changed in this stage, so no benchmark is claimed. The
reclamation path's measured behaviour is covered by ColO-05
(`reports/performance/colo-05-fragmentation.md`), which exercises mixed live/dead
pages and reports `SPAN_REUSE`, recycled pages and page-set growth.

## 6. Limitations

* The bridge test asserts the observable outcome (same page, reuse counter), not
  the internal free-span list; a bridge that produced a correct allocation via a
  different path would pass. That is acceptable: the roadmap asks that the
  behaviour be validated, not that an implementation detail be pinned.
* No test forces a page to become empty *during* a sweep and then verifies its
  storage is reused in the same cycle; the existing recycling test covers the
  next-cycle boundary, which is when recycling happens by design.

## 7. Recommendation

**Accept.** The page-ordered design is intact, the previously untested bridge case
is now covered, and no production change was needed to make it pass.
