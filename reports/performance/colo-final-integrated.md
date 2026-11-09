# Final integrated benchmark --- 0.257 vs 0.260 vs 0.269

**Stage:** 8 (final) of the ColO Roadmap 2 sequence
**Revisions:** `98a1099` (0.257, last healthy before the regression),
`904aee0` (0.260, defective), `d23a2b4` (0.269, current working build)
**Date:** 2026-11-09

## 1. Method and why it is fair

The defect 0.260 introduced is in the collector: the same program, built from the
same compiler, collects about twice as slowly. Building three whole compilers
would add LLVM/toolchain variance and mix the compiler's own BUILD changes into a
runtime comparison. Instead each revision supplies only its runtime library:

1. `git archive <rev> runtime` extracts that revision's runtime tree.
2. `benchmarks/bottleneck_discovery/kits/build_kit_lib.py` compiles it with the
   pinned MSVC toolset at `/O2 /DNDEBUG /std:c++20 /MD`,
   `NRQ_RUNTIME_DEBUG_ENABLED=0`, into `nirqoba_runtime.lib`.
3. The current 0.269 compiler driver emits `benchmarks/stress_test_2` tests 02 and
   03 once per kit. The driver locates `nirqoba_runtime.lib` beside itself, so the
   kit is a driver + one revision's runtime library.
4. 8 process runs per kit/mode, **interleaved across kits within each round**
   (0.257 -> 0.260 -> 0.269, repeated), same shell, same machine, no other load.
5. Each process prints 5 measured rounds; the per-process median is one sample.

The program text and the compiler are identical for all three kits, so the only
variable is the collector. This is exactly the scope of the 0.260 regression and of
ColO-01/02. `[SOURCE]` + `[MEASURED]`

Scripts: `benchmarks/bottleneck_discovery/kits/run_final_benchmark.py`.
Raw output: `build/kits/raw/<rev>_<mode>_run{1..8}.txt`.

## 2. Results (ms, `COLLECT`, per-process median of 5 rounds)

| revision | mode | min | median | max | samples |
| --- | --- | --- | --- | --- | --- |
| 0.257 (`98a1099`) | 02 | 90.0 | **93.5** | 110.0 | 90, 94, 93, 110, 94, 95, 93, 93 |
| 0.260 (`904aee0`) | 02 | 156.0 | **158.0** | 185.0 | 158, 158, 158, 185, 156, 158, 157, 158 |
| 0.269 (`d23a2b4`) | 02 | 90.0 | **90.5** | 97.0 | 90, 90, 91, 93, 97, 90, 91, 90 |
| 0.257 (`98a1099`) | 03 | 24.0 | **26.0** | 28.0 | 24, 26, 25, 27, 25, 26, 26, 28 |
| 0.260 (`904aee0`) | 03 | 57.0 | **58.0** | 61.0 | 58, 61, 57, 58, 60, 59, 58, 58 |
| 0.269 (`d23a2b4`) | 03 | 26.0 | **28.5** | 30.0 | 29, 30, 26, 28, 28, 29, 29, 27 |

> The 0.269 test-03 row is superseded: a dedicated 16-round A/B of 0.257 against
> 0.269 measured 24.5 vs 25.0 ms (parity). The 28.5 above is drift in this
> longer eight-kit session. See `colo-07-residual-attribution.md` §4.

`BUILD` (same runs, same driver):

| revision | 02 | 03 |
| --- | --- | --- |
| 0.257 | 467.5 | 89.0 |
| 0.260 | 512.5 | 88.5 |
| 0.269 | 468.0 | 91.0 |

All 48 runs report `LIVE_AFTER 2097151` / `CHECKSUM 2199022206976` (02) and
`LIVE_AFTER 0` / `CHECKSUM 499999500000` (03): the collectors reclaim identical
object sets, so the timing comparison is not bought with different work.
`[MEASURED]`

## 3. Conclusions

| Comparison | 02 | 03 |
| --- | --- | --- |
| 0.269 vs 0.260 | **-42.7 %** (158.0 -> 90.5) | **-50.9 %** (58.0 -> 28.5) |
| 0.269 vs 0.257 | **-3.2 %** (93.5 -> 90.5) | **parity** (24.5 -> 25.0, +2.0 %; see below) |

* The 0.260 regression is fully removed for test 02: 0.269 is at or below the last
  healthy build.
* Test 03, **corrected**: the 28.5-vs-26.0 in the table above was session drift.
  A dedicated 16-round interleaved A/B of just those two kits measured 0.257 at
  24.5 ms and 0.269 at 25.0 ms, **+0.5 ms / +2.0 %**, with overlapping spreads
  (21--27 vs 21--29). Test 03 is at parity with 0.257.
  `reports/performance/colo-07-residual-attribution.md` §4. `[MEASURED]`
* The 0.260 kit is slower than the other two even in `BUILD` (512.5 ms vs 467.5 /
  468.0 for 02), which confirms the defect was not confined to the sweep loop:
  the defective runtime's allocation path was also slower. `[MEASURED]`
* 0.269 beats the gates of the roadmap (`<= 140` / `<= 40`) with large margin on
  both tests.

### Cause of the 03 regression (bisected)

`reports/performance/colo-07-residual-attribution.md` bisects the runtime chain
with the same kit method and attributes the test-03 regression to a single
commit:

* `98a1099` 24.0, `f1d2b42` 26.0, `1ca3c37` 28.5, **`e830b65` 57.0**, `1646b93`
  67.0, `0708f75` 62.0, `5d62abf` 64.0, `9d5b6ac` 31.0 (ms, 8 interleaved
  rounds). The step is `e830b65` (sweep pages in address order), whose per-bit
  bitmap walk tests 10.04 million zero bits per collection against 875,000 set
  bits: stage sweep 8.61 -> 29.89 ms.
* 0.269 removes it (sweep 10.54 ms, zero zero-bit iterations), and the dedicated
  A/B shows the remainder is inside noise.

The residual cost in the probe's harness workload is +1.56 ms of sweep per
collection (page-bitmap word reads) offset by 1.66 ms less mark (per-object list
walk removed), i.e. parity in total.

## 4. Correctness at the current build

* `./build/msvc-dev/tests/nirqoba_unit_tests.exe`: **192/192 passed**.
* Harness `LIVE_AFTER`/checksums identical across kits.
* 0.269 binary reports `nirqoba compiler version 0.269-beta`.

## 5. Limitations

* Millisecond-resolution output from the stress tests (integer `COLLECT_MS`), so
  medians are quantised, and the 8-kit matrix has heavy tails (single samples up
  to 146 ms). The 03 residual claim rests on the separate 16-round two-kit A/B.
* One driver revision (0.269) for all kits. That is what isolates the collector,
  but it means the compiler-side cost of the handoff-fusion fix (~2--3 % on BUILD,
  `43c223f`) is present in every row and is **not** part of this comparison. A
  compiler-level 0.257-vs-0.269 comparison was not run.
* 5 in-process rounds per process x 8 processes, as the stress test defines; the
  harness benchmarks in this directory use 5 cycles per process as well.
* `BUILD` differences between kits come from their runtime libraries (allocation
  path), not from compiler code generation.

## 6. Recommendation

**Accept 0.269 as the working build.** It removes the 0.260 regression (-43 % /
-51 %), matches or beats 0.257 on test 02, is at parity with 0.257 on test 03
(+0.5 ms / +2.0 %, inside spread), and passes all 192 tests. The test-03
regression is bisected to `e830b65`'s zero-bit scan and is removed; no residual
regression is left to excuse.
