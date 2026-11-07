// gc_debug.h — observation surface of the development runtime.
//
// The runtime is built twice from one set of sources. The production library
// (nrqoba_runtime) records nothing; the development library
// (nrqoba_runtime_debug, linked when the compiler runs with --debug-enabled)
// carries the event stream, the allocation-site tags and the root snapshot
// below.
//
// Everything here observes. None of it decides anything: the collector's
// reachability, promotion and reclamation rules are identical in both
// configurations, so a trace taken from a development build describes the same
// collector a production build runs. A development build only reports more
// slowly.
//
// These entry points exist only in the development runtime. A program that
// calls them must be compiled with --debug-enabled; without it the symbols are
// absent, which turns a mistaken production call into a link error instead of a
// silent no-op.
//
// Consumers drain the stream from a single thread. Events are produced by every
// mutator thread and by the collector, so the stream is a merge of per-thread
// rings: `sequence` is globally ordered, but the order in which a `drain` call
// returns rings is the order the threads registered, not the order events
// happened. Sort or key on `sequence` when an exact order matters.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One bit per event kind; `nrq_gc_debug_set_event_mask` selects what is
// recorded. Nothing is recorded by default, so a development build that has not
// asked for a kind pays only a relaxed atomic load per candidate event.
enum {
  // A managed object became live. `aux` is the allocation-site id when the
  // program was built with site tagging, `value` is the payload size in bytes
  // and `address` is the object.
  NRQ_GC_EVENT_ALLOCATION = 1u << 0,
  // A collection cycle started. `aux` is 1 for a young-only (minor) cycle and 0
  // for a full (major) cycle.
  NRQ_GC_EVENT_CYCLE_BEGIN = 1u << 1,
  // A collection cycle finished. `value` is the bytes reclaimed by it.
  NRQ_GC_EVENT_CYCLE_END = 1u << 2,
  // A survivor was promoted to the old generation. `address` is the object.
  NRQ_GC_EVENT_PROMOTE = 1u << 3,
  // One sweep chunk completed. `value` is the objects visited by the chunk and
  // `aux` the objects reclaimed in it.
  NRQ_GC_EVENT_SWEEP = 1u << 4,
  // A root frame opened or closed. `aux` is 1 for open and 0 for close,
  // `value` is the frame marker.
  NRQ_GC_EVENT_ROOT_FRAME = 1u << 5,
};

typedef struct nrq_gc_event {
  // Globally increasing across all threads. Gaps mean dropped events.
  uint64_t sequence;
  // Monotonic nanoseconds since runtime start.
  uint64_t timestamp_nanos;
  // Object address, or 0 when the kind has none.
  uint64_t address;
  // Kind-specific payload; see the enum above.
  uint64_t value;
  // One of NRQ_GC_EVENT_*.
  uint32_t kind;
  // Kind-specific payload; see the enum above.
  uint32_t aux;
} nrq_gc_event;

// An allocation-site tag: the object that was allocated and the site id the
// compiler assigned to the allocation expression. The site id is a hash of the
// source file and line, so it is stable across builds of unchanged code and
// needs no allocation of its own.
typedef struct nrq_gc_debug_tag {
  uint64_t site;
  uint64_t object;
} nrq_gc_debug_tag;

// One root slot as observed by `nrq_gc_debug_snapshot_roots`.
typedef struct nrq_gc_debug_root {
  // Address of the root storage (the slot itself).
  uint64_t slot;
  // Object currently stored at the slot's first reference offset, or 0.
  uint64_t referent;
  // Reference offsets the slot was registered with.
  uint32_t reference_count;
  // Number of elements when the slot is an array/external root; 1 otherwise.
  uint32_t element_count;
  // Byte size of one element of the rooted value.
  uint32_t element_size;
  // 1 when the slot was registered as a pointer root, 0 otherwise.
  uint32_t is_pointer_root;
} nrq_gc_debug_root;

// Returns 1 when the linked runtime is the development one. Both runtimes define
// it, so a program can report which configuration it was built against.
int32_t nrq_runtime_debug_enabled(void);

// Selects the event kinds to record (a mask of NRQ_GC_EVENT_*). Recording is
// per-thread ring based and never blocks an allocator.
void nrq_gc_debug_set_event_mask(uint32_t mask);
uint32_t nrq_gc_debug_event_mask(void);

// Copies up to `capacity` queued events into `out` and removes them. Returns the
// number of events copied. Events that do not fit stay queued for the next call.
size_t nrq_gc_debug_drain_events(nrq_gc_event* out, size_t capacity);

// Copies up to `capacity` queued allocation tags into `out` and removes them.
size_t nrq_gc_debug_drain_tags(nrq_gc_debug_tag* out, size_t capacity);

// Counts records discarded because a ring was full. A non-zero value means the
// trace is incomplete, not that the program did anything wrong.
uint64_t nrq_gc_debug_dropped_events(void);
uint64_t nrq_gc_debug_dropped_tags(void);

// Registers the source location of an allocation site. `site` is the id the
// compiler emitted; `file` is not copied past `file_length` bytes and does not
// need to be null terminated. Repeated declarations of the same id are ignored,
// so a function that is entered many times declares its sites once.
void nrq_gc_debug_declare_site(uint64_t site, const char* file,
                              size_t file_length, uint32_t line);

// Looks up a declared site. Writes the file path (null terminated, truncated to
// `capacity`) and the line into `line`. Returns 1 on success, 0 when the id was
// never declared.
int32_t nrq_gc_debug_describe_site(uint64_t site, char* file, size_t capacity,
                                   uint32_t* line);

// Tags `object` with the allocation site that produced it. The tag is appended
// to the calling thread's tag ring; a full ring drops the record and counts it.
void nrq_gc_debug_tag_allocation(void* object, uint64_t site);

// Copies up to `capacity` live root slots into `out` and returns how many were
// written; a return equal to `capacity` means the answer was truncated. The
// snapshot parks no mutator for longer than the stack mutex of each registered
// thread, so it is a coherent view of the roots, not a stop-the-world freeze.
size_t nrq_gc_debug_snapshot_roots(nrq_gc_debug_root* out, size_t capacity);

#ifdef __cplusplus
}  // extern "C"
#endif
