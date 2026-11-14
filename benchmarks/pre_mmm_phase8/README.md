# Pre-MMM Phase 8 benchmark

`run.py` measures the compile-time cost of the Phase 8 allocation-boundary policy.
The generated module contains 400 allocation sites across explicit, owned-array,
shared and collector-managed storage, each using a struct payload with a managed field.
The payload type is inspected at every site, so this exercises the new ownership check
without relying on runtime behavior. The generated program is compiled but not run.

The baseline is `0.499-beta` (`c0ddcfd`) built in a disposable worktree with
`scripts/build.ps1`; only `toolchain/toolchain.json` is copied in. Both compilers
alternate on every repeat.

## Results

Seven repeats, `RelWithDebInfo`, 10 functions × 10 scopes × 4 allocation kinds
(400 allocation sites):

| Measurement | Baseline `0.499-beta` | Current `0.511-beta` | Change |
|---|---:|---:|---:|
| Compiler wall | 287.989 ms | 287.472 ms | **-0.18%** |
| Compiler peak RSS | 53,813,248 B | 53,653,504 B | -0.30% |
| Object size | 32,319 B | 32,319 B | identical |
| Executable size | 146,944 B | 146,944 B | identical |

The difference is neutral and within run-to-run spread. No speedup is claimed; the
policy checks are compile-time-only and add no runtime state. A larger 4,000-site
variant exceeded the available LLVM memory in the benchmark environment, so the
recorded workload is deliberately bounded.

## Reproduce

```sh
git worktree add .phase8-baseline c0ddcfd
# copy toolchain/toolchain.json into the worktree, then build it
python benchmarks/pre_mmm_phase8/run.py \
  --baseline .phase8-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 7 \
  --output benchmarks/pre_mmm_phase8/results_allocation_policy_compile.json
```

Remove the worktree after measurement; it is not part of the repository.
