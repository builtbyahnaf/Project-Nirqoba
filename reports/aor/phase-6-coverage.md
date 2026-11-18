# AOR Phase 6-zeta to 6-lambda: coverage, cache, wiring, validation

This is the second half of Phase 6. The first half built the plan and the site
cache; this half asked which paths still bypassed them, fixed the memo they depend
on, wired the remaining strategies, and validated the result.

Full path table: `docs/memory-model/allocation-coverage.md`.

## 6-zeta: coverage

Every compiler allocation site was read off `TirToMlir.cpp` and checked against the
runtime entry point it calls.

| Path | Before this phase | After |
|---|---|---|
| `array.allocate<T>` | planned (previous commit) | planned |
| `memory.owned_soa<T>`, managed column | planned | planned |
| `memory.owned_soa<T>`, no managed column | planned | planned |
| records (`memory.allocate`, owned) | `nrq_memory_allocate` | unchanged: owned storage, no representation, no plan applies |
| explicit arena | `nrq_arena_allocate` | unchanged; a plan-driven arena entry now exists for callers that have a plan |
| explicit pool | `nrq_cdmm_pool_allocate` | unchanged; a plan-driven pool constructor now exists |

The one genuine gap was on the runtime side rather than the compiler side: there
was no planned entry point for a *record* allocation, so an API caller that knew
its record's representation had to fall back to `nrq_gc_allocate` (Auto) or
`nrq_gc_allocate_repr` (explicit). `nrq_gc_allocate_planned` fills that gap and
takes the same validated plan and site cache as the array form.

Nirqoba records are owned storage, not collector objects, so the compiler emits no
plan for them. That is recorded as an intentional fallback, not an omission.

## 6-eta: the layout memo

The four-entry ring was the measured problem: eight alternating shapes re-interned
on every allocation. The replacement is a sixteen-set, two-way set-associative
table indexed by shape, with one replacement bit per set.

- Hit path: one hash, two comparisons, no lock, no allocation.
- Captured shape: element size, reference count, array/record kind, tracer, and
  the first and last reference offsets — the fields that distinguish tables of
  equal length.
- Two ways exist for one reason: a single hash collision between two hot shapes
  would otherwise make both miss forever. `AllocPlanPropertyTest.EightAlternatingShapesStopThrashingTheMemo`
  fails if any of eight shapes misses after warmup.
- Cost: 260 bytes of thread-local storage.

Measured: the typed eight-shape workload fell from 78.15 to 52.26 ns/op (1.50×).
Post-fix, typed and planned allocation are indistinguishable at 24 and 40 shapes,
because the memo now holds all of them and the remaining cost is page-class
diversity, not lookup.

## 6-theta: wiring

- `nrq_gc_allocate_planned` (records) — representation from the plan's two-bit
  field, validated by the allocator's own `select_representation`, which upgrades a
  hint the payload cannot support.
- `nrq_cdmm_pool_create_planned` and `nrq_cdmm_arena_allocate_planned` — slot size
  and alignment from the plan; refused for plans with headers, references, arrays
  or the rejected bit, because a deterministic region cannot trace.
- Existing descriptor-based CDMM entry points kept, since a caller with a
  descriptor has no plan to produce.
- Compiler compatibility: the plan word the compiler emits is
  `1 | (3 << 8)` and is re-checked, not trusted. The typed entry points are
  untouched, so a compiler from before this phase still links and runs.

## 6-iota: performance

All numbers, ranges and the null results are in `phase-6-performance.md`. Summary:

| Change | Measured effect | Kept |
|---|---|---|
| sorted chunk index + per-chunk free lists + live-only reset | 3.9–4.2× faster release, 1.8× faster partial reset | yes |
| site cache for planned array allocation | no effect at 1–2 shapes; 9 % at 8 shapes inside range overlap; no effect at 24–40 shapes | yes, for the descriptor work it removes |
| 16×2 shape-indexed memo | 1.50× at 8 alternating shapes | yes |
| planned record entry | not benchmarked against `nrq_gc_allocate` separately; same allocation path, cache validated | yes |
| planned arena/pool | no timing claim; same allocator entry, geometry from the plan | yes |

## 6-kappa: validation

Added:

- `tests/unit/alloc_plan_property_test.cpp` — 14 record sizes and 4 array shapes
  checked plan-against-runtime (representation and header size), eight-shape memo
  behaviour, a same-size/different-table cache rejection, a rejected plan falling
  back, and four threads allocating planned arrays concurrently with their own site
  caches.
- `tests/unit/cdmm_layout_test.cpp` — planned pool and arena creation, alignment
  and containment, and refusal of collector-shaped plans.
- `tests/unit/gc_test.cpp` — the memo test was rewritten for the new table: five
  shapes all hit after warmup, a sixth inserts, offsets still distinguish shapes.
- Existing suites: 434 unit tests, 624 integration cases, 67 configuration
  equivalence agreements for `*gc*`.

Not covered by a new test: allocation failure under page exhaustion for the planned
path specifically (the underlying allocator's failure path is exercised by the
existing allocation tests), and a stress run beyond the benchmark's 200 000
objects.

## 6-lambda: delivery

- Nothing was removed as obsolete. The typed entry points are the documented
  fallback and the compatibility surface; removing them would break the promise
  that a caller with no plan still allocates correctly.
- No known integration defects remain after the fixes above; the two test failures
  found while doing this work (an old ring-order expectation and a hash collision
  between two of the eight benchmark shapes) were fixed rather than tolerated, the
  second by giving the memo a second way.
- Documentation: `allocation-coverage.md` (new), `alloc-hints.md` (memo, record
  entry, deterministic plan entry points), `cdmm.md` (plan-driven constructors),
  this report, `phase-6-performance.md` (updated with post-fix numbers), and the
  version log.

## Remaining limitations

- Page-class diversity, not descriptor lookup, dominates allocation once a program
  uses dozens of shapes (110–160 ns/op at 24–40 shapes). No attempt was made to
  reduce page-class fragmentation; that is a Phase 7 question.
- The planned record entry has no benchmark of its own; its cost is the same
  allocation path plus a cache comparison.
- Code-size delta of the planned path versus the typed path was not isolated; only
  the 8-byte per-site global and the 260-byte thread-local memo were measured.
- One machine, development runtime, separate-process baselines.
