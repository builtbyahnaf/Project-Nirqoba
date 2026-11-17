# AOR Phase 3 Migration Notes

**Status:** complete  
**Object header:** H32  
**Compatibility:** no source-language or ownership change

## What changed

- `ObjectHeader::mark_next` was removed. Marking uses the inline worklist plus a cycle-reused external vector.
- `ObjectHeader::tlab_next` was removed. TLAB publication uses per-mutator `{page, begin, end}` ranges.
- `ObjectHeader::page` was removed. Ordinary pages resolve through `PageDirectory`; large and fallback pages store their page in the exact `ObjectIndex` entry.
- `nrq_gc_object_header_bytes()` is now 32. Allocation facts are 24 instance bytes plus 8 shared bytes.

## What did not change

- Managed payloads still begin at `header + 1` and remain max-align aligned.
- `element_count` remains at `payload - 16`; compiler/SoA code that reads the count at that offset needs no change.
- State masks, young/age encoding, root registration, store barriers, SATB, and deterministic ownership cleanup are unchanged.
- TLAB batching, spare-run reuse, cycle-boundary flushes, whole-page retirement, and free-span reuse are unchanged.
- Large objects still use the exact object index and the unindexed sweep list.
- Page bitmap cost remains 3072 bytes per ordinary 64 KiB page on x64.

## Operational notes

- `find_pending_header` now walks publication ranges; the ranges remain valid until the batch is published at a cycle boundary, thread exit, or range-table overflow.
- A full range table (32 ranges) publishes the current batch early under `gc_mutex`; it does not grow.
- The mark worklist may allocate once when a heap first exceeds 256 pending marks; it keeps capacity between cycles.
- Page lookup is a 1024-slot direct map with an open-addressed fallback. It allocates only when the page directory itself grows.
- Pages that failed directory insertion still set `use_object_index`, and their objects carry the page in the index entry.

## Migration order

Phase 3 removed the intrusive dependencies that blocked compact representations. Phase 4 can introduce headerless, compact, and extended representations; H32 remains the fallback until those pass validation. Uniform page classes remain inactive because the current TLAB cursor can mix spans.
