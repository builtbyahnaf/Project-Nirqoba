# AOR Phase 6 performance

Two questions were measured in this phase:

1. what the Phase 5 pool release path actually cost (alpha);
2. what a compiler-emitted plan and a per-site descriptor cache are worth (beta).

Everything below is measured, not estimated. Where a change did not show an
improvement, that is reported as a null result.

## Method

- Benchmark: `nirqoba_phase5_benchmark`, the Phase 5 harness extended with the
  rows this phase needed. It prints every run and a median row per workload.
- 200 000 objects, 7–9 runs, median of per-run means; the spread across runs is
  quoted alongside every headline figure.
- Development runtime: poisoning, generation validation and descriptor validation
  are all inside the timed regions.
- Machine: i5-1235U, Windows, MSVC, RelWithDebInfo.
- Work is warmed before timing; the allocation phase is outside the timed region
  in the release-specific rows.

## Alpha: pool release

| Workload | Before | After | Change | Runs after |
|---|---|---|---|---|
| `pool-release-plain` | 61.98 ns/op | 16.02 ns/op | 3.9× | — |
| `pool-release-gen` | 63.83 ns/op | 15.08 ns/op | 4.2× | 14.70–20.95 ns/op |
| `pool-release-stale-check` | 93.28 ns/op | 14.21 ns/op | 6.6× (not like-for-like) | — |
| `pool-reset-64B` | 7.69 ns/slot | 5.90 ns/slot | 1.3× | — |
| `pool-reset-partial` | 58.8 µs | 33.0 µs | 1.8× | — |
| `pool-64B` allocation | — | unchanged | none intended | — |

The stale-check row changed API as well as cost: it used the registry-locking
release before and uses the scope release now. It is kept as a row because the
number it reports — generation validation with a scope already held — is the one
that matters, but it should not be read as a 6.6× improvement of one function.

Before the rewrite the same workload spread 61.2–64.9 ns/op over nine runs, so the
after figure sits an order of magnitude of variance below the before figure, not
just outside one standard deviation.

## Beta: compiler plan and site cache

Three new rows allocate four-element managed arrays (64-byte payload) in a loop:

| Workload | Median | Range over 7 runs |
|---|---|---|
| `gc-array-typed` | 44.31 ns/op | 41.10–48.82 |
| `gc-array-planned` (warm site cache) | 43.19 ns/op | 41.02–48.95 |
| `gc-array-planned-cold` (cache cleared per call) | 42.33 ns/op | 40.82–48.28 |

**Null result, reported as such.** At one shape the planned path is not faster.
The typed path's layout memo hits almost always, so removing that lookup removes
work that was already a few instructions; the difference between the three rows is
smaller than their spread. The planned path does not cost anything either, and the
unit test `GcPlannedAllocationTest.PlannedArrayUsesTheExtendedRepresentation`
proves the cache does what it claims: with a warm site cache, the layout-memo miss
counter does not move at all across a second allocation.

The case where the cache matters is a program with more shapes than the layout
memo holds. Eight element sizes allocated round-robin evict each other from the
four-entry memo, so every typed allocation is a miss:

| Workload | Median | Range over 7 runs | Change |
|---|---|---|---|
| `gc-array-multishape-typed` | 78.15 ns/op | 75.94–82.42 | — |
| `gc-array-multishape-planned` | 53.37 ns/op | 47.81–57.59 | 1.46× faster |

That is 24.8 ns less per allocation, about 32 % lower, and the two ranges do not
overlap. This is the honest headline for beta: the site cache is worth about a
third of an allocation when the shapes do not fit the memo, and worth nothing
measurable when they do.

## Reproduction

```
nirqoba_phase5_benchmark 200000 7
```

Rows: `gc-array-typed`, `gc-array-planned`, `gc-array-planned-cold`,
`gc-array-multishape-typed`, `gc-array-multishape-planned`,
`pool-release-plain`, `pool-release-gen`, `pool-release-stale-check`,
`pool-reset-64B`, `pool-reset-partial`.

## Limitations

- Absolute numbers are from one machine and one compiler; treat the ratios, not the
  nanoseconds, as the transferable result.
- The development runtime is measured, so safety work is inside the timed region.
  A release runtime would be faster on both sides of every comparison.
- The multishape workload is synthetic: eight shapes in a tight loop is the worst
  case for a four-entry memo, chosen because it is the case the site cache exists
  for. Real programs will land between the two rows above.
- No measurement was taken of the compiler's own cost (one extra global and one
  extra argument per allocation site); it is a compile-time and code-size effect,
  not a runtime one, and was not quantified here.
