# Pre-MMM Phase 3 cleanup benchmark

This benchmark compares the pre-Phase-3 `0.457-beta` compiler against the Phase 3
cleanup lowering. It separates compiler work from runtime behavior:

- `cleanup_compile.nrqba` has 250 lexical cleanup scopes and 100 conditional
  scopes with defers and moves. `nirqoba check` is timed over this file.
- `cleanup_runtime.nrqba` runs one million iterations of nested shared
  allocations, a move, and an explicit defer. The executable wall time is timed
  after a build with `--ipo=minimal`.

Run from the repository root:

```text
python benchmarks/pre_mmm_phase3/run.py \
  --baseline .baseline-0457/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe \
  --runs 15
```

The generated LLVM raw IR contains four `nrq_memory_shared_release` call sites
per runtime loop in the baseline and three in the Phase 3 build. The removed call
is the automatic release of a local that was already moved.

## Measured results

Three independent trials, seven compiler samples and three runtime samples per
trial, Windows 11 x64 / MSVC 14.51.36231 / LLVM 23.1.4, `RelWithDebInfo`:

| Metric | Baseline `0.457-beta` | Phase 3 | Difference |
|---|---:|---:|---:|
| Compile cleanup trial medians | 44.632 / 47.542 / 45.144 ms | 43.937 / 43.781 / 45.793 ms | median-of-medians 45.144 vs 43.937 ms |
| Compile cleanup mean medians | 44.993 / 46.800 / 45.551 ms | 44.575 / 44.218 / 45.331 ms | 45.551 vs 44.575 ms |
| Runtime cleanup trial medians | 465.018 / 467.828 / 521.086 ms | 481.992 / 453.638 / 471.902 ms | 467.828 vs 471.902 ms |
| Runtime cleanup mean | 488.073 / 547.513 / 541.021 ms | 475.270 / 479.543 / 485.951 ms | 541.021 vs 479.543 ms |

The compiler benchmark improved by about 2.7% at the median-of-medians. Runtime
medians overlap within run-to-run noise (the Phase 3 median is 0.9% higher while
the mean is 11.4% lower); this report does not claim a runtime speedup. The
stable result is the eliminated cleanup site: three releases instead of four per
loop iteration, with identical program output (`2999997`).

The baseline is a detached worktree at the pre-Phase-3 commit. The `.baseline-0457`
directory is temporary benchmark infrastructure and is not part of the release.
