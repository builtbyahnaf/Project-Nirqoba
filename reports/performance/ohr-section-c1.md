# OHR Section C1 — the header, recalculated

Scope: the real geometry of the current header and its cost, measured from the
runtime that ships rather than reused from the historical H96 study. This is
C1's "recalculate" step. C2 (four candidate variants) has not been started.

## 1. Geometry

Reproduced by `benchmarks/bottleneck_discovery/header_probe/current_header.cpp`,
which links the runtime and checks its own `sizeof` against
`nrq_gc_get_allocation_facts().object_header_bytes`, so the reproduction cannot
silently drift from the struct.

| field | offset | size | bytes |
| --- | ---: | ---: | --- |
| `next` / `tlab_next` (union) | 0 | 8 | 0–7 |
| `mark_next` | 8 | 8 | 8–15 |
| `page` | 16 | 8 | 16–23 |
| `birth_epoch` | 24 | 8 | 24–31 |
| `element_count` | 32 | 4 | 32–35 |
| `state` | 36 | 4 | 36–39 |
| `layout` | 40 | 8 | 40–47 |

MEASURED: `sizeof(ObjectHeader)` = 48, `alignof` = 8 on this target,
`offsetof(layout)` = 40, internal padding 0, trailing padding 0. Payload starts at
offset 48, and the facts report 40 instance bytes + 8 shared bytes, which is the
same 48 as 40 + 8.

`alignof(ObjectHeader)` is 8 because no member needs more, and the object start
spacing is `alignof(std::max_align_t)`, which MSVC reports as 8 and libstdc++
reports as 16. That difference matters for every candidate smaller than 48: a
40-byte header is aligned on Windows and misaligned on Linux. The runtime now
asserts `sizeof(ObjectHeader) % alignof(std::max_align_t) == 0`, so a variant
cannot be adopted on one target by accident.

## 2. What 48 bytes costs per page

MEASURED through `nrq_gc_get_allocation_profile` (page 65,536 bytes, TLAB 65,536,
smallest reusable run 8,192):

| payload | span | objects/page | page slack | MB per 100k objects |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 56 | 1170 | 16 | 5.34 |
| 16 | 64 | 1024 | 0 | 6.10 |
| 24 | 72 | 910 | 16 | 6.87 |
| 32 | 80 | 819 | 16 | 7.63 |
| 48 | 96 | 682 | 64 | 9.16 |
| 64 | 112 | 585 | 16 | 10.68 |
| 96 | 144 | 455 | 16 | 13.73 |
| 128 | 176 | 372 | 64 | 16.78 |
| 256 | 304 | 215 | 176 | 28.99 |
| 512 | 560 | 117 | 16 | 53.41 |

The header is 48 of the 56 bytes charged for an 8-byte payload: 86% of the span.
That is the whole argument for Section C, stated in one number.

Bitmap density: one start bit per 8-byte slot, so an ordinary page carries
65,536 / 64 = 1,024 bitmap bytes, 1.6% of the page, whether its objects are 8 bytes
or 512.

## 3. Field by field: what genuinely remains

| field | can it go? | finding |
| --- | --- | --- |
| `next` / `tlab_next` | not yet | `tlab_next` chains unpublished TLAB batches, `next` chains published objects and the sweep list. Both are live during marking, so neither can borrow the other's slot. |
| `mark_next` | not yet | it exists only for the mark worklist's overflow. Removing it needs the overflow to live somewhere else, and the alternatives are worse than 8 bytes: an auxiliary link array is 8 bytes per object anyway, and a growable buffer allocates on the marking path, which this design forbids because marking must not fail or allocate while mutators are parked. |
| `page` | yes, for ordinary pages | a page is 64 KiB and starts on a 64 KiB boundary, so `address & ~(64 KiB - 1)` is its base. Pages larger than 64 KiB (large-object pages) break the mask and would need the existing chunk map as a fallback. This is the only field of the four that is genuinely derivable. |
| `birth_epoch` | no, not safely | narrowing it to 32 bits saves nothing at 48 bytes (44 rounds to 48 on Windows and to 48 on Linux too), and it would introduce wrap behaviour for objects that outlive 2^32 collections. Nothing to gain, something to lose. |
| `element_count` | no | an array's length is per instance and cannot come from the descriptor. It is already the narrowest type that holds a legal array. |
| `state` | no | it is the encoding measured in the Section B report, and it is the only mutable per-instance field left. |
| `layout` | no | it is the 8 shared bytes, and replacing it with a 32-bit id would add a lookup to every trace. Unmeasured trade, deliberately not taken here. |

The honest conclusion is that the 8–12 byte instance core the roadmap targets is
**not reachable** on this design without changing something larger than the header:
the four link-like fields are 32 of the 40 instance bytes, and each of them exists
because some path needs an intrusive chain or a stable page identity. Section C2
should therefore build variants around what is actually available - a 40-byte
header that derives `page`, and a 32-byte header that additionally spends the
saved space on an owned link table - rather than around the target number.

## 4. Verification

- 210 unit, 263 integration, all passing at the time of writing.
- `TheHeaderSplitsIntoInstanceAndSharedBytes` pins 48 / 40 / 8.
- `static_assert(sizeof(ObjectHeader) % alignof(std::max_align_t) == 0)` pins the
  alignment rule the variants must respect.
