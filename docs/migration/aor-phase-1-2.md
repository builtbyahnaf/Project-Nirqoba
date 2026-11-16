# AOR Phases 1–2 Migration Notes

**Status:** complete  
**Object header:** H48 retained temporarily  
**Compatibility:** no source-language or ownership change

## What changed

The collector now keeps mark and remembered membership in per-page bitmaps, alongside object starts. Pages carry representation-class metadata and derived counts. A direct-mapped cache answers repeated page lookups before the authoritative directory hash.

No object-header field was removed. The H48 layout, public allocation facts, state masks, root protocol, TLAB publication, barriers, and sweep semantics remain compatible.

## What did not change

- Managed object payloads still begin at `header + 1` and remain max-align aligned.
- `nrq_gc_object_header_bytes()` remains 48.
- Young and promotion age remain in the object state word.
- Large objects still use the exact object index and the unindexed sweep list.
- Root registration/store rules and deterministic ownership cleanup are unchanged.
- Production and development builds use the same page metadata.

## Operational notes

- `nrq_gc_get_page_metadata_facts()` reports the bitmap cost of an ordinary page: 3072 bytes on the current x64 target.
- A page that failed directory insertion is never placed in the direct lookup cache; it must continue to use the exact object index.
- A recycled page's bitmaps and counters are cleared before it can serve another allocation.
- Remembered-set compaction runs before page recycling and rejects entries that no longer resolve to their exact object start.

## Migration order

Phase 1 found that every H48 field is still required for mixed pages. Phase 2 supplies the page-local replacement infrastructure. Phase 3 is expected to remove intrusive links and the stored page pointer; Phase 4/5 can then activate compact representations. Uniform page fields are present but not yet used because the current TLAB cursor can mix spans.
