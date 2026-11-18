# AOR Phase 4 verification

**Status:** complete  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Build:** `build/msvc-dev`, MSVC 19.51 (VS 18 BuildTools), `RelWithDebInfo`, x64

This report records what was run, not only what was intended to run. Command
lines are included so each result can be reproduced.

## Unit tests

```bat
build\run_build.bat
build\msvc-dev\tests\nirqoba_unit_tests.exe
```

| | Result |
|---|---|
| Tests | 360 |
| Passed | 360 |
| Failed | 0 |

The suite is a custom harness: `--gtest_filter` and `GTEST_FILTER` are ignored, so
every run executes every test.

### Representation coverage added in Phase 4

| File | What it pins |
|---|---|
| `tests/unit/gc_representation_test.cpp` | fixed header costs, auto policy, rejection, statistics, per-representation tracing/promotion/reclamation, mixed heap |
| `tests/unit/gc_representation_reuse_test.cpp` | slab anti-thrashing, representation-local runs, three page lives with a kind and flag reset each time |
| `tests/unit/gc_representation_entrypoints_test.cpp` | descriptor/layout-array/typed/rooted entry points, 256/257-byte boundary, pinning, large-object index |
| `tests/unit/gc_representation_concurrency_test.cpp` | four mutators against a cycling collector, unpublished-object visibility |

The existing `gc_test.cpp` metrics were updated where the representation changes
them: instance/shared bytes, the 64-byte profile span, page slots, and the
second-wave reuse test that assumed H32 density.

## Integration suite

```bat
python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe --configuration RelWithDebInfo
```

| | Result |
|---|---|
| Cases | 624 |
| Passed | 624 |
| Failed | 0 |

The first Phase 4 run failed one case,
`positive/aggregate_soa_field_move`, with
`owned array index is outside the array bounds`. That was the one-element
container described as a record; see bug 5 in `phase-4.md`. It passes now.

## Phase 7 isolation

```bat
python tests/phase7_isolation.py --compiler <absolute path to nirqoba.exe>
```

Result: `Phase 7 isolation checks passed.`

The compiler path must be absolute on this host; a relative path makes Python's
`CreateProcess` fail before any check runs.

## Phase 9 optimization compatibility

```bat
python tests/phase9_optimization.py --compiler <absolute path to nirqoba.exe>
```

Result: every fixture reports `O0/O2 production+debug equivalent`, then
`Phase 9 optimization compatibility checks passed.`

## Configuration equivalence

```bat
python scripts/compare_configuration_equivalence.py --compiler <absolute path> --filter "*gc*"
```

Result: `67 agreed, 0 skipped, 0 failed`.

## Benchmark

```bat
cmake --build build\msvc-dev --target nirqoba_phase4_benchmark
build\msvc-dev\benchmarks\aor\phase4\nirqoba_phase4_benchmark.exe 200000 9
```

Four shapes were measured; results and the derived comparison are in
`phase-4-performance.md`. The benchmark links the development runtime.

## What is not covered

- No H32 or H48 binary was rebuilt, so there is no measured before/after timing;
  the header comparison is arithmetic.
- The benchmark is one machine, one compiler, one synthetic workload per shape.
- Pause times were not measured; collection figures are whole-cycle costs.
- The production runtime is covered by the Phase 9 matrix and configuration
  equivalence, not by the benchmark.

## Failures found while verifying

| Symptom | Cause | Fix |
|---|---|---|
| `owned array index is outside the array bounds` in `aggregate_soa_field_move` | one-element container described as a record | shape stated by the entry point, part of the memo key |
| allocation returning null after 37 headerless pages | recycled large page handed to an ordinary request | exact-capacity page reuse |
| a 200 KB object surviving its collection | large page indexed only by bitmap path | large pages always use the exact index |
| `MultiSlotFrameChurnDoesNotGrowCapacity` failing at 37,888 frames | same recycled-page capacity bug | same fix |
| a large object retained because a stack slot aliased a dead root | a test registered a root and never unregistered it | test releases the slot |
| mixed workload allocating one page per object | mutator abandoned its slab on every representation switch | per-class page cache, slab kept for its own shape |

Each fix is a separate commit and each has either a regression test or a statistics
assertion that would catch it again.
