# AOR Phase 2 — Page-Local Metadata Architecture

Status: implemented in the current tree  
Baseline: H48 retained as the correctness fallback  
Scope: page descriptors, lookup, object-start/allocation metadata, mark and remembered bitmaps, generation counters, large-page fallback, recycling, and debug validation.

## 1. What Phase 2 delivers

Phase 2 does **not** remove an object-header field. Doing so before the representation classes exist would convert direct loads into directory probes or make arrays and mixed pages incorrect. It delivers the page-local infrastructure those removals require:

1. A direct-mapped page cache in front of the authoritative directory.
2. One reset path for every page-derived counter and bitmap.
3. Page-class metadata (`Mixed`, `Uniform`, `Large`, `Fallback`) with slot size, slot capacity, and uniform layout fields.
4. Off-heap remembered-set membership and an exact per-page remembered count.
5. Per-page live, live-byte, young, born-this-cycle, marked, marked-young, and indexed-object counts.
6. Large/directory-fallback accounting that stays separate from ordinary pages.
7. Cycle-end development validation of page metadata invariants.
8. Tests for recycling, stale remembered entries, and measured bitmap overhead.

The header remains at 48 bytes. This is deliberate: Phase 2 is the foundation, and H48 is the reference implementation against which later representations must be verified.

## 2. Page lookup

`PageDirectory` keys ordinary page-aligned storage by its page number and uses open addressing. Every metadata read that starts from an arbitrary object used to pay a hash probe even when the same page was read repeatedly.

Phase 2 adds a 256-entry direct-mapped cache keyed by the low eight bits of the page number. A hit is one compare; a miss falls back to the hash table and fills the cache. The cache is only populated after the hash table accepts a page, so a page that failed directory insertion remains on the exact `ObjectIndex` path.

The cache is `mutable` because a const lookup may fill it. All callers already hold `gc_mutex`; no new lock and no atomic is introduced. An erase clears the matching direct entry.

## 3. Page classes

`PageKind` records the metadata representation a page is prepared for:

| Kind | Meaning | Current use |
|---|---|---|
| `Mixed` | Objects may have different spans/layouts | Ordinary pages |
| `Uniform` | One slot size and layout for every object | Reserved for the representation migration |
| `Large` | No walkable start bitmap; exact index | Objects larger than 64 KiB |
| `Fallback` | Directory insertion failed; exact index | Rare directory failure |

`slot_bytes`, `slot_capacity`, and `uniform_layout` are initialized to zero/null for mixed pages and reset whenever a recycled page is handed out. They are not yet used to allocate, because the current allocator deliberately mixes sizes and the TLAB cursor can serve different spans. Activating uniform allocation without per-size TLABs would be a regression and is Phase 4/5 work.

## 4. Page-derived counters

Every page now carries:

- `live_objects`, `live_payload_bytes`;
- `young_objects`, `born_this_cycle`;
- `marked_objects`, `marked_young_objects`;
- `remembered_objects`;
- `indexed_objects`.

The counts are updated at allocation/publication, marking, promotion, and reclaim. Whole-page sweep uses them to skip all-live pages and retire all-dead pages without visiting headers. Development builds verify at cycle end that:

- young, indexed, marked, marked-young, and remembered counts do not exceed live/young totals;
- an empty page carries no live metadata;
- no mark remains outstanding.

These checks caught a stale remembered entry during the preceding optimization work. That bug is fixed by compacting the remembered list before page recycling and requiring the header to resolve to its exact published start.

## 5. Remembered-set metadata

Remembered membership moved from the header state word to a page bitmap. The write barrier now updates a bitmap bit and a page count instead of a header word. The header's remembered mask remains reserved so the public debug state encoding does not change in Phase 2.

Promotion clears the page bit; reclaim clears it; page reset clears the whole bitmap. `compact_remembered_locked` runs before retired pages are recycled and validates the header through the page directory and start bitmap. This prevents a reused address from inheriting an old entry.

## 6. Measured metadata overhead

For this target, an ordinary 64 KiB page has 8192 max-align slots:

| Bitmap | Bytes |
|---|---:|
| Object starts | 1024 |
| Marks | 1024 |
| Remembered | 1024 |
| **Total** | **3072** |

That is 4.7% of an otherwise empty page. The figure is exposed by `nrq_gc_get_page_metadata_facts` and pinned by a unit test. It is the cost Phase 4 must beat with larger objects per page or denser representations; it is not hidden in the header accounting.

## 7. Large pages and fallback

Large pages retain:

- the exact `ObjectIndex` for reference resolution;
- the `unindexed_objects` list for sweep enumeration;
- the same page mark bitmap;
- a per-page `indexed_objects` count.

The whole-page fast paths intentionally do not apply to large pages: every dead object needs its exact index entry erased. The direct cache is also not populated for a page whose directory insertion failed, preserving the fallback contract.

## 8. Verification

- Production and development builds pass.
- Unit suite: **335/335**.
- Full integration/memory-model suite: **624/624**.
- Phase 7 isolation: passed.
- Phase 9 production/debug matrix: passed.
- Configuration equivalence for `*gc*`: **67 agreed, 0 failed**.
- The two new recycling regressions pass: stale remembered entries do not keep reused addresses alive, and recycled pages clear all derived metadata.
- No performance claim is made for Phase 2. The only measured number is the page bitmap cost (`3072` bytes per ordinary page), which is deliberately reported as an overhead rather than an improvement.

## 9. Limitations and next work

- H48 remains the object representation. Phase 3 must remove intrusive links and the per-object page pointer before any compact representation can be evaluated.
- Uniform page classes are metadata-only until per-size TLABs exist.
- Young and age remain in the header; moving young would add a page lookup to every marker visit.
- The remembered bitmap uses one bit per slot. A sparse page pays for empty slots; this is acceptable while pages are the allocation unit and will be revisited with page classes.
