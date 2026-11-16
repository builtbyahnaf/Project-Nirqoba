# Migration Note — Phase 6: Explicit Shared Ownership

**Version:** `0.571-beta`
**Scope:** MMM Phase 6. No new syntax, no GC redesign, no automatic shared promotion.

Phase 5 added grouped arena and pool lifetimes. Phase 6 completes the existing
explicit shared-ownership path and makes its runtime control state safe to share
across threads without putting reference counting on ordinary owned values.

## What now holds

- `memory.shared_allocate<T>(bytes)` creates one strong token. A copy made with
  `memory.shared_retain` creates another token; a move transfers a token without
  retaining.
- Aggregate construction takes the initial token without an extra retain.
  Aggregate copy retains every shared field, including nested fields. Aggregate
  move transfers the token without retaining. Destruction releases each field
  once in reverse declaration order.
- The final strong release removes the token before deallocation, so a concurrent
  retain cannot resurrect an object whose destructor has started.
- Shared counts live in pointer-selected shards. Retain, non-final release and
  count query take one shard lock and one atomic count; they no longer serialize
  on the global allocator mutex.
- `memory.shared_retain` returns null when the count is exhausted or the token
  is gone. `memory.shared_release` returns zero for an unknown or already-final
  token. The language does not expose a separate overflow error object.
- A shared owner cannot be released while a borrow of its storage is live. The
  diagnostic now appears at the release site.
- TIR requires a shared token or shared-pointer type for `memory.shared_retain`
  and `memory.shared_count`; malformed TIR is rejected before lowering.
- Plain shared storage still emits no `nrq_gc` reference. Shared storage that
  contains managed fields keeps its typed root until the final release.

## Concurrency contract

The runtime uses 64 pointer-selected shards. Each shard owns a control map and
mutex; each control has an atomic strong count. The shard lock protects map and
control lifetime, while the atomic count gives a defined 1→0 transition.

The final transition uses release ordering; retain uses acquire/release CAS and
refuses `UINT64_MAX`. A non-final release uses acquire/release subtraction under
the shard lock. The allocator ownership record remains in flight during final
deallocation, so allocator reset/destroy cannot race the callback.

Access to the pointed-to data is not made thread-safe by the reference count.
Use the existing mutex resource when data is shared mutably across threads.

## Weak references and cycles

Weak references remain intentionally unimplemented. There is no `weak` source
type, parser production or standard-library container that requires one. The
reserved `ValueOwnership::Weak` category remains a representation placeholder.

A `memory.shared_allocate<T>` payload that contains a shared field is rejected
because the final release frees raw storage without walking field destructors.
A supported shared payload therefore cannot contain an owning edge back to
another shared allocation, so a shared-to-shared reference cycle is not
constructible through the supported path. Cyclic graphs remain the domain of the
existing explicit collector. This is a documented limitation, not a claim that
reference counting collects cycles.

## Arena reset reuse

`memory.arena_reset` now retains only the initial capacity-bounded chunk. Growth
chunks are released, so repeated reset avoids a fresh allocation without pinning
an arbitrarily large peak working set. Destroy releases the retained chunk.
Existing reset invalidation and `ArenaBound` identity rules are unchanged.

## Compatibility

Valid Phase 1–5 programs keep their meaning. The new source diagnostic rejects a
shared release while a borrow is live; the old behavior failed later in TIR
verification. Raw allocator handles, arena/pool semantics, ordinary allocation
and collector behavior are unchanged.

## Verification

`323` unit tests and `593` full-run cases pass. Phase 6 adds focused language
cases for multiple owners, copy retain, move without retain, parameter release,
nested aggregate copy, loop scope, borrow scope, double release, shared release
with a live borrow and shared cycle-payload rejection. The C++ runtime tests cover
independent shard progress, concurrent retain/release and arena reset reuse.

The benchmark runner is
`benchmarks/memory-model/phase-6/run_benchmark.py`; exact results are in
`results.json` and summarized in `reports/memory-model/phase-6.md`.
