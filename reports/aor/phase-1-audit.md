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

## 4. Consumers outside `gc.cpp`

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
