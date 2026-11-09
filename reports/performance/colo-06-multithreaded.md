# ColO-06 --- Multithreaded collector stress

**Stage:** 7 of the ColO Roadmap 2 sequence
**Base / result:** new diagnostic mode only; production runtime unchanged
(`f8040d4`).
**Date:** 2026-11-09

## 1. Workload (`gc_harness` modes `mt` / `mt1`)

`benchmarks/bottleneck_discovery/gc_harness/main.cpp`, `run_multithreaded`:

* N mutator threads each `nrq_gc_mutator_enter()`, open a root frame, allocate
  16-byte traced nodes and keep every 7th one registered as a root in a
  pre-reserved vector (no reallocation, so root slot addresses stay stable), then
  `nrq_gc_root_frame_end()` and `nrq_gc_mutator_leave()`.
* The main thread is a registered mutator and calls `nrq_gc_collect()` every
  20 ms while the workers are still allocating, so park-before-scan runs against
  mutators that are actively allocating and publishing TLABs.
* Per-allocation latency is sampled with `steady_clock`; the maximum is reported.
* Total work is constant across thread counts: `mt1` = 1 thread x 2,000,000,
  `mt` = 4 threads x 500,000 = 2,000,000. `CHECKSUM` (sum of the index values)
  verifies that every allocation happened.

## 2. Results (3 runs each, production flags, `gc_harness_d23a2b4.exe`)

Raw: `build/bottleneck_discovery/mt/{mt1,mt4}_run{1..3}.txt`.

| mode | threads | total allocs | elapsed ms (min--max) | ns / allocation | max single-allocation pause |
| --- | --- | --- | --- | --- | --- |
| `mt1` | 1 | 2,000,000 | 366.1--395.7 (med 394.1) | 183--198 | 2.59--3.38 ms |
| `mt` | 4 | 2,000,000 | 497.5--524.8 (med 506.0) | 249--262 | 4.80--9.99 ms |

Throughput on the same total work: 1 thread 5.07 M allocations/s, 4 threads
3.95 M/s. **Four threads are 22 % slower than one thread at equal work**; there is
no parallel speedup to report. Each collection count is 2 in every run, because the
workers finish in under 600 ms; `LIVE_AFTER 0` after the final collection and the
expected `CHECKSUM` in all runs.

## 3. Interpretation

`[MEASURED]` The negative scaling is the honest result. The allocation fast path is
per-thread (TLAB bump), but every TLAB refill, page reservation, accounting flush
and collection entry takes `gc_mutex`, and the collector parks all mutators twice
per cycle (`begin_cycle` prepare and resume). At 16-byte objects a TLAB of 64 KiB
only covers ~4,000 allocations, so refills are frequent enough for the shared lock
to dominate. That is a `[HYPOTHESIS]` for the cause; this stage did not profile it.

The roadmap's instruction was not to weaken synchronisation to improve a
single-thread result, and no synchronisation was changed. This stage adds
measurement only.

The maximum single-allocation pause (~3 ms / ~5--10 ms) bounds the longest
observed mutator stall; it is a proxy for the collection park/wait, not a direct
measurement of `gc_mutex` hold time or barrier wait.

## 4. Validation coverage

| Property | Where it is validated |
| --- | --- |
| Park-before-scan handshake against active allocators | this workload (collections during allocation) and `ConcurrentCollectionRequestsDoNotDeadlock`, `CollectionMarksIncrementallyWhileMutatorsRun` |
| TLAB publication at collection | this workload (`LIVE_AFTER 0` after collecting everything, no lost objects) and `ThreadLocalBumpAllocationsPublishAtCollectionSafepoints` |
| Root-frame LIFO per thread | this workload (per-thread frames, stable slots) and `NestedRootFramesWithConcurrentCollection`, `DeeplyNestedRootFramesRemoveOnlyOwnedRoots` |
| Mutator registration/unregistration | this workload (enter/leave per thread) and `ThreadExitPublishesPendingStateBeforeRegistryRemoval` |
| Shutdown while threads are registered | unit test `ThreadTerminationWithActiveRootFramesRemovesDanglingRoots`; not exercised by the harness (workers leave cleanly) |
| Remembered-set updates under mutation | unit tests `RootStoreSynchronizesConcurrentMutationAndCollection`, `OldToYoungStoreBarrierKeepsYoungObjectAliveDuringMinorCollection`; the harness allocates unrooted nodes and performs no old-to-young stores, so it does **not** exercise the remembered set |
| Pin-transfer folding | unit tests `TransferPinFallbackSettlesBufferedDeltasFirst`, `BufferedPinsAndTlabObjectsSurviveCollectionBeforeRootEnumeration`; not exercised by the harness |
| Fast-barrier entry/exit | `[UNVERIFIED]` for the harness; the barrier depth handshake is covered by `wait_for_fast_barriers()` tests only indirectly |

Race/concurrency diagnostics: MSVC has no ThreadSanitizer; `[UNVERIFIED]` under a
race detector. The concurrent unit tests plus this stress are the available
evidence.

## 5. What is not measured

* TLAB refill contention, barrier wait and `gc_mutex` hold time: no counters are
  exposed; measuring them would require runtime instrumentation (debug events or a
  new ABI), which was not added.
* Scaling at thread counts above 4.
* Collections are rare (2 per run) because the workload is too short; a longer run
  would spend more time in the collector, but the roadmap's 8-run discipline was
  applied to the canonical benchmarks, and this mode is a stress/correctness
  workload first. `[UNVERIFIED]` for collector-bound multithreaded throughput.

## 6. Recommendation

**Accept as a stress workload, with the scaling result reported as measured.** It
validates the handshake and publication paths and shows the collector does *not*
scale on this allocation-heavy workload; that is a truthful finding to carry into
the final report, not something to hide behind single-threaded numbers.
