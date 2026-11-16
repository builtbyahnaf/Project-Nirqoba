# Phase 7 Audit — GC Isolation and Explicit Selection

## 1. Baseline and provenance

| Item | Observed |
| --- | --- |
| Branch | `main` |
| HEAD at audit start | `55e19a4` (`docs: place the Phase 7 note with its retrospective entries`) |
| Expected starting commit | `3e01af1` (Phase 6 completion) |
| Working tree | clean before this audit |
| Compiler version | `0.571-beta` |
| CLI identification | `nirqoba compiler version 0.571-beta (Phase 7)` |
| Unit suite | 323/323 passing |
| Full integration suite | 593/593 passing, 0 failed, 0 skipped |

The starting commit differs from the expected Phase 6 hash because two later
user commits are already on `main`: `f42dc69` (the Phase 10 plan addition and
unrelated README/documentation cleanup) and `55e19a4` (placement of the Phase 7
retrospective note in `docs/VERSION_LOG.md`). Both are preserved verbatim. No
Phase 10 content is modified by this phase.

Baseline evidence is retained in `build/phase7_unit_baseline.log` and
`build/phase7_full_baseline.log` (ignored build artifacts, not committed).

## 2. What "GC isolation" means here

Nirqoba has one collector: a precise, non-moving, generational mark-sweep
collector with typed roots, root frames, mutator regions, safepoints, write
barriers, SoA tracing and cyclic reclamation. Phase 7 does not replace or weaken
it.

The invariant under test is:

> A program that creates no GC-managed value must not execute collector
> allocation, root registration, mutator registration, safepoints, write
> barriers or collection bookkeeping.

Isolation is a property of the *selected allocation strategy and its transitive
effects*, not a property of whether the runtime library happens to be linked.

## 3. Allocation entry-point inventory

The source-level database is in `compiler/lib/Sema/TypeChecker.cpp`, the TIR
allocation contract in `compiler/lib/TIR/Verifier.cpp`, the lowering plan in
`compiler/lib/Lowering/TirToMlir.cpp`, and the runtime allocation/root ABI in
`runtime/src/gc.cpp` and `runtime/include/nirqoba/runtime/gc.h`.

| Entry point | Source category | TIR representation | Runtime allocator | Root establishment | Safepoint / barrier | Reclamation | Classification |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `array.allocate<T>(count)` | Explicit GC-managed array; result is `managed<T>` | `Call` → `GcManaged` managed pointer | `nrq_gc_allocate_array_typed_rooted` | Rooted allocation slot plus frame/static descriptor; established before the value is used | GC allocation is a collection point; managed field writes use `nrq_gc_root_store`; tracing via typed layout | Collector mark-sweep, including cycles | **Explicit and valid** |
| `array.allocate<T>(count)` where `T` contains managed fields | Explicit GC-managed aggregate array | Same, with recursive typed layout | Same | Same; layout describes every managed offset | Same; nested references are traced and writes barriered | Collector | **Explicit and valid** |
| `memory.owned_soa<T>` | Explicit GC-backed SoA container; legacy name | `Soa` value with GC ownership metadata | `nrq_gc_allocate_soa_rooted` | Frame/heap root registered by lowering; layout describes the base slot and columns | GC allocation point; base/column writes use barriers where managed | Collector | **Explicit and valid, legacy name documented** |
| `memory.allocate<T>(bytes)` where `T` contains managed fields | Ordinary owned storage that legitimately holds GC references | Owned pointer plus a frame/static root registration for each managed offset | `nrq_memory_allocate` / `nrq_memory_allocate_from` | Typed root registration in the enclosing frame; released on `memory.deallocate` or aggregate drop | The allocation itself is not a GC allocation; calls that collect make the root plan live; field writes to managed slots use barriers | Ordinary deallocation unregisters the typed root; the GC objects are reclaimed independently | **Explicit and valid, tracked** |
| `memory.reallocate<T>` on such storage | Ordinary reallocation preserving managed fields | Owned pointer plus root registration | `nrq_memory_reallocate` | Re-registration for the new pointer, old root released by the reallocation path | Same as ordinary storage | Ordinary reallocation/deallocation | **Explicit and valid, tracked** |
| `memory.shared_allocate<T>(bytes)` where `T` contains managed fields | Shared-owned storage holding GC references | Shared pointer with managed payload | `nrq_memory_shared_allocate_rooted` | Runtime registers a typed shared root; final `memory.shared_release` unregisters it | Shared allocation records GC eligibility; managed field writes use barriers | Final shared release removes the root; GC objects remain collector-managed | **Explicit and valid, tracked** |
| `memory.owned_array<T>` where `T` contains managed fields | Ordinary owned array holding GC references | Owned array with managed element layout | `nrq_memory_allocate` / `nrq_memory_allocate_from` plus root registration | Element typed root registration in the frame (or heap root for an aggregate field) | Element writes to managed slots use barriers; release unregisters | Owned-array release unregisters; collector reclaims objects | **Explicit and valid, tracked** |
| Managed references in stack aggregates/tuples/arrays | Ordinary storage holding GC references | Aggregate value with recursive managed offsets | Stack storage | Root-frame registration of the aggregate's managed slots | Managed stores use barriers; frame end unregisters | Frame end; GC objects separately | **Explicit and valid, tracked** |
| Managed references in ordinary heap aggregates | Fields of `memory.allocate`/`shared_allocate` storage | Recursive managed offsets | Ordinary/shared allocator | Frame/static/heap typed root per containing storage | Barriers and frame end/release | Release unregisters | **Explicit and valid, tracked** |
| `memory.secure_allocate<T>` | Manual secure allocation | Owned pointer | `nrq_memory_secure_allocate` | None, because managed payload is rejected by Sema | None | `memory.secure_release` | **Correctly excluded from GC** |
| `memory.allocate<T>(bytes, arena)` / `(bytes, pool)` with managed payload | Tracked arena/pool allocation | `ArenaBound` ordinary pointer plus root registration | `nrq_arena_allocate` / `nrq_memory_allocate_from` | Lowering registers the managed offsets as typed roots | No GC allocation; but reset/destroy has no unregistration | Arena/pool reset/destroy frees storage; root remains registered | **Defect: unsupported combination accepted** |
| Internal `nrq_gc_allocate_layout`, `nrq_gc_allocate_typed`, SoA/array helpers | Compiler-generated managed allocation | Only reachable from GC-classified lowering | GC page/span allocator | Caller supplies rooted slot or explicit root operation | Collector protocol | Collector | **Explicit internal path, valid** |
| `gc.collect`, `gc.disable`, `gc.enable`, statistics | Diagnostics/control, not allocation | Calls to runtime | None | None | Explicit collection/coordination | Not applicable | **Not an allocation entry point** |

### 3.1 The arena/pool defect

`memory.allocate<Holder>(16, arena)` with

```nirqoba
struct Holder {
    managed<Child> child;
}
```

passes Sema when the allocator is a tracked `memory.arena(...)` (or
`memory.pool(...)`) handle. Lowering assigns `ArenaBound` ownership and still
emits `emit_gc_root_registration`, because the payload has a managed field.
`memory.arena_reset` and `memory.arena_destroy` free the payload memory but never
call `nrq_gc_root_unregister`. The collector therefore retains a typed root that
points into freed arena storage.

A probe compiled and ran against the current tree confirmed the malformed
state: the root registration survives arena destruction and `gc.collect()`
observes the stale slot. This is not a test-only concern; it is a stale-root and
use-after-free hazard.

The correct Phase 7 decision is to reject the combination explicitly, rather
than promise a lifetime that the arena API does not implement. Arena-backed
storage with no managed fields remains fully supported. Managed references can
be stored in ordinary owned storage, shared-owned storage, aggregates, and
explicit GC containers, where release/root cleanup is defined.

The same rejection applies to a pool handle, which has the same per-value
release path but no root-unregistration hook for bulk pool destruction.

## 4. Root lifecycle findings

### 4.1 Frame roots

- `emit_frame_gc_root_registration` runs at function entry, before managed
  values can be created.
- The root plan is computed from parameters, local aggregates, owned storage,
  and recursive managed offsets. It is not "every pointer".
- `nrq_gc_root_frame_begin` / `nrq_gc_root_frame_end` delimit the frame. Frame
  end restores the shadow-stack depth, so early returns and nested frames
  release roots in LIFO order.
- Recursive calls create nested frames. The runtime keeps the frame marker on
  the thread-local shadow stack; no global frame stack is used.
- Branch and loop lowering does not duplicate frame registration; the frame is
  function-scoped and remains valid across control flow.
- Managed references in aggregate fields are registered by offset. The layout
  descriptor is derived from the same recursive offset collector used by the
  collector's tracing metadata.
- Ordinary owned objects with no managed fields get no root frame and no
  registration.

### 4.2 Heap and shared roots

- `memory.allocate` with managed payload uses typed root registration for the
  containing storage, not for the managed object itself.
- `memory.deallocate` and aggregate drop release the typed root only when the
  storage owns managed references.
- `memory.shared_allocate_rooted` registers a shared root at allocation and
  final `memory.shared_release` removes it.
- `array.allocate` and `memory.owned_soa` use rooted allocation entry points, so
  the returned managed value is rooted before any further allocation can
  trigger collection.
- Root stores go through `nrq_gc_root_store` rather than a raw store. This
  updates the shadow slot and applies the barrier contract.

### 4.3 Root cleanup gaps

The arena/pool case above is the only audited source path where a registered
root outlives the storage without an unregistration operation. Raw allocator
handles created by `memory.allocator_create` are not tracked as arena identities
by Sema, so they follow the same manual contract as other raw storage; this is
documented as a manual-memory responsibility, not presented as a GC guarantee.

## 5. Transitive GC effects

`function_uses_gc` in `compiler/lib/Lowering/TirToMlir.cpp` is the seed:
managed types, managed fields, GC allocation calls, typed root registration,
GC runtime calls, and explicit collection request all make a function
GC-dependent. `summarize_collections` computes a least fixpoint over direct
calls, so a function that calls a collecting helper is a collection point.
`summarize_mutator_regions` propagates mutator-region requirements over direct
calls.

Audited behavior:

| Case | Current behavior | Assessment |
| --- | --- | --- |
| Direct `array.allocate` | `function_uses_gc` true from the managed result; mutator region and root frame planned | Correct |
| Direct `gc.collect` | Collection summary marks the function; callers inherit a collection point | Correct |
| Helper called by a GC function | If the helper is not itself GC-classified but can collect, the caller's collection summary is conservative | Correct |
| GC-independent helper chain | Remains free of GC calls; no region or frame is emitted | Correct for the audited fixtures |
| Recursive/mutually recursive helpers | Least-fixpoint summaries reach a stable answer | Correct for finite direct-call graphs |
| Undefined/unknown callee | Treated as a collection point; no silent "GC-independent" assumption | Correct conservative policy |
| Indirect function pointer call | No complete target set is available; treated conservatively at the call site | Conservative, documented limitation |
| Managed field in `memory.allocate` payload | Special-cased because the result type is an ordinary pointer | Correct |
| Managed field in `memory.shared_allocate` payload | The shared result is an ordinary pointer; the special case only covers `memory.allocate`/`reallocate` | **Gap: shared managed allocation is not a seed for `function_uses_gc`** |

The shared-allocation gap does not change ordinary-program isolation (a plain
shared value has no managed payload), but it is a correct transitive-effect
hole for a function that allocates shared storage containing a managed
reference. Phase 7 fixes the classification and tests the generated IR.

## 6. Safepoints

- Mutator entry emits `nrq_gc_mutator_enter`; exit emits
  `nrq_gc_mutator_leave`.
- A function with GC effects and a loop gets a safepoint poll on the backedge.
  The poll is a collection opportunity, not a second frame mechanism.
- GC allocation entry points are collection points in the summary. A collection
  may therefore run at an allocation call even without a separate loop poll.
- Functions with no GC effects and no collecting callees receive no mutator
  region and no poll.
- The existing fixtures cover recursion, branches, early return, and nested
  root frames. No required poll is deliberately removed by optimization; the
  lowering pass is before LLVM optimization, and the ABI-level calls are not
  marked readnone.
- `-O0` and `-O2` preserve the calls for managed fixtures; isolation fixtures
  produce neither the calls nor their declarations after the GC-classification
  gate.

## 7. Write barriers

`nrq_gc_root_store` is the barrier-bearing store for a shadow root slot. Managed
field stores are lowered through the GC store path when the containing object's
type includes a managed reference. The barrier is needed for generational and
concurrent tracing; the collector's tracing contract in
`runtime/include/nirqoba/runtime/gc.h` requires it for any store that can expose
a new reference.

The audit found no ordinary owned or shared field that receives a GC barrier
solely because it is heap allocated. A field without managed references uses the
ordinary store path. `memory.deallocate`, `array.owned_release` and
`memory.shared_release` do not acquire GC barriers when their payload has no
managed reference.

Optimization preserves the required stores because they are observable runtime
calls with pointer arguments, not dead loads. The existing tests cover direct
field stores and nested managed references; Phase 7 adds explicit barrier
evidence for a mixed owned/GC fixture.

## 8. Mutator coordination

- `nrq_gc_mutator_enter` registers a thread and its TLAB; leave flushes/publishes
  and detaches.
- Root frames are thread-local and LIFO.
- Collection parks registered mutators at safepoints and waits for their
  operation pins; unregistered ordinary threads are not part of the collector's
  mutator set.
- Shared ownership uses 64 pointer-selected control shards. The audit found no
  lock-order inversion introduced by root registration: root operations do not
  acquire shared-control shard locks, and shared retain/release does not acquire
  the root-frame lock while holding a shard.
- A thread that performs only ordinary owned/arena/pool/shared operations and
  never enters a mutator region does not acquire GC synchronization. A thread
  that performs a GC effect enters once and leaves at function exit.
- Existing multithreaded GC tests cover concurrent allocation and collection,
  thread exit, repeated collection and progress. Phase 7 benchmarks add a
  fresh-process multithreaded allocation/collection run.

Known limitation: there is no source-level thread-spawn surface, so multithread
coverage is through the runtime unit tests and the benchmark harness, not a
Nirqoba language fixture.

## 9. Mixed-strategy interoperability

### 9.1 Owned object referenced by a GC object

A GC-managed object cannot safely retain a non-owning pointer to an owned object
beyond the owner's lifetime. The language's supported guarantee is an explicit
shared handle or a compiler-checked borrow. Phase 7 does not silently promote an
owned object into the GC heap.

### 9.2 GC object referenced by an owned object

Supported through typed roots in ordinary owned storage. The root is registered
for the containing storage, not for every ordinary field; release/drop
unregisters it. This is the existing `owned_pointer_gc_roots`/aggregate model.

### 9.3 Arena and pool

Ordinary arena/pool payloads with no managed fields are supported. Managed
payloads are rejected after this phase because arena reset/destroy has no root
unregistration operation. This is a documented unsupported combination, not a
silent semantic change.

### 9.4 Shared ownership

Shared storage containing managed fields is supported: allocation registers a
shared root, copies/moves preserve the shared token, and final release
unregisters it. Plain shared storage with no managed fields receives no GC root.
The Phase 6 restriction on shared-to-shared owning cycles is preserved; no weak
references or cycle collector are added.

### 9.5 Cyclic managed structures

`array.allocate` graphs are traced through their typed layouts. The collector's
mark-sweep handles unreachable cycles; there is no reference-count-only
reclamation path for them. Reachable cycles remain live while a root exists.
Phase 7 preserves the existing cycle tests and adds a repeated
construct/collect benchmark.

### 9.6 FFI boundary

Phase 7 audits only the existing runtime ABI boundary. The runtime exposes raw
`void*` allocation/root operations to compiler-generated code; there is no
user-facing foreign ownership contract yet. FFI ownership remains a Phase 8
concern and is not claimed complete here.

## 10. GC isolation evidence plan

Representative fixtures must prove the absence of:

- `nrq_gc_allocate*` and GC object-header initialization;
- `nrq_gc_root_*`, root-frame begin/end, typed root registration;
- `nrq_gc_mutator_enter`/`leave`;
- `nrq_gc_safepoint`/loop polls;
- `nrq_gc_root_store` barriers;
- GC mutex/coordination calls.

The fixtures cover stack values, ordinary heap allocation, owned arrays,
borrows, moves, nested owned aggregates, arena allocation/reset, pool
allocation/release, plain shared allocation/release, shared aggregate copies,
GC-independent helper chains, and ordinary pointer/container traversal.

Assertions must inspect generated IR at `-O0` and `-O2`, not just source timing.
The check must also reject a fixture that merely optimized a call away while
leaving a GC declaration or runtime dependency behind. Existing
`gc_pointer_free`/`gc_no_gc`-style fixtures are the starting point; Phase 7
adds a declaration-aware assertion script.

Production and development runtime builds share one source list. The development
library adds observation only; `--debug-enabled` must not alter allocation
strategy, root lifetime, or collector semantics. Both configurations are
checked.

## 11. Planned corrections

1. **Sema contract:** reject `memory.allocate`/`memory.reallocate` with a
   tracked arena or pool allocator when the payload contains a managed
   reference. Diagnostic text explains that reset/destroy does not unregister
   roots and points at ordinary/shared/GC storage.
2. **TIR contract:** add a verifier rule so a malformed `ArenaBound`
   `memory.allocate` result whose element contains a managed reference is
   rejected even if Sema is bypassed.
3. **Transitive effects:** seed `function_uses_gc` from a shared allocation whose
   payload contains a managed reference, and test the resulting mutator/root
   behavior.
4. **Evidence:** add focused semantic and IR tests for roots, effects,
   safepoints, barriers, mixed strategies, cycles, isolation at `-O0`/`-O2`,
   production/debug runtime parity, and malformed TIR.
5. **Benchmarks:** add `benchmarks/memory-model/phase-7/` with managed
   allocation, root/frame, safepoint, barrier, collection/latency, cycle,
   memory, multithread and isolation measurements. Record medians and ranges;
   do not claim a universal speedup.
6. **Documentation:** update the plan, progress report, target architecture,
   version log, README where guarantees are user-facing, migration note, and
   the Phase 7 completion report.

## 12. Explicit non-goals

- No Phase 8 work.
- No full FFI ownership integration.
- No automatic compiler-directed GC selection or CDMM.
- No weak references or new cycle collector.
- No replacement of the existing collector.
- No broad optimization campaign.
- No change to completed Phase 1–6 semantics except the proven arena/pool
  managed-payload defect above.
