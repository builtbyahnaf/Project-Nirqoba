# ColO-02 --- Bound sweep work by actual work

**Stage:** 3 of the ColO Roadmap 2 sequence
**Base:** `25224b3` (`perf(gc): sweep only the set bits of a page's start bitmap`)
**Result:** `9d5b6ac` --- `perf(gc): bound a sweep chunk by bitmap words, not only object bytes`
**Date:** 2026-11-09

## 1. Problem statement

`[SOURCE]` After ColO-01 the sweep consumes only set bits, but a chunk was still
bounded only by *object bytes examined* (`kSweepSliceBytes`) and by
`kSweepObjectsPerChunk`. On a sparse page those two bounds can both be far from
the real cost of the chunk: a page with one surviving object per bitmap word still
requires touching every word up to that object, and a page range with almost no
survivors can make a "small" chunk walk megabytes of bitmap before its byte budget
is consumed. The roadmap requires the work budget to be expressed in the unit the
loop actually spends: bitmap words (and pages), not only objects.

A second gap: there was no way to observe per-chunk work or lock-hold duration, so
the claim "the chunk is bounded" could not be checked without a patched build.

## 2. What changed (`9d5b6ac`)

`runtime/src/gc.cpp`:

* `kSweepBitmapWordsPerChunk = 4096`. A sweep chunk now stops when it has examined
  4,096 bitmap words (32 KiB of bitmap, i.e. 262,144 start-bit positions) ---
  whichever of the object, byte and word budgets is reached first.
* A page is charged as a unit when the cursor enters it: the cursor accounts the
  words of the page it is on, so a page whose bitmap is dense and a page whose
  bitmap is nearly empty both cost their real word count, and the chunk cannot
  spend an unbounded number of page transitions.
* `NRQ_GC_EVENT_SWEEP_WORK` (`1u << 6`) reports per-chunk objects visited and
  bitmap words examined; `NRQ_GC_EVENT_SWEEP_CHUNK_NS` (`1u << 7`) reports the
  wall-clock duration a chunk held `gc_mutex`.
* Both events live in `runtime/include/nirqoba/runtime/gc_debug.h` and are only
  emitted when a listener selects them under `NRQ_RUNTIME_DEBUG_ENABLED=1`. The
  production configuration compiles them out; no event store, allocation or
  branch is added to the production chunk loop. `[SOURCE]`

`benchmarks/bottleneck_discovery/gc_harness/run_matrix.ps1` / `build_and_run.ps1`:
the extraction step now calls `%SystemRoot%\System32\tar.exe` explicitly, because a
`tar` earlier on `PATH` (bsdtar from Git for Windows) rejected the archive written
by `git archive` in this shell. Diagnostics only; no benchmark input changed.

Nothing about the sweep order, the cursor, the free-run path or the allocation path
changed.

## 3. Invariants and proof obligations

| Invariant | Status |
| --- | --- |
| A chunk that makes no progress cannot repeat forever | `[SOURCE]` every chunk iteration either advances the mask/word cursor or finishes a page; the word budget only lowers the amount of work per chunk |
| Lock hold is bounded by words, bytes, objects and pages | `[SOURCE]` all four bounds are enforced; page transitions are charged |
| Resume is exact | `[SOURCE]` cursor is unchanged in shape; ColO-01's restart proof applies |
| No debug cost in production | `[SOURCE]` events under `#if NRQ_RUNTIME_DEBUG_ENABLED`; production binary unchanged in code path |
| Sweep still processes every set bit exactly once | `[MEASURED]` stage probe below: 0 zero-bit iterations, set bits = objects |

## 4. Measurements

### 4.1 Interleaved A/B, 8 process runs each, alternating A/B per round

Same machine, same toolset, `/O2 /DNDEBUG`, `NRQ_RUNTIME_DEBUG_ENABLED=0`.
A = `25224b3` (ColO-01), B = `9d5b6ac` (ColO-02). Raw samples:
`build/bottleneck_discovery/raw/ab_{25224b3,9d5b6ac}_{02,03}_run{1..8}.txt`.

Binary SHA-256:
`a77635897e3f8b28a2ee61deab452c4b1bfb4c8fea16e7ceb1067fd335669ba4` (A),
`f48a2adf9e6e437bdeee52edfee6f2b69eeb5d4dcd01719f44f10a359c019b1a` (B).

`COLLECT` (ms, median of 5 cycles per process):

| mode | A min / med / max | B min / med / max | delta (B vs A) |
| --- | --- | --- | --- |
| 02 | 73.3 / **74.9** / 80.8 | 74.5 / **75.4** / 76.6 | +0.7 % |
| 03 | 23.2 / **26.2** / 27.8 | 21.4 / **24.6** / 27.5 | -6.1 % |

`BUILD` (ms, from the same processes):

| mode | A min / med / max | B min / med / max |
| --- | --- | --- |
| 02 | 271.2 / 288.9 / 314.3 | 277.3 / 285.0 / 325.5 |
| 03 | 54.3 / 56.1 / 57.6 | 53.5 / 55.6 / 56.7 |

The earlier, non-interleaved 8-run matrices on the same binaries:

| revision | mode | min | median | max |
| --- | --- | --- | --- | --- |
| `25224b3` | 02 | 74.1 | 77.3 | 86.2 |
| `25224b3` | 03 | 22.1 | 24.2 | 32.5 |
| `9d5b6ac` | 02 | 76.8 | 83.8 | 102.3 |
| `9d5b6ac` | 03 | 24.4 | 25.2 | 27.3 |

The non-interleaved pair suggested a 6 % regression on 02. The interleaved pair
does not reproduce it (+0.7 %, inside the run-to-run spread of the same binary),
so the apparent regression was machine drift: the two 8-run matrices were taken
at different times, not alternating. `[MEASURED]`

### 4.2 Stage probe (`gc_stage_9d5b6ac.exe`, 3 runs, production flags)

SHA-256 `9944dccbff64cdbfd40cecbf9ecbe8421ba4d6ac346a0b4e93248cba73157996`.
Raw: `build/bottleneck_discovery/stage_raw/9d5b6ac_{02,03}_run{1..3}.txt`.

| mode | collections | begin ms (total) | sweep ms (total) | sweep per collection | zero bits | set bits |
| --- | --- | --- | --- | --- | --- | --- |
| 02 | 41 | 19.3 / 27.9 / 27.5 | 862.7 / 859.4 / 1052.5 | **21.0** | **0** | 83,886,040 |
| 03 | 8 | 97.3 / 93.8 / 99.4 | 100.3 / 101.1 / 104.7 | **12.5** | **0** | 7,000,000 |

`set_bits` equals the work the workload predicts: mode 02 publishes 2,046,000
start bits per collection (2,097,151 live nodes, minus objects born after the
cycle's epoch) over 41 collections; mode 03 has 875,000 per collection over 8.
Every set bit was visited once; no zero-bit iteration exists to count.
`[MEASURED]`

The stage probe's `begin` time for mode 03 (~12 ms per collection) is not part of
this stage's work; it is TLAB publication of 875,000 freshly allocated objects at
the cycle boundary and is recorded here only so the attribution is not misread.

### 4.3 Acceptance gates

| Gate | Target | Result | Verdict |
| --- | --- | --- | --- |
| Stress 02 `COLLECT` median | <= 140 ms | 75.4 ms (interleaved) | **pass** |
| Stress 03 `COLLECT` median | <= 40 ms | 24.6 ms (interleaved) | **pass** |
| Stage sweep 02 | <= 35 ms | 21.0 ms | pass |
| Stage sweep 03 | <= 12 ms | 12.5 ms | **miss (4 %)** |
| Zero-bit inner-loop iterations | 0 | 0 | pass |
| Words examined reported | required | `SWEEP_WORK` reports objects + words per chunk `[SOURCE]` | pass (event path), `[UNVERIFIED]` under a live listener |
| No unbounded sparse scan per chunk | required | 4,096 words + page charge | pass `[SOURCE]` |
| No correctness failure | required | 190/190 unit tests at this commit | pass |
| No material build/allocation regression | required | interleaved `BUILD` 02 288.9 -> 285.0, 03 56.1 -> 55.6 | pass |

## 5. Tests run

```
./build269.bat                                   # build
./build/msvc-dev/tests/nirqoba_unit_tests.exe    # 190 passed at 9d5b6ac
```

After the ColO-03/04 coverage landed (commit after this stage) the same binary
reports 192/192; no test that fails at `9d5b6ac` is hidden by that later commit.
Harness outputs on every run: `LIVE_AFTER 2097151`, `PAGES 3841`, unchanged
checksum/ROOT_VALUE versus the pre-stage binary. `[MEASURED]`

## 6. Production vs debug

* Production (`NRQ_RUNTIME_DEBUG_ENABLED=0`): all numbers above. No event code is
  compiled in; `kSweepBitmapWordsPerChunk` is a plain constant.
* Debug (`NRQ_RUNTIME_DEBUG_ENABLED=1`): the two new event kinds exist and are
  exercised only when a listener selects their masks. Their format and the meaning
  of their words are `[SOURCE]`-verified in `gc_debug.cpp`/`gc_debug.h`; no
  debug-build performance claim is made. `[UNVERIFIED]`

## 7. Regression analysis

* 02 `COLLECT`: +0.7 % vs ColO-01 in the interleaved comparison --- neutral.
* 03 `COLLECT`: -6.1 % vs ColO-01 interleaved. This is plausibly the word budget
  ending chunks before the older byte budget would, on a heap whose pages 03
  reclaims wholesale (1,710 pages, 875,000 starts). `[HYPOTHESIS]`; not isolated.
* `BUILD`: unchanged within spread in both modes; the allocation path is not
  touched by this stage.
* Peak memory: `PAGES 3841` (02) and `PAGES 1710` (03) are identical to the
  pre-stage runs; no memory trade-off. `[MEASURED]`

## 8. Limitations / unverified

* The per-chunk lock-hold duration (`SWEEP_CHUNK_NS`) and the words-examined field
  were not read back by a listener in this stage; they were added and source-
  verified, but no debug-build measurement of chunk duration exists yet.
  `[UNVERIFIED]`
* `begin` cost for 03 is reported but not attributed; it is outside this stage's
  scope.
* Stage-level sweep for 03 remains 0.5 ms above the roadmap's target, carried over
  from ColO-01.

## 9. Recommendation

**Accept.** The stage removes the unbounded sparse-page cost by construction,
leaves the fast path untouched in production, passes both end-to-end gates and the
correctness suite, and its apparent non-interleaved regression does not survive an
interleaved comparison. The 03 stage-probe target stays a recorded miss; it is not
`reached`, and the report does not round it down.
