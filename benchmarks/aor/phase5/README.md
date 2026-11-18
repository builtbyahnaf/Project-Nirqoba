# CDMM Phase 5 benchmark

Compares the explicit arena and pool strategies against ordinary owned allocation
on the workloads they are meant for.

## Build and run

```bat
cmake --build build\msvc-dev --target nirqoba_phase5_benchmark
build\msvc-dev\benchmarks\aor\phase5\nirqoba_phase5_benchmark.exe 200000 9
```

Arguments are the object count (default 200,000) and the run count (default 5).
Each run prints one row; the last row per workload is the median of the runs.

## Workloads

| Row | What it does |
|---|---|
| `owned-64B` | 200,000 × `nrq_memory_allocate(64, 8)`, then every object released individually |
| `owned-churn-500` | 400 rounds of a 500-object working set, freed at the end of each round |
| `arena-64B` | one scope, 200,000 × 64-byte arena allocations, then one `arena_reset` |
| `arena-destructible` | same, with a destructor registered for every object; reset runs all of them |
| `arena-mixed-size` | same allocation count, sizes cycling 16 to 512 bytes |
| `arena-churn-500` | 400 rounds: begin scope, allocate 500, end scope, reset |
| `pool-64B` | one scope, 200,000 slot allocations, then a bulk release through the scope |
| `pool-generations` | same with `NRQ_CDMM_POOL_GENERATIONS` |
| `pool-reset-64B` | allocate 200,000 slots, then one `pool_reset` |
| `pool-churn-500` | 400 rounds of 500 slots allocated and released in a scrambled order |

The `release ns` column is per object for individual releases and per object for a
bulk reset too, so the two can be read against each other: a bulk reset's total
time does not grow with the object count, so its per-object figure falls as the
count rises.

`meta b/o` is the strategy's own per-object metadata, read from
`nrq_cdmm_object_facts`: zero for a plain arena or pool object, 32 for a
destructible arena object (its destructor record), and about 4 for a pool with
generations (1 liveness bit plus a 4-byte generation per slot).

The benchmark checks the destructor-call count against the object count and exits
non-zero on a mismatch, so a broken destructor list cannot produce a fast, wrong
number.

Results for the recorded run are in `reports/aor/phase-5-performance.md`.
