# Stress Test 3 — PO-27 Regression Guard

This benchmark suite is a targeted regression guard for the hang identified
in **PO-27** (commit `0b2d457`, "perf(gc): index root slots in one flat table
instead of a node map").

## Root cause (brief)

`SlotIndex::insert` always doubled capacity when the load factor (computed as
`size_ + tombstones_`) exceeded 75 %. In workloads where one or two slots are
live at any time but millions have been registered and released, the tombstone
count saturates the threshold without the table being full of live entries.
Each iteration triggered a `2× capacity` rehash. After ~1 000 000 benchmark
iterations (one root registration/deregistration per `make_node()` call) the
table's backing vector reached ≈1 GB, and every probe scanned gigabytes —
causing an apparent hang at 100 % CPU with no kernel block.

See `reports/regression/po-27-concurrency-analysis.md` for the full analysis.

## Tests

| ID | Name | What it proves |
|---|---|---|
| `01` | `slot_index_churn` | 500 000 insert/release cycles with 1 live entry — table must not grow |
| `02` | `interleaved_alloc_collect` | Batch alloc + explicit collect + discard, 10 rounds — alloc time must be constant |
| `03` | `root_frame_depth` | Depth-18 binary tree rebuilt 5 times — peak live slots = 18; tombstones = 18 × 2 × rounds |

## Pass criteria

- Process exits `0` within **120 seconds** per run.
- Every `ALLOC_MS` / `BUILD_MS` phase is **< 5 000 ms**.
- `LIVE_AFTER == 0` (or 1 for the sentinel object in tests 02/03) after every explicit `gc.collect()`.
- `CHECKSUM` matches the expected value where applicable.
- Last round's growth metric is **< 3× the first round's** (exponential-growth detector).

## Unit tests (C++)

`tests/unit/gc_slot_index_test.cpp` contains 7 API-level regression tests
registered in the `nirqoba_unit_tests` executable under the `GcSlotIndexTest`
suite. They exercise the same failure modes against the public GC API and
include a wall-clock timeout assertion so a regressed build fails quickly
instead of hanging the test suite.

`tests/unit/slot_index_test.cpp` contains 10 white-box tests under the
`SlotIndexTest` suite. They include `runtime/src/slot_index.h` directly and
assert capacity, live/tombstone counts, rehash/compaction/growth counts and
maximum probe length for the exact table implementation, including
collision-heavy and full-table termination cases.

## Usage

```powershell
# From the project root:
python benchmarks/stress_test_3/run_benchmark.py

# Skip recompile if already built:
python benchmarks/stress_test_3/run_benchmark.py --skip-compile

# Single test:
python benchmarks/stress_test_3/run_benchmark.py --test 01
```

Output: `benchmarks/stress_test_3/benchmark_results.json`
        `benchmarks/stress_test_3/results/raw/`

## Relationship to stress_test_2

Stress test 2 (`benchmarks/stress_test_2/`) is the cross-language performance
comparison suite. Stress test 3 is not a performance comparison — it is a
correctness and liveness regression guard. The two suites are independent and
can be run in either order.
