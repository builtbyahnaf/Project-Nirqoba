# AOR Phase 6: release path and compiler-directed allocation

Phase 6 had two halves. Alpha investigated the Phase 5 pool release path and
removed its measured costs. Beta connected compiler layout knowledge to the
allocator: a hint becomes a plan, the plan becomes a representation, and the
allocator keeps the final say.

Numbers are in `phase-6-alpha.md` and `phase-6-performance.md`.

## Alpha: what the pool release path cost

The audit found three costs, in this order of significance:

1. a linear scan of every chunk to find the chunk that owned a slot;
2. a single pool-wide free-list head, so releases from unrelated chunks wrote the
   same cache line;
3. a reset that rebuilt every slot instead of only the live ones.

The fixes were a sorted chunk index with a binary search, per-chunk free lists with
a set of chunks that have something to give, and a reset driven by the liveness
bitmap. `nrq_cdmm_pool_scope_release_checked` was added so generation validation
can be exercised and measured without the registry lookup, and
`nrq_cdmm_pool_seed_generation` (debug builds only) lets a test reach generation
wraparound without allocating four billion objects.

Nothing protective was removed: exact-slot validation, generation comparison,
double-release refusal, poison on release, and newest-first destructors on reset
and destroy all still run.

Individual release went from ~62 ns/op to ~16 ns/op with the same checks enabled;
reset of a mostly free pool dropped from 58.8 µs to 33.0 µs.

## Beta: hints, plans and the site cache

### The pieces

| Piece | Where | What it does |
|---|---|---|
| `NrqAllocHint` | `runtime/include/nirqoba/runtime/alloc_hint.h` | the facts a compiler knows about a site |
| `NrqAllocPlan` | same | representation, header size, payload, slot size |
| `nrq_alloc_plan` | `runtime/src/alloc_hint.cpp` | pure hint → plan |
| `nrq_alloc_plan_for_layout` | same | descriptor → plan, so both sides can be compared |
| `nrq_alloc_plan_flags` | same | packs a plan into one 32-bit word for codegen |
| `nrq_gc_allocate_array_planned` | `runtime/src/gc.cpp` | rooted array allocation with a plan and a per-site cache |
| `nrq_gc_allocate_soa_planned` | same | the same contract for strided column layouts |
| `nrq_cdmm_pool_create_layout` | `runtime/src/allocator_windows.cpp` | pool geometry from a descriptor |
| `nrq_cdmm_arena_allocate_layout` | same | arena allocation from a descriptor |

The compiler emits one zero-initialized 8-byte global per array site
(`nrq_alloc_site_N`), passes its address, and passes the plan as an immediate. The
runtime stores the resolved descriptor in that word after the first allocation.

### Where the runtime keeps authority

The plan is checked on every call, and so is the cached descriptor. A cache that
names a descriptor with a different element size, a different reference count, a
different reference table, or the wrong kind (record where an array is required,
interleaved table where a column table is required) is discarded and replaced. A
compiler that emits a plan which does not hold loses the optimization, never
correctness.

Deterministic storage is stricter still, because a pool or arena region does not
trace: `nrq_cdmm_pool_create_layout` and `nrq_cdmm_arena_allocate_layout` accept
only a plain record layout, and refuse a layout with managed references, SoA
columns, a custom trace callback, or an array.

### Coverage added

- `tests/unit/alloc_hint_test.cpp` — planner policy, boundaries, rejection,
  packing, and agreement with `nrq_alloc_plan_for_layout`.
- `tests/unit/gc_planned_allocation_test.cpp` — planned array and SoA allocation,
  cache population and reuse (proved through the layout-memo miss counter),
  rejection of a record descriptor used for an array, rejection of an interleaved
  descriptor used for a SoA, one-element arrays, and planned/typed agreement.
- `tests/unit/cdmm_layout_test.cpp` — pool and arena geometry from a descriptor,
  and refusal of layouts a deterministic region cannot trace.
- `tests/unit/cdmm_pool_release_test.cpp` — the alpha rewrite: scattered release
  and reset, duplicate release, destructor counts, and generation wraparound.
- Integration expectations were updated where they name the allocation symbol
  (`gc_array_element_barrier`, `gc_owned_storage_not_collector`,
  `gc_return_root_handoff`, `MlirPipelineTest`).

### Verification

| Check | Result |
|---|---|
| Unit tests | 426/426 pass (394 before Phase 6, 402 after alpha, 426 after beta) |
| Integration suite | 624/624 pass |
| Configuration equivalence, `*gc*` | 67 agree, 0 disagree |
| Pool benchmark | before/after recorded with spread |

## Second half: coverage, memo, wiring, validation

After the plan and the site cache existed, the remaining work was to find what still
bypassed them and to fix the lookup they sat on. Full detail in
`phase-6-coverage.md`; the path table is
`docs/memory-model/allocation-coverage.md`.

- The four-entry layout memo was replaced by a sixteen-set, two-way table indexed
  by shape. Typed eight-shape allocation went from 78.15 to 52.26 ns/op.
- `nrq_gc_allocate_planned` added record allocation with a plan.
- `nrq_cdmm_pool_create_planned` and `nrq_cdmm_arena_allocate_planned` added
  plan-driven deterministic regions.
- Records were confirmed to be owned storage, so no compiler record path needs a
  plan; that is recorded as an intentional fallback rather than left unexplained.
- Validation gained property tests across sizes, a memo-behaviour test, cache
  rejection tests, and a four-thread planned-allocation test.

## What this phase did not do

- No unified garbage-collection redesign. That is Phase 7; nothing here changes
  the collector's reachability, page classes or marking.
- No static *selection* of arena or pool from a hint. The API exists, the planner
  can describe deterministic storage, and the compiler still lowers explicit
  `arena`/`pool` code as it did before; choosing between them automatically is not
  part of this phase.
- No attempt to remove the runtime's descriptor interning. The cache shortens the
  path to a descriptor; it does not replace the descriptor.

## Limitations

- The site cache only pays when a program's shapes exceed the layout memo, which is
  measured honestly in `phase-6-performance.md`: no difference for one shape, about
  1.46× for eight round-robined shapes.
- Measurements come from one machine and the development runtime.
- The alpha and beta benchmarks were run in separate processes from their
  baselines, not as paired per-iteration samples.
