# Memory model overview (Phase 8)

**Version:** `0.583-beta`

Nirqoba keeps six allocation strategies distinct:

| Strategy | Producer | Release | Lifetime |
|---|---|---|---|
| Ordinary owned | `memory.allocate`, `memory.owned_array` | `memory.deallocate`, `array.owned_release` | lexical / transferred owner |
| Borrowed | `&`, `borrow` parameters, views | none | source-bound |
| Arena / pool | `memory.arena`, `memory.pool` | reset/destroy, `memory.pool_release` | allocator region |
| Shared | `memory.shared_allocate` | `memory.shared_release` | explicit strong count |
| GC-managed | `array.allocate`, `managed<T>` | collector | reachability |
| Manual | `memory.manual_allocate` | `memory.manual_release` | function-local explicit owner |

Manual storage is an escape hatch, not the default. Its TIR ownership category
is `Manual`; it never silently becomes `Owned`, `Raw`, `Shared`, `ArenaBound` or
`GcManaged`. See [ownership.md](ownership.md) for the manual contract,
[interop.md](interop.md) for the absent FFI boundary and
[garbage-collection.md](garbage-collection.md) for rooting.

The canonical detailed manuals remain [managed and explicit
memory](../memory/management.md), [allocator strategies](../memory/allocators.md)
and [ownership representation](../memory/ownership-representation.md).
