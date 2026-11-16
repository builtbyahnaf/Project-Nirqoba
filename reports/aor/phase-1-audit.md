# AOR Phase 1 — Object Metadata Dependency Audit

Status: in progress  
Baseline: H48 (`sizeof(ObjectHeader) == 48`)  
Audited tree: `d9941f8` plus this report  
Scope: `runtime/src/gc.cpp`, the public GC layout ABI, compiler layout emission, TLAB publication, barriers, sweeping, page recycling, debug instrumentation, arrays, and large objects.

## 1. Method

The audit walks every field of `ObjectHeader` and every consumer of it, then classifies the dependency:

| Class | Meaning |
|---|---|
| **Derive** | The value can be computed from a page, slot, or allocation table without per-object storage. |
| **Move** | The value is mutable GC state that can live in a page-local bitmap or counter. |
| **Pack** | The value must remain object-specific but can share a word with other fields. |
| **Keep** | The value is genuinely per-instance and cannot be recovered cheaply. |

Line references are to the implementation at the start of Phase 2. The audit is deliberately field-by-field: a smaller header is not useful if it turns a hot direct load into a hash probe or an allocation.

## 2. Baseline header

Current layout from `runtime/src/gc.cpp`:

| Offset | Field | Bytes | Written by | Read by |
|---:|---|---:|---|---|
| 0 | `next` / `tlab_next` union | 8 | allocation, publication, sweep | publication, unindexed sweep |
| 8 | `mark_next` | 8 | mark worklist | mark worklist overflow |
| 16 | `page` | 8 | allocation | publication, sweep, mark bitmap, page checks |
| 24 | `birth_epoch` | 8 | allocation | marker, sweep |
| 32 | `element_count` | 4 | allocation | payload size |
| 36 | `state` | 4 | allocation, sweep, barrier | young/remembered/age/mark tests |
| 40 | reserved padding | 4 | — | alignment |
| 44 | padding before `layout` | 4 | — | alignment |
| 48 | payload begins | — | — | — |

The header has two cache lines of metadata before the payload. `layout` is declared last so instance fields stay in the first line; the payload starts at 48 bytes.

## 3. Field inventory

### 3.1 `next` / `tlab_next`

- `tlab_next` links unpublished thread-local allocations in `MutatorState::pending_objects`.
- `next` links published objects on pages that cannot be walked by a start bitmap (`unindexed_objects`), and links detached objects during sweep.
- The two meanings never overlap in time because an object is either unpublished or published.
- The field is also the intrusive list link for large-object pages, which have no page start bitmap.
- It is **not** used by ordinary bitmap pages: those objects are discovered from `object_starts`.

### 3.2 `mark_next`

- The mark worklist has 256 inline entries; objects beyond that spill through `mark_next`.
- The link is written on push, read on overflow pop, and cleared on pop.
- It is only needed while an object is pending in the mark queue. It is not meaningful during allocation or sweep.

### 3.3 `page`

- Allocation constructs the header with its owning page.
- Publication uses it to choose start-bitmap publication or the unindexed list.
- Sweep uses it for page counts, span release, and object-start clearing.
- Page lookup can already derive the page from an address for ordinary 64 KiB pages; the stored pointer is a cache of that lookup.
- Large pages are not recoverable by masking and use the page directory or object index.

### 3.4 `birth_epoch`

- Set at allocation from `next_cycle_epoch`.
- The marker refuses to mark objects born during the current cycle.
- Sweep retains them and clears any mark left by a barrier.
- A page-local allocation sequence or birth bitmap could replace it for ordinary pages, but only if allocation during a cycle is represented without per-object storage.

### 3.5 `element_count`

- Fixed records store 1; arrays store their element count.
- `object_payload_bytes` multiplies it by `layout->payload_size`.
- Arrays and variable-sized payloads need either this field or a page/slot size table that can describe the object.
- A uniform page class can derive payload size for fixed-size objects; mixed pages cannot.

### 3.6 `state`

- Bit 0 was the mark bit; it has been moved to the page mark bitmap.
- Bit 1 is young, bit 2 is remembered, bits 8–15 are promotion age.
- The state word is read in the marker, barrier, sweep survivor path, and debug API.
- Marking no longer dirties the header. Young/remembered/age still do.
- The public debug API exposes the word, so any re-encoding must preserve its documented masks or update that API deliberately.

### 3.7 `layout`

- The descriptor holds payload size, reference count, reference offsets, trace callback, SoA geometry, and the inline narrow map.
- Every traced object needs a layout lookup. The pointer is the cheapest current form: one load from the header.
- Page-local uniform-layout metadata can remove it only for pages whose objects all share one descriptor.

## 4. Intrusive-link audit

### 4.1 TLAB publication

`allocate_tlab` constructs an object and pushes it on `thread_mutator.pending_objects` through `tlab_next`. `publish_tlab_locked` walks that list, inserts ordinary objects into their page start bitmap or the exact object index, then rebuilds the unindexed list through `next`.

The link is therefore a **batch cursor**, not object identity. A page can publish a contiguous allocation range without a per-object link if it records the range and the objects are visited in allocation order. The current link is cheap because it reuses the same pointer-sized slot as the published large-object list.

**Classification:** Move to page/span publication metadata in Phase 3; keep as the migration fallback until then.

### 4.2 Unindexed and sweep lists

Large-object pages and pages whose directory insertion failed are linked through `next`. The sweep freezes this list at mark completion and walks it after the bitmap pages. An object on that list has no page start bitmap to discover it.

This is the only case where `next` is required for reachability of the object set. Ordinary pages never use it after publication.

**Classification:** Keep for unindexed/large pages until the large-object directory owns their enumeration.

### 4.3 Mark overflow

`MarkWorklist::push` writes `mark_next` only when the 256-entry inline ring is full. `pop` reads and clears it. The link is not needed for objects that fit in the ring, which is the common case.

**Classification:** Replace with a preallocated/segmented external worklist in Phase 3. No object-specific storage is needed once the worklist is not intrusive.

## 5. Page ownership audit

Ordinary pages are 64 KiB aligned, so an address inside one can be mapped to its base by masking the low 16 bits. `PageDirectory` additionally hashes the page number and returns the descriptor; large pages use exact-object entries.

`header->page` is read in these places:

| Site | Purpose | Can derive? |
|---|---|---|
| `publish_tlab_locked` | choose start-bitmap vs. index publication | Yes: page is known from the TLAB cursor, but the header is the publication unit today. |
| `sweep_object` | page counts and span release | Yes for bitmap pages; large pages need the directory. |
| `header_marked`, `set_header_marked` | page mark bitmap | Yes for ordinary pages; directory for large. |
| `find_object_locked` | prove the exact header belongs to the page | Already has the page from lookup. |
| `object_payload_bytes` callers | release span and statistics | No direct need for the pointer beyond the already-known page. |

The stored pointer saves a page-directory probe on every mark and publication. Removing it before a direct page table exists would make marking depend on a hash lookup, which the roadmap explicitly rejects.

**Classification:** Derive for ordinary pages once a direct page table exists; keep a large-page fallback. Do not remove the pointer while it is the cheapest correct path.

## 6. Birth-epoch audit

Allocation stores `next_cycle_epoch`; the cycle increments it before marking. A header is "born during the cycle" when its epoch is greater than or equal to the active cycle epoch.

Uses:

- `enqueue_header_locked` refuses to mark such an object, because the sweep retains it by the same comparison. Marking it would also add a survivor count that the snapshot cannot attribute.
- The bitmap sweep clears a barrier mark on such an object and leaves it in place.
- The unindexed sweep does the same before re-linking it.
- Publication increments `born_this_cycle` on the page when the phase is not idle and the epoch matches.

The field is a 64-bit monotonic stamp. A page-local allocation epoch would work only if every object on a page shares the page's birth cycle; mixed pages and span reuse break that assumption today. A per-page birth bitmap would need one bit per slot and allocation-time marking, which is larger than the current 8 bytes for sparse pages but smaller for dense ones; it is not a safe drop-in until the page-class allocator exists.

**Classification:** Keep for mixed pages; derive for uniform page classes in Phase 3/4. Do not remove before the page-local birth representation is validated against concurrent allocation.

## 7. Element-count and array audit

`element_count` is the only per-object size multiplier. `layout->payload_size` describes one record or one array element; `object_payload_bytes` multiplies them. It is read by:

- tracing, to bound reference offsets and array strides;
- sweep, to size the released span and account reclaimed bytes;
- allocation profiling and statistics;
- the public object-layout query, which returns the descriptor but not the count.

Fixed records always store 1. Arrays may store any count that fits 32 bits. A page-class allocator can derive the payload size for a page whose objects have one fixed span, but an array page with variable counts cannot use that derivation.

The layout descriptor already distinguishes `NRQ_OBJECT_LAYOUT_ARRAY`; it does not carry a per-instance count. The count is genuinely per-instance for arrays, so it must stay in an extended representation or in a page-local array table until the compiler can prove a fixed element count.

**Classification:** Keep for arrays and mixed pages; derive for fixed-size uniform pages. Do not assign every array an extended header automatically.

## 8. Mutable-state audit

The mark bit is already off-heap. The remaining mutable state word contains young, remembered, and age. Their consumers are:

| State | Consumers | Extraction risk |
|---|---|---|
| young | allocation, minor filtering, promotion, barrier, state API | High: read on every trace and sweep survivor |
| remembered | remembered-set insertion, compaction, promotion clearing | Medium: one bit per young object; already set-like |
| age | minor promotion | Low: only the survivor path updates it |
| mark | marker, sweep, debug state API | Already page-local (Phase 2 precondition) |

The remembered bit is a duplicate of membership in `remembered_young`. Moving it to a page bitmap removes a header read-modify-write from the store barrier and makes the remembered set page-local, which is the Phase 2 direction. The young bit is hotter: moving it to a bitmap would add a page lookup to every marker visit. It stays in the header until a compact representation encodes it directly.

**Classification:** Move remembered to page-local metadata in Phase 2; keep young/age packed in the header for now; preserve the debug masks or update the API deliberately.

## 9. Layout-descriptor audit

`layout` is the last header field and the only route to precise tracing for a normal object. The descriptor is shared, immutable, and interned. Its fields are:

- `payload_size`, `reference_count`, `flags`;
- `reference_offsets` table;
- optional `trace` callback;
- optional SoA column geometry;
- the inline offsets added for narrow records in the preceding optimization.

A page-local uniform page class can replace the per-object pointer with a page descriptor pointer. A mixed page cannot: two adjacent objects can have different layouts and sizes. The current allocator deliberately mixes sizes in a page to reduce tail waste, so removing `layout` requires either uniform page classes or a per-slot descriptor index.

**Classification:** Keep for mixed pages; move to page metadata for uniform pages in Phase 4/5. The descriptor itself is already shared and must not be duplicated per object.

## 10. Consumers outside `gc.cpp`

- `runtime/include/nirqoba/runtime/gc.h` exposes header size and instance/shared byte counts as allocation facts. Tests assert 48/40/8.
- `nrq_object_layout` is a public immutable descriptor. Compiler lowering emits reference-offset arrays for root registration; those arrays are not object headers.
- `tests/unit/gc_test.cpp` asserts the state masks and header facts; these assertions must move with any representation change rather than being weakened.
- No finalizer or destructor runs for managed payloads. `destroy_object_storage` only invokes the trivial `ObjectHeader` destructor.

## 5. Remaining audit sections

The following sections are added as the audit proceeds:

1. Large-page handling and fallback.
2. Array and variable-size layouts.
3. Page recycling and metadata reset.
4. Debug instrumentation and production/debug equivalence.
5. Dependency conclusions and Phase 2 extraction order.
