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

The case where the cache mattered was a program with more shapes than the layout
memo held. Eight element sizes allocated round-robin evicted each other from the
four-entry memo, so every typed allocation was a miss:

| Workload (four-entry memo) | Median | Range over 7 runs |
|---|---|---|
| `gc-array-multishape-typed` | 78.15 ns/op | 75.94–82.42 |
| `gc-array-multishape-planned` | 53.37 ns/op | 47.81–57.59 |

That was 24.8 ns less per allocation and the two ranges did not overlap. It was
also a measurement of the memo's degradation, not of anything intrinsic: the typed
path was slow because it was re-interning descriptors it had already seen.

## Beta, after the memo redesign

The memo is now a sixteen-set, two-way table indexed by shape (see
`docs/memory-model/alloc-hints.md`). The same eight-shape workload, measured again
on the same machine with 7 runs:

| Workload | Median | Range over 7 runs |
|---|---|---|
| `gc-array-multishape-typed` | 52.26 ns/op | 47.33–60.90 |
| `gc-array-multishape-planned` | 47.52 ns/op | 45.80–57.97 |

The typed path improved from 78.15 to 52.26 ns/op — 1.50× — with no change to the
work it does, only to where it finds the descriptor. The planned path stayed where
it was, and the 9 % gap that remains between the two is inside the overlap of
their ranges, so it is not claimed as an improvement.

With larger shape counts the workload stops being about descriptor lookup and
becomes about page classes, because every distinct element size is a distinct page
class:

| Workload | Shapes | Typed median (range) | Planned median (range) |
|---|---|---|---|
| `gc-array-alt2-*` | 2 | 43.04 (40.18–49.45) | 41.35 (37.16–49.78) |
| `gc-array-multishape-*` | 8 | 52.26 (47.33–60.90) | 47.52 (45.80–57.97) |
| `gc-array-diverse24-*` | 24 | 114.12 (106.51–135.17) | 114.88 (92.74–124.44) |
| `gc-array-diverse40-*` | 40 | 152.57 (140.93–198.77) | 158.27 (114.00–171.48) |

At 24 and 40 shapes the two paths are indistinguishable: the memo still holds all
40 shapes (32 entries, two ways), so the planned cache has nothing left to save,
and the cost is dominated by allocating across many page classes. Reported as a
null result rather than as a cache win.

## Metadata and code size

Measured by compiling a generated program with 40 `array.allocate<int>` sites and
inspecting its IR and object file:

| Item | Measured |
|---|---|
| per-site cache global | 8 bytes |
| 40 sites | 320 bytes of module data |
| object file, 40-site program | 6 288 bytes |
| thread-local layout memo | 260 bytes per thread |

The per-call cost is one extra argument and one immediate. No separate
typed-versus-planned code-size comparison was run; the 8-byte global per site is
the only new module data, and it is counted above.

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
