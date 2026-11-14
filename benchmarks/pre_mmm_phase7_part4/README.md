# Pre-MMM Phase 7 Part 4 benchmark

`run.py` compares `0.495-beta` (`bb99ffd`) with the Part 4 tree. It builds a
generated module containing 5,000 independent aggregate lifetimes, each with
eight owned-array fields, and reports compiler wall time, peak RSS, object size
and executable size. The workload isolates the lowering of
`aggregate.owned_drop`: the generated program never runs, so the numbers do not
include runtime allocation or collector behavior.

The baseline is a disposable worktree built with `scripts/build.ps1` after
copying only `toolchain/toolchain.json`. Both compilers are alternated on every
repeat.

## Results

Fifteen repeats, `RelWithDebInfo`, generated source with 20 functions and 250
scopes per function (5,000 drop sites):

| Measurement | Baseline | Part 4 | Change |
|---|---:|---:|---:|
| Compiler wall | 7,454.763 ms | 7,507.598 ms | **+0.71%** |
| Compiler peak RSS | 417,304,576 B | 417,480,704 B | +0.04% |
| Object size | 1,362,995 B | 1,362,995 B | identical |
| Executable size | 622,592 B | 622,592 B | identical |

The small wall difference is within the observed run-to-run spread and is
reported as neutral. Replacing the per-site `std::function` removes a compiler
heap allocation, but this workload does not turn it into a measurable speedup;
no performance improvement is claimed. The main Part 4 changes are correctness
fixes for SoA fields, which this benchmark does not exercise because the
baseline crashes on them.

## Reproduce

```sh
git worktree add .phase7-part4-baseline bb99ffd
# copy toolchain/toolchain.json into the worktree, then build it
python benchmarks/pre_mmm_phase7_part4/run.py \
  --baseline .phase7-part4-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 15 \
  --output benchmarks/pre_mmm_phase7_part4/results_aggregate_drop_compile.json
```

Remove the worktree after measurement; it is not part of the repository.