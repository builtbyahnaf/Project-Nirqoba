# AOR Page-Local Metadata

The Adaptive Object Representation (AOR) work starts from the H48 collector. Phase 1 audited every per-object field and Phase 2 moved the metadata that can be derived from a page out of the object header.

## Page classes

Every `GcPage` records a `PageKind`:

- `Mixed` — the current ordinary page. Objects may have different spans and layouts; the header remains authoritative.
- `Uniform` — reserved for a page whose objects share one slot size and layout. The fields exist and are reset, but the current TLAB cursor can serve different spans, so uniform allocation is not activated yet.
- `Large` — an object or object group larger than 64 KiB. There is no walkable start bitmap; exact lookup uses `ObjectIndex`.
- `Fallback` — a page whose directory insertion failed. It also uses the exact index.

## Derived counters

An ordinary page tracks live objects and live payload bytes, young and born-this-cycle objects, marked and marked-young objects, remembered objects, and indexed objects. The counts authorize whole-page sweep decisions:

- a fresh page with no marked young survivors and no dead objects is skipped after its marks are cleared;
- a fresh page with no survivors is retired without visiting headers;
- a large or fallback page always uses the per-object path because its exact index entries must be erased.

Development builds validate the count relationships and mark cleanup at cycle end.

## Bitmaps

A 64 KiB page has one bit per max-align slot for object starts, marks, and remembered membership. On the current x64 target that is 1024 bytes each, 3072 bytes total, or 4.7% of an empty page. `nrq_gc_get_page_metadata_facts` reports the measured split.

The mark and remembered bitmaps are off-heap: setting either does not rewrite the object's state word. Young and promotion age remain in the state word because they are read on every marker visit and every survivor update.

## Recycling

A page that becomes empty is retired until a cycle boundary, when no mutator can hold a cursor into it. The reset path clears start, mark, and remembered bitmaps, live/young/born/payload/indexed counts, free-span metadata, and page-class slot fields before the page can serve another allocation. Remembered-set compaction runs before recycling and requires each entry to resolve to its exact published object start.

## Large pages

Large pages keep the exact object index and the unindexed sweep list. Their marks are page-local like ordinary pages, but whole-page reclamation is not used because each object index entry must be removed individually.
