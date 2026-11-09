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
| 0.269 vs 0.257 | **-3.2 %** (93.5 -> 90.5) | **+9.6 %** (26.0 -> 28.5) |

* The 0.260 regression is fully removed for test 02: 0.269 is at or below the last
  healthy build.
* Test 03 is **not** fully back to 0.257: 28.5 ms against 26.0 ms, a 2.5 ms /
  9.6 % residual that is consistent across all eight interleaved samples (0.269
  range 26--30, 0.257 range 24--28; the medians are separated by 2.5 ms while the
  intra-revision spread is 4 ms, so the ordering is visible but not large).
  `[MEASURED]`
* The 0.260 kit is slower than the other two even in `BUILD` (512.5 ms vs 467.5 /
  468.0 for 02), which confirms the defect was not confined to the sweep loop:
  the defective runtime's allocation path was also slower. `[MEASURED]`
* 0.269 beats the gates of the roadmap (`<= 140` / `<= 40`) with large margin on
  both tests.

### Cause of the 03 residual

`[HYPOTHESIS]` The runtime between `98a1099` and `d23a2b4` contains more than the
sweep change (versions `1ca3c37`, `1646b93`, `5d62abf`, `0708f75`, `f1d2b42`,
`e830b65`, plus the ColO work); the residual is not attributed to a specific
commit. It is recorded, not explained away. Test 03 allocates 875,000 nodes per
collection and collects 1,000,000 objects with near-zero live set, so per-object
sweep and page-recycling costs are at their most visible there; the ColO-01/02
chunk accounting may also play a part (stage sweep 12.5 ms vs the 12 ms target).

## 4. Correctness at the current build

* `./build/msvc-dev/tests/nirqoba_unit_tests.exe`: **192/192 passed**.
* Harness `LIVE_AFTER`/checksums identical across kits.
* 0.269 binary reports `nirqoba compiler version 0.269-beta`.

## 5. Limitations

* Millisecond-resolution output from the stress tests (integer `COLLECT_MS`), so
  medians are quantised; the 0.269-vs-0.257 03 difference (2.5 ms) is above that
  resolution.
* One driver revision (0.269) for all kits. That is what isolates the collector,
  but it means the compiler-side cost of the handoff-fusion fix (~2--3 % on BUILD,
  `43c223f`) is present in every row and is **not** part of this comparison. A
  compiler-level 0.257-vs-0.269 comparison was not run.
* 5 in-process rounds per process x 8 processes, as the stress test defines; the
  harness benchmarks in this directory use 5 cycles per process as well.
* `BUILD` differences between kits come from their runtime libraries (allocation
  path), not from compiler code generation.

## 6. Recommendation

**Accept 0.269 as the working build.** It removes the 0.260 regression (-43 % / -51 %),
matches or beats 0.257 on test 02, passes all 192 tests, and leaves one recorded
residual: test 03 is 2.5 ms (9.6 %) slower than 0.257, unattributed. That residual
is documented in `docs/VERSION_LOG.md` rather than presented as a win.
