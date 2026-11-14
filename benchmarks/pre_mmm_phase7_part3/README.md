# Pre-MMM Phase 7 Part 3 benchmarks

`run.py` compares `0.483-beta` (`70126a6`) with the Part 3 tree. The baseline is
a disposable worktree; build it with `scripts/build.ps1` after copying only
`toolchain/toolchain.json`, which points at the main checkouts' LLVM SDK.

## Modes

- **`build`** compiles `build_hello.nrqba` end to end into a temporary
  `--out-dir` for each compiler and records wall time, peak RSS, object size and
  executable size. It measures the publication transaction, not code generation
  alone.
- **`mutex`** builds `mutex_roundtrip.nrqba` once per compiler, then runs
  100,000 create/lock/unlock/destroy cycles. It measures the registry slot table.
- The read-string rerun uses `../pre_mmm_phase7_runtime/run.py` with the Part 3
  baseline and writes `results_read_text.json` here.

Every run alternates compiler order and checks the program's output. The build
mode also verifies that both object and executable exist at their final paths.

## Results

| Workload | Baseline | Part 3 | Change |
|---|---:|---:|---:|
| Build wall | 108.433 ms | 111.931 ms | **+3.23%** |
| Build peak RSS | 30,412,800 B | 30,486,528 B | +0.24% |
| Object size | 815 B | 815 B | identical |
| Executable size | 21,504 B | 19,968 B | **-7.14%** |
| Mutex wall | 47.396 ms | 42.319 ms | **-10.71%** |
| Mutex peak RSS | 9,768,960 B | 9,781,248 B | +0.13% |
| Read wall | 867.168 ms | 868.243 ms | +0.12% |
| Read peak RSS | 91,066,368 B | 92,069,888 B | +1.10% |

The build regression is the cost of two hard-link backups and rollback safety.
The executable-size reduction comes from removing the mutex map machinery from
the linked runtime. The read difference is noise; its memory is process-lifetime
by design. No universal speedup is claimed.

## Reproduce

```sh
git worktree add .phase7-part3-baseline 70126a6
# copy toolchain/toolchain.json into the worktree, then build it
python benchmarks/pre_mmm_phase7_part3/run.py --mode build \
  --baseline .phase7-part3-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 9 \
  --output benchmarks/pre_mmm_phase7_part3/results_build.json
python benchmarks/pre_mmm_phase7_part3/run.py --mode mutex \
  --baseline .phase7-part3-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 9 \
  --output benchmarks/pre_mmm_phase7_part3/results_mutex.json
```

Remove the worktree after measurement; it is not part of the repository.
