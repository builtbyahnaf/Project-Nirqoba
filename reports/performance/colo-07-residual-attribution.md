# ColO-07 --- Attribute the test-03 residual

**Base:** `98a1099` (0.257) to `b4f8a81` (0.269), runtime library only
**Question:** the integrated report recorded test 03 at 28.5 ms for 0.269 against
26.0 ms for 0.257 (+9.6 %). Which runtime change between them is responsible, and
how much of the gap is real?
**Answer:** one commit is responsible for the whole large regression ---
`e830b65` (sweep pages in address order, testing one bitmap bit at a time), which
costs roughly +28 ms per test-03 collection at 0.260. The ColO-01 set-bit sweep
removes it completely. The remaining gap is **not** a regression: a dedicated
16-run interleaved A/B puts 0.269 at **+0.5 ms (+2.0 %)** over 0.257, inside the
run-to-run spread. The earlier +2.5 ms figure was session drift.

All evidence below is `[MEASURED]` on this machine; `[SOURCE]` marks claims read
from the commit diff rather than timed.

## 1. Method

Three measurements, each answering a different part of the question.

1. **Chain bisect (kit method).** `run_bisect_benchmark.py` builds one
   `nirqoba_runtime.lib` per revision from the already-extracted tree in
   `build/bottleneck_discovery/<rev>`, links the canonical
   `03_cyclic_graph_reclamation.nrqba` with the **same 0.269 driver** for every
   kit, and runs the eight kits interleaved, 8 rounds each. Only the runtime
   library differs between executables, so the collector is isolated.
2. **Tight A/B.** The same script restricted to 0.257 and 0.269, 16 interleaved
   rounds, to decide whether the small residual is real. More rounds on two kits
   beat eight rounds on eight kits because the eight-kit session has heavier
   tails (the whole file takes longer, and test 03 is allocation- and
   scheduling-sensitive).
3. **Stage probe (per phase).** `gc_stage_<rev>.exe 03`, 3 runs per revision,
   revisions interleaved, production flags (`NRQ_RUNTIME_DEBUG_ENABLED=0`). The
   probe patches a copy of `gc.cpp` in the extracted tree; no repository source
   was modified. Raw: `build/bottleneck_discovery/stage_raw/attr_<rev>_03_run{1..3}.txt`.

`5d62abf`'s runtime is byte-identical to 0.260 (`904aee0`), and `9d5b6ac`'s is
byte-identical to HEAD (`9d5b6ac..HEAD` touches no file under `runtime/`), so the
chain below is complete. `[MEASURED]` (`git diff --stat` empty in both cases)

## 2. Chain bisect --- where the regression enters

`COLLECT` median per kit, test 03, 8 interleaved rounds, ms:

| Revision | Change | min | **median** | max | Samples |
| --- | --- | --- | --- | --- | --- |
| `98a1099` | 0.257 | 23.0 | **24.0** | 41.0 | 23 24 24 41 23 28 24 38 |
| `f1d2b42` | free-span list stays ordered | 23.0 | **26.0** | 54.0 | 24 24 29 27 23 32 25 54 |
| `1ca3c37` | page index for reusable runs | 23.0 | **28.5** | 46.0 | 24 27 30 27 23 30 46 32 |
| `e830b65` | **sweep pages in address order** | 48.0 | **57.0** | 88.0 | 48 49 65 53 88 55 59 63 |
| `1646b93` | minor collections skip young-free pages | 51.0 | **67.0** | 90.0 | 51 65 90 55 76 69 55 70 |
| `0708f75` | slices sized by heap bytes | 51.0 | **62.0** | 146.0 | 51 64 67 57 56 66 60 146 |
| `5d62abf` | 0.260 runtime | 50.0 | **64.0** | 143.0 | 50 61 77 58 77 67 54 143 |
| `9d5b6ac` | 0.269 runtime | 25.0 | **31.0** | 64.0 | 27 28 50 25 27 34 46 64 |

The step is unambiguous and belongs to `e830b65`: 28.5 ms -> 57.0 ms in the
adjacent pair, while every earlier pair moves by at most 2.5 ms. The later three
commits add noise on top of it (their medians sit at 62--67); they do not create
a new step. `[MEASURED]`

The 0.269 kit recovers most of it in the same session (64.0 -> 31.0), which is
the ColO-01 set-bit sweep.

## 3. Why `e830b65` costs so much --- the stage probe

Sweep/begin/mark per collection, mode 03, production flags, 3 runs each,
interleaved (`total ms / 8 collections`, median of the three runs):

| Revision | begin | mark | **sweep** | zero-bit iterations (8 collections) | set bits |
| --- | --- | --- | --- | --- | --- |
| `98a1099` (0.257) | 9.56 | 0.21 | **8.98** | 0 | 7,000,000 |
| `1ca3c37` | 10.33 | 0.21 | **8.61** | 0 | 7,000,000 |
| `e830b65` | 11.64 | 0.21 | **29.89** | 80,326,512 | 7,000,000 |
| `9d5b6ac` (0.269) | 10.25 | 0.01 | **10.54** | 0 | 7,000,000 |

`e830b65` moved the sweep from a flat object list to each page's start bitmap and
tested one bit at a time: `if (((bits >> cursor.bit) & 1) == 0) { ++cursor.bit; }
`. On test 03 that is 10.04 million zero-bit iterations per collection against
875,000 set bits --- 92 % of the walk is testing zeros. Sweep goes 8.61 -> 29.89 ms
(3.5x), which is the +28 ms seen on `COLLECT`. `[MEASURED]` sweep times;
`[SOURCE]` the zero-bit loop in the `e830b65` diff.

0.269 visits zero zero bits and sweep returns to 10.54 ms, within 1.6 ms of 0.257.

## 4. Is the residual real? Tight A/B

0.257 vs 0.269, test 03, 16 interleaved rounds:

| Kit | min | **median** | max | Samples |
| --- | --- | --- | --- | --- |
| 0.257 (`98a1099`) | 21.0 | **24.5** | 27.0 | 24 27 25 21 25 27 25 22 22 22 21 21 21 26 25 26 |
| 0.269 (`9d5b6ac`) | 21.0 | **25.0** | 29.0 | 27 21 27 24 27 26 24 25 24 23 29 26 24 29 25 25 |

Difference: **+0.5 ms (+2.0 %)**. The distributions overlap almost completely, and
the 0.257 median moved 24.0 -> 24.5 while the 0.269 median moved 31.0 -> 25.0
between sessions, so **the +2.5 ms recorded in the integrated report was run-to-run
drift, not a collector regression**. It is wrong to carry "9.6 % slower on 03"
into the version log, and it has been corrected there.

What survives, and is small: in the probe's harness workload the sweep itself is
10.54 ms versus 8.98 ms (`+1.56 ms`, `+17 %`), because a page walk reads 128
bitmap words per 64 KiB page (1,710 pages -> 218,880 word reads per collection)
where the list walk read one pointer per object. On the same harness the mark
phase got *cheaper* by 1.66 ms (0.06 vs 1.67 ms; `mark_calls` 3,005 vs 109,384),
so the harness `COLLECT` median is 22.92 (0.269) against 22.60 (0.257) --- parity.
`[MEASURED]`

## 5. Conclusion

* The 0.260 collector regression on test 03 is **one commit**: `e830b65`'s
  per-bit zero scan, +28 ms on a 26 ms collection.
* ColO-01 removes it: sweep 29.89 -> 10.54 ms per collection, zero zero-bit
  iterations.
* There is **no attributable 0.269 regression on test 03**. The dedicated A/B is
  +0.5 ms (+2.0 %), inside spread. The 9.6 % figure in
  `colo-final-integrated.md` is superseded by section 4 of this report.
* The only surviving cost is structural and small: page-bitmap sweeping reads
  every bitmap word of every page (about +1.6 ms per collection in the harness
  workload), offset by the 1.66 ms the mark phase no longer spends walking a
  per-object list.

## 6. Limitations

* The probe measures the C++ harness workload, not the canonical `.nrqba`
  program; the production `.nrqba` path has no phase counters. The chain bisect
  and the A/B are on the canonical workload and are the numbers that matter.
* Test 03 output is integer milliseconds and the 8-kit matrix has heavy tails
  (single samples up to 146 ms). The two-kit / 16-round A/B was used for the
  residual claim precisely because of that.
* One driver revision for all kits; the compiler-side fusion fix is present in
  every row and is not part of this comparison.
* No canonical workload was modified, and no production source change came out of
  this attribution.
