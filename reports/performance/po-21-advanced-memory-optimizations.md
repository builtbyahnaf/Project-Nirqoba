# PO-21 - Advanced Memory Optimizations

**Task:** P2 3.12 - Advanced Memory Optimizations
**Date:** 2026-11-06
**Status:** Completed & Verified (unit and integration suites; runtime timing,
  compilation-time, and peak-memory measurements deferred under the project
  policy that this task must not be benchmarked without explicit permission)
**Compiler:** Nirqoba 0.127-beta (repository HEAD), LLVM/MLIR 23.1.4, MSVC x64
  Debug

---

## 1. Problem Identified

The hybrid memory architecture already had ownership, borrowing, TLABs,
generational/incremental GC, and explicit arena/pool/slab allocators. The
allocation and collection paths still carried avoidable fixed costs:

1. **Each managed object paid a 112-byte header** before its payload. For the
   small nodes and arrays that dominate allocation-heavy programs, the metadata
   was larger than the data.
2. **Every managed allocation updated three process-wide atomics.**
   `bytes_since_collection`, `pending_allocation_bytes`, and
   `pending_allocation_count` were written by every mutator thread on every
   allocation. PO-20 moved them onto an isolated cache line, which removed false
   sharing but not the true sharing between mutator threads.
3. **Emptied pages were freed and re-allocated every cycle.** At each cycle
   boundary the collector called `operator delete` for every page with no live
   objects; the next allocation wave called `operator new` again. Churn-heavy
   programs paid that cost on every collection.
4. **Reference diagnostics were expensive even when disabled.**
   `nrq_gc_get_ref_count` published the calling thread's TLAB before checking
   whether diagnostics were enabled, and the enabled path built an
   `unordered_map` for every object in the heap even though the caller asked
   about one object.

Ownership, borrowing, and lifetime guarantees were treated as fixed
constraints: none of the changes below changes when an object becomes
unreachable, which storage owns it, or which references the collector traces.

## 2. What Changed

### 2.1 A 96-byte managed object header

`ObjectHeader` now stores its forward link and pending-TLAB link in one pointer
slot. An object is only ever on one of those lists: it uses `tlab_next` while it
belongs to its owning mutator's unpublished batch, and `next` after it is
published. The payload address is no longer stored; it is computed as
`header + 1`, which is exactly where the page bump allocator places it.

| Field block | Before | After |
| --- | --- | --- |
| Links, page, size, element size, trace | 64 bytes (8 pointers) | 48 bytes (6 pointers) |
| Mark/young/age and birth epoch | 16 bytes | 16 bytes |
| Reference layout (2 inline offsets + overflow) | 32 bytes | 32 bytes |
| **Total** | **112 bytes** | **96 bytes** |

`static_assert`s pin both the size and the `max_align_t` alignment. The new
`nrq_gc_object_header_bytes()` accessor exposes the value so tests and tooling
can audit it without parsing the runtime source.

### 2.2 Per-mutator allocation accounting

`MutatorState` now owns the allocation counters for its thread:

- `pending_bytes` and `pending_count` are relaxed atomics. They are updated by
  the owning thread and read by `nrq_gc_get_stats` under `gc_mutex`, so
  statistics remain exact without a global write per allocation.
- `unflushed_bytes` accumulates allocation bytes locally. It is folded into the
  shared `bytes_since_collection` trigger only when a TLAB is published or when
  the local batch reaches `min(16 KiB, collection_threshold)`. A threshold of 1
  byte still flushes per allocation, so automatic-collection tests and the
  existing trigger semantics are preserved.
- `publish_tlab_locked` flushes the batch before resetting the per-mutator
  counters, and `nrq_gc_get_stats` sums the per-mutator pending fields while
  holding `gc_mutex`.

The steady-state allocation path no longer performs three shared atomic
operations. It performs no global atomic operation at all until a 16 KiB batch
(or the configured threshold, if smaller) is reached.

### 2.3 Bounded page recycling

Empty pages now flow through a bounded recycle pool instead of returning to the
process heap at every cycle:

- `recycle_page` moves an empty page into `recycled_pages` (at most 64
  ordinary-sized pages, roughly 4-8 MiB of backing storage) and resets its bump
  cursor, live-object count, and retired flag. Oversized large-object pages are
  still returned to the process heap so the page-count bound also bounds bytes.
- `acquire_page` takes a retained page whose capacity satisfies the request, or
  creates a new one. `nrq_gc_page_reuse_count` records successful reuses.
- Recycling still happens only in `prepare_cycle_locked`, after
  `stop_mutators()` has parked every mutator and after `gc_page_generation` has
  invalidated stale TLAB cursors. The sweep can therefore mark a page retired
  without moving its storage while another thread is still allocating from it.

The pool is bounded, so a program that stops allocating does not retain an
unbounded amount of address space.

### 2.4 Cheaper reference diagnostics

`nrq_gc_get_ref_count` now:

1. returns `-1` immediately when diagnostics are disabled, before publishing a
   TLAB batch;
2. when enabled, publishes the calling mutator's batch (semantics unchanged) and
   walks objects, global roots, shadow stacks, and pins with a
   `ReferenceCountTarget` that increments only when a visited reference equals
   the requested object;
3. adds the requested object's pin count directly instead of building a map and
   looking itself up in it.

The walk is still exact and still reports `-1` on failure; it simply stops
allocating an `unordered_map` proportional to the heap for a one-object query.

## 3. Compatibility With Ownership, Borrowing, and Lifetimes

- **Reachability is unchanged.** Header packing removes a derived field; it does
  not change which objects the collector traces or when a page is eligible for
  reuse. Page recycling waits for the same parked-mutator boundary as the
  previous delete path.
- **Borrowing is unchanged.** The compiler still decides ownership transfer,
  borrow exclusivity, and lifetime ends; the runtime only stores bookkeeping for
  objects the compiler already created.
- **TLAB batching is unchanged.** Objects remain invisible to the marker until a
  safepoint publishes them, and a rooted allocation still publishes before the
  root snapshot can run. The new accounting counter is published alongside the
  same batch.
- **Diagnostics are read-only.** Reference counting still observes ownership and
  pin state without changing it.

## 4. Structural Evidence

| Metric | Before | After |
| --- | --- | --- |
| Managed object header | 112 bytes | 96 bytes (14.3% smaller) |
| Shared atomic updates per TLAB allocation | 3 | 0 until a 16 KiB/threshold batch |
| Page storage operations per empty-page cycle | `operator new` + `operator delete` | recycled from a bounded pool |
| Disabled `get_ref_count` work | lock + TLAB publish + `-1` | lock + `-1` |
| Enabled `get_ref_count` temporary map | one entry per referenced object | one target counter |

No wall-clock, hardware-counter, compilation-time, or peak-memory numbers are
claimed. Those measurements are part of the Section 5 verification that this
task must not run without explicit permission.

## 5. Tests Added or Modified

| Test | What it locks down | Result |
| --- | --- | --- |
| `GcTest.ObjectHeaderStaysWithinItsMetadataBudget` | Header is 96 bytes, stays `max_align_t` aligned, and payloads are aligned | pass |
| `GcTest.UnpublishedTlabAllocationsRemainVisibleInStats` | Per-mutator pending batches are included in `live_objects` and `allocated_bytes` before and after leave | pass |
| `GcTest.EmptiedPagesAreRecycledAcrossCollections` | Empty pages are retired, then recycled and handed back out on the next wave | pass |
| `GcTest.ReferenceDiagnosticsCountOnlyTheRequestedObject` | 32 referrers are counted exactly while 64 unrelated allocations are ignored | pass |
| Existing GC, allocator, concurrency, root-lifetime, and pin tests | No regression in reachability, TLAB publication, barriers, or statistics | pass |

## 6. Verification

- `build/msvc-dev/tests/nirqoba_unit_tests.exe` - **144/144**.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe
  --configuration Debug` - **241/241**.
- The build directory was configured from a working tree that also carried an
  unrelated pending `0.131-beta` version bump. No version file is part of this
  task's commits, and the report records the repository HEAD version.
- Runtime performance, compilation time, and peak memory remain deferred; this
  report makes no timing or throughput claim.

## 7. Not Implemented, and Why

1. **Page-local object-start metadata and allocation bitmaps.** These change how
   marking discovers and resolves objects. They belong to the dedicated
   `3.13` object-discovery work and would have expanded this task into an
   algorithm change rather than an allocation-path optimization.
2. **Replacing the global allocator ownership registry with a lock-free
   structure.** Explicit-allocator frees must validate ownership and allocator
   identity, and the current registry also coordinates destruction and reset.
   The existing thread-local cache already removes the registry from the common
   default allocation path; a lock-free registry is a separate design with its
   own lifetime proofs.
3. **Parallel or concurrent sweeping.** The collector already slices marking and
   sweeping around mutator execution. Making reclamation itself parallel changes
   the collector's synchronization model and is deferred to the advanced GC
   work.
4. **Arena-specific fast path in the language lowering.** Arena allocation is
   already a bump pointer inside `ArenaAllocator`; the remaining overhead is the
   shared ownership registry, not the arena algorithm, and the compiler does not
   yet have an escape proof that would let it bypass the registry safely.

## 8. Remaining Opportunities

1. Size-class allocation for the managed heap to reduce alignment padding
   between differently sized objects.
2. Per-page free-space indexing so a TLAB refill avoids scanning even the short
   `gc_pages` vector.
3. A lock-free or sharded explicit-allocator ownership registry.
4. Page-local object-start metadata and allocation bitmaps as the foundation for
   `3.13`.
