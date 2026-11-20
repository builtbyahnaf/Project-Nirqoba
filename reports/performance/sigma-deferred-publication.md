# Nirqoba — Sigma: Deferred Publication (0.621-beta + `5b667b6`)

**Scope:** the O(N) preparation walk that dominated collection of a dropped graph, and what
replaced it
**Status:** implemented and verified; Tau (evacuation), Upsilon (bulk region reclamation) and Phi
(region pacemaker) are **not** part of this change
**Date:** 2026-11-20
**Compiler:** `nirqoba compiler version 0.621-beta (Phase 9)`, RelWithDebInfo and Development
**Commits:** `5b667b6` (runtime), `cb86626` (tests)

---

## 1. The measurement that started it

`prepare_cycle_locked` publishes every mutator's pending TLAB ranges before a cycle marks
anything. `publish_one_range_locked` walked each range object by object: read the header to learn
the layout, compute the span, set the object-start bit, read the state word for the young flag,
and update the page counters. For 1,000,000 dead objects that is 4 ms per cycle, and it is the
whole of the collection cost in Tests 01 and 03.

Measured at `301f10b` with `NRQ_GC_TLAB_DIAG=1`, 8 cycles per process:

| Test | `publish_ms` | `prepare_ms` | `sweep_ms` | `COLLECT_MS` |
|---|---:|---:|---:|---|
| 01 managed-object allocation | 32.60 | 32.66 | 0.76 | 3–7 |
| 03 cyclic graph reclamation | 34.96 | 35.04 | 0.78 | 5 |

The sweep was already cheap because an all-dead page is released from its counters. Everything
above it was the publication walk reading memory it was about to free.

## 2. What changed

Two ideas, and the second is what makes the first exact.

**A publication window is one page.** `flush_window_for_page_switch` closes the open window in
`install_tlab` and `install_h0_tlab` whenever the allocator moves to another page. A window is
therefore the objects one page's counters describe, and `fast.pending_count` /
`fast.pending_bytes` — which the compiler already maintains inline on the allocation fast path —
are that page's object count and payload bytes. `apply_window_locked` applies them with two adds
and the usual statistics updates. No object is read, so the accounting is exact *at publication*
rather than at some later point; that is what keeps `nrq_gc_get_stats()` exact at every instant,
which is the contract several tests pin (`UnpublishedTlabAllocationsRemainVisibleInStats`,
`StatisticsStayExactWhileAnotherThreadAllocates`).

**Discovery is deferred.** The window's ranges are recorded on the page as up to eight
`PendingStartRun` intervals (`record_window_runs_locked`), and the start bits are written by
`materialize_page_starts_locked` the first time a reference resolves into the page — from
`find_object_locked`, `find_child_in_page_locked`, or the live-object enumeration. A page that
nothing reaches is never walked at all: the sweep classifies it from its (now exact) counters and
releases it in O(1). A page that marking does reach materialises once, with the same header-driven
span walk the eager path used, so a page whose runs interleave payload sizes decodes exactly as
before. The fallbacks are explicit: born-this-cycle ranges (allocated during a cycle), exact-index
pages, a window whose ranges do not share one page, and a page whose run list is full all publish
eagerly.

## 3. Result

Same host, same session, `--repeats`-equivalent rounds (five measured rounds per process, six
processes for Test 01):

| Test | Metric | Before | After | Target |
|---|---|---:|---:|---:|
| 01 | `ALLOC_MS` | 4 (3–5) | 4 (3–5) | < 5 |
| 01 | `COLLECT_MS` | 3–7 | **0–1** | **< 1** |
| 02 | `BUILD_MS` | 20 | 19–20 | < 22 |
| 02 | `TRAVERSE_MS` | 5 | 4–6 | < 7 |
| 02 | `COLLECT_MS` | 27 | 28–29 | < 35 |
| 03 | `BUILD_MS` | 4 | 4–5 | < 6 |
| 03 | `COLLECT_MS` | 5 | **0–1** | **< 1** |

Diagnostics after the change, 8 cycles per process:

| Test | `publish_ms` | `prepare_ms` | `sweep_ms` |
|---|---:|---:|---:|
| 01 | 0.17 | 0.08 | 0.64 |
| 03 | 0.13 | 0.07 | 0.80 |

Test 02's collection moved from 27 to 28–29 ms because the walk it used to pay during `prepare`
now happens during marking, once per page, when the first reference resolves into it. The
workload is unchanged and the target is met with margin; the honest reading is "the same walk,
paid where it is needed" rather than a saving on that test.

## 4. Verification

- **Unit:** 478/478 in `build/msvc-release` and 478/478 in `build/msvc-dev`, including three new
  tests (`tests/unit/gc_lazy_publication_test.cpp`): a dropped 40,000-node chain is collected
  without reading its objects while `live_objects` and `collected_bytes` stay exact; a pinned
  chain of 20,000 nodes survives and remains traversable through bits that were never written;
  a page that receives four windows keeps exact counters.
- **Integration:** `python tests/run_tests.py` with the memory-model suite included — 626/626 in
  `build/msvc-release` and 626/626 in `build/msvc-dev`.
- **Configuration parity:** `scripts/check_runtime_check_parity.py` reports 158 invariant checks
  present in both configurations; `scripts/compare_configuration_equivalence.py` reports 266
  agreed, 4 skipped, 0 failed against each compiler.
- **Stress Test 2:** the three programs were rebuilt with the same compiler and run directly; the
  numbers above are medians of the runs shown.

## 5. What this does not claim

It is not the region work in the handoff. No object is evacuated, no forwarding pointer exists,
no region is reclaimed as a unit, and the pacemaker does not poll region utilisation. The
`GcRegion` geometry and the page-per-region routing from `3940b9b` / `301f10b` are what the
reclamation path already uses, but the survivor-evacuation phase (Tau) and the region-utilisation
trigger (Phi) are still open, and this report makes no claim about them. `COLLECT_MS` for Tests
01 and 03 is met by removing the publication walk, not by evacuation.
