# AOR page-local metadata

The Adaptive Object Representation (AOR) work started from the H48 collector.
Phase 1 audited every per-object field, Phase 2 added page-local metadata, Phase 3
removed the intrusive mark link, the TLAB publication link, and the stored page
pointer, and Phase 4 replaced the remaining 32-byte header with three
representations.

The current state is documented in:

- [aor-headers.md](aor-headers.md) — the H0, H8, and H16 layouts and the page
  metadata cost;
- [aor-eligibility.md](aor-eligibility.md) — how the allocator chooses one and
  when an explicit request is rejected.

**No standard GC-managed object carries more than 16 header bytes.**

## Why a page names the representation

An object cannot describe its own header size without carrying a size tag, and a
size tag is exactly the per-object field this work exists to remove. The page
carries it instead:

```
page.kind == Headerless  →  header 0,  payload at span start
page.kind == Compact     →  header 8,  payload at span start + 8
page.kind == Extended    →  header 16, payload at span start + 16
```

Discovery is then one subtraction from the span start, and the span start comes
from the page's own start bitmap. No global lookup is needed on a marker visit or
a sweep step.

## Page classes

- `Headerless` — one layout, one slot size, zero header bytes per object, state in
  a page byte array.
- `Compact` — ordinary mixed page; every object has an 8-byte header.
- `Extended` — ordinary mixed page; every object has a 16-byte header. Arrays and
  SoA containers live here.
- `Large` — an object or group larger than 64 KiB. H16, exact object index, no
  start bitmap.
- `Fallback` — a page whose directory insertion failed. H16, exact index, no
  start bitmap.

## Publication

A mutator records up to 32 `{page, begin, end}` ranges. Publication walks a range
by the page's span rule: a headerless range advances by `page->slot_bytes`, the
others read the span from each object's header. Publication marks the span start
in the page bitmap and updates the page's live counters. No per-object
publication link is written.

## Birth

`birth_epoch` was an 8-byte field in every object and is gone. A page now keeps a
born bitmap and a `born_epoch` stamp:

- the first allocation of a cycle clears the bitmap and stamps the page with the
  active cycle;
- a bit is meaningful only while the page's stamp equals the current cycle, so a
  stale bit from an earlier cycle is never read;
- no page walk is needed to reset the bitmap.

## Layout identity

An H0 page holds the descriptor pointer directly. H8 and H16 store a 32-bit
layout id, resolved once at allocation from a thread-local 8-entry memo and a
locked id table bounded at `1 << 14` ids. The id is never resolved on a read
path that already has the page.

## Bitmaps

A 64 KiB page has one bit per `max_align_t` slot for object starts, marks,
remembered membership, and birth: 1024 bytes each on this 8-byte-slot target. A
headerless page adds one state byte per slot (8192 bytes) because it has no state
word to hold young and age. Mark and remembered bits are off-heap: setting either
never rewrites an object.

## Derived counters

An ordinary page tracks live objects and live payload bytes, young and
born-this-cycle objects, marked and marked-young objects, remembered objects, and
indexed objects. Those counts authorize whole-page sweep decisions:

- a fresh page with no marked young survivors and no dead objects is skipped
  after its marks are cleared;
- a fresh page with no survivors is retired without visiting headers;
- a large or fallback page always takes the per-object path because its exact
  index entries must be erased.

Development builds validate the count relationships and mark cleanup at cycle
end.

## Recycling

A page that becomes empty is retired until a cycle boundary, when no mutator can
hold a cursor into it. The reset path clears the bitmaps, the counts, the
free-span metadata, and the page-class fields — including `use_object_index`,
which a recycled page used to keep from its previous life and which made the
sweep skip the page. Remembered-set compaction runs before recycling and requires
each entry to resolve to its exact published start.

## Large pages

Large pages keep the exact object index and the unindexed sweep list. Their index
entries carry the owning page because their addresses are not recoverable by
aligning to a 64 KiB page. Their marks are page-local, but whole-page reclamation
is not used because each index entry must be removed individually.

## Free storage

A reclaim records a run in the page's free-run list (384 entries). Allocation
consults runs before fresh page space, first-fit within the page, and requires the
page kind and — for headerless — the layout and slot size to match. Headerless
refills reserve a batch of runs so a fragmented page does not put the allocator
lock back on the allocation path.

## What Phase 4 did not change

Precise collection, SATB, generational remembered sets, deterministic
destruction, TLAB range publication, the external mark worklist, large objects,
arrays, pinning, multithreaded collection, and production/debug equivalence. The
`*gc*` configuration-equivalence matrix and the Phase 7 isolation and Phase 9
optimization checks pass unchanged.
