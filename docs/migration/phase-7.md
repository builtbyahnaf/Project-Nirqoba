# Phase 7 Migration — GC Isolation and Explicit Selection

**Version:** `0.575-beta`  
**Previous phase:** Phase 6 — Explicit Shared Ownership (`0.571-beta`)  
**Scope:** explicit collector selection, root correctness, transitive effects,
safepoints, barriers, mutator coordination, mixed strategies and ordinary-path
isolation. Phase 8 was not started.

---

## 1. The rule

Nirqoba's default remains deterministic ownership. A program that creates no
managed value must not allocate from the collector, register roots, open root
frames, enter a mutator region, poll a safepoint, execute a GC write barrier or
take collector bookkeeping locks.

Garbage collection remains available and fully functional. It is selected by
using a GC-managed value, not by linking the runtime or starting the process.

## 2. Explicit collector selection

| Surface | Meaning |
| --- | --- |
| `array.allocate<T>(count)` | Explicit GC-managed array allocation; returns `managed<T>` |
| `managed<T>` | A collector reference; fields of this type are traced |
| `memory.owned_soa<T>(count)` | Explicit GC-backed SoA container, retained under its legacy name |
| `gc.collect()` | Explicit full collection |
| `gc.enable()` / `gc.disable()` | Automatic-collection control; starts disabled |
| `memory.allocate<T>` | Ordinary owned storage, never a GC allocation |
| `memory.shared_allocate<T>` | Shared-owned storage, never a GC allocation |
| `memory.arena` / `memory.pool` | Bulk-lifetime storage; never a GC allocation |

There is no automatic compiler-selected GC and no CDMM in this phase. A source
program that writes `memory.allocate<T>` does not become a collector program
because a field happens to be a managed reference: the storage remains ordinary
and receives a typed root only for the managed field.

### 2.1 Managed graph

```nirqoba
struct Node {
    int value;
    managed<Node> next;
}

managed<Node> function make_pair() {
    var first = array.allocate<Node>(1);
    var second = array.allocate<Node>(1);
    first[0].value = 7;
    second[0].value = 9;
    first[0].next = second;
    return first;
}

int function main() {
    var head = make_pair();
    gc.collect();
    print(head[0].next[0].value);
    return 0;
}
```

`array.allocate` selects the collector. The typed layout describes the `next`
offset, so tracing follows the second node and reclamation handles a cycle when
the root disappears.

## 3. Roots and root frames

A managed value is published through a typed root before a collection can
observe it:

- `array.allocate`/`memory.owned_soa` use rooted allocation entry points.
- A managed local, parameter, tuple or aggregate gets a frame slot and a typed
  descriptor. The frame is opened at function entry and ended in LIFO order.
- Managed fields inside ordinary `memory.allocate` storage get a typed root for
  the containing storage. `memory.deallocate` or aggregate drop unregisters it.
- Managed fields inside `memory.shared_allocate` storage get a shared root,
  removed on the final `memory.shared_release`.
- Managed elements in `memory.owned_array<T>` get an array root, removed by the
  owned-array release.

Early returns, branches, loops, nested frames and recursion all use the same
frame marker; there is no branch-local root registration.

```nirqoba
struct Holder {
    managed<Node> child;
}

int function main() {
    var holder = memory.allocate<Holder>(64);
    (*holder).child = make_pair();
    gc.collect();
    print((*holder).child[0].value);
    return 0;
}
```

The owned block is outside the collector heap, but its managed field is still a
root. The collector must not reclaim the child while `holder` is live.

### 3.1 Arena and pool payloads

Tracked arena and pool allocations reject a payload containing a managed
reference:

```text
error[NRQ3015]: arena or pool allocations cannot contain managed references in
'Holder': reset and destroy do not unregister GC roots
```

Bulk reset/destroy frees storage without running a root-unregistration hook. A
registered typed root would therefore point into freed memory. The rejection is
enforced in Sema and repeated by the TIR verifier for malformed or hand-built
TIR. Plain arena/pool payloads remain supported and GC-free. Use ordinary
storage, shared storage or an explicit GC container when a grouped payload must
hold managed references.

## 4. Transitive GC effects

A function is GC-dependent when it:

- returns, accepts, stores or creates a managed value;
- allocates a GC container;
- registers a typed root;
- calls a runtime operation that allocates or collects; or
- calls a function that has one of these effects.

The effect summary is a least fixpoint over direct calls, so helpers, recursion
and mutual recursion propagate it. A GC-independent call chain stays free of
mutator registration and safepoints.

A shared allocation whose payload contains a managed reference is a GC effect
even though the returned shared token is an ordinary pointer. Phase 7 adds that
case to the seed set; without it, a helper that only created shared storage with
a managed child would not open the mutator region needed around the root
registration.

Undefined or external calls are treated conservatively as collection points
unless they are on the runtime's explicit collection-free list. Indirect calls
have no complete target set in the current language surface and are treated as
unknown/collecting at the call site; this is documented rather than assumed
safe.

## 5. Safepoints

A function that needs a mutator region enters through `nrq_gc_mutator_enter` and
leaves through `nrq_gc_mutator_leave`. A GC-dependent loop gets a safepoint poll
on its backedge. Allocation and explicit collection calls are collection points
in their own right.

Ordinary loops contain no safepoint and no mutator enter/leave. Root liveness
ensures a managed slot needed after a collection point is registered; a dead
slot is not kept registered merely because it has a managed type.

## 6. Write barriers

A store that can publish a new managed reference uses `nrq_gc_root_store` or
the corresponding root-copy barrier. This covers shadow root slots, managed
fields of ordinary/shared storage and managed array elements. The barrier is
required by the collector's generational/incremental tracing contract and is not
removed by optimization.

A field without a managed reference uses an ordinary store and receives no
barrier merely because its containing object is heap allocated. Plain shared
ownership does not imply GC tracking.

## 7. Mutator coordination

- `nrq_gc_mutator_enter` attaches the thread and its allocation state.
- Root frames are thread-local and strictly LIFO.
- The collector parks registered mutators at safepoints and publishes roots
  before marking.
- A thread that performs only ordinary owned, arena, pool or plain-shared work
  does not enter the mutator set.
- Shared control remains on 64 pointer-selected shards; root registration and
  shared retain/release do not introduce a lock-order inversion.

The runtime's unit tests exercise concurrent allocation, collection, thread
exit and repeated collection. There is no source-level thread-spawn API, so
language fixtures are single-threaded; this is a limitation of the current
language surface, not a claim that the collector is single-threaded.

## 8. Mixed strategies

### 8.1 Owned storage holding a GC object

Supported through a typed root in the owned storage. Release/drop unregisters
the root; the collector reclaims the child only after the root disappears.

### 8.2 GC object holding an owned object

A managed field must not retain a non-owning pointer to an ordinary owner past
the owner's lifetime. Use an explicit shared handle or a compiler-checked borrow.
There is no silent promotion of an ordinary object into the collector.

### 8.3 Shared storage holding a GC object

Supported. Shared allocation registers a typed shared root; retain creates
another strong owner without a second root; the final release removes the root.
The Phase 6 restriction on shared-to-shared owning cycles remains.

### 8.4 Cycles

Managed cycles are traced and reclaimed when unreachable. Ordinary owned and
shared objects are not swept into a collector graph merely because a managed
field points near them.

### 8.5 FFI

Phase 7 audits the existing runtime ABI only. A complete foreign ownership
contract, foreign pointer rules and cross-boundary cleanup remain Phase 8 work.

## 9. Isolation evidence

The repository checks ordinary fixtures at both `-O0` and `-O2`:

```text
python tests/phase7_isolation.py
```

For each fixture the check:

1. compiles `--emit=llvm` and rejects any `nrq_gc` name, including declarations;
2. compiles `--emit=obj` and rejects any `nrq_gc` symbol in the object, so an
   unused declaration or runtime dependency cannot hide;
3. runs the executable and checks its exit code;
4. does the same for the development runtime with `--debug-enabled`;
5. builds a managed control and requires GC symbols, proving the check is not
   vacuous.

Fixtures cover owned pointers, owned arrays, nested owned aggregates, borrowed
heap views, reallocation, secure storage, plain shared storage, helper chains,
arena allocation/reset, pool allocation/release and the debug runtime.
Production and development runtimes share one source list; observation differs,
collector semantics do not.

## 10. Known limitations

- Tracked arena/pool payloads cannot contain managed references. This is a
  deliberate rejection, not a hidden promotion.
- Manual allocator handles obtained from `memory.allocator_create` are not
  tracked as arena identities by Sema; their root lifetime is a manual contract.
- Indirect calls are conservatively collection points.
- No weak references, new cycle collector or compiler-directed strategy
  selection exists in this phase.
- Sources do not yet expose a thread-spawn surface; concurrency evidence comes
  from the runtime tests and the Phase 7 benchmark harness.
- FFI ownership is not complete; Phase 8 owns it.
- `memory.owned_soa<T>` is GC-backed despite its legacy name. It is documented
  as an explicit collector container, not silently renamed.
