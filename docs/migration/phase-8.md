# MMM Phase 8 migration — manual memory and the FFI boundary

**From:** `0.575-beta`  
**To:** `0.583-beta`  
**Previous phase:** Phase 7 — GC Isolation and Explicit Selection

## 1. The rule

Ordinary ownership remains the default. Manual storage is an explicit, distinct
owner, not a raw pointer and not an ordinary `memory.allocate` block:

```nrqoba
int function main() {
    var value = memory.manual_allocate<int>(4);
    *value = 41;
    print(*value);
    memory.manual_release(value);
    return 0;
}
```

`memory.manual_allocate<T>(bytes[, allocator])` produces
`ValueOwnership::Manual`. `memory.manual_release` is the only valid release.
The compiler still tracks moves, borrows, exactly-once release and the lexical
fallback; manual code chooses the allocation and release point but does not
leave the ownership model.

## 2. What changed

| Area | Before `0.583-beta` | At `0.583-beta` |
|---|---|---|
| Manual category | No `Manual` category; `Raw` was non-owning | `Manual` is owning, transferable and requires destruction |
| Surface | No explicit manual allocation distinct from owned allocation | `memory.manual_allocate<T>` / `memory.manual_release` |
| Release pairing | `memory.deallocate` covered ordinary storage only | Manual storage rejects ordinary/secure/shared/resource releases; manual release rejects other families |
| TIR | Allocation contracts covered owned/shared/GC | Manual allocation and release contracts are verified |
| Lowering | Ordinary allocation path only | Manual allocation uses the selected allocator and typed-root handoff; release uses the checked deallocator |
| Function boundary | Owned pointers could cross signatures | Manual storage is function-local; owned parameters and returns reject it |
| FFI | Deferred | Still absent; documented and tested as an unsupported boundary |

## 3. Supported manual patterns

- Explicit allocate/write/read/release.
- Local moves (`var moved = owner`) with the source invalidated.
- Scoped borrows; release is rejected while a borrow is live.
- A lexical fallback release when no explicit release is written.
- An explicit allocator handle (`memory.manual_allocate<T>(bytes, handle)`).
- Managed payloads under the existing typed-root contract, including collection.
- Conditional release when every path discharges the owner.

## 4. Rejected patterns

| Pattern | Diagnostic |
|---|---|
| `memory.deallocate(manual)` | manual allocations must be released with memory.manual_release |
| `memory.secure_release(manual)` | secure release cannot destroy a different resource type |
| `memory.shared_release(manual)` | shared operation requires a shared allocation |
| `memory.manual_release(ordinary)` | manual release requires a memory.manual_allocate allocation |
| Double release / use after release | use of '...' after its allocation was deallocated |
| Release with a live borrow | cannot release a manual owner while a borrow is live |
| `memory.reallocate(manual, ...)` | manual allocations cannot be resized; release and allocate a replacement |
| Passing manual to an owned parameter | manual allocations cannot cross a function boundary |
| Returning manual storage | manual allocations cannot cross a function return |
| Manual from a tracked arena/pool | manual allocation cannot use a tracked arena or pool |
| Payload with owned/shared/borrow fields | manual allocations cannot contain ... |
| `extern` declaration with a body | expected ';' after an extern declaration |
| Handing a borrowed value to C | a borrowed value cannot be handed to C |
| Handing storage with a live view to C | cannot hand ... to C while an array view of it is live |
| Reading after a handoff | use of ... after its allocation was deallocated (`NRQ3033`) |

## 5. GC, shared, arena and native resources

- A manual payload containing `managed<T>` is registered as a typed root and
  unregistered before release. Plain manual storage emits no `nrq_gc` IR or
  object symbol.
- Manual and shared storage stay distinct: no implicit shared retain/release and
  no manual release of a shared control block.
- Tracked arena/pool storage stays bulk-owned. Manual allocation rejects those
  handles; `pool_release` and arena reset/destroy remain the only release paths.
- File, socket, mutex and mapping owners keep their resource-domain cleanup.
  They are not manual memory and cannot be released with `memory.manual_release`.
- `memory.reallocate` keeps its ordinary owned contract. Manual reallocation is
  deliberately unsupported in this phase.

## 6. FFI boundary

One foreign surface exists: `extern <type> function <name>(<params>);` declares a
C symbol, and the declaration stops at the parameter list. The only implemented
contract is ownership out of Nirqoba: passing an owned aggregate to a `ptr`
parameter hands the buffer to C, discharges the caller's destruction obligation
and marks the binding moved, so a later use is `NRQ3033`. The declaration chooses
the ABI shape: a `ptr` parameter receives the buffer alone, while a
`dynamic_array<T>` parameter destructures the header into
`(pointer, length, capacity)` as three scalars. Any other aggregate parameter is
rejected rather than given a layout C cannot know.

Therefore the following contracts are **not supported**: borrowed foreign
pointers, ownership transferred into Nirqoba, foreign allocations released by a
supplied allocator, retained foreign pointers, callbacks and reentrant foreign
calls, GC handles across an ABI, shared tokens across an ABI, arena/pool
pointers across an ABI, and aggregates returned from C. The declaration shape
and the handoff rule are covered by regression fixtures. See
`docs/memory-model/interop.md` for the contract table and
`reports/memory-model/phase-8.md` for the full completion report.

## 7. Migration checklist

1. Replace any intended manual lifetime with `memory.manual_allocate<T>` and
   `memory.manual_release`; keep `memory.allocate` for ordinary owned storage.
2. Do not pass manual storage to owned parameters or returns. Use a `borrow`
   parameter for temporary access, or release it before the call.
3. Replace manual reallocation with release + allocate.
4. Do not use a tracked arena/pool handle for manual allocation.
5. Keep managed fields rooted through the existing typed-root path; do not
   assume a foreign pointer is a GC root.
6. Keep FFI out of source until an ABI and ownership contract are implemented.

## 8. Verification

At `0.583-beta` the clean `RelWithDebInfo` build passes 329 unit tests and 624
integration/memory-model cases, 0 failed, 0 skipped. Phase 7 GC isolation and
the production/debug configuration corpus remain green. Benchmarks are in
`benchmarks/memory-model/phase-8/`.

---

## Historical pre-MMM Phase 8 notes: ownership by default

**From:** `0.499-beta`  
**To:** `0.511-beta`  
**Scope:** the completed ownership surface, not new generics or language features.

Phase 8 makes the hybrid memory model consistent at every storage boundary. A value
that owns or borrows a resource is accepted only by storage that can run the matching
release path. A boundary that cannot do that now rejects the value; it does not hand
the value to the collector or silently free raw bytes.

## 1. The rule in one sentence

Deterministic ownership and borrowing remain the default, the collector is an explicit
choice for managed graphs, and every unsupported combination is a compile-time error.

## 2. What changed

| Area | Before `0.511-beta` | At `0.511-beta` |
|---|---|---|
| Collector arrays | Could contain structs with owned, shared or borrowed fields; the sweep had no finalizer | Rejected unless every field is trivial or `managed<T>` |
| Explicit/reallocated/secure storage | Could be allocated for an owning payload; release ran no field destructors | Rejected unless every field is trivial or `managed<T>` |
| Shared allocations | Accepted owning payloads; final decrement freed only the block | Rejected unless every field is trivial or `managed<T>` |
| Owned arrays | Rejected nested owned arrays, but accepted shared/borrowed fields | Reject all element payloads whose fields need per-element destruction |
| SoA | Rejected owned arrays and views, but accepted shared and borrowed columns | Reject shared and borrowed columns as well |
| Nested generic types | Required a space before the final `>` (`owned_array<managed<T> >`) | `owned_array<managed<T>>` works in type contexts |
| Legacy runtime names | `nrq_gc_init`, `nrq_gc_shutdown`, `nrq_gc_collect`, `nrq_memory_allocate`, `nrq_memory_deallocate` were source builtins | Undeclared; use the namespaced GC API and typed allocation |

The previously documented `file.read_text` lifetime, publication crash window and
busy-registry termination behavior are unchanged. They are not part of this transition.

## 3. Supported patterns

These remain valid and are covered by the integration suite:

```nrqba
struct Node {
    int value;
}

struct Edge {
    int tag;
    managed<Node> target;
}

struct Holder {
    owned ptr int data;
    shared ptr int count;
}

int function main() {
    // Collector storage: managed references are traced by typed roots.
    var edges = array.allocate<Edge>(2);
    edges[0].target = array.allocate<Node>(1);
    edges[0].target[0].value = 7;

    // Owned array: the payload has no deterministic fields.
    var values = memory.owned_array<int>(4);
    values[0] = 3;

    // Lexical aggregate: owned and shared fields receive generated destruction.
    var holder = Holder {
        data: memory.allocate<int>(1),
        count: memory.shared_allocate<int>(1)
    };

    gc.collect();
    print(edges[0].target[0].value);
    return 0;
}
```

Nested generic types use the normal closing token:

```nrqba
var nodes = memory.owned_array<managed<Node>>(4);
var cells = array.allocate<managed<Node>>(2);
```

## 4. Rejected patterns and what to do instead

| Rejected code shape | Diagnostic theme | Migration |
|---|---|---|
| `array.allocate<Holder>` where `Holder` has an owned, shared or borrowed field | `collector storage has no finalizer` | Keep the holder in a lexical variable, or make the collector payload contain only trivial/`managed<T>` fields |
| `memory.allocate<Holder>`, `memory.reallocate<Holder>`, `memory.secure_allocate<Holder>` with a deterministic field | `... frees the storage without running field destruction` | Use `Holder` as a lexical value, or allocate a managed-only representation |
| `memory.shared_allocate<Holder>` with a deterministic field | `the final release frees storage without running field destruction` | Use a lexical holder or a managed-only representation |
| `memory.owned_array<Holder>` with a shared token or borrowed field | `bulk release does not run ... field destruction` | Store trivial/managed elements, or manage each holder lexically |
| `memory.owned_soa<Row>` where `Row` has a shared or borrowed column | `columns cannot hold reference-counted or borrowed fields yet` | Use trivial/managed columns, or store rows as lexical values |
| Passing a collector array to `reallocate` | `requires an explicitly allocated pointer` | Use `memory.allocate<T>` for raw storage and `array.allocate<T>` for collector storage |

A borrow is not an owning value. Borrowed fields and views stay inside the lexical
aggregate that carries their lifetime; they are not accepted by heap, collector or
bulk storage.

## 5. Explicit GC usage

Collector use is opt-in and unchanged in its controls:

```nrqba
// Automatic collection starts disabled.
print(gc.is_enabled());   // false

var node = array.allocate<Node>(1);  // explicit collector allocation
gc.collect();                        // explicit full cycle, always allowed

gc.enable();                         // opt in to automatic triggers
gc.set_threshold(64 * 1024);
gc.disable();                        // opt out again
```

There is no implicit fallback from a rejected owned/shared/borrowed payload to this
collector. A program either uses a supported managed representation or uses
deterministic storage that can run the release path.

## 6. Compatibility checklist

1. Replace `nrq_gc_init()` / `nrq_gc_shutdown()` with process lifetime (no source
   call is required) and `nrq_gc_collect()` with `gc.collect()`.
2. Replace `nrq_memory_allocate(bytes, alignment)` with a typed
   `memory.allocate<T>(bytes)` and `nrq_memory_deallocate(p, bytes, alignment)` with
   `memory.deallocate(p)`.
3. Remove spaces before nested generic closers; the `> >` spelling still parses, but
   `>>` is now the normal form.
4. Audit collector arrays and shared/owned allocations whose payload types contain
   `owned`, `shared` or `borrow` fields. Move those payloads to lexical aggregates or
   remove the deterministic fields.
5. Audit SoA element structs for shared or borrowed columns.
6. Keep `gc.collect()` where explicit reclamation is required; automatic collection is
   still disabled at process start.

Code that already uses trivial payloads, `managed<T>` references, lexical structs with
declared field contracts, `memory.allocate<T>` for raw bytes, or explicit GC controls
needs no source change.

## 7. Verification

At `0.511-beta` the clean `RelWithDebInfo` build passes 258 unit tests and 489
integration tests. The integration count includes positive tests for nested generic
and secure-storage managed payloads plus focused negative tests for every rejected
storage boundary. The phase report records the full inventory, benchmark comparison
and remaining limitations.
