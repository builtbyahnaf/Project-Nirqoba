# Phase 8 migration notes: ownership by default

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
