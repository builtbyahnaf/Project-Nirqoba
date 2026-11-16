# Phase 9 memory-strategy benchmarks

These measurements cover the existing strategies; they do not claim that one
strategy is universally faster than another.

## Method

- Harness: `main.cpp`, compiled by `run_benchmark.py` with MSVC `/O2 /MD` and
  linked against the built `nirqoba_runtime.lib`.
- Public runtime C ABI only, matching the entry points the compiler emits.
- Every mode runs in a fresh process. The runner preserves all samples and
  reports median, mean, minimum, maximum and population standard deviation.
- Timer: `steady_clock` around the measured operation loop.
- Peak RSS is process-wide (`GetProcessMemoryInfo`), so it includes runtime
  metadata and the harness executable.
- `results-baseline.json` is the pre-optimization baseline (9 samples).
  `results.json` is the post-optimization run (15 samples).
- Checksums are compared across samples so a compiler/runtime change cannot
  silently change the measured result.

## Modes

| Mode group | Workload |
|---|---|
| `owned` | 1M allocate/release, 200k retained 72-byte objects, 1M manual-path allocate/release, 4.096M borrowed reads |
| `arena` | 1M bump allocations in an 8 MiB arena, then 1,000 resets |
| `pool` | 1M 8-byte pool allocate/release pairs |
| `shared` | 200k shared blocks with 8 retains/releases each; 4 threads × 250k retain/release |
| `gc` | 500k managed objects, then one full collection |
| `mixed` | 200k rounds interleaving owned, arena, pool, shared and manual operations |

## Run

```text
python benchmarks/memory-model/phase-9/run_benchmark.py --runs 15
```

Build artifacts stay in the ignored `build/` directory. The compiler version is
recorded in the JSON report.

## Limitations

- The harness measures ABI entry points, not source syntax; cleanup lowering is
  covered by `tests/phase9_optimization.py`.
- Manual storage has no separate heap, so the manual mode measures the same
  checked runtime path, not a distinct allocator.
- Arena reset is measured separately; retained memory is not a full allocator
  efficiency score.
- Shared contention depends on the host scheduler.
- One full collection is not a pause distribution.
- No FFI benchmark exists because the language has no FFI surface.
- A faster time is not correctness evidence; the test suite and generated-code
  checks provide that evidence.
