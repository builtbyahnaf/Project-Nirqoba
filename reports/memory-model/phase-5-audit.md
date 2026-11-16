# Phase 5 audit — arena and pool support

**Starting version:** `0.551-beta`  
**Starting commit:** `793cd97` (`docs: add the Phase 4 completion report`)  
**Working tree at audit start:** clean before Phase 5 edits; branch `main`.

## 1. What already exists

### Ownership representation

- `ValueOwnership::ArenaBound` already exists in `compiler/include/nirqoba/Common/Type.h`.
- `ownership_compatible_with_type` accepts pointer, owned-array, SoA and aggregate
  shapes for `ArenaBound`.
- The TIR verifier already rejected function returns with `ArenaBound` ownership.
- No arena/pool allocation ever produced an `ArenaBound` value. The category was
  representational only.

### Allocator runtime

`runtime/src/allocator_windows.cpp` already provides:

- `ArenaAllocator`: bump allocation, aligned allocation, additional backing chunks
  when the current chunk cannot serve a request, no-op per-object deallocation, and
  `reset()` that frees all chunks.
- `FixedPoolAllocator`: size-class buckets, free-slot reuse, alignment-aware
  strides, additional chunks, `deallocate()` returning a slot to its bucket, and
  `reset()` that frees all chunks.
- A per-pointer ownership registry with allocator identity, size and alignment.
- `nrq_memory_allocate_from(handle, bytes, alignment)` and
  `nrq_memory_deallocate_checked(pointer)`.
- `nrq_allocator_reset` refuses to reset while live allocations are registered.

The runtime was therefore reusable. Phase 5 does not introduce a parallel
allocator.

### Compiler allocation paths

- `memory.arena_allocator(capacity)` and `memory.pool_allocator(max_object_size)`
  already created opaque `u64` allocator handles.
- `memory.allocate<T>(bytes, handle)` already allocated from a specific handle.
- Raw allocator handles had no language lifetime: any handle could be copied,
  reset, destroyed or used after destruction, and allocated pointers were ordinary
  `Owned` values with individual `memory.deallocate` cleanup.

### Verification infrastructure

- `TIR/Verifier.cpp` verifies ownership categories, allocation strategy contracts,
  destruction obligations, and consuming release calls.
- `TIR/BorrowFlow.cpp` verifies borrows over the CFG, including merges and loop
  back-edges.
- Sema tracks allocation ids, lexical borrows, aggregate field borrows and
  owner lifetimes.

## 2. Gap analysis

Phase 5 needs four things the existing state lacks:

1. **Arena identity on values.** `ArenaBound` without an arena id cannot answer
   "which lifetime owns this value?" or "which values does this reset invalidate?".
2. **Bulk reset/destroy that is legal while registered allocations exist.** The
   existing `nrq_allocator_reset` correctly refuses live allocations, but arena
   bulk release must invalidate them deliberately as part of reset, not reject the
   arena's whole purpose.
3. **Scoped source semantics.** Creation, allocation, reset, destruction and pool
   release need language rules that reject escapes, individual deallocation and
   use after reset.
4. **TIR preservation and verification.** Arena identity must survive Sema → TIR,
   and invalid arena states must be rejected without a second, disconnected
   verifier.

## 3. Chosen model

### 3.1 Tracked versus raw allocator handles

The existing raw constructors stay exactly as they are:

```text
memory.arena_allocator(capacity)   // raw allocator handle, existing semantics
memory.pool_allocator(max_size)    // raw allocator handle, existing semantics
```

Phase 5 adds tracked strategy constructors:

```text
memory.arena(capacity)             // tracked arena handle
memory.pool(max_object_size)       // tracked pool handle
```

The source representation remains an opaque `u64`; the compiler attaches a unique
arena id and kind (1 = arena, 2 = pool) to the binding. Raw handles are not
silently promoted. This preserves all existing allocator tests and gives the new
semantics an explicit entry point.

### 3.2 Arena-bound allocation

Allocation reuses the existing generic allocation spelling and lowering:

```text
let p = memory.allocate<T>(bytes, arena);
```

When the second argument resolves to a tracked arena/pool binding, Sema marks the
result `ValueOwnership::ArenaBound` and assigns the arena id. Lowering is unchanged:
it calls `nrq_memory_allocate_from` with the runtime handle. As a result the value
never receives GC headers, roots, barriers or safepoints.

Arena/pool payloads must satisfy the existing explicit-allocation rule: no
deterministic cleanup obligation. This is the frozen target-architecture rule for
storage-only arenas. Phase 5 does not pretend to run destructors from bulk release;
instead it rejects payloads whose destruction bulk release could not perform.

### 3.3 Reset and destruction

```text
memory.arena_reset(arena)          -> i32
memory.arena_destroy(arena)        -> i32
memory.pool_reset(pool)            -> i32
memory.pool_destroy(pool)          -> i32
```

Sema rejects reset/destroy when an arena-bound value or borrow with that arena id
is still live in a visible scope. After a valid reset, every previous arena-bound
binding is marked moved, so use-after-reset is a compile-time error. New
allocations after reset receive the same arena identity but are fresh values.

The runtime functions `nrq_arena_reset` and `nrq_arena_destroy`:

- validate the handle and its allocator kind;
- refuse when an allocation operation is in flight;
- erase every registry record owned by that region, deliberately invalidating its
  storage;
- reset the region's backing chunks;
- destroy also removes the allocator handle.

They do not run object-level destructors. The language restriction above makes
that sound for every accepted payload. Repeated reset succeeds when quiescent;
repeated destroy returns zero without invalidating anything else.

### 3.4 Escape rules

Arena-bound values are rejected in:

- function returns;
- owning function parameters;
- borrowed returns whose source is arena-bound;
- aggregate/struct fields and array/tuple literals;
- assignment to a non-arena owner or to a different arena id;
- `memory.deallocate` and other ordinary consuming releases.

Borrows of arena-bound values remain normal Phase 3 borrows. Reset/destroy checks
see both arena-bound bindings and their borrows. Pointer arithmetic and address-of
propagate the arena id in TIR so a borrow cannot lose its arena identity.

### 3.5 Pool release

```text
memory.pool_release(pointer)       -> void
```

Only a value allocated from a tracked pool is accepted. Sema rejects a second
release, a release while a borrow is live, and any later use of the released
binding. Lowering calls `nrq_memory_deallocate_checked`, which returns the slot to
the pool's size class; the backing storage stays with the pool until reset/destroy.

### 3.6 TIR contract

`tir::Value` carries `arena_id` in addition to ownership. The verifier checks:

- every `ArenaBound` value has a non-zero arena id;
- that id names a `memory.arena`/`memory.pool` handle in the same function;
- `memory.allocate` from an arena produces ordinary pointer storage whose arena id
  equals its second argument's arena id;
- reset/destroy target a tracked handle;
- no arena-bound or borrowed value is used after reset/destroy;
- no borrow tied to the arena is live at reset/destroy;
- no `ArenaBound` value feeds `memory.deallocate` or another ordinary release;
- no `ArenaBound` value feeds an owning parameter;
- `ArenaBound` values do not create individual drop obligations.

## 4. Required changes

| Area | Change |
|---|---|
| AST | `arena_id`/`arena_kind` on expressions |
| Sema symbols | arena id, kind, `arena_bound`; live-value and invalidation helpers |
| Sema checking | tracked constructors; allocation ownership; reset/destroy/release; escape rules |
| TIR | arena id on `Value`; load/call/GEP/address-of propagation |
| TIR verifier | arena identity, allocation contract, reset/destroy, release and parameter rules |
| Lowering | reset/destroy/pool-release runtime calls; tracked constructors map to existing kinds |
| Runtime | `nrq_arena_reset`, `nrq_arena_destroy` bulk invalidation |
| Tests | lifecycle, ownership, lifetime, escapes, pool and GC-independence |
| Docs | target architecture, plan status, progress, version log, Phase 5 report |

## 5. Explicit non-goals

- No Phase 6 shared/weak work.
- No Phase 7 strategy selection.
- No Phase 8 FFI/manual-memory work.
- No general custom allocator syntax.
- No automatic arena inference or promotion.
- No runtime borrow tracking.
- No new GC implementation.
- No object destructors created for storage-only arenas; unsupported payloads are
  rejected instead.

## 6. Known risk carried into implementation

Arena identity is source/TIR metadata. MLIR and LLVM see ordinary pointer storage
and the runtime handle; they do not receive a per-object arena tag. That is
intentional: adding per-object tags would create the per-object overhead Phase 5 is
meant to avoid. The correctness boundary is therefore Sema + TIR verification plus
the runtime handle contract, and the report records this limitation explicitly.
