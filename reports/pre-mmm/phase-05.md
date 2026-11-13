# Pre-MMM Phase 5 — GC/ownership boundary

**Version:** `0.469-beta`  
**Date:** `2026-11-14`  
**Scope:** explicit allocator-handle lifetime experiment; typed GC roots for owned
pointer storage; roots for shared allocations; cleanup-exit and reallocation
lifetimes.

## 1. Result

Phase 5 establishes the boundary in both directions:

- GC-managed objects stay in the collector domain. They are not released by
  `memory.deallocate`, `memory.reallocate`, or shared release.
- Explicitly owned and shared storage stays outside the collector heap, but any
  managed fields inside it are visible to tracing while the storage is live.
- The root is published only for storage that actually contains a managed field.
  Ordinary owned allocations and shared blocks without managed fields keep no GC
  metadata.
- Roots are removed before explicit release, before reallocation publishes the
  replacement, and when an activation frame ends for stack-placed storage.

Two implementation upgrades were required: typed roots for owned pointer
storage, then lifetime-aware roots for shared allocations. A follow-up within the
first upgrade fixed a stack-root scope bug found by the new cleanup-exit test.

## 2. Part A — explicit allocator-handle lifetime

The Phase 4 path kept the process-default fast path free of the global allocator
mutex and operation pin. Explicit per-allocation handles still took both on
release, reallocation, and final shared release even though every live allocation
already has an ownership record.

The experiment replaced that pin with the ownership record as the lifetime
reference. `nrq_allocator_destroy` and `nrq_allocator_reset` scan the shards under
the allocator mutex and reject while a record owned by that allocator is live or
has `operation_in_flight` set. A release callback therefore keeps its allocator
alive through the record, not through an extra pin. Allocation keeps its pin
because no record exists until the allocator returns a pointer.

Measured with 10,000 allocate/write/free pairs per worker, five runs, median
microseconds, Windows 11 x64 / MSVC 14.51 / `RelWithDebInfo`:

| Workers | 0.465-beta | 0.469-beta | Change |
|---:|---:|---:|---:|
| 1 | 2,330 | 1,840 | **-21.0%** |
| 2 | 5,005 | 3,650 | **-27.1%** |
| 4 | 16,068 | 8,622 | **-46.3%** |

The explicit path remains slower than the default path because handle resolution
and the allocation-side pin still use the global registry. The change is retained:
it removes redundant synchronization without weakening reset/destroy safety. The
focused tests block inside a custom allocator deallocate callback and assert that
destroy and reset return 0 while the in-flight record exists, then return 1 after
the callback and record are gone. Raw samples and methodology are in
`benchmarks/pre_mmm_phase5/`.

## 3. Upgrade 1 — owned pointer storage as a typed root

### Defect found

An owned pointer allocated with `memory.allocate<T>` could contain `managed`
fields, but the allocation was not registered as a GC root. A probe that stored a
managed child at `(*holder).child` and forced two collections returned stale data
with `live_objects == 0`; after the fix the same probe returns `1` and `73`.

### Implementation

- Heap `memory.allocate<T>` publishes a typed root with the pointee's managed
  offsets and object size.
- Stack-placed `memory.allocate<T>` publishes the same typed root inside the
  activation root frame, so frame end removes it.
- `memory.reallocate<T>` unregisters the old address and registers the new one
  only when reallocation succeeds; a failed reallocation leaves the original root
  valid.
- `memory.deallocate` removes the typed root before the storage is freed.
- `nrq_gc_root_register_typed` treats a null slot as a no-op; a malformed non-null
  descriptor remains fatal.

### Stack scope correction

The first implementation registered stack-placed storage directly, before the
activation frame began. The new cleanup-exit test returned from a helper and found
`live_objects == 1` after the return: the slot still pointed at dead stack
storage. The corrected lowering marks a function with managed fields inside owned
storage as GC-using, opens its frame, and publishes the root immediately after the
stack address exists. The regression now prints `1`, `73`, `7`, `0` before and
after the helper returns.

## 4. Upgrade 2 — shared allocation roots

`memory.shared_allocate<T>` creates reference-counted storage outside the GC heap.
Non-final releases return early without touching the block, so the root must
survive every non-final release and disappear only with the final one.

The runtime stores root metadata for a shared block when the pointee has managed
offsets. `nrq_memory_shared_allocate_rooted` registers the typed descriptor after
the block and ownership record exist. `nrq_memory_shared_release`:

- leaves the root registered while the count remains above one;
- unregisters it before the final free;
- re-registers it if the allocator rejects the final release and the block remains
  valid.

`memory.shared_allocate` without managed fields keeps using the unrooted entry
point, so ordinary shared storage pays nothing for this feature.

## 5. Verification

| Check | Result |
|---|---|
| Build (RelWithDebInfo) | PASS |
| Unit suite | 228/228 |
| Integration suite | 455/455 |
| Owned heap pointer root probe | `1`, `73` |
| Stack-owned pointer root probe | `1`, `73` |
| Reallocated owned pointer probe | `1`, `73` |
| Cleanup-exit owned pointer probe | `1`, `73`, `7`, `0` |
| Shared allocation root probe | `1`, `73`, `0` |
| Explicit release/reallocation races with blocked callback | PASS |
| Default-path contention benchmark | PASS, see section 2 |

The integration suite includes the new positive tests
`owned_pointer_gc_roots`, `stack_owned_pointer_gc_roots`,
`owned_pointer_gc_reallocation`, `owned_pointer_cleanup_gc_roots`, and
`shared_pointer_gc_roots`. Existing negative tests already reject
`memory.deallocate` on managed, borrowed, shared, and resource values, and
`shared_wrong_release` / `shared_to_unique_parameter` reject wrong-domain shared
operations.

## 6. Domain boundary matrix

| Value | Reclamation | Root strategy |
|---|---|---|
| `managed<T>` / `array.allocate<T>` | GC tracing and collection | activation/frame roots plus typed array descriptors |
| `owned ptr T` with no managed fields | explicit deallocate or cleanup | none |
| `owned ptr T` with managed fields | explicit deallocate or cleanup | typed root while live; unregistered before free / at frame end |
| `shared ptr T` with no managed fields | shared reference count | none |
| `shared ptr T` with managed fields | shared reference count | typed root until the final shared release |
| `owned array<T>` | explicit release or cleanup | existing array root table, including managed elements |
| native resource | lexical cleanup or explicit close | none; GC never finalizes it |
| cyclic managed graph | GC | collector trace and cycle handling |

## 7. Limitations

- The explicit allocator-handle path remains globally serialized by handle lookup
  and the allocation-side operation pin. The measured improvement is on release,
  reallocation, and final shared release, not on allocation.
- The ownership record is the allocator lifetime reference; there is no separate
  refcount. This is sufficient because destroy/reset refuse while a record is live
  or in flight, but it means destroy/reset can transiently return 0 under
  concurrent explicit allocation.
- Stack-placed owned roots live only for the activation. Returning the pointer
  still requires the normal move/return rules; the root follows the moved owner.
- A pre-existing compiler crash remains for an aggregate field declared as
  `shared ptr T` or `owned ptr T` when `T` itself contains a managed field. The
  same shape compiled from a local allocation is covered by the new tests. The
  crash reproduces at `0.465-beta` and is not introduced by this phase; it needs
  separate front-end/lowering work.
- No ASan/UBSan run is available in the MSVC configuration. The known
  `GcTest.MinorReclaimsOnlyYoungGarbageAndKeepsOldGarbage` stress flake recorded in
  Phase 4 remains separate.

## 8. Commits

- `allocator: use live ownership records as the explicit release lifetime guard`
- `benchmark: record explicit allocator-handle lifetime results`
- `gc: register explicitly owned pointer storage as typed GC roots`
- `gc: keep shared allocation roots alive until the final release`
- `gc: scope stack-owned roots to the activation frame`
- `test: cover owned pointer root removal on an early cleanup exit`
- documentation, roadmap, version, and progress commits follow this report.

## 9. Assessment

**Implementation completeness:** Complete for the supported Phase 5 boundary.
**Verification confidence:** High for heap/stack owned pointers, reallocation,
cleanup exits, shared final release, and explicit allocator races. The aggregate
field compiler crash is a known unsupported shape, not a verified guarantee.
**Recommended next step:** fix the aggregate-field lowering crash, then extend the
shared-root descriptor to nested shared fields once that shape compiles.
