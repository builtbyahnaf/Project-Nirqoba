# Migration Note — AOR Phase 5: arena and pool strategies

**Scope:** runtime API only. No language syntax, no compiler strategy selection,
no change to the collector, and no change to existing programs.

Phase 4 removed the object header from managed objects. Phase 5 gives the two
deterministic strategies the same treatment and makes them first-class runtime
APIs: an arena that destroys a whole region at once, and a fixed-slot pool that
reuses objects without a header or a per-object record.

## What is new

`nirqoba/runtime/cdmm.h`:

```c
void* nrq_cdmm_arena_create(size_t initial_bytes, size_t max_bytes);
void* nrq_cdmm_arena_allocate(void* arena, size_t bytes, size_t alignment);
void* nrq_cdmm_arena_allocate_destructible(void* arena, size_t bytes,
                                           size_t alignment,
                                           NrqCdmmDestructor fn, void* context);
int32_t nrq_cdmm_arena_reset(void* arena);
int32_t nrq_cdmm_arena_destroy(void* arena);

void* nrq_cdmm_pool_create(size_t slot_bytes, size_t alignment,
                           size_t slots_per_chunk, uint32_t flags,
                           NrqCdmmDestructor fn, void* context);
void* nrq_cdmm_pool_allocate(void* pool);
int32_t nrq_cdmm_pool_release(void* pool, void* slot);
int32_t nrq_cdmm_pool_release_checked(void* pool, void* slot, uint64_t generation);
int32_t nrq_cdmm_pool_reset(void* pool);
int32_t nrq_cdmm_pool_destroy(void* pool);
```

Plus scope structs for the bulk path, bulk allocate/release helpers, statistics,
containment checks, and `nrq_cdmm_strategy_of` / `nrq_cdmm_object_facts` for
introspection.

## What existing code does differently

Nothing. `memory.arena(capacity)` and `memory.pool(max_size)` still create the
legacy allocators they always did, with their existing reset and destroy rules,
and `memory.arena_reset` / `memory.arena_destroy` still refuse while a value is
live. The new handles live in a separate table, so the legacy entry points cannot
reset or destroy them and vice versa.

One behavior changed inside the shared arena implementation: an arena created
through `nrq_allocator_create(NRQ_ALLOCATOR_ARENA, ...)` now commits its first
chunk at creation instead of at the first allocation, and `nrq_arena_reset`
runs registered destructors (there are none for legacy handles). Both are
observable only through the existing capacity/occupancy reporting, which is now
accurate before the first allocation.

## Adopting arenas

1. Create one per phase of work, with a cap if the phase has a known bound.
2. Allocate from a scope when more than a handful of objects is allocated; the
   scope is how the registry lock and the operation lock are paid once instead of
   once per object.
3. Register a destructor for any object that owns another allocation. The
   destructor may free ordinary owned storage.
4. Reset at the end of the phase, or destroy the arena. Every pointer into it is
   invalid afterwards; a stored generation makes that checkable at runtime.

## Adopting pools

1. Create one per object type, with the slot size and alignment the type needs.
2. Enable generations if handles to slots can outlive the allocation and a stale
   release must be rejected rather than accepted by address.
3. Use a scope for bulk allocation and release.
4. A destructor is per pool, which follows from a pool being one type.

## What is deliberately not here

- No compiler pass chooses a strategy. The metadata a future pass needs
  (`strategy_of`, header size, object size, generation) is exposed; the decision
  is not automated.
- No moving collector integration: arena and pool storage never becomes
  collector-managed, and a managed object is never relocated into an arena.
- No change to `memory.allocate<T>` lowering. A tracked arena allocation still
  lowers to `nrq_arena_allocate`, the registry-free bump path from the earlier
  phase; the new API is additive.

## Guarantees kept

- Deterministic ownership stays the default for ordinary allocation.
- The collector remains available and unchanged.
- Debug and production runtimes expose the same API and semantics; only their
  internal checks differ, as before.
