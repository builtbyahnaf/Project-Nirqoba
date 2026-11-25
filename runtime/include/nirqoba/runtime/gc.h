// Precise, non-moving tracing collector interface.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nrq_gc_visit_fn)(void* referenced_object, void* visitor_context);
typedef void (*nrq_gc_trace_fn)(void* object, size_t object_bytes,
                                nrq_gc_visit_fn visit, void* visitor_context);

// One immutable description of an object layout, shared by every instance that
// has the same structural requirements. A header holds a pointer to its layout
// instead of a copy of the payload size, element size, trace callback and
// reference map, so 10,000 instances of one record carry one description.
//
// A published descriptor is read-only and lives for the life of the process, so
// its address is a stable identity and a live object can always read it. Only
// layout creation takes a lock; tracing and allocation read through the pointer
// without one.
enum {
  // The payload repeats `payload_size` byte elements and the element count is
  // recorded per instance. Without this bit the payload is a single record.
  NRQ_OBJECT_LAYOUT_ARRAY = 1u << 0,
  // A record with at most two references carries those offsets in the
  // descriptor itself. The marker's narrow-record path reads them without a
  // second dependent load through `reference_offsets`. Producers other than
  // nrq_gc_layout_intern leave this clear and keep using the table.
  NRQ_OBJECT_LAYOUT_INLINE_MAP = 1u << 1
};

struct nrq_object_layout {
  // Bytes of one record, or of one array element when the array bit is set.
  uint32_t payload_size;
  uint32_t reference_count;
  uint32_t flags;
  // Offsets of the payload words that hold managed references, relative to the
  // payload for a record and to the start of one element for an array. They are
  // uint32 so a descriptor stays compact; a payload larger than 4 GiB is
  // described by `trace` instead.
  const uint32_t* reference_offsets;
  // Visits references this table cannot describe. Null when the table is
  // complete, which is the usual case and keeps tracing table-driven.
  nrq_gc_trace_fn trace;
  // Strided reference columns of a structure-of-arrays payload: (offset, stride)
  // byte pairs. A column is contiguous per column rather than interleaved per row,
  // so the flat offset table cannot describe it. Null when there are none.
  const uint32_t* soa_columns;
  uint32_t soa_column_pairs;
  // Inline map for a hot record layout (`NRQ_OBJECT_LAYOUT_INLINE_MAP`). The
  // count is `reference_count`; entries beyond it are zero. This is part of the
  // descriptor's immutable data, so it costs no per-object storage.
  uint32_t inline_reference_offsets[2];
};

// Returns the descriptor for a layout, creating it the first time it is seen.
// `element_bytes` is zero for a fixed record and the element size for an array.
// Returns null when the layout cannot be described exactly, which is the case
// for a zero-size payload, an offset outside the record or element, or an
// offset that does not leave room for a pointer.
const struct nrq_object_layout* nrq_gc_layout_intern(
    size_t payload_bytes, size_t element_bytes, nrq_gc_trace_fn trace,
    const size_t* reference_offsets, size_t reference_count);
// Allocates a structure-of-arrays payload whose managed references live in strided
// columns. `column_pairs` holds (offset, stride) byte pairs; the descriptor is
// interned by that geometry, so every instance of one layout shares a description.
void* nrq_gc_allocate_soa_rooted(void** root_slot, size_t count,
                                 size_t element_bytes,
                                 const size_t* column_pairs, size_t pair_count);
// Allocation through a descriptor the caller owns. No copy is made and no
// interning lookup runs: the pointer the object stores is the one passed here.
// The descriptor must be immutable and must outlive every object allocated with
// it, which is what makes compiler-emitted read-only descriptors the preferred
// source. An array entry point requires the array flag and a record entry point
// rejects it. The development configuration validates the descriptor; the
// production one trusts it.
void* nrq_gc_allocate_layout(const struct nrq_object_layout* layout);
void* nrq_gc_allocate_array_layout(const struct nrq_object_layout* layout,
                                   size_t count);
void* nrq_gc_allocate_array_layout_rooted(void** root_slot,
                                          const struct nrq_object_layout* layout,
                                          size_t count);
// Number of distinct layouts described so far. Instances sharing a layout do
// not increase this, which is the direct measure of descriptor sharing.
size_t nrq_gc_layout_count(void);
// Per-thread hit and miss counts for the typed-allocation layout memo. They are
// diagnostics for tests and benchmarks; the memo itself stores no extra state.
uint64_t nrq_gc_layout_memo_hits(void);
uint64_t nrq_gc_layout_memo_misses(void);
// Bytes those descriptors and their offset tables occupy. They are never freed.
size_t nrq_gc_layout_bytes(void);

struct nrq_gc_stats {
  uint64_t allocated_bytes;
  uint64_t collected_bytes;
  uint64_t collections;
  uint64_t live_objects;
};

// Adaptive object representations. Every managed object belongs to exactly one:
//
//   headerless  no per-object header; layout, size and state come from the page
//   compact     8-byte header (state + layout id); one element
//   extended    16-byte header (element count, state, layout id, flags)
//
// The allocator chooses by shape: arrays are extended, a fixed record up to 256
// bytes is headerless, and a larger fixed record is compact. A caller may state a
// requirement with nrq_gc_allocate_repr; an impossible combination (an array as
// headerless, for example) is rejected rather than silently coerced.
typedef enum nrq_gc_representation {
  NRQ_GC_REPR_AUTO = 0,
  NRQ_GC_REPR_HEADERLESS = 1,
  NRQ_GC_REPR_COMPACT = 2,
  NRQ_GC_REPR_EXTENDED = 3
} nrq_gc_representation;

// Allocates with a representation requirement. `representation` is one of
// nrq_gc_representation; AUTO behaves exactly like nrq_gc_allocate.
void* nrq_gc_allocate_repr(size_t bytes, nrq_gc_trace_fn trace,
                           int32_t representation);
// Reports the representation of a live object, or NRQ_GC_REPR_AUTO when the
// address is not a live managed object.
int32_t nrq_gc_object_representation(void* object);
// Reports the bytes that object actually spends on its header: 0, 8 or 16.
// Returns 0 for an address that is not a live managed object.
size_t nrq_gc_object_header_size(void* object);

// The fixed per-representation costs, for auditing and tests. `payload_alignment`
// is the alignment the representation's payload keeps: a headerless or extended
// object starts its payload on the span grid, a compact one eight bytes after it.
struct nrq_gc_representation_facts {
  size_t headerless_header_bytes;
  size_t compact_header_bytes;
  size_t extended_header_bytes;
  size_t max_header_bytes;
  size_t headerless_payload_alignment;
  size_t compact_payload_alignment;
  size_t extended_payload_alignment;
  // Page metadata a headerless page adds beyond the shared bitmaps: one state
  // byte per object (young + promotion age). Indexed by object ordinal, not by
  // the 8-byte grid, so it does not grow with the payload size.
  size_t headerless_state_bytes_per_object;
};
struct nrq_gc_representation_facts nrq_gc_get_representation_facts(void);

// Live counts by representation, page counts by page class, and the header bytes
// the two smaller representations save against an extended header for the same
// live set. This is the measurement the phase report quotes; it is computed from
// the page set, not cached.
struct nrq_gc_representation_stats {
  uint64_t headerless_objects;
  uint64_t compact_objects;
  uint64_t extended_objects;
  size_t headerless_pages;
  size_t compact_pages;
  size_t extended_pages;
  size_t large_pages;
  size_t fallback_pages;
  uint64_t header_bytes_saved_vs_extended;
  // Page metadata actually allocated for each representation: the off-heap
  // bitmaps plus, for a headerless page, the per-object state table. Large and
  // fallback pages are counted with the extended representation, because their
  // objects use the extended header.
  size_t headerless_metadata_bytes;
  size_t compact_metadata_bytes;
  size_t extended_metadata_bytes;
};
struct nrq_gc_representation_stats nrq_gc_get_representation_stats(void);

// Allocates a zero-initialized managed object. The trace callback visits only
// fields which contain managed references; scalar fields must not be visited.
// A null callback describes an object with no managed references.
void* nrq_gc_allocate(size_t bytes, nrq_gc_trace_fn trace);
// Records which payload words hold a reference count. Marking walks managed
// references, so a count inside a collected object is not a traced edge: the
// object is the count's last owner, and the sweep drops it. Offsets are relative
// to the payload and must describe a pointer-sized word. Registering the same
// object twice replaces its table.
void nrq_gc_register_shared_fields(void* object, const size_t* offsets,
                                   size_t count);
// Number of live registrations. A registration is dropped when the object is
// reclaimed, so this is the direct measure of objects still awaiting a sweep.
size_t nrq_gc_shared_field_registrations(void);
void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace);
// Allocates a contiguous array whose elements contain managed references at
// the supplied byte offsets. The collector copies the offsets into metadata.
void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count);
// Allocates and publishes the result into an already registered pointer root
// in one collector critical section. The root slot must be registered with
// nrq_gc_root_register before calling this function.
void* nrq_gc_allocate_array_typed_rooted(
    void** root_slot, size_t count, size_t element_size,
    const size_t* reference_offsets, size_t reference_count);
// Compiler-directed record allocation: one fixed payload, plan-flags giving H0,
// H8 or H16, and a per-site cache word. The cache is validated against the shape
// on every call, exactly like the array form.
void* nrq_gc_allocate_planned(void** root_slot, size_t bytes,
                              const size_t* reference_offsets,
                              size_t reference_count, void** site_cache,
                              uint32_t plan_flags);
// Compiler-directed array allocation. `plan_flags` is the static eligibility the
// compiler computed (see alloc_hint.h). `site_cache` is a compiler-emitted,
// zero-initialized word: the runtime stores the resolved descriptor there on the
// first allocation and reuses it afterwards, so a site pays descriptor lookup
// once instead of once per object. The runtime validates both the plan and the
// cached descriptor and falls back to its own decision when either does not hold.
void* nrq_gc_allocate_array_planned(void** root_slot, size_t count,
                                   size_t element_size,
                                   const size_t* reference_offsets,
                                   size_t reference_count, void** site_cache,
                                   uint32_t plan_flags);
// The same contract for a structure-of-arrays payload: `column_pairs` is the
// (offset, stride) table and `site_cache` caches that column layout per site.
void* nrq_gc_allocate_soa_planned(void** root_slot, size_t count,
                                  size_t element_size,
                                  const size_t* column_pairs,
                                  size_t pair_count, void** site_cache,
                                  uint32_t plan_flags);

// Root slots are addresses containing managed object pointers. Register the
// slot before a collection can run and unregister it before its storage ends.
// Root slot updates shared with incremental/concurrent collection must use
// root_store (or root_copy for aggregates); those functions are write barriers.
// Managed object fields written by native code must use the same barriers.
void nrq_gc_root_register(void** slot);
void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count);
// Registers every ordinary root of one activation in a single call. `slots`
// holds the activation's slot addresses; `descriptor` is the read-only frame
// descriptor generated for that function, laid out as
//
//   descriptor[0]                  number of slots (must equal slot_count)
//   descriptor[1]                  total word count, including the two header
//                                  words, so a truncated table is detectable
//   descriptor[2 + 2i]             object size of slot i
//   descriptor[3 + 2i]             reference count of slot i
//   ...                            slot i's reference offsets, in order
//
// The whole table is validated before any root is published, and the slots are
// pushed under one barrier region instead of one per root.
void nrq_gc_root_register_frame(void* const* slots, size_t slot_count,
                                const size_t* descriptor);
// Same registration for a descriptor the compiler emitted. The lowering only
// calls this with a constant private table it built itself from the slots it is
// publishing, so the table cannot disagree with the frame; the development
// configuration validates it anyway, and the production configuration does not
// walk it. A caller that is not the compiler must use the dynamic entry point
// above, which validates every table it is given.
void nrq_gc_root_register_frame_static(void* const* slots, size_t slot_count,
                                       const size_t* descriptor);
// Opens a root frame and publishes that frame's static roots in one call. Only
// for compiler-generated code: `descriptor` must be the immutable table emitted
// for exactly the `slots` array passed here, exactly like the static entry point
// above. Returns the frame marker `nrq_gc_root_frame_end` expects. A caller that
// supplies its own descriptor must use `nrq_gc_root_register_frame`.
uint64_t nrq_gc_root_frame_open(void* const* slots, size_t slot_count,
                                const size_t* descriptor);
// Registers an externally owned contiguous array as a root region. Offsets are
// relative to each element and are repeated count times with element_size as
// the stride. The caller must unregister the data address before freeing it.
// `nrq_gc_root_clear_array_references` is for freshly allocated, uninitialized
// storage: it zeros only the described pointer slots before root registration.
void nrq_gc_root_clear_array_references(void* data, size_t count,
                                       size_t element_size,
                                       const size_t* reference_offsets,
                                       size_t reference_count);
void nrq_gc_root_register_array(void* data, size_t count, size_t element_size,
                                const size_t* reference_offsets,
                                size_t reference_count);
// Ordinary root slots must be unregistered by a thread that registered them.
// Repeated array roots are external storage and may be unregistered by owner.
void nrq_gc_root_unregister(void** slot);
void nrq_gc_root_store(void** slot, void* value);
// Publishes a pinned managed result into a root slot or field and releases the
// pin in one call. Publishing happens first, so the value is rooted before its
// pin is released. What this saves is one call boundary (and its argument
// setup): both operations it performs still open their own fast-barrier region,
// so this entry does not merge barrier regions.
void nrq_gc_root_store_release(void** slot, void* value);
void nrq_gc_root_copy(void* destination, const void* source, size_t bytes);
uint64_t nrq_gc_root_frame_begin(void);
// Root frames are thread-local and must be ended in strict LIFO order.
void nrq_gc_root_frame_end(uint64_t marker);

void nrq_gc_enable(void);
void nrq_gc_disable(void);
// A full collection reclaims both generations. Marking is incremental and
// interleaved with mutator execution; sweeping proceeds slice by slice while
// mutators continue executing ordinary code. Mutators are parked only briefly at
// cycle boundaries to publish thread-local allocations and prepare roots.
void nrq_gc_collect(void);
// A young-generation collection reclaims unreachable young objects and
// promotes survivors after a few cycles. Unreachable old objects are retained
// until a full collection. Root-store/root-copy barriers record old-to-young
// references for the next minor cycle. When generational collection is disabled
// this performs a full collection.
void nrq_gc_collect_minor(void);

// Generational collection is enabled by default. Disabling it makes automatic
// collections full collections; explicit minor collections remain available.
void nrq_gc_set_generational(int32_t enabled);
int32_t nrq_gc_is_generational(void);
uint64_t nrq_gc_get_minor_collection_count(void);
uint64_t nrq_gc_get_major_collection_count(void);

// Generated mutators register each function activation and poll at function
// entry and loop backedges. Native code that accesses managed references over
// long-running work may use the same enter/safepoint/leave protocol.
void nrq_gc_mutator_enter(void);
void nrq_gc_mutator_leave(void);
void nrq_gc_safepoint(void);
// Sets an explicit automatic-collection trigger in bytes and stops the collector
// from adapting the trigger to the heap.
void nrq_gc_set_threshold(size_t bytes);
// Reads the current automatic-collection trigger. After each cycle the collector
// sets it to the live heap plus headroom, unless a program pinned it with
// nrq_gc_set_threshold.
size_t nrq_gc_get_threshold(void);
// Enables (or freezes) adaptive trigger selection. Freezing keeps the current
// value; enabling recomputes it from the live heap immediately.
void nrq_gc_set_adaptive(int32_t enabled);
int32_t nrq_gc_policy_is_adaptive(void);
// Diagnostics: how many times the trigger was recomputed, and how many bytes the
// last completed cycle reclaimed.
uint64_t nrq_gc_policy_adaptation_count(void);
uint64_t nrq_gc_last_reclaimed_bytes(void);
struct nrq_gc_stats nrq_gc_get_stats(void);
// Reports the per-object metadata bytes the collector charges for every managed
// allocation. Exposed so the layout can be audited by tests and tooling.
size_t nrq_gc_object_header_bytes(void);
// AO-07 handover: the allocator facts the header and reference-layout work needs
// to reason about a new layout before changing it. These are constants of the
// current implementation, not measurements - the profile below turns them into
// the footprint of one payload size.
struct nrq_gc_allocation_facts {
  // Metadata charged per object, and the alignment every payload pointer keeps.
  size_t object_header_bytes;
  // How that header splits: the bytes that describe this one instance, and the
  // bytes that only name metadata shared with every other instance of the same
  // layout. They sum to `object_header_bytes`, so growth in either half is
  // visible without reading the header definition.
  size_t object_instance_bytes;
  size_t object_shared_bytes;
  size_t payload_alignment;
  // The ordinary page, which is also the largest thread-local slab.
  size_t page_bytes;
  size_t tlab_bytes;
  // The smallest reclaimed run that is worth handing to a thread.
  size_t tlab_min_slab_bytes;
  // Runs one reservation holds without touching the lock again.
  size_t spare_run_capacity;
  // Structural metadata that is shared rather than repeated: how many immutable
  // layout descriptors exist, and the bytes they and their offset tables hold.
  // Instances sharing a layout do not increase either, so these two numbers next
  // to the live-object count are the direct measure of descriptor sharing.
  size_t layout_descriptors;
  size_t layout_bytes;
};
struct nrq_gc_allocation_facts nrq_gc_get_allocation_facts(void);
// Bytes of page-local metadata an ordinary page carries. The start, mark, and
// remembered bitmaps each cover every max_align_t slot in the page; the total
// is the per-page cost that AOR extraction trades against per-object bytes.
struct nrq_gc_page_metadata_facts {
  size_t page_bytes;
  size_t object_start_bitmap_bytes;
  size_t mark_bitmap_bytes;
  size_t remembered_bitmap_bytes;
  size_t total_bitmap_bytes;
};
struct nrq_gc_page_metadata_facts nrq_gc_get_page_metadata_facts(void);
// What one payload size costs in space: the aligned span the allocator stores,
// how many of them an ordinary page holds, the space left at the end of that
// page, and how much a full slab reservation serves before a refill. A payload
// whose span exceeds a page is charged a page of its own. Returns 0 for a
// payload size that cannot be represented.
struct nrq_gc_allocation_profile {
  size_t payload_bytes;
  size_t span_bytes;
  size_t slots_per_page;
  size_t page_slack_bytes;
  size_t objects_per_refill;
  size_t page_bytes;
};
int32_t nrq_gc_get_allocation_profile(size_t payload_bytes,
                                  struct nrq_gc_allocation_profile* out);
// Page-pool diagnostics. `nrq_gc_page_count` counts active bump pages,
// `nrq_gc_recycled_page_count` counts empty pages retained for reuse, and
// `nrq_gc_page_reuse_count` counts how often a retained page was handed back
// out instead of allocating fresh storage.
size_t nrq_gc_page_count(void);
size_t nrq_gc_recycled_page_count(void);
uint64_t nrq_gc_page_reuse_count(void);
// Counts thread-local slab reservations: the allocation call's slow path. A
// heap that can only hand out short reclaimed runs turns every few objects
// into a refill, which this makes observable.
uint64_t nrq_gc_tlab_refill_count(void);
// Counts allocations that reused space freed by an earlier sweep inside an
// existing page instead of advancing that page's bump cursor. Together with the
// page counts this makes reclamation locality observable.
uint64_t nrq_gc_span_reuse_count(void);
// AO-06 observation: the quantities a thread-local reservation size decision has
// to be made on. `nrq_gc_tlab_tail_bytes` is the total unused reservation space
// threads gave up when moving to another slab or across a cycle, and
// `nrq_gc_tlab_tail_max_bytes` the largest single tail, so stranded memory is
// measured rather than assumed. `nrq_gc_free_run_count` is how many reclaimed
// runs are recorded over all pages and `nrq_gc_page_with_runs_count` how many
// pages hold them, which is the fragmentation those refills work from.
// `nrq_gc_reused_span_bytes` is how much of it has been handed back out; the
// reuse hit rate is that over `nrq_gc_get_allocated_bytes()`.
uint64_t nrq_gc_tlab_tail_bytes(void);
uint64_t nrq_gc_tlab_tail_max_bytes(void);
// Reservation space actually returned to its page at a safepoint. With the
// stranded total above, this is what a reservation size decision is judged on:
// waste that comes back is a delay, waste that does not is a leak.
uint64_t nrq_gc_tlab_tail_reclaimed_bytes(void);
size_t nrq_gc_free_run_count(void);
size_t nrq_gc_page_with_runs_count(void);
uint64_t nrq_gc_reused_span_bytes(void);
// A page records a bounded number of reclaimed runs, so space it cannot record is
// decided on purpose: the allocator keeps the largest runs and drops the smallest.
// These report how much that policy discarded, which is what tells a fragmentation
// figure whether the bound or the workload stranded the space.
uint64_t nrq_gc_dropped_free_run_bytes(void);
uint64_t nrq_gc_dropped_free_run_count(void);
// Reservation span handed out, how much of it the allocator had to write as zeros
// itself, and how much came from storage already known to be zero. The split makes
// the initialization cost visible instead of folding it into the allocation
// figure, where it cannot be separated from the rest of the entry point. It is
// counted per reservation, because that is where the decision is made; a payload
// filled per object inside a reservation that needs filling is part of that span.
struct nrq_gc_zeroing_facts {
  uint64_t payload_bytes;
  uint64_t filled_bytes;
  uint64_t prezeroed_bytes;
};
struct nrq_gc_zeroing_facts nrq_gc_get_zeroing_facts(void);
void nrq_gc_reset_zeroing_facts(void);
// Reports how many objects are tracked by the exact address index. Objects that
// live in a page with start metadata are discovered without the index, so this
// stays proportional to the number of large-object pages rather than the heap.
size_t nrq_gc_object_index_entries(void);
// Reports whether an address is the exact start of a published managed object.
// Interior pointers, stale addresses, and arbitrary invalid references report 0.
// This exposes the collector's object-discovery path for auditing and tests.
int32_t nrq_gc_pointer_is_managed(void* object);
// Diagnostic-only history of reclamation decisions, newest last. Each entry
// names storage the sweep freed and the collector's view of it at that moment:
// the cycle, the object's start offset, its layout, and whether it was born in
// that cycle or marked. A stress test that reads a survivor whose stamp vanished
// can ask which cycle freed that address instead of inferring it from counters.
// `whole_page` marks an entry the page-level reclamation recorded, where the
// address is the page's storage and `live_objects` is what the page claimed to
// hold when the collector decided it was entirely dead.
struct nrq_gc_sweep_record {
  void* address;
  uint64_t cycle;
  uint64_t offset;
  uint32_t payload_bytes;
  int32_t kind;
  int32_t born;
  int32_t marked;
  int32_t whole_page;
  int32_t live_objects;
  // Generation of the reclaimed object (0 young, 1 old, -1 for a whole-page
  // record) and whether the cycle that reclaimed it was a full one (1) or a minor
  // one (0). A young victim that was unmarked in a minor cycle is a remembered-set
  // miss; an old or unborn one in a full cycle is a snapshot-barrier miss.
  int32_t generation;
  int32_t full;
};
size_t nrq_gc_sweep_history(struct nrq_gc_sweep_record* out, size_t max);
// The shared layout descriptor the object was allocated with, or null when the
// address is not a live managed object. A diagnostic: because instances of one
// layout share a descriptor, equal pointers mean equal layout.
const struct nrq_object_layout* nrq_gc_object_layout(void* object);
// The object's packed state word, as the collector encodes it: bit 0 marked,
// bit 1 young, bit 2 remembered, bits 8-15 the promotion age. A diagnostic that
// exists so the encoding can be asserted field by field instead of inferred from
// collection outcomes; it reports 0 for an address that is not a live object.
#define NRQ_GC_STATE_MARKED (1u << 0)
#define NRQ_GC_STATE_YOUNG (1u << 1)
#define NRQ_GC_STATE_REMEMBERED (1u << 2)
#define NRQ_GC_STATE_AGE_SHIFT 8u
uint32_t nrq_gc_object_state_bits(void* object);
// Reports whether the old-to-young store barrier is armed. Allocation must not
// touch this state: it is raised by a collection cycle and by
// `nrq_gc_set_generational`, and a program that turned generations off must keep
// its fast store path.
int32_t nrq_gc_store_barrier_active(void);
uint64_t nrq_gc_get_allocated_bytes(void);
uint64_t nrq_gc_get_collected_bytes(void);
uint64_t nrq_gc_get_collection_count(void);
uint64_t nrq_gc_get_live_objects(void);
// Per-phase cost of the collection cycles that have run since the totals were
// last reset. `prepare` is the safepoint, root enumeration and page-boundary
// work, `mark` the marking slices including the reference resolution they
// drained, `sweep` the reclamation slices, and `finish` the end-of-cycle
// accounting. A whole-cycle figure hides which of those a change moved, so this
// reports them separately from one monotonic clock.
//
// `mark_objects` and `mark_references` count what the marking loop traversed, so
// a per-object cost comes from observed work rather than from the heap size.
// `sweep_objects` is the same count for the reclaimer. The counters are written
// by the collecting thread only, which the cycle lock makes single.
struct nrq_gc_cycle_timing {
  uint64_t cycles;
  uint64_t pause_ns;
  uint64_t prepare_ns;
  uint64_t mark_ns;
  uint64_t sweep_ns;
  uint64_t finish_ns;
  uint64_t mark_objects;
  uint64_t mark_references;
  uint64_t sweep_objects;
};
struct nrq_gc_cycle_timing nrq_gc_get_cycle_timing(void);
// Clears the phase totals, the traversal counts and the cycle count they were
// accumulated over, so one measurement covers exactly the cycles that follow.
void nrq_gc_reset_cycle_timing(void);
void* nrq_gc_pin(void* object);
void nrq_gc_unpin(void* object);
// Balanced compiler handoff pin for managed-pointer returns. Unlike the
// diagnostic user pin API, this has no small per-object nesting limit.
void* nrq_gc_pin_transfer(void* object);
// Pinning diagnostics. `nrq_gc_pin_count` reports how many distinct objects are
// pinned right now. A pin request that would exceed the documented per-object
// pin limit is rejected (returns null) and counted by `nrq_gc_pin_violations`,
// which makes excessive pinning observable from the language without failing
// the program.
uint64_t nrq_gc_pin_count(void);
// Pins and unpins that found no reusable transfer-pin slot and therefore had to
// take the collector mutex. The buffered path reuses retired slots, so this stays
// at zero for a workload that returns and releases one handoff at a time, however
// many distinct objects it walks through.
uint64_t nrq_gc_transfer_pin_fallbacks(void);

uint64_t nrq_gc_pin_violations(void);
// Reports whether automatic collection is currently enabled. Explicit
// collection stays available in both states.
int32_t nrq_gc_is_enabled(void);
void nrq_gc_ref_count_start(void);
void nrq_gc_ref_count_stop(void);
int64_t nrq_gc_get_ref_count(void* object);

// Diagnostic-only TLAB hit, refill, and slow-path fallback classification.
// Zero cost on the inline fast path when disabled; activated either via
// `nrq_gc_set_tlab_diag(1)` or setting `NRQ_GC_TLAB_DIAG=1`.
struct nrq_gc_tlab_diag_facts {
  uint64_t inline_fast_hits;
  uint64_t slow_planned_calls;
  uint64_t slow_tlab_handled;
  uint64_t slow_locked_fallbacks;
  uint64_t refill_count;
  uint64_t spare_run_hits;
  uint64_t site_count;
  uint64_t reason_cold_site_init;
  uint64_t reason_gc_active;
  uint64_t reason_collection_due;
  uint64_t reason_debug_alloc_gate;
  uint64_t reason_diag_roots_gate;
  uint64_t reason_plan_ineligible;
  uint64_t reason_accounting_batch;
  uint64_t reason_tlab_kind_mismatch;
  uint64_t reason_tlab_exhausted;
  uint64_t reason_other;
};
void nrq_gc_set_tlab_diag(int32_t enabled);
struct nrq_gc_tlab_diag_facts nrq_gc_get_tlab_diag_facts(void);
void nrq_gc_reset_tlab_diag_facts(void);

// Region-based evacuating collector telemetry and configuration.
// Region accounting. The object, byte and mark totals behind these counters are
// derived from the page set by the collector's audit at the end of every cycle,
// in both configurations, so a report between cycles describes the last audited
// cycle rather than a number maintained incrementally.
//
// Reserved fields are exactly zero and always will be: evacuation was designed
// and deliberately not built, because deferred region-backed allocation already
// brought sweep time under a millisecond and copying put the headerless (H0) and
// SoA representations at risk. They are kept so the struct's shape does not churn
// and so the absence is visible rather than implied by a missing field.
struct nrq_gc_region_stats {
  size_t region_bytes;
  size_t active_regions;
  size_t recycled_regions;
  size_t survivor_regions;              // reserved: survivor regions unused
  size_t region_limit;                  // see nrq_gc_set_region_limit
  uint32_t trigger_percent;             // see nrq_gc_set_trigger_percent
  uint32_t evacuation_threshold_percent;  // reserved
  uint64_t evacuated_regions;           // reserved
  uint64_t evacuated_objects;           // reserved
  uint64_t evacuated_bytes;             // reserved
  uint64_t bulk_reclaimed_regions;      // regions recycled whole, in O(1)
  uint64_t pinned_region_fallbacks;     // reserved
};
// Declares the region budget the pacemaker paces against. Zero (the default)
// keeps the collector's original allocation-volume trigger.
void nrq_gc_set_region_limit(size_t bytes);
// Percentage of the budget that may be allocated before a cycle starts, clamped
// to 1..100. Default 75.
void nrq_gc_set_trigger_percent(uint32_t percent);
struct nrq_gc_region_stats nrq_gc_get_region_stats(void);
void nrq_gc_reset_region_stats(void);

#ifdef __cplusplus
}
#endif
