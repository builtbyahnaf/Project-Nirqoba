# AOR Phase 4 — Adaptive Representation Dependency Audit

Status: complete
Baseline: H32 (`runtime/src/gc.cpp`, `sizeof(ObjectHeader) == 32`)
Scope: every H32 consumer, page metadata, allocation arithmetic, compiler coupling,
debug instrumentation, and the representation decision this phase will implement.

This report is the evidence for the Phase 4 implementation. It replaces the
illustrative layouts in the roadmap where the repository's actual constraints
(16-byte payload alignment, the start-bitmap grid, and the SoA count load) rule
them out.

## 1. Method

Every `ObjectHeader` occurrence in `runtime/src/gc.cpp` was classified by what it
needs from the header:

| Need | Source today | Cost if read per access |
|---|---|---|
| object identity | start bitmap bit | one bitmap test (already paid) |
| payload address | `header + 1` | arithmetic |
| payload size | `element_count * layout->payload_size` | one dependent load |
| element count | header word | one load |
| layout | header pointer | one dependent load |
| young / age | header state word | one load |
| remembered | page bitmap | already off-heap |
| marked | page bitmap | already off-heap |
| birth cycle | header `birth_epoch` | one load |

The audit's conclusion is that only **layout** and **element count** are genuinely
per-instance for arbitrary objects. Everything else can be page-local without a
lookup that is more expensive than the current load, which is what makes H0
possible for the objects that never vary.

## 2. H32 field inventory and destination

| Field | Bytes | Destination in Phase 4 |
|---|---:|---|
| `next` | 8 | 32-bit page-relative link, only on extended large/fallback pages |
| `birth_epoch` | 8 | page-local born bitmap + lazy cycle stamp |
| `element_count` | 4 | H16 only; implicit 1 for H8/H0 |
| `state` | 4 | H8/H16 header word; page-local byte array for H0 |
| `layout` | 8 | H16: 32-bit id; H8: 32-bit id; H0: page `uniform_layout` |

Moving birth out of the object costs one bitmap bit per 16-byte slot (512 bytes
per 64 KiB page, 0.125 bytes per maximum-density object) and removes 8 bytes from
every object. It is the single largest saving in the phase and it is what makes a
16-byte extended header possible at all.

## 3. The two hard constraints

### 3.1 Payload alignment

`kObjectStartAlignment` is `alignof(std::max_align_t)` (16 on the x64 target), the
object-start bitmap grid is 16 bytes, and the current public contract reports a
16-byte payload alignment (`nrq_gc_allocation_facts::payload_alignment`). The unit
suite asserts it for a plain allocation:

```
ASSERT_EQ(reinterpret_cast<uintptr_t>(object) % alignof(std::max_align_t), 0);
```

A leading 8-byte header therefore cannot put the payload at `header + 8` while
keeping 16-byte alignment. Two candidate layouts were rejected:

- **pad the slot so the payload stays 16-aligned** — the slot costs
  `align16(16 + payload)`, identical to H16, so the "8-byte header" saves no
  memory at all;
- **trailing 8-byte header** — the payload stays aligned but, because the header
  follows the payload, it only saves bytes when `payload % 16 == 8`.

The implementation therefore defines the alignment contract per representation:

| Representation | Payload alignment | Reason |
|---|---|---|
| H0 | 16 | the object *is* the payload; it sits on the slot grid |
| H8 | 8 | header is 8 bytes, payload follows at `header + 8` |
| H16 | 16 | header is 16 bytes |

The language has no managed type whose alignment exceeds 8 (no `alignas(16)`,
SIMD, or `long double` payload exists in the compiler or runtime), so an 8-byte
payload alignment is sufficient for every type the compiler emits today. A future
type that needs more must request the extended representation.

### 3.2 The object-start grid records header starts, not payload starts

The bitmap bit currently identifies the payload offset divided by 16. H8's payload
is at an 8-byte offset, which has no bit. The bit is redefined to identify the
**span start** (the header's address), which is always 16-aligned because every
span is aligned. The payload is then `span_start + header_bytes(page kind)`.

This keeps the bitmap at 1024 bytes per 64 KiB page instead of doubling it to
2048 for an 8-byte grid, and it makes discovery a single subtraction:

```
header = storage + span_offset
payload = header + header_bytes(kind)
```

`find_object_locked` validates `object == payload` exactly, so interior and stale
pointers still miss.

## 4. Compiler coupling

`compiler/lib/Lowering/TirToMlir.cpp` reads the element count for a SoA container
at `payload - 16` and documents it as "offset 32 of the header, payload at 48".

Phase 4 places `element_count` at offset 0 of the 16-byte extended header, so the
payload is at 16 and the count is still at `payload - 16`. The lowering is
therefore unchanged; only its comment becomes wrong and must be updated. This is
why the extended header keeps the count first rather than packing it after state.

SoA containers always have the array flag set, and Phase 4 assigns every array to
H16, so the `payload - 16` read can never land in an H8 or H0 object.

No other compiler or runtime translation unit names `ObjectHeader`. The public
GC header only exposes facts, layout descriptors, and state masks.

## 5. Page metadata available for extraction

`GcPage` already carries `kind`, `slot_bytes`, `slot_capacity`, `uniform_layout`,
per-page live/young/born/marked counts, and start/mark/remembered bitmaps from
Phases 2 and 3. Phase 4 activates `Uniform` as the headerless page class, renames
the kinds to name representations directly, and adds:

- a born bitmap with a lazy cycle stamp (replaces `birth_epoch`);
- a per-slot state byte for headerless pages (young and age, since there is no
  object word to hold them);
- zeroed `uniform_layout`/`slot_bytes` on reuse so a recycled page cannot inherit
  another representation's geometry.

The mark and remembered bitmaps are already indexed by slot and need no change.

## 6. Representation selection rules

The allocator selects by shape and size, statically, at allocation:

| Condition | Representation |
|---|---|
| array flag set, or `elements > 1` | H16 extended |
| fixed record, `payload <= 256` | H0 headerless on a uniform page |
| fixed record, `payload > 256` | H8 compact |

A caller may override the decision through a representation hint (used by tests
and available to Phase 5's compiler hints). Invalid combinations — an array
requested as H0, an element count other than one for a non-extended object — are
rejected rather than coerced.

H0 pages are per-layout and per-slot-size. An H0 allocation takes a uniform page
whose `uniform_layout` and `slot_bytes` match exactly; a page switch or layout
switch starts a new reservation. Publication walks a uniform range by the constant
slot size, so it needs no header at all.

## 7. Consumers that must be rewritten

| Site | Change |
|---|---|
| `publish_tlab_locked` | span from page kind; mark the span start, not the payload |
| `allocate_locked`, `allocate_tlab` | three representation paths and page-kind-aware refill |
| `find_object_locked`, `page_for_header` | return a `{span start, page}` reference |
| `trace_children`, narrow path | layout from header id or page |
| `enqueue_header_locked`, worklist | carry the page with the object |
| `sweep_object`, `sweep_chunk_locked` | decode by page kind; 32-bit large-page chain |
| remembered set | `ObjectRef` entries instead of header pointers |
| `find_pending_header` | decode spans by page kind |
| `nrq_gc_object_layout`, state bits | decode through the representation |
| allocation facts/profile | report the maximum header and per-representation cost |

## 8. Non-goals

- Compiler-emitted representation hints are Phase 5. Phase 4 exposes the runtime
  hint and the validation it needs.
- Pointer compression is not introduced; references stay native width.
- H48/H32 fallback is not retained: the completion criterion forbids any standard
  object above 16 bytes. `nrq_gc_object_header_bytes` reports 16 as the maximum.
