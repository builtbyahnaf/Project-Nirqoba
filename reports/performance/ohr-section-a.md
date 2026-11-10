# OHR Section A — shared immutable metadata

Scope: the first part of `project_context/OHR_ROADMAP.md` Section A (descriptor
architecture, descriptor reuse, removal of per-instance structural metadata).
Every figure below is measured on this machine unless it is labelled otherwise.
Production configuration throughout (`NRQ_RUNTIME_DEBUG_ENABLED=0`).

## 1. What was shareable, and what was not

`ObjectHeader` was 96 bytes:

| field | bytes | kind |
| --- | ---: | --- |
| `next` / `tlab_next` (union) | 8 | per instance |
| `mark_next` | 8 | per instance |
| `page` | 8 | per instance |
| `size` | 8 | per layout for records, per instance for arrays |
| `element_size` | 8 | per layout |
| `trace` | 8 | per layout |
| `marked`, `young`, `remembered`, `age` | 4 | per instance |
| `birth_epoch` | 8 | per instance |
| `ReferenceLayout` (two inline offsets + owned vector + count) | 32 | per layout |

So 56 of the 96 bytes described the layout, not the instance, and a record with
three or more references additionally allocated a `std::vector` per object.
Roots repeated the same mistake: every `nrq_gc_root_register_typed` and every
frame descriptor copied the offsets into the shadow stack and again into the
per-slot index.

## 2. The descriptor

`nrq_object_layout` is immutable, interned once per distinct layout, and never
reclaimed, so its address is a stable identity:

```c
struct nrq_object_layout {
  uint32_t payload_size;              // one record, or one array element
  uint32_t reference_count;
  uint32_t flags;                     // NRQ_OBJECT_LAYOUT_ARRAY
  const uint32_t* reference_offsets;  // read-only, relative to the element
  nrq_gc_trace_fn trace;              // only when a table cannot describe it
};
```

Interning keys on payload size, element size, trace callback **and the exact
offset table**, so equal sizes with different reference order or different
tracing never merge. Layouts that cannot be described exactly (zero payload, an
offset that does not leave room for a pointer) are rejected instead of stored.

Allocation never takes the interning lock on a repeat: each thread memoises the
last descriptor it resolved. A hit is proved by comparing the incoming offset
table against the descriptor's own table, which the runtime owns and never
frees. The caller's table is never retained: the typed entry points document
that it is copied, so callers build it on the stack and its address says
nothing about its contents or its lifetime.

## 3. Header after the change

| field | bytes |
| --- | ---: |
| `next` / `tlab_next` (union) | 8 |
| `mark_next` | 8 |
| `page` | 8 |
| `birth_epoch` | 8 |
| `layout` | 8 |
| `element_count` (array length; 1 for a record) | 4 |
| `marked`, `young`, `remembered`, `age` | 4 |
| **total** | **48** |

`static_assert(sizeof(ObjectHeader) == 48)`, `alignof == max_align_t`, and the
payload address is still `header + 1`, so it stays 16-byte aligned. Payload size
is `element_count * layout->payload_size`; nothing per instance stores it.

## 4. Measured footprint

Probe: 100,000 objects of a 64-byte payload, identical workload both ways.

| | 96-byte header | 48-byte header |
| --- | ---: | ---: |
| span per object | 160 B | 112 B |
| objects per 64 KiB page | 408 | 585 |
| pages | 245 | 171 |
| page storage | 15.7 MB | 11.2 MB |

MEASURED: 30% less page storage for the same live payload, with the same live
count and payload bytes. The allocation profile API reports the same geometry
(`span_bytes` 112, `slots_per_page` 585).

Descriptor sharing, MEASURED through the new facts: 1,000 instances of one record
add at most one descriptor and at most 64 bytes of structural storage (descriptor
plus its offset table). A three-element array root with four reference words per
element adds at most one layout, not one per element.

## 5. A regression this exposed, and its fix

A page records reclaimed runs in a fixed table and `release_span_locked`
**silently dropped** a run when the table was full (96 entries). With the denser
header, 64-byte payloads pack a page exactly (512 × 128 B), and the existing
"hold the live set steady across rounds" test frees two of every three objects,
which needs ~171 runs per page. The dropped runs turned reclaimed space into dead
space and the heap grew a page per round.

MEASURED with a four-round probe, before and after making each run entry two
32-bit page-relative fields (192 runs in the same 1,536 bytes): pages per round
2, 3, 3, 3 → 2, 2, 2, 2; a 200-byte payload is unchanged at 7 pages.

## 6. Array allocation

MEASURED, 300,000 arrays of four 24-byte elements, medians of three runs, runtime
built both ways: 19.5 ms before, 18.4 ms after removing a 64-bit division from the
header constructor (the caller already knew the element count). The ranges
overlap (18.75–19.81 vs 18.22–18.62), so this is a small consistent improvement,
not a large one.

## 7. Limits introduced, deliberately

- A payload beyond 4 GiB is rejected (`nrq_object_layout.payload_size` is
  32-bit), and an array with more than 2^32 elements is rejected rather than
  truncated into `element_count`. Both return null from the allocator; neither
  silently reinterprets a payload.
- A page beyond 4 GiB can no longer record runs (`FreeSpan` offsets are 32-bit);
  such a page behaves like the bump allocator.

## 8. What Section A still needs

1. **Compiler-emitted descriptors.** The compiler already emits one read-only
   offset table per element type in `.rdata`; it does not yet emit the 32-byte
   `nrq_object_layout` around it, so `nrq_gc_allocate_array_layout_rooted` is
   available but unused by generated code. This needs a constant initializer with
   a relocation (``llvm.mlir.addressof`` inserted into an `llvm.mlir` struct
   global) and was not attempted rather than half-done.
2. **Alignment requirement** in the descriptor (A2 lists it; nothing needs it
   yet because every payload is `max_align_t` aligned).
3. Section B (compact per-instance metadata) and Section C (the H48/H40/H32/H24
   variants). The 48-byte result above is a complete representation, not a
   packed one: no field was sacrificed to reach it.

## 9. Verification

- 207 unit tests, 263 integration tests, all passing after the final change.
- New tests: `OneLayoutServesEveryCompatibleInstance`,
  `InstancesOfOneRecordShareOneDescriptor`, `AStaticDescriptorIsUsedAsGiven`,
  `AWideArrayRootUsesOneSharedElementLayout`, `StructuralMetadataIsSharedNotRepeated`,
  `AllocationProfileCountsTheCompactHeader`, and the updated
  `ObjectHeaderStaysWithinItsMetadataBudget` (48 bytes).
