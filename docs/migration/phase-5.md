# Migration Note — Phase 5: Arena and Pool Support

**Version:** `0.563-beta`
**Scope:** MMM Phase 5. No new parser syntax, no second allocator, no GC removal.

Phase 4 separated ordinary owned allocation from the collector. Phase 5 adds
tracked grouped lifetimes on top of the existing arena and pool allocators.

## New tracked constructors

```text
let arena = memory.arena(capacity_bytes);
let pool  = memory.pool(max_object_size);
```

`memory.arena_allocator(capacity)` and `memory.pool_allocator(max_size)` remain
the raw, untracked handles with their previous explicit reset/destroy behavior.
The compiler does not silently promote a raw handle.

## Allocation

```text
let p = memory.allocate<T>(bytes, arena);
```

An allocation from a tracked handle is `ArenaBound` and carries the handle's
arena id. It is not individually owned and gets no automatic per-object
destruction. The payload must not require deterministic cleanup; rejecting such
payloads is what makes bulk release sound without a destructor walk.

Arena allocations lower to `nrq_arena_allocate` (a registry-free bump path).
Pool allocations keep `nrq_memory_allocate_from` because release must recover the
slot's size class.

## Reset, destruction and release

```text
memory.arena_reset(arena);
memory.arena_destroy(arena);
memory.pool_reset(pool);
memory.pool_destroy(pool);
memory.pool_release(p);
```

Reset/destroy require the matching live tracked handle and are rejected while an
arena-bound value or borrow is live. A successful operation invalidates every
prior binding in that lifetime, so later use is a compile-time error. Destroy also
invalidates the handle. Repeated reset is supported; a second destroy is rejected.

Pool release accepts only a value from a tracked pool, rejects a live borrow or a
second release, and marks the slot moved. The backing pool storage remains until
reset/destroy.

## Escape rules

Arena-bound storage may not be returned, passed to an owning parameter, returned
through a borrowed-return contract, stored in an aggregate field or array
literal, assigned to a non-arena owner, moved to a different arena id, or released
with `memory.deallocate`. Borrowed parameters and ordinary reads remain valid
while the arena is live.

## Verification

TIR values preserve arena id and kind through loads, calls, moves, projections
and address-of. The verifier rejects unknown arena identities, allocation/result
mismatches, ordinary release of arena storage, owning escapes, and use of a value
or borrow after bulk release. The existing CFG-aware borrow-flow pass still runs
over the function.

## Compatibility

Valid Phase 1–4 programs are unchanged. The new rules apply to the new tracked
constructors and to values they allocate. Raw allocator handles, pool/arena
runtime entry points and ordinary allocation keep their old contracts. Bulk
release never runs destructors; payloads that need them are rejected rather than
silently leaked.

## What did not change

The collector remains fully operational for explicit managed arrays, SoA,
managed fields and cyclic graphs. No weak-reference or shared-completion work,
automatic arena selection, FFI, manual-memory integration, runtime borrow
tracking or exception unwinding was added. Phase 6 and later were not started.

## Verification

`320` unit tests and `583` full-run cases pass. New `tests/memory_model` cases
cover lifecycle, growth, repeated reset, borrow scopes, branches and loops,
escapes, invalid bulk release and pool lifetimes. Three IR fixtures assert that
arena and pool programs contain no `nrq_gc` reference at `-O0` or `-O2`.
