# Foreign and strategy interoperability (Phase 8)

## FFI status: pointer handoff only

Nirqoba can now declare a C symbol:

```text
extern void function consume_buffer(ptr int buffer);
```

The declaration stops at the parameter list: the body belongs to the C object the
program is linked against, so an `extern` declaration with a `{` is a parser
error. The symbol is emitted as an ordinary C-linkage declaration, which means
the name written in the declaration is the symbol resolved at link time.

The surface is deliberately narrow. What exists is exactly the ownership handoff:

| Boundary | Implemented rule |
|---|---|
| Ownership out of Nirqoba | Passing an owned aggregate (`dynamic_array<T>`, `owned_array<T>`, a struct holding one) to a `ptr` parameter transfers the buffer. C releases it; Nirqoba emits no destruction for the binding, and a later use is `NRQ3033` |
| Borrowed value into C | Rejected: C has no bound on how long it keeps the pointer |
| Handoff with a live view | Rejected: an array view of the storage would outlive the transfer |
| ABI shape | Only field 0 of the container header (the buffer) crosses the boundary; length and capacity stay behind, so a callee that needs them must take them as separate arguments |
| Return value | Void or a scalar the declared type maps to; no aggregate is returned from C |
| C releases a Nirqoba buffer | The buffer is released through the C entry point it was handed to, never through a Nirqoba destructor, so exactly one release happens |

That is the whole contract. Nothing else about foreign code is implemented or
claimed.

## Contracts a future FFI must satisfy

These are design requirements, not implemented features:

| Boundary | Required contract |
|---|---|
| Borrowed foreign pointer | non-owning, bounded by a declared external lifetime; never freed by Nirqoba |
| Ownership into Nirqoba | a compatible release function/allocator, preserved provenance, exactly-once release |
| Foreign allocation returned | explicit deallocator; ambiguous ownership rejected |
| Retained foreign pointer | explicit lifetime contract; unsupported if it cannot be represented |
| Callback | declared argument ownership, state lifetime, cleanup, roots, reentrancy and thread rules |
| GC handle | rooted for the required duration; no raw unmanaged pointer outliving the root |
| Shared token | correct strong token transferred or retained, never freed through an ordinary deallocator |
| Arena/pool pointer | call-scoped or explicitly bounded; no retention past reset/destroy |
| Passing length and capacity | a documented multi-argument convention, or a struct the C side understands |

## GC, shared and arena interaction today

- Foreign code cannot receive a GC-managed handle, shared token or arena/pool
  pointer: the handoff rule covers owned aggregates only, and the other kinds are
  rejected before lowering.
- Unknown/external calls in lowering are conservatively treated as collection
  points. That is an internal generated-code policy, not FFI support.
- Managed fields in manual storage are registered as typed roots and removed
  before release.
- Manual and shared storage never convert into one another.
- Tracked arena/pool storage cannot be manually released or reset while a value
  or borrow is live.
- A buffer handed to C is no longer tracked by Nirqoba, so region and pool
  selection never claims it.
