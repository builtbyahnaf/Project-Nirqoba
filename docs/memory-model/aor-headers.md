# AOR header layouts

**Status:** Phase 4, complete  
**Claim:** no standard GC-managed object retains a header larger than 16 bytes.  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)

A managed object's header size is a property of the page it lives on, not of the
object. A page is dedicated to one representation, so discovery is one
subtraction: the header begins at the object's span start and ends where the
payload starts.

## The three representations

| | H0 headerless | H8 compact | H16 extended |
|---|---|---|---|
| Header bytes | 0 | 8 | 16 |
| Page kind | `Headerless` | `Compact` | `Extended` (also `Large`, `Fallback`) |
| Layout | page (`uniform_layout`) | header (`layout_id`) | header (`layout_id`) |
| Element count | implicit `1` | implicit `1` | header field |
| Young / age | page byte (`h0_states`) | header `state` word | header `state` word |
| Mark / remembered / born | page bitmaps | page bitmaps | page bitmaps |
| Payload alignment | `alignof(max_align_t)` | 8 | `alignof(max_align_t)` |
| Payload offset | span start + 0 | span start + 8 | span start + 16 |

`nrq_gc_get_representation_facts()` reports these numbers from the build that is
running; `tests/unit/gc_representation_test.cpp` pins them.

## H0 — no per-object header

```
span start ─────────────────────────────► payload (no header bytes)
```

Everything an object used to carry is either page-level or unnecessary:

- the layout is `page->uniform_layout`; an H0 page holds exactly one layout and
  one slot size (`slot_bytes`);
- the element count is always one, because an array is never headerless;
- young and promotion age are one byte per span slot in `page->h0_states`
  (bit 0 young, bits 1–7 age);
- mark, remembered, born, and object-start are off-heap page bitmaps.

A headerless page therefore exists only for a fixed record whose payload is at
most 256 bytes (`kHeaderlessMaxPayload`) and whose span fits a page.

## H8 — 8 bytes

```
offset  field       bytes
     0  state           4     bit 0 young, bits 1-7 age, bit 8+ reserved
     4  layout_id       4     1-based index into the layout id table
```

The payload begins 8 bytes after the span start. On an 8-byte
`max_align_t` target the payload keeps full alignment; on a 16-byte target a
compact payload is 8-byte aligned, which `nrq_gc_get_representation_facts()`
reports as `compact_payload_alignment`.

`layout_id` is a 32-bit id. Ids are assigned once per descriptor by
`layout_id_for()`, cached in a thread-local 8-entry memo, and bounded by
`kMaxLayoutIds = 1 << 14`. Id 0 is never valid, so an uninitialised header is
detectable rather than resolvable.

## H16 — 16 bytes

```
offset  field            bytes
     0  element_count       4     the array count; 1 for a record
     4  state               4
     8  layout_id           4
    12  flags               4     reserved for Phase 5; written as 0
```

`element_count` is deliberately first. The SoA lowering reads the count at
`payload - 16`, which is now `header + 0`; the read needed no change. SoA
containers and multi-element / oversized arrays always take the extended
representation (`H16`), while eligible non-SoA one-element arrays use `Compact`
(`H8`, distinguished from fixed records by `NRQ_OBJECT_LAYOUT_ARRAY` on their
descriptor and with implicit `element_count == 1`).

`flags` is written and preserved but carries no meaning yet. It is the word
Phase 5 will use for compiler-side hints; leaving it in the header now keeps the
layout stable across that change.

## Large and fallback pages

`Large` (an object or object group larger than 64 KiB) and `Fallback` (a page
whose directory insertion failed) use the H16 header and the exact object index.
They never use the start bitmap, so their objects are found by index entry, which
also stores the owning page. Large objects are therefore always 16-byte headers;
there is no headerless or compact large object.

## What is no longer in any object

| Removed field | Phase removed | Replaced by |
|---|---|---|
| `mark_next` intrusive link | 3 | external worklist |
| `tlab_next` publication link | 3 | `{page, begin, end}` ranges |
| stored page pointer | 3 | page directory / exact index |
| `birth_epoch` | 4 | `born_bits` + lazy `born_epoch` stamp |
| `layout` pointer | 4 | page layout (H0) or `layout_id` (H8/H16) |
| `element_count` on records | 4 | H0/H8 implicit one |
| `state` word on H0 | 4 | `h0_states` page byte |

## Page metadata cost

Metadata is off-heap and paid once per 64 KiB page. On this MSVC x64 target
`alignof(std::max_align_t) == 8`, so a page has 8192 span slots.

| Page kind | start | mark | remembered | born | `h0_states` | total | share of page |
|---|---|---|---|---|---|---|---|
| H0 headerless | 1024 | 1024 | 1024 | 1024 | 8192 | 12288 B | 18.75% |
| H8 / H16 | 1024 | 1024 | 1024 | 1024 | — | 4096 B | 6.25% |

`nrq_gc_get_page_metadata_facts()` reports the three bitmaps; `h0_states` is the
price of removing the per-object state word from a headerless page and is
counted in the performance report rather than hidden.

## Invariants

- Every managed object's header is 0, 8, or 16 bytes. Nothing allocates a larger
  one.
- The object-start bitmap indexes span starts, not payloads. Payload is
  `span_start + page_header_bytes(kind)`.
- An H0 page's `uniform_layout` and `slot_bytes` never change while any object on
  it is live.
- A recycled page's kind is reset before it serves an allocation; the
  object-index flag is reset with it.
- `element_count` is meaningful only for H16; H0 and H8 report one.
