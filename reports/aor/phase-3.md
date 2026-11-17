# AOR Phase 3 — Remove Intrusive Object Dependencies

Status: implemented in the current tree  
Baseline: Phase 2 page-local metadata  
Scope: external mark worklist, range-based TLAB publication, directory-based page resolution, and the H48 → H32 header reduction.

## 1. What changed

Phase 3 removes the three intrusive fields that Phase 1 identified as the blockers for a compact header:

| Removed dependency | Replacement |
|---|---|
| `ObjectHeader::mark_next` | External, cycle-reused mark worklist |
| `ObjectHeader::tlab_next` | Per-mutator `{page, begin, end}` publication ranges |
| `ObjectHeader::page` | Page-directory lookup, with the owning page carried by large/fallback `ObjectIndex` entries |

The header is now 32 bytes:

```
offset  field          bytes
     0  next             8   unindexed-page publication only
     8  birth_epoch      8
    16  element_count    4
    20  state            4
    24  layout           8
```

`element_count` remains 16 bytes before the payload, so the compiler's SoA count load (`payload - 16`) and all payload arithmetic are unchanged.

## 2. External mark worklist

`MarkWorklist` keeps the existing 256-entry FIFO inline ring. Objects beyond it go to a `std::vector<ObjectHeader*>` owned by the worklist instead of into a per-object link.

- The vector is cleared at the start and end of a cycle but keeps its capacity, so a steady-state heap does not allocate per object and normally does not allocate per cycle.
- The inline ring is unchanged, so shallow graphs still mark without touching the vector.
- Prefetching follows the next inline entry or the vector's back entry.
- A failed vector growth is fatal instead of silently dropping a mark; an unmarkable object would be a correctness failure.
- Marking still runs under the collector's existing synchronization. No new lock, atomic, or per-object allocation was introduced.

The worklist is now a single persistent object (`active_mark`) reset between cycles rather than a value-assigned temporary, which is what retains the vector's capacity.

## 3. Range-based TLAB publication

A mutator records unpublished allocations as up to 32 contiguous `{GcPage*, begin, end}` ranges in `MutatorState`. Consecutive allocations from one cursor extend the last range; a cursor move or page switch starts a new one.

Publication walks each range in address order:

1. Read the header at the range cursor.
2. Recompute its payload size from `element_count * layout->payload_size`.
3. Advance by the same `object_span_bytes` used by allocation.
4. Insert into the start bitmap (or exact index), then update page and global accounting.

This preserves batch publication while removing one header write per allocation. If the range table fills, the current batch is published under `gc_mutex` and a new table starts; the table never grows and the hot path never allocates. `find_pending_header` uses the same ranges, so barriers and transfer pins still recognize unpublished objects.

## 4. Page resolution

`page_for_header` replaces the stored pointer:

- Ordinary pages: one direct-mapped `PageDirectory` probe, increased from 256 to 1024 slots. A miss falls back to the authoritative open-addressed table and fills the direct slot.
- Large and fallback pages: the exact `ObjectIndex` entry now stores the owning page alongside the header, so the same lookup that resolves the object also resolves its page.

The directory and index are only mutated under `gc_mutex`; the existing direct cache remains `mutable` for const lookups. No new lock or allocation was added.

Hot loops that already know the page — sweep, range publication, page reset — pass it explicitly, so a page is resolved once per page or once per object only where the object arrives without one. Mark enqueue and the remembered-set barrier resolve once and reuse the page for both the test and the update.

## 5. Preserved behavior

- Precise marking, SATB, generational remembered sets, and deterministic destruction are unchanged.
- TLAB batching, spare-run reuse, and cycle-boundary flushing are unchanged.
- Whole-page retirement, lazy all-live sweep, free-span reuse, and exact large-object accounting are unchanged.
- Production and development builds share the same metadata and paths.
- `nrq_gc_object_header_bytes()` now reports 32; allocation facts report 24 instance bytes plus 8 shared bytes.
- The page bitmap cost is unchanged: 3072 bytes per ordinary 64 KiB page.

## 6. Verification

- Production and development builds pass.
- Unit suite: **335/335**.
- Full integration/memory-model suite: **624/624**.
- Large-object (`128 KiB` and `512 KiB`) allocation, lookup, reclamation, and unindexed sweep tests pass.
- Page recycling, stale remembered-entry, derived-counter, and page-metadata-facts tests pass.
- No performance claim is made. The change removes per-object writes and 16 header bytes; the page lookup is new work on paths that do not already hold a page, and its net effect has not been benchmarked.

## 7. Limitations and next work

- The external worklist can allocate once when a heap first exceeds 256 pending marks. It never allocates per object.
- Range publication is capped at 32 ranges per mutator between publications; overflow flushes early rather than growing the table.
- The header still carries `next`, `birth_epoch`, `element_count`, `state`, and `layout`. Phase 4 can now evaluate compact/headerless representations without an intrusive link or stored page pointer.
- Uniform page classes remain inactive because the current TLAB cursor can mix spans.
