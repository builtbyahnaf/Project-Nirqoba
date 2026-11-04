# Nirqoba — GC 1.1–1.5 Regression Correction

**Scope:** Corrective implementation of GC 1.1–1.5  
**Status:** Implemented, correctness-tested; no benchmarks executed  
**Date:** 2026-11-04

---

## 1. Root cause and affected locations

`nrq_gc_root_frame_end()` in `runtime/src/gc.cpp` cleared and rebuilt the
entire per-thread `pointer_root_slots` index on every frame exit:

```cpp
stack.pointer_root_slots.clear();
for (const ShadowRoot& r : stack.roots)
  if (is_pointer_root(r.info))
    stack.pointer_root_slots.insert(r.slot);
```

The cost of ending one frame was therefore proportional to **all** roots still
active on the thread, not to the roots owned by the exiting frame. In recursive
managed workloads (Test 02) this produced quadratic work per traversal level.

A second, related hotspot was in `nrq_gc_root_register_typed()`: duplicate-layout
validation scanned the complete `stack.roots` vector for every registration.
That made each registration O(active roots) as well. Both paths are in
`runtime/src/gc.cpp`:

- `nrq_gc_root_frame_end()` — full index rebuild.
- `nrq_gc_root_register_typed()` — full-root duplicate-layout scan.
- `ShadowStack::pointer_root_slots` — the set that was rebuilt.

## 2. Files modified

- `runtime/src/gc.cpp`
- `tests/unit/gc_test.cpp`
- `reports/performance/gc-regression-fix.md` (this report)

No compiler, benchmark workload, language semantic, or reference implementation
was modified.

## 3. How incremental root removal works

`ShadowStack` now keeps a per-thread `slot_states` index:

```cpp
std::unordered_map<void*, SlotState> slot_states;
```

Each `SlotState` stores the registration count for the slot, its validated
layout (object size + reference offsets), and whether it is a simple pointer
root. Registration adds one count; unregistration removes one count.

`nrq_gc_root_frame_end()` uses the frame's saved `root_depth` checkpoint exactly
as required:

```cpp
for (size_t index = target_depth; index < stack.roots.size(); ++index)
  remove_slot_state(stack, stack.roots[index].slot);
stack.roots.resize(target_depth);
```

Only entries in `[target_depth, roots.size())` are touched. No remaining outer
root is scanned, rehashed, or reinserted. Frame destruction is O(roots owned by
the exiting frame), and slot removal is O(1) average.

The collector continues to read `stack.roots` under `stack.mutex`; the shadow
stack remains registered in `shadow_stacks` for the lifetime of the thread. The
fast path keeps the same reader-count handshake (`begin_fast_barrier` /
`end_fast_barrier`) so the collector cannot take a snapshot while a frame is
being shrunk. The fast path now also holds `stack.mutex`, which makes diagnostic
root enumeration (`nrq_gc_get_ref_count`) safe against concurrent frame exit.

## 4. Duplicate root registrations

Duplicate registrations of the same slot are preserved with a reference count:

- First registration inserts a `SlotState` with `count = 1`.
- Every further registration of the same slot verifies that its layout matches
  the stored layout and increments `count`.
- Unregistration / frame end decrements `count`.
- The slot is removed from the index only when `count` reaches zero.

Thus an inner frame can register a slot already owned by an outer frame; ending
the inner frame leaves the outer registration, and therefore pointer-root
validation for `nrq_gc_allocate_array_typed_rooted`, intact. Multiple
registrations inside one frame are removed together when that frame ends.

## 5. Other hot-path corrections

### 5.1 Registration is now O(1) per root

The full `stack.roots` scan used for duplicate-layout validation was replaced by
the `slot_states` lookup above. Root layouts are also stored in
`ReferenceLayout` (two inline offsets) instead of a heap-allocated
`std::vector<size_t>`, so the common one- or two-reference root no longer
performs a heap allocation per registration.

### 5.2 Remembered-set barrier gated while no old generation exists

The generational write barrier previously performed `object_index.find()` plus
`remembered_young.insert()` for every non-null store, even when every live
object was young. A young-to-young store cannot create an old-to-young edge.

A `generational_barrier_active` flag now gates the remembered-set work:

- Raised at the start of every collection cycle, before marking, because a
  concurrent promotion can make an object old while mutators run.
- Lowered after a cycle that leaves zero old objects.
- Raised immediately if an object is allocated with `young == false`.

`old_object_count` tracks promotions, old allocations, and old-object
reclamation so the flag is only lowered when there is definitively no old
generation. This preserves concurrent-marking correctness and old-to-young
tracking; it only skips work that cannot produce a useful remembered entry.
`buffer_pointer_words()` likewise returns without scanning when the barrier is
inactive.

### 5.3 Managed-return handoff no longer flushes the TLAB

`nrq_gc_pin_transfer()` previously published the whole pending TLAB batch on
every managed return. A helper that returns one managed pointer therefore paid
one object-index insertion and one publication per call, defeating TLAB
batching.

Pinning now accepts an object that is still in the current thread's pending
TLAB and records the pin without publishing. This is safe because cycle
preparation publishes every registered mutator's TLAB **before** enumerating
pinned roots, so the pinned object is visible to exactly the collection that
needs it. The pin remains balanced and `nrq_gc_unpin()` is unchanged.

### 5.4 Thread-lifecycle cleanup

A thread-local cleanup guard now runs when a thread terminates:

- Publishes any pending TLAB allocations and drains its remembered buffer.
- Removes the thread from the mutator registry if it exited without
  `nrq_gc_mutator_leave()`.
- Detaches and deletes its shadow stack under `gc_mutex`, so the collector can
  never enumerate root slots whose owning thread stack is gone.

The guard uses the same lock order as `nrq_gc_mutator_leave()`
(`mutator_mutex` then `gc_mutex`) to avoid inversion with a concurrently
finishing mutator.

### 5.5 Unchanged synchronization that was intentionally kept

- `nrq_gc_safepoint()` remains a single acquire load on the common path; it
  takes `mutator_mutex` only when a collection is requested.
- Nested `nrq_gc_mutator_enter/leave` remain thread-local depth updates; only
  the outermost entry registers with the global mutator list.
- The write barrier still performs the `barrier_readers` handshake and the root
  snapshot invariant is unchanged.
- The collector still enumerates all live roots under `stack.mutex`; no roots
  were hidden or dropped.

## 6. Correctness tests executed and results

Added to `tests/unit/gc_test.cpp`:

- `DeeplyNestedRootFramesRemoveOnlyOwnedRoots` — 128 nested frames, one root
  each; ending inner frames retains exactly the outermost root.
- `RecursiveManagedTraversalKeepsAncestorRootsVisible` — 64 recursive managed
  frames; a collection at the deepest point observes all ancestor roots.
- `DuplicateRootSlotRegistrationsPreserveOuterOwners` — duplicate registration
  in nested frames and within one frame; outer pointer-root validation remains
  valid.
- `EarlyReturnUnwindsRootFrameAndReleasesRoots` — RAII frame cleanup during
  exception unwinding.
- `ThreadTerminationWithActiveRootFramesRemovesDanglingRoots` — a thread exits
  with an active frame, registered root, and unpublished allocation; the
  collector later reclaims the object without touching a dangling slot.
- `DuplicateRootRegistrationsInConcurrentCollectionStayConsistent` — duplicate
  slot registration/removal while another thread runs collections.

Execution result:

```text
[==========] 116 tests ran.
[  PASSED  ] 116 tests.
```

This includes all existing GC tests (generational promotion, old-to-young
barrier, pinned objects, concurrent collection, rooted allocation, remembered
buffer behavior) and the new regression tests. The runtime was also rebuilt in
the release configuration without warnings.

## 7. Remaining risks / incomplete work

- No performance benchmark was run, so no speedup or frontier-language
  comparison is claimed here.
- `nrq_gc_pin_transfer()` still takes `gc_mutex` for pin bookkeeping. Removing
  that lock would require a per-thread pin buffer or a lock-free pin registry
  and is outside this corrective change.
- The pin handoff change relies on the existing invariant that cycle
  preparation publishes all registered mutator TLABs before enumerating pinned
  roots. The added tests cover the handoff, but a dedicated stress run with
  automatic collection enabled during managed returns is still advisable.
- `root_frame_begin()` uses a per-thread monotonic marker. Marker exhaustion is
  still a fatal condition after `UINT64_MAX` frames on one thread.
- The thread-exit guard assumes process-wide GC statics outlive terminating
  threads. That holds for normal worker-thread termination; main-thread exit
  during static destruction was not separately stress-tested.

## 8. Benchmarks

**No benchmarks were run.** In particular, `stress_test_2` and the performance
benchmarks were not executed. The only executions performed were compilation and
the existing unit-test suite.
