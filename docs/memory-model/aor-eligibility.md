# AOR representation eligibility

**Status:** Phase 4, complete  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

The allocator picks a representation from the object's shape and size. The choice
is deterministic and static: the same request always gets the same
representation, and no access path re-decides it.

## Auto selection

| Request | Representation |
|---|---|
| array or SoA container (any element count, including one) | H16 extended |
| fixed record, payload ≤ `kHeaderlessMaxPayload` (256 B), span fits a page | H0 headerless |
| fixed record, larger payload, span fits a page | H8 compact |
| fixed record whose span exceeds a page | large object, H16 |
| page larger than 64 KiB | large object, H16 |

`kHeaderlessMaxPayload = 256` exists because H0 spends a dedicated uniform page
on one shape. A page per small record pays off; a page per 1 KiB record does not,
because the slot rounding and the loss of mixed-layout packing cost more than the
8 bytes H8 saves.

An array is never H0 or H8. The element count is part of the object, not of the
page, and two containers of the same element size can have different counts, so
a uniform page cannot describe them.

## Explicit requests

`nrq_gc_allocate_repr(bytes, trace, representation)` states a requirement:

| Hint | Accepted when | Otherwise |
|---|---|---|
| `NRQ_GC_REPR_AUTO` | always | behaves like `nrq_gc_allocate` |
| `NRQ_GC_REPR_HEADERLESS` | fixed record, payload ≤ 256 B, fits a page | rejected (`nullptr`) |
| `NRQ_GC_REPR_COMPACT` | fixed record, fits a page | rejected (`nullptr`) |
| `NRQ_GC_REPR_EXTENDED` | always | never rejected |
| any other id | never | rejected (`nullptr`) |

A rejected request allocates nothing. It is not coerced into another
representation, because a caller that asked for a representation is usually
measuring or validating one, and a silent substitution would make the result a
lie. `NRQ_GC_REPR_EXTENDED` is the escape hatch: it always succeeds where the
payload itself is legal.

Array entry points (`nrq_gc_allocate_array`, `nrq_gc_allocate_array_typed`,
`nrq_gc_allocate_array_layout`, and their rooted forms) are always extended.
Passing an array through `nrq_gc_allocate_repr` is not possible; the typed and
layout entry points carry the count and the array flag.

## What the decision reads

The descriptor, not the caller's intent:

```
payload_size      element size for an array, record size for a record
flags             NRQ_OBJECT_LAYOUT_ARRAY when the shape repeats
```

The "repeats" fact cannot be inferred from the byte counts. A one-element
container has `count * element_size == element_size`, exactly like a fixed record
of that size. The entry point states it, and it is part of the layout memo key.
That distinction was a real bug during Phase 4: a one-element SoA container was
described as a record, became headerless, and the compiler's `payload - 16` count
read returned page metadata.

## Page dedication

- A headerless page holds one layout and one slot size.
- Compact and extended pages accept any shape; each object's header names its
  layout.
- Reuse is representation-local. A free run on a compact page is never used for a
  headerless object even if the span matches, because the page's own kind decides
  how the object is decoded.
- Large and fallback pages are never reused for ordinary objects.

## Mixed-shape threads

A thread keeps one active headerless slab. If a second small shape asks for a
headerless object while that slab still has room, the second shape is allocated
as H8 compact instead of evicting the half-full slab. Without that rule a
workload alternating two record shapes would take the allocator lock and reserve
a fresh uniform page on almost every allocation. The cost is that the second
shape spends 8 header bytes; the benefit is that neither shape thrashes.

When the owning slab is exhausted, the next request for any shape may reserve a
new slab, so a shape that becomes hot again can take the headerless
representation back.

## Fragmentation

A headerless page packs twice as many objects as the H32 page it replaced, so it
produces twice as many freed runs for the same churn. `kMaxFreeSpans` is 384, and
a refill reserves a batch of runs (up to `kSpareRunCapacity = 24`, within twice a
page of bytes) so fragmented pages still answer without taking the allocator lock
per run. A page that exhausts its run list simply stops answering reuse requests;
nothing is corrupted, but a fresh page is used instead.
