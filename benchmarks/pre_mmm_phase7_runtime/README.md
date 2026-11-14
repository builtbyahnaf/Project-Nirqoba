# Pre-MMM Phase 7 runtime benchmarks

Two workloads isolate the runtime ownership changes made in `0.483-beta`:

- `read_text.nrqba` — 20,000 reads of a 4 KiB file. Every returned string stays
  live, so the file-read arena's peak memory and shutdown cost are both visible.
- `handle_roundtrip.nrqba` — 100,000 `file.open("NUL", true)` /
  `file.close` pairs, isolating registry allocation rather than disk I/O.

`run.py` builds one executable per compiler, alternates execution order, records
wall time and peak working set from `psutil`, and verifies the program's stdout
on every run. It writes a JSON report beside the results files.

## Baseline

The baseline compiler is a disposable worktree at `30a6ae6` (`0.477-beta`), the
last commit before the runtime session:

```sh
git worktree add .phase7-runtime-baseline 30a6ae6
powershell -NoProfile -ExecutionPolicy Bypass -File .phase7-runtime-baseline/scripts/build.ps1 -Configuration RelWithDebInfo
python benchmarks/pre_mmm_phase7_runtime/run.py \
  --baseline .phase7-runtime-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 9
```

For the handle workload, pass `--source .../handle_roundtrip.nrqba` and a
different `--output`. Remove the worktree after measurement.

## Recorded results

`results.json` is the 9-sample read workload; `results_handle.json` is the
5-sample handle workload. Both were produced on Windows 11 x64 with MSVC 14.51,
`RelWithDebInfo`, alternating order.

| Workload | Baseline | Current | Change |
|---|---:|---:|---:|
| Reads, median wall | 1,059.59 ms | 1,045.33 ms | -1.35% |
| Reads, median peak RSS | 93,106,176 B | 91,041,792 B | -2.22% |
| Handles, median wall | 303.85 ms | 297.66 ms | -2.04% |
| Handles, median peak RSS | 9,752,576 B | 9,773,056 B | +0.21% |

The read change is close to run-to-run variation and is directional. The handle
change comes from removing one map-node allocation per open; its one-page RSS
difference is noise. The 64 KiB arena layout tried first measured +3.4% wall and
+3.4% peak RSS and was replaced by 1 MiB blocks before these numbers were taken.

## What is not measured

- The static argument descriptor removes one allocation per process; process
  startup noise is larger than the effect, so no speedup is claimed.
- Native-resource shutdown cleanup costs time proportional to live resources at
  exit. It is a correctness guarantee, not an optimization.
