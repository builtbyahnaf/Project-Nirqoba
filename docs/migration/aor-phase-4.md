# AOR Phase 4 migration notes

**Status:** complete  
**Object representations:** H0 headerless, H8 compact, H16 extended  
**Maximum header:** 16 bytes  
**Compatibility:** no source-language or ownership change

## What changed

- `ObjectHeader` as a single 32-byte instance record is gone. A page names its
  representation and every object's header size follows from it.
- H0 objects have no header at all. Their layout comes from the page, their state
  from a one-byte-per-object page table, and their element count is implicitly
  one.
- H8 objects carry `{state, layout_id}`.
- H16 objects carry `{element_count, state, layout_id, flags}`.
- `birth_epoch` was removed from every object and replaced by a page-local born
  bitmap plus a lazy `born_epoch` stamp.
- The layout pointer was removed from every object, replaced by a 32-bit layout
  id table (`kMaxLayoutIds = 1 << 14`) with a thread-local 8-entry memo.
- The object-start bitmap now indexes span starts (16- or 8-byte aligned header
  addresses), not payloads. Payload is `span_start + page_header_bytes(kind)`.
- Large and fallback objects moved from intrusive `next` links to a
  `std::vector<ObjectRef>` sweep list.
- `kMaxFreeSpans` rose from 192 to 384 because a headerless page packs twice as
  many objects per page.

## Public API additions

```c
typedef enum nrq_gc_representation {
  NRQ_GC_REPR_AUTO = 0,
  NRQ_GC_REPR_HEADERLESS = 1,
  NRQ_GC_REPR_COMPACT = 2,
  NRQ_GC_REPR_EXTENDED = 3
} nrq_gc_representation;

void* nrq_gc_allocate_repr(size_t bytes, nrq_gc_trace_fn trace, int32_t repr);
int32_t nrq_gc_object_representation(void* object);
size_t nrq_gc_object_header_size(void* object);
struct nrq_gc_representation_facts nrq_gc_get_representation_facts(void);
struct nrq_gc_representation_stats nrq_gc_get_representation_stats(void);
```

`nrq_gc_allocate_repr` rejects an impossible request (`nullptr`) instead of
coercing it. `nrq_gc_allocate` and every array entry point keep their old
behaviour and choose automatically.

## What did not change

- Managed payloads still begin at the end of the header and remain
  `max_align_t` aligned for H0/H16; H8 payloads are 8-byte aligned.
- `element_count` remains at `payload - 16` for arrays, so the compiler and SoA
  lowering needed no arithmetic change. Arrays are always extended.
- State masks, young/age encoding, root registration, store barriers, SATB, and
  deterministic ownership cleanup are unchanged.
- TLAB batching, range publication, cycle-boundary flushes, whole-page
  retirement, and free-run reuse are unchanged in behaviour.
- Large objects still use the exact object index and the unindexed sweep list.
- Page bitmap cost remains 4096 bytes per ordinary 64 KiB page on an 8-byte
  `max_align_t` target; a headerless page adds 1 byte per object.

## Behaviour to be aware of

- A one-element container is extended, not headerless. The shape is stated by the
  allocation entry point because `count * element_size == element_size` cannot
  distinguish it from a record.
- A thread owns one headerless slab. A second small shape is served as H8 while
  that slab has room, instead of evicting it. This is a deliberate
  anti-thrashing rule, not an eligibility change.
- Free runs are representation-local: a compact page's run never becomes a
  headerless object.
- A headerless page is only usable when its layout and slot size match the
  request exactly.

## Migration order

Phase 5 can now make the same representation decision in the compiler: the
selection is a pure function of `payload_size`, the array flag, and
`kHeaderlessMaxPayload`, and no runtime state affects it. The H16 `flags` word is
reserved for that work.

## Verification

- `360` unit tests pass.
- `624` integration cases pass.
- Phase 7 isolation passes.
- Phase 9 optimization compatibility passes (O0/O2 × production/debug).
- `*gc*` configuration equivalence: 67 agreed, 0 skipped, 0 failed.
