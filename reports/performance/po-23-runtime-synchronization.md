# PO-23 — Runtime Synchronization and GC Protocol Optimization

**Task:** PO 3.14 — Runtime Synchronization and GC Protocol Optimization
**Date:** 2026-11-06
**Status:** Completed & Verified (unit and integration suites; runtime timing and
hardware-counter measurements deferred per the project policy that this task must
not be benchmarked)
**Compiler:** Nirqoba 0.153-beta, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Problem Identified

The collector was already incremental, but *every* mutator operation that touched
the collector protocol went through process-wide state:

1. **The barrier reader count was shared.** A store barrier performed a
   read-modify-write on one global counter, so two threads storing managed
   references in unrelated functions contended on a single cache line, and the
   collector sampled that line to decide when a snapshot was safe.
2. **Every root slot owned a hash node.** `ShadowStack::slot_states` was a
   `std::unordered_map<void*, SlotState>`, so a registration allocated a node,
   linked it into a bucket chain, and a frame end freed it again — two allocator
   round trips per local variable that came in and out of scope.
3. **Root fast paths took the stack mutex.** Even with no collection running,
   registration and frame end acquired the shadow stack's mutex twice per
   operation (entry and exit of the region).
4. **Managed returns serialized on the collector mutex.** `nrq_gc_pin_transfer`
   took `gc_mutex` and inserted into `pinned_objects` for every managed result
   handed back by a callee, so a helper-heavy call chain was effectively
   single-threaded through the GC lock.
5. **The store barrier read two atomics per word.** `buffer_pointer_words` tested
   `generational_collection_enabled` and `generational_barrier_active` for every
   machine word of an aggregate store, and `buffer_remembered` tested them again
   for each word.
6. **Frame end probed the slot index twice per root.** Removal did a lookup for
   the entry and then a second probe chain to erase it, and a frame that named the
   same slot twice paid that per registration.
7. **Parking broadcast to everybody.** Raising a collection request performed a
   `notify_all`, although parked mutators were waiting for the request to
   *clear*; every parker broadcast again, and the collector rescanned the whole
   mutator registry on each wakeup.
8. **Root stores paid for writes that change nothing.** A generated root store
   that rewrote the pointer already in the slot, or cleared it to null, still paid
   the barrier handshake and the remembered-set append.

## 2. Root Cause

The runtime treated "shared" as the default for protocol state. Barrier progress,
root-slot bookkeeping, handoff pins and parking status are all facts that belong
to *one* mutator, and the collector only needs them at a cycle boundary, when
every mutator is parked anyway. Because they lived in globals, every operation on
the mutator side had to be made safe for concurrent observation with an atomic
RMW, a global mutex, or a hash table — and the collector paid the mirror cost of
scanning those globals to find out what the mutators had done.

## 3. Files Modified

| File | Change |
| --- | --- |
| `runtime/src/gc.cpp` | Per-mutator `barrier_depth`; open-addressing `SlotIndex`; diagnostics-gated stack mutex; per-mutator transfer-pin deltas; single published `store_barrier_active` switch with conservative ordering; batched frame-end slot release with in-place tombstoning; parked counter, registry count and dedicated collector condition variable; identical/null root-store early returns. |
| `tests/unit/gc_test.cpp` | Three new tests (see §6). |
| `CMakeLists.txt` | Version `0.153.0`, label `0.153-beta` (in-development). |
| `README.md`, `PROGRESS_REPORT.md`, `docs/readiness.md`, `docs/architecture/06-runtime.md`, `docs/VERSION_LOG.md`, `tests/integration/positive/memory_advanced/expect.json` | Version strings, released `0.147-beta` section, `0.153-beta (dev)` section. |

No public API was added or removed; `runtime/include/nirqoba/runtime/gc.h` is
unchanged in this task.

## 4. Transformations Implemented

### 4.1 Barrier progress lives with the mutator

`barrier_readers` — a single process-wide counter — is gone. `MutatorState` owns
`alignas(nrq_cache_line_bytes) std::atomic<uint32_t> barrier_depth`, and
`begin_fast_barrier` increments the mutator's own counter after checking that the
thread is registered. Two mutators storing references in different functions now
touch different cache lines, and the collector reads the counters only during its
snapshot handshake, iterating the (already locked) mutator registry.

`end_fast_barrier` decrements the owning counter, and `wait_for_fast_barriers`
polls the registered mutators' counters, yielding, until every one reads zero.

### 4.2 Root slots are indexed by a flat table, not by a node per slot

`SlotIndex` replaces the `unordered_map`: one power-of-two entry array with linear
probing, tombstones, a 64-bit mix hash, and growth at 75% occupancy. Each entry
holds the slot pointer next to its state, so a registration or removal touches one
cache line and allocates nothing. Semantics are preserved exactly:

* `count` still tracks duplicate registrations of the same slot;
* `insert` returns the existing state for a repeated slot, and `add_slot_state`
  still fatals on an incompatible `object_size`/layout/pointer-root flag;
* `pointer_root` is still answered from the index, so
  `shadow_has_pointer_root` stays O(1) with no allocation.

### 4.3 The shadow stack's mutex is off the fast paths

The collector enumerates a shadow stack only after `stop_mutators()` has parked
every registered mutator, so a *running* mutator's stack cannot be read by it.
The one other reader is reference diagnostics (`nrq_gc_get_ref_count` /
`nrq_gc_ref_count_start`). Root registration and frame end therefore take
`stack.mutex` only while diagnostics are enabled, and a diagnostic query first
drains in-flight barrier regions (`wait_for_fast_barriers`) so no mutator can
still be inside a lock-free region when it reads. Mutators that observe the
diagnostics flag afterwards take the mutex themselves.

### 4.4 Managed handoffs are buffered per mutator

A managed return publishes a transfer pin. `nrq_gc_pin_transfer` and
`nrq_gc_unpin` now record a *signed delta* in the calling mutator's
`transfer_pins[32]` buffer instead of taking `gc_mutex`:

* the first pin of an object appends an entry, a repeat increments its delta;
* an unpin against a positive delta cancels in place, an unpin with no pending
  delta queues `-1`, which the table applies at the next drain;
* when the buffer is full the region is closed *before* taking `gc_mutex`, so the
  collector is never waiting on a barrier region that is waiting on its lock;
* `prepare_cycle_locked` folds every registered mutator's buffer into
  `pinned_objects` right after TLAB publication and before pinned roots are
  enumerated, so a buffered pin protects its referent for exactly the collections
  a table pin would;
* `nrq_gc_mutator_leave` and thread-exit cleanup settle the buffer, because a
  mutator that leaves the registry is never visited by cycle preparation again;
* `nrq_gc_pin_count` folds the querying thread's own buffer so diagnostics stay
  exact.

Pointer validity, which the table path could check with the heap lock held, is
checked at the drain: a delta naming a pointer the heap does not own is counted as
a pin violation and dropped. Code generation only ever transfers managed results,
so this is a defence-in-depth path rather than a behaviour change for valid code.

### 4.5 One published switch for the store barrier

`store_barrier_active` is the conjunction of "generational collection is enabled"
and "remembered-set recording is required". Mutators read that single flag, and
`buffer_pointer_words` reads it once per store rather than once per machine word.
Writers maintain it with conservative ordering — raised *before*
`generational_barrier_active` is raised and before generations are enabled,
lowered *after* it is lowered — so a mutator that observes a stale value records
extra remembered-set entries it did not need, never skips one it did.

`append_remembered` (the old `buffer_remembered` body) no longer re-tests the
flags; the scalar barrier `buffer_remembered` checks the switch once and delegates.

### 4.6 Frame-end removal is one index pass

`SlotIndex::release` decrements in place and tombstones the entry only when its
last registration disappears, so one probe chain replaces a lookup plus an erase
probe. `remove_frame_slots` counts the frame's slots into a fixed 32-entry batch
(linear search, no allocation) so a slot named several times inside one frame is
released once, and the whole frame is settled with one probe per *distinct* slot.
Wider frames fall back to one `release` per registration, which is still a single
probe chain each.

### 4.7 Parking is coordinated by a counter and a dedicated wakeup

* `parked_mutator_count` is published by the parking mutator under
  `mutator_mutex`; `registered_mutator_count` is maintained by enter, leave and
  thread-exit cleanup.
* `all_mutators_parked` compares the two, so the collector no longer walks the
  registry on every wakeup.
* A new `collector_condition` carries the park notifications: a parking mutator
  wakes only the collector instead of every thread parked behind
  `mutator_condition`.
* Raising a collection request no longer broadcasts at all — mutators observe it
  at their next safepoint poll, which is where they were going to observe it
  anyway. `resume_mutators` still uses `notify_all`, since every parked mutator
  has to wake for that one.

`park_mutator` is shared by `nrq_gc_safepoint`, `stop_mutators` and
`acquire_collection_cycle`, so all three park through the same counter.

### 4.8 Root stores that change nothing skip the barrier

`nrq_gc_root_store` returns before the barrier when the slot already holds the
value being stored: the store is not observable, and a snapshot-at-the-beginning
marker would only re-mark a referent the slot never stopped holding. Null stores
still write the slot but skip the remembered-set append, since clearing a root
cannot create an old-to-young edge.

## 5. Correctness Argument

**Barrier counters.** A snapshot is taken only when every registered mutator's
depth reads zero, which is the same handshake as before — it is only the storage
that moved. Unregistered threads never enter a region (`begin_fast_barrier`
requires a registered mutator), so they serialize on `gc_mutex` and the handshake
does not have to know about them.

**Slot index.** The index is thread-private state used only by the thread that
owns the shadow stack. Duplicate counts, layout validation, pointer-root
classification and the fatals for inconsistent bookkeeping are unchanged, and the
table is a pure re-implementation of the map's `find`/insert/erase contract.

**Mutex-free stack access.** The collector cannot read a running mutator's stack
because it enumerates only after parking everyone. The remaining concurrent reader
is diagnostics, which is fenced two ways: the flag is published before the query
reads any stack, and the query drains in-flight barrier regions first, so a
mutator either took the mutex (flag visible) or has left its region (drained). The
lock-free path is entered only when the flag is clear, and the flag can only
become true while that mutator is inside a region that the diagnostic drains.

**Handoff buffers.** Pins and unpins from one thread are a delta on one object, so
order within a thread is preserved by arithmetic rather than by insertion order;
`pinned_objects` only ever observes the net effect. The drain runs under
`gc_mutex` with every registered mutator parked, so it cannot race a buffer write.
Because it runs before pinned roots are enumerated, no collection can miss a
buffered pin, and the timestamp of the fold is the cycle boundary — the same
moment a table pin would have been consulted.

**Single switch.** `store_barrier_active` is only ever false when both inputs are
false *at some point after* they were lowered, and only ever raised before its
inputs, so any stale observation is conservative: extra entries are discarded at
the next drain unless they name a young object.

**Frame-end batching.** Counting multiplicities first and releasing the total is
equivalent to one release per registration, and `release` tombstones only at
count zero, so a slot shared with an outer frame stays registered for it.

**Parking.** `parked_mutator_count` is written under `mutator_mutex` and read
inside the wait predicate, which is only evaluated while holding that mutex, so
every increment is either seen by the collector's evaluation or is followed by a
notification on the collector's condition variable. Registry changes (enter,
leave, thread exit) also wake the collector, so a shrinking registry cannot leave
a stale threshold in force.

**Identical/null stores.** An identical store leaves every reachable object
reachable; a null store removes an edge and can only make the graph simpler for
the collector. Neither requires marking, remembering or a barrier region.

## 6. Tests Added or Modified

| Test | What it locks down | Result |
| --- | --- | --- |
| `BufferedTransferPinsCancelAndSurviveCollection` | Balanced handoff pins cancel in the mutator's buffer; an unbalanced handoff survives a collection through the fold at cycle preparation and disappears after the matching unpin | pass |
| `WideRootFrameReleasesEveryRegistration` | A 64-slot frame plus a duplicate registration releases every registration, including the overflow path of the batched removal | pass |
| `RootStoreSkipsIdenticalAndNullBarrierWork` | An identical root store keeps the referent alive and a null root store removes it, without the barrier changing liveness | pass |

Existing tests that exercise the rewritten protocol unchanged:
`DuplicateRootSlotRegistrationsPreserveOuterOwners`,
`DuplicateRootRegistrationsInConcurrentCollectionStayConsistent`,
`RootStoreSynchronizesConcurrentMutationAndCollection`,
`NestedRootFramesWithConcurrentCollection`,
`ConcurrentCollectionRequestsFromMutatorsSerializeWithoutDeadlock`,
`ThreadTerminationWithActiveRootFramesRemovesDanglingRoots`,
`ReferenceDiagnosticsCountHeapAndRootReferencesWhenEnabled`,
`TransferPinsSupportBalancedManagedReturnHandoffs`,
`PinDiagnosticsReportCountAndRejectExcessivePins`.

## 7. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — **154/154** (repeated three
  times to catch a parking or barrier race).
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  **241/241**.
- Structural evidence only: operation counts per runtime call, not wall-clock
  timings or hardware counters, per the project policy for this task.

## 8. Structural Evidence (before → after, per operation)

| Operation | Before | After |
| --- | --- | --- |
| Store barrier, aggregate of *w* words | 1 shared RMW + 2 atomic loads × *w* | 1 own-counter RMW (own cache line) + 2 atomic loads total |
| Root registration | `stack.mutex` lock/unlock + map node allocation + bucket insert | own-counter RMW + one table probe, no allocation, no mutex |
| Frame end, *k* distinct slots | *k* finds + *k* erases (2*k* probe chains) + *k* node frees | *k* probe chains (one chain releases in place) |
| Managed return (`pin_transfer`) | `gc_mutex` + `pinned_objects` insert | local delta increment; one fold per cycle |
| Managed release (`unpin`) | `gc_mutex` + map find/erase | local delta decrement or one queued delta |
| Collection request | 1 `notify_all` waking every parked mutator to re-check a predicate that had not changed | 0 broadcasts; mutators observe the request at their own safepoint |
| Mutator park | `notify_all` (wakes all mutators) + collector rescan of the registry | one `notify_one` on the collector's condition variable + two counter loads |
| Root store of the value already in the slot | barrier region + remembered append | function return |
| Root store of null | barrier region + remembered append (rejected inside) | pointer store only |

Memory: the slot index trades one 16-byte node plus bucket overhead per active
root for one 64-byte table entry (loaded at ~50% occupancy), and the transfer-pin
buffer is 32 × 16 = 512 bytes per registered thread. The mutator state grows by one
cache line for the barrier counter and is not shared between threads.

Compilation time: no compiler change, so compilation time is unaffected.

## 9. Known Limitations

1. **Buffered pins are validated at the fold, not at the call.** `pin_transfer`
   returns the pointer it was given even for a non-managed pointer when the buffer
   path is taken; the pin is then rejected and counted as a violation instead of
   being refused up front. This only affects API misuse, and generated code never
   produces it.
2. **Pin visibility is cycle-granular.** A buffered pin is visible to the pin
   table at the next cycle boundary. It protects its referent because preparation
   folds it before enumerating pinned roots, but a diagnostic query from another
   thread does not see it until then (the querying thread's own buffer is folded).
3. **Diagnostics still pay the stack mutex**, and a diagnostic query drains
   in-flight barrier regions, so enabling diagnostics can still stop mutators
   briefly.
4. **Parking still uses one process-wide mutex.** The counter removed the registry
   scan and the broadcasts, not the lock; a per-mutator futex would be the next
   step but needs a different safepoint design.
5. **Frames wider than 32 distinct slots** fall back to one release per
   registration, which is correct but gives up the dedup win for such frames.

## 10. Remaining Opportunities

1. **Concurrent marking.** With barrier progress per mutator and handoff pins out
   of the global lock, the remaining serialization is the mark phase itself; a
   mark work-queue with per-mutator steal buffers would be the natural follow-up.
2. **Per-mutator parking primitives.** Replacing the shared condition variable
   with a per-mutator futex word would remove the last global wakeup from the
   safepoint path.
3. **Publish the protocol counters** (`barrier_depth` regions, folded handoff
   deltas, park churn) through `gc.get_statistics` so programs can observe the
   protocol cost directly.
4. **Adaptive remembered-buffer sizing.** The 64-entry buffer is fixed; sizing it
   from observed store rates would help store-heavy workloads without changing
   semantics.
