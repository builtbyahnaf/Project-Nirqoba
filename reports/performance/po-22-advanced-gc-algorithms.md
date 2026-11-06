# PO-22 — Advanced Garbage Collection Algorithms

**Task:** P2 3.13 — Advanced Garbage Collection Algorithms
**Date:** 2026-11-06
**Status:** Completed & Verified (unit and integration suites; runtime timing and
hardware-counter measurements deferred per the project policy that this task must
not be benchmarked)
**Compiler:** Nirqoba 0.141-beta, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

The collector was correct but did expensive work per reference and per object in
every phase. Working from the requirement list in `tasks/CRITICAL_CO_TARGETS.md`
§3.13:

1. **Object discovery paid a hash probe per reference.** Every resolved pointer
   went through the page directory and then, when the page-local start bitmap
   rejected it as an interior or stale pointer, through a second full hash probe
   of the global `ObjectIndex`. Ordinary allocations also paid a hash insert, and
   ordinary reclamation a hash erase, although the bitmap already answered the
   same question.
2. **Marking used shared state.** Each reference compared against the global cycle
   epoch and loaded the collection phase through atomics, even though a mark
   context only exists while a cycle is open.
3. **The mark stack chased pointers.** The pending set was an intrusive list
   threaded through `ObjectHeader::mark_next`, so every pop followed a pointer to
   a different object, and traversal was depth-first.
4. **The remembered set was a hash set.** Minor collections walked it bucket by
   bucket with one node indirection per young object, and every old-to-young store
   paid a hash lookup purely to deduplicate.
5. **Sweeping touched shared atomics per object.** Promoting a survivor performed
   two shared read-modify-write operations (`old_object_count`,
   `generational_barrier_active`) and reclaiming an old object a third.
6. **Reclaimed space was unusable.** A page's bump cursor only moves forward, so a
   page holding a mix of short- and long-lived objects was abandoned with most of
   its storage dead, and the heap grew by a fresh page for each survivor cohort.
7. **Collection scheduling ignored the heap.** The trigger was a fixed 1 MB, and
   every eighth automatic cycle was a full collection regardless of whether the
   old generation held anything worth reclaiming. Live heap size was recomputed by
   walking every live object at each cycle boundary.

## 2. Root Cause

The collector treated metadata as something to look up rather than something the
page already knows. Page-local start bitmaps, cycle state and remembered-set
membership are all facts that can live next to the object or next to the cycle;
each one lived in a global structure instead, so every reference and every object
paid for a shared lookup it did not need. Scheduling had the same shape: the
information to decide *whether* a collection (or a full collection) is worth
running was available at the end of the previous cycle, but the decision used a
constant.

## 3. Files Modified

| File | Change |
| --- | --- |
| `runtime/src/gc.cpp` | Mark worklist ring, context-owned cycle state, page-local rejection of dead addresses, index only for pages without start metadata, flat remembered set, incremental live-heap accounting, adaptive trigger, growth-based full-collection scheduling, per-page free runs with coalescing and TLAB reuse, chunked generation bookkeeping. |
| `runtime/include/nirqoba/runtime/gc.h` | Trigger/policy accessors (`nrq_gc_get_threshold`, `nrq_gc_set_adaptive`, `nrq_gc_policy_is_adaptive`, `nrq_gc_policy_adaptation_count`, `nrq_gc_last_reclaimed_bytes`), `nrq_gc_object_index_entries`, `nrq_gc_span_reuse_count`. |
| `tests/unit/gc_test.cpp` | Six new tests (section 6). |
| `CMakeLists.txt`, `docs/readiness.md`, `docs/architecture/06-runtime.md`, `docs/VERSION_LOG.md`, `tests/integration/positive/memory_advanced/expect.json` | Version 0.141-beta. |

## 4. Transformations Implemented

### 4.1 Breadth-first marking through a fixed ring

`MarkWorklist` is a 256-entry FIFO ring; objects beyond that capacity spill onto
the header link exactly as before, so the worklist still allocates nothing.
Pending headers are now contiguous, so consecutive pops touch consecutive
addresses instead of following the heap graph.

### 4.2 Cycle state in the mark context

`MarkContext` carries the cycle's birth epoch. `enqueue_header_locked` compares
against it directly:

```text
- if (collection_phase.load(relaxed) != Idle && header->birth_epoch >= active_cycle_epoch) return;
+ if (header->birth_epoch >= context.epoch) return;
```

A context only exists while a cycle is open, and `mark_value_locked` already
refuses to touch it when the phase is idle, so the phase test was redundant and
the epoch is now a plain field read.

### 4.3 Page-local rejection is final

`find_object_locked` used to fall through to the global index whenever the start
bit was clear:

```text
  page = page_directory.find(object)
  if (page has start bitmap):
      if (bit set and header agrees) return header
      return nullptr          // <= new: the bitmap is authoritative
  return object_index.find(object)
```

The bitmap is authoritative for every address inside an indexed page: interior
pointers, stale addresses and garbage all miss without a hash probe.

### 4.4 The exact index only holds what the bitmap cannot describe

Publication, allocation and sweeping now consult `page_uses_index(page)`:

- pages with start bitmaps store nothing in `ObjectIndex` (a bit is set instead),
- pages without start metadata (large-object pages) keep the exact entry,
- and a page whose directory insertion fails is explicitly marked
  `use_object_index` rather than becoming silently undiscoverable.

The index therefore stays proportional to large-object pages instead of to the
heap: `ObjectIndexOnlyTracksPagesWithoutStartMetadata` allocates 256 small
objects and observes no growth at all.

### 4.5 Flat remembered set

`ObjectHeader` gained a `remembered` flag that fits the padding the header already
had (the 96-byte `static_assert` still holds). Insertion is a flag test plus a
`push_back`; clearing is a flag store. Entries whose object is gone are dropped by
`compact_remembered_locked()` at the cycle boundary, *before* anything traverses
the list, so a traversal never dereferences a stale address.

### 4.6 Incremental live-heap accounting and an adaptive trigger

`live_heap_bytes` is maintained by publication, allocation and reclamation.
`finish_cycle_locked` reports it directly:

```text
- for (ObjectHeader* header = objects; header; header = header->next)
-     statistics.allocated_bytes = saturated_add(statistics.allocated_bytes, header->size);
+ statistics.allocated_bytes = live_heap_bytes;
```

The next trigger is `live + max(512 KiB, live / 2)`, with the headroom doubled
when the cycle reclaimed less than an eighth of the heap. Worst-case growth
between collections is bounded by twice the live set, and a program that calls
`gc.set_threshold` pins the value and switches adaptation off.

### 4.7 Full collections follow heap growth

A minor cannot reclaim unreachable old objects; only heap growth reveals them. An
automatic full collection is therefore scheduled when the live heap has grown by
half since the last one — never twice in a row (minimum two minors), never later
than thirty-two minors:

```text
- minor_collections_since_major < 8          // fixed every-eighth rule
+ growth >= max(512 KiB, live_at_last_major / 2) || minors >= 32
```

### 4.8 Reclaimed page space is reusable

`GcPage` keeps up to 96 `{offset, span}` runs. Sweeping releases the span of every
reclaimed object; allocation takes a fitting run before advancing a cursor:

```text
  page = take_free_span_locked(span, header_offset);   // first-fit, same page
  if (!page) { ...existing bump path... }
  page->used += span only when the span came from the bump region
```

Runs are kept ordered by offset and merged with neighbours, so a cohort that dies
together collapses into one region. `reserve_tlab_locked` takes a whole 4 KiB
reservation from such a region, which is what lets a fragmented page serve new
allocations again instead of being abandoned.

### 4.9 Generation bookkeeping once per sweep chunk

Promotion and reclamation counts, and the generational barrier flag, are applied
once per 64-object chunk instead of once per object. The heap lock is held for the
whole chunk and every other reader of those counters takes it, so no intermediate
value is observable.

## 5. Correctness Argument

**No allocation in any new path.** The worklist spills to the header link, the
remembered set is a vector whose push failure is fatal, and free runs live in a
fixed array. A page whose run list is full simply behaves like the old bump
allocator.

**Every change preserves the snapshot-at-the-beginning invariant.** The epoch
filter moved from the global to the context unchanged; the phase test it replaced
was already implied by the context's lifetime. Marking order changed from
depth-first to breadth-first, which affects visit order only.

**Page-local rejection is exact.** A start bit is set at publication (after
`object_index.insert`, which the bit replaces for small pages) and cleared before
the object's storage is released. An address inside an indexed page is a valid
object start if and only if its bit is set, so returning `nullptr` on a clear bit
cannot hide a live object. Large pages remain on the exact index, so they are
unaffected.

**Object-index elision keeps ownership explicit.** `page_uses_index` is stored on
the page rather than recomputed, so a failed directory insertion is a persistent
decision: allocations, publication and sweeping all agree on where entries live.

**Free runs cannot overlap live data.** Runs are created only for headers the
sweeper decided to reclaim, and only while `gc_mutex` is held; reclamation never
frees a marked object. A run is removed or trimmed before the span is handed back
out, so no two allocations can share storage. Rollback paths return a reused span
to the run list instead of rewinding a cursor.

**Adaptive scheduling preserves liveness.** The trigger only decides *when* a
collection runs; explicit `gc.collect()` still collects immediately, `gc.enable`
still gates automatic collection, and the policy never collects less often than
every 2× live bytes, so growth is bounded rather than unbounded.

**Generational counters.** Batching is safe because `old_object_count` and
`generational_barrier_active` are only read under `gc_mutex` (allocation,
cycle finish) and the chunk holds it for its whole duration.

## 6. Tests Added or Modified

| Test | What it locks down | Result |
| --- | --- | --- |
| `ObjectIndexOnlyTracksPagesWithoutStartMetadata` | 256 small objects add no index entries; a 128 KiB object adds one and removing it drops it again | pass |
| `CollectionThresholdFollowsTheLiveHeap` | Empty heap → 512 KiB floor; an 8 MiB live object raises it above its own size; `gc.set_threshold` pins and adaptation stops | pass |
| `YoungChurnDoesNotForceRepeatedFullCollections` | 20 000 short-lived objects trigger many minors and at most a fifth as many full cycles | pass |
| `ReclaimedSpansAreReusedBeforePagesGrow` | 200 objects with nine in ten reclaimed are replaced from reused spans with no new page | pass |
| `MergedFreeRunsServeThreadLocalReserves` | A fragmented page serves 2 000 further thread-local allocations without growing | pass |
| `ObjectHeaderStaysWithinItsMetadataBudget` (existing) | The remembered flag did not push the header past 96 bytes | pass |

## 7. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — **151/151**.
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  **241/241** (includes every cyclic-reference, root-lifetime, multithreaded and
  GC-API integration fixture).
- Structural evidence only: index sizes, page counts and reuse counters, not
  wall-clock timings, per the project policy for this task.

**Compilation time and memory:** no compiler change, so compilation time is
unaffected. Runtime memory: `GcPage` grows by 96 × 16 bytes = 1.5 KiB of run
bookkeeping per 64 KiB page (2.4%), the object header stayed at 96 bytes thanks to
the flag fitting existing padding, and the object index only shrinks. The
remembered set trades a hash node per young object for an 8-byte slot.

## 8. Not Implemented, and Why

1. **Compaction / moving collection.** Ownership, borrowing and lifetime
   guarantees in the language are checked against object identity; a moving
   collector would need pinning semantics the type system does not carry. Not in
   scope for this task.
2. **Adaptive promotion age.** `kPromotionAge = 2` is a documented semantic that
   the language's generational tests rely on (`MinorCollectionPromotesSurvivors…`
   asserts promotion after exactly two minors). Changing it under survival
   statistics would change observable behaviour; the scheduling side of
   generational tuning is implemented in 4.7 instead.
3. **True concurrency for marking.** Marking still runs under `gc_mutex` in
   slices. Making it concurrent needs a write barrier design that belongs to
   `3.14` (runtime synchronization and GC protocol).
4. **Per-generation size accounting.** The adaptive policy uses total live bytes;
   splitting it into young/old would let the collector size each generation
   separately, but that requires the same accounting `3.14` introduces.

## 9. Remaining Opportunities

1. Publish the trigger's history (adaptations, survival ratios) through
   `gc.get_statistics` so programs can observe and tune the policy.
2. Sweep in address order per page so run coalescing sees adjacent spans in
   sequence and the run list stays small on very large pages.
3. Size-class the free runs, so an allocation does not have to scan every run in a
   page to find a fit.
4. `3.14` will remove the remaining per-store `barrier_readers` traffic, which is
   now the largest synchronization cost on the mutator path.
