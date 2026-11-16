# Foreign and strategy interoperability (Phase 8)

## FFI status: unsupported

Nirqoba has no source-level foreign-function interface. There is no `extern`
declaration, foreign pointer type, callback type, calling-convention annotation,
unsafe scope or dynamic-library builtin. The runtime's `extern "C"` entry points
are used by generated code and are not callable from source.

An `extern` declaration remains a parser error and is covered by a regression
fixture. No foreign ownership contract is implemented or claimed.

## Contracts a future FFI must satisfy

These are design requirements, not implemented features:

| Boundary | Required contract |
|---|---|
| Borrowed foreign pointer | non-owning, bounded by a declared external lifetime; never freed by Nirqoba |
| Ownership into Nirqoba | a compatible release function/allocator, preserved provenance, exactly-once release |
| Ownership out of Nirqoba | original binding invalidated, release responsibility documented, no automatic destruction |
| Foreign allocation returned | explicit deallocator; ambiguous ownership rejected |
| Retained foreign pointer | explicit lifetime contract; unsupported if it cannot be represented |
| Callback | declared argument ownership, state lifetime, cleanup, roots, reentrancy and thread rules |
| GC handle | rooted for the required duration; no raw unmanaged pointer outliving the root |
| Shared token | correct strong token transferred or retained, never freed through an ordinary deallocator |
| Arena/pool pointer | call-scoped or explicitly bounded; no retention past reset/destroy |

## GC, shared and arena interaction today

- Foreign code cannot receive a GC-managed handle, shared token or arena/pool
  pointer because there is no FFI path.
- Unknown/external calls in lowering are conservatively treated as collection
  points. That is an internal generated-code policy, not FFI support.
- Managed fields in manual storage are registered as typed roots and removed
  before release.
- Manual and shared storage never convert into one another.
- Tracked arena/pool storage cannot be manually released or reset while a value
  or borrow is live.
