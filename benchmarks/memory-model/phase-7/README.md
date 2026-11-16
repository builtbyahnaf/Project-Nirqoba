# Phase 7 — GC isolation and explicit-selection benchmarks

These measurements answer two separate questions:

1. What does explicitly selected collector work cost (allocation, roots,
   frames, safepoints, barriers, collection, cycles, multithreaded allocation)?
2. Do ordinary owned, borrowed, arena, pool and plain-shared operations touch
   the collector when they contain no managed references?

They are not a comparison between memory strategies with different semantics.
The ordinary workloads never allocate a managed object, so a lower number is a
statement about isolation, not about one strategy being universally faster.

## Method

- Harness: `main.cpp`, compiled by `run_benchmark.py` with MSVC `/O2 /MD` and
  linked against `build/msvc-dev/lib/nirqoba_runtime.lib`.
- Public C ABI only, matching the entry points emitted by the compiler.
- Each mode runs in a fresh process. The runner keeps every sample and reports
  median, mean, minimum, maximum and population standard deviation.
- The timer is `steady_clock` around the measured loop. Process peak resident
  memory comes from `GetProcessMemoryInfo`.
- No fixed sleep is used to force collection. Collection modes measure one
  explicit full collection after a fixed garbage load.
- The isolation modes snapshot GC allocated bytes, collections, live objects,
  page count and layout count before and after ordinary work and require every
  counter to be unchanged.

## Modes

| Mode | Workload |
| --- | --- |
| `managed` | 600,000 managed 8-byte objects, unpinned as they are created; followed by one full collection |
| `roots` | 1,000,000 root register/unregister pairs and 1,000,000 root frame begin/end pairs |
| `safepoint` | 5,000,000 safepoint polls inside one mutator region |
| `barrier` | 5,000,000 root-store write barriers against one live object |
| `collect` | 500,000 unreachable managed objects, one full collection |
| `cycles` | 100,000 two-node cycles with traced next pointers, then one full collection |
| `mt` | 4 threads × 100,000 allocations while the main thread requests collections |
| `isolation` | 500,000 ordinary owned allocations, 1,024,000 borrowed reads, 200,000 arena allocations, 200,000 pool allocation/release pairs, 200,000 plain-shared allocation/release pairs |

## Run

```text
python benchmarks/memory-model/phase-7/run_benchmark.py --runs 7
```

Results are written to `results.json`; build output and the executable stay in
the ignored `build/` directory.

## Limitations

- A single full collection after a fixed load is not a worst-case pause
  distribution.
- Peak RSS is process-wide and includes runtime metadata and the benchmark
  executable.
- The harness measures ABI entry points. Language-level root elision and
  optimization are covered by the integration fixtures, not by this timer.
- Multithreaded progress depends on the host scheduler; the reported values are
  not a real-time guarantee.
