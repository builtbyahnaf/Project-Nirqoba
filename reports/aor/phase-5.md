# AOR Phase 5 — arena and pool strategies

**Status:** complete  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Version:** `0.601-beta`  
**Build:** `build/msvc-dev`, MSVC 19.51 (VS 18 BuildTools), `RelWithDebInfo`, x64

## What this phase delivers

Phase 4 removed the header from collector-managed objects. Phase 5 does the same
for the two deterministic strategies and makes them usable as runtime APIs:

| Requirement | Delivery |
|---|---|
| Arena creation, allocation, reset, destruction | `nrq_cdmm_arena_*`; chunks committed at creation, capped by `max_bytes`, reset rewinds to the first chunk |
| Efficient bulk allocation and bulk destruction | scope + bulk calls (one pin per batch); reset is O(chunks), not O(objects) |
| Destructors for non-trivial objects, including nested owned resources | per-object record inside the arena, run newest-first on reset/destroy |
| Prevent references outliving the arena | generation advanced per reset, `arena_contains`, scope pin that blocks reset; compile-time rules stay where they were |
| Alignment, growth, exhaustion, failure | power-of-two alignment, growth chunks, hard cap, every failure returns null with the arena unchanged |
| Fixed-size pools, slot allocation/release/reuse | `nrq_cdmm_pool_*`; intrusive free list in the slots, one liveness bit per slot |
| Object state without per-object overhead | 1 bit/slot; 4 bytes more only when generations are requested; destructor is per pool |
| Double-free, stale references, reuse corruption | liveness bitmap, exact-slot address check, 32-bit generations, poison on release |
| AOR integration | `strategy_of` / `object_facts` report header size 0 and amortized metadata; range table separates the two metadata systems |
| Coexistence | arenas, pools, owned allocation and the collector run together; tests assert neither claims the other's storage |
| No speculative strategy selection | no compiler pass, no language change; only runtime APIs and metadata |

## Design decisions

**Why a second handle table.** Explicit-strategy handles live in their own table
(`cdmm_handles`) guarded by a shared mutex, separate from the legacy allocator
map. A shared lock covers lookup plus pinning, so a concurrent destroy cannot
reclaim an allocator between the two, while the allocation itself runs outside the
lock. It also means the legacy `nrq_arena_reset`/`nrq_arena_destroy` cannot
invalidate a CDMM handle behind its back.

**Why destructor records live in the arena.** An arena is heterogeneous, so the
destructor cannot be per arena. Putting the record in the arena keeps the payload
headerless and charges only objects that asked for deterministic cleanup. A
failed payload allocation rewinds the arena to its pre-call checkpoint, so a
half-registered object cannot survive into the next generation.

**Why the pool's free list is in the slots.** A side vector of free indices costs
8 bytes per free slot — 50% overhead at 16-byte slots. Threading the list through
the slots themselves costs nothing and keeps reuse O(1).

**Why generations are optional.** A liveness bit plus an exact-address check
catches double release and foreign pointers for 1 bit per slot. Catching a
release through a stale reference *after* the slot was reused requires a
generation per slot; making it optional keeps the default metadata minimal and
makes the cost explicit (4 bytes per slot) for pools that need it.

**Why a scope exists.** The single-object entry points take a shared handle lock
and the allocator's operation lock. That is correct for arbitrary callers, but a
frame loop allocates thousands of objects; the scope pays both once and keeps the
strategy free of locks for the duration. Reset and destroy are refused while it is
open, which is the same pin count that makes the refusal reliable.

**Why the first chunk is committed eagerly.** A created arena or pool either has
its initial capacity or does not exist. Lazy commit made `committed` read zero
after creation, which is a reporting lie and hides OOM to the first allocation.

## Bugs found and fixed

| Symptom | Cause | Fix |
|---|---|---|
| `committed` reported 0 after creating an arena with a cap | first chunk was committed lazily on first allocation | both strategies commit their first chunk at creation and fail creation if it cannot be met |
| Pool bulk release cost a mutex per slot in a scope | scope release reused the serialized entry point | scope release uses an unserialized path; the liveness checks are identical |
| Benchmark reported twice the destructor calls | the benchmark's counter was static across runs, not a runtime defect | counter reset per run; the benchmark now exits non-zero on a mismatch |
| A frame loop could not reset inside its scope | reset refuses while pinned, by design | benchmark and documentation close the scope before reset |

The first two were real runtime defects introduced in this phase; both are fixed
and covered by tests (`CapIsHonouredAndFailuresLeaveUsageUnchanged`,
`ScopeBulkAllocatesAndReleasesWithOnePin`). No defect was deferred.

## Runtime layout

| Piece | Location |
|---|---|
| Public API | `runtime/include/nirqoba/runtime/cdmm.h` |
| Arena extensions, destructor records, cap, generation | `runtime/src/allocator_windows.cpp` (`ArenaAllocator`) |
| Fixed-slot pool | `runtime/src/allocator_windows.cpp` (`CdmmPool`) |
| Address-range table | `runtime/src/allocator_windows.cpp` (`cdmm_register_range` and friends) |

## Tests

| File | Coverage |
|---|---|
| `tests/unit/cdmm_arena_test.cpp` | layout, alignment, reset/reuse, generation, destructor order, nested owned release, destroy, cap, failed-payload rollback, scope, bulk prefix, invalid arguments |
| `tests/unit/cdmm_pool_test.cpp` | distinct/aligned slots, LIFO reuse, double release, foreign and interior addresses, generations, destructor on release/reset, reset reclaim, growth, facts, scope bulk, destroy, invalid creation |
| `tests/unit/cdmm_coexistence_test.cpp` | explicit storage is not managed, collection leaves explicit storage intact, arena reset leaves collector objects reachable, pool release does not disturb other strategies, destructors run while the collector is live |
| `tests/unit/cdmm_stress_test.cpp` | 200 frame resets on one chunk, 100 rounds of 512-slot churn with scrambled release order, four threads allocating and releasing concurrently, range unregistration over 50 create/destroy cycles, no aliasing between strategies, owned allocation unaffected |

Result: 394 unit tests pass (366 before this phase plus 28 new cases), and the
full 624-case suite passes.

## Limitations

- The pool's generation is 32-bit and wraps; with 2^32 allocations of one slot the
  stale-release check would accept one stale generation. Reaching it is not
  realistic, but it is not impossible.
- Released slots are poisoned with `0xDD`, which costs a write over the whole slot
  on release. The benchmark measures it; it is not optional.
- A scope is single-threaded by contract. Two threads sharing one scope would race
  on the strategy's internal state; the API does not detect it.
- `nrq_cdmm_strategy_of` is a diagnostic: it takes a global range lock. It is not
  intended for per-object use in a hot loop.
- The new API is not yet reachable from the language. `memory.arena` and
  `memory.pool` keep their existing lowering, and connecting the two is compiler
  work for a later phase.
- Benchmarks are one machine, one compiler, one synthetic workload per shape; see
  `phase-5-performance.md`.
