// Precise non-moving mark-and-sweep collector for explicitly described objects.
// The runtime owns a linked allocation registry, while roots are addresses of
// pointer slots registered by generated code or native callers. Marking follows
// only trace callbacks, so integer data that happens to resemble an address is
// never treated as a reference.
//
// Collection is generational and incremental. A minor cycle traces only young
// objects plus the remembered old-to-young set; survivors are promoted after a
// few cycles. Marking runs in bounded slices while mutators execute, and the
// sweep uses the same pattern after detaching its snapshot list. Mutators are
// parked only briefly at cycle boundaries to publish TLabs and prepare roots.

#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/cache_line.h"
#include "nirqoba/runtime/fatal.h"

#include "gc_debug_internal.h"
#include "slot_index.h"

#include <algorithm>
#include <array>
#include <bit>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

using namespace std;

// The slot index lives in a header so the unit tests can exercise the exact
// table implementation instead of approximating it through the public GC API.
using nirq::gc::detail::SlotIndex;
// Emission points below name these directly. Every helper is `if constexpr` on
// the build configuration, so in a production build the calls and their
// arguments disappear from this translation unit entirely.
using nirq::gc::detail::debug_emit;
using nirq::gc::detail::debug_tag;

namespace {

constexpr size_t kGcPageShift = 16;
constexpr size_t kGcPageBytes = size_t{1} << kGcPageShift;
constexpr size_t kObjectStartAlignment = alignof(std::max_align_t);

struct GcPage {
  unsigned char* storage = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  size_t live_objects = 0;
  // Young objects in this page. A minor collection can only reclaim or promote
  // young objects, so a page where this count is zero is passed straight over:
  // a minor never sets a mark bit on an old object
  // (enqueue_header_locked refuses it), so there is nothing in such a page for
  // the minor to change.
  size_t young_objects = 0;
  // Page-local object-start metadata. One bit per max_align_t-sized slot lets
  // reference resolution validate an address and derive its header without a
  // global hash probe. Pages larger than kGcPageBytes are not indexed; their
  // references keep using ObjectIndex.
  std::unique_ptr<uint64_t[]> object_starts;
  size_t object_start_words = 0;
  // Pages emptied by a concurrent sweep are retained until the next
  // stop-the-world cycle boundary, when no mutator can still be inside an
  // allocator or holding a TLAB cursor into the page.
  bool retired = false;
  // Set when this page cannot be found through the page directory, either
  // because its start bitmap was not allocated or because indexing it failed.
  // Objects in such a page are tracked by the global object index instead.
  bool use_object_index = false;
  // Runs of storage whose object was reclaimed during a sweep. A bump allocator
  // can only move forward, so without these a page that churned through a mix of
  // short- and long-lived objects had to be abandoned while most of it was dead
  // space. Reuse is first-fit within the page and requires no allocation: a fixed
  // array keeps the bookkeeping inline, and a page that fills it simply behaves
  // like the bump allocator it used to be.
  // Page-relative and 32-bit: a page is at most `kGcPageBytes` for the ordinary
  // case, and the whole representation is bounded by the 4 GiB payload limit the
  // layout descriptor enforces. Eight bytes per run doubles the number of runs a
  // page can remember for the same 1.5 KiB, which matters now that a denser
  // header packs twice as many objects into a page: churn over 128-byte objects
  // leaves one run per two dead objects, and the old 16-byte entry capped a page
  // at 96 of them and silently dropped the rest.
  struct FreeSpan {
    uint32_t offset = 0;
    uint32_t span = 0;
  };
  static constexpr size_t kMaxFreeSpans = 192;
  static constexpr size_t kNoSpanIndex = SIZE_MAX;
  std::array<FreeSpan, kMaxFreeSpans> free_spans{};
  size_t free_span_count = 0;
  size_t free_span_bytes = 0;
  // Largest run currently recorded in this page. It lets a refill ask whether a
  // page can host a slab without walking the run list, and lets a single-object
  // search reject a page with one comparison. Kept exact incrementally: a
  // release can only raise it, and a take only matters when it consumes the last
  // run of that size, which `max_free_span_count` identifies.
  size_t max_free_span = 0;
  // Number of recorded runs whose span equals `max_free_span`. Consuming one of
  // several equal runs leaves the maximum unchanged, so the O(runs) rescan that
  // recomputes it runs once per page rather than once per refill.
  size_t max_free_span_count = 0;
  // Position in the index of pages that record at least one freed run, or
  // `kNoSpanIndex` while this page records none. Keeping the position here makes
  // joining and leaving that index a constant-time swap-remove instead of a
  // lookup, and lets every reuse path iterate only pages that can answer a
  // request.
  size_t span_index = kNoSpanIndex;
  // Position in the index of pages with unused tail space, or `kNoSpanIndex`
  // while the page has none. Refills used to walk the whole page set looking for
  // a page with room and find none, once per TLAB; the index answers that in
  // constant time and holds only pages that can serve a request.
  size_t tail_index = kNoSpanIndex;
};


// Small pages are discoverable from their own start bitmap, so their objects
// never touch the global index. Only pages without start metadata (large-object
// pages, or a page whose directory insertion failed) need the exact index.
bool page_uses_index(const GcPage* page) {
  return !page || page->use_object_index || !page->object_starts;
}

// Maps page-aligned storage to its page. Small pages are aligned to
// kGcPageBytes, so every address in a page has the same chunk key and the
// table holds one entry per page. The table is only mutated under gc_mutex.
class PageDirectory {
 public:
  bool insert(GcPage* page) {
    if (!page || !page->storage || !page->object_starts) return false;
    if (entries_.empty()) {
      if (!rehash(64)) return false;
    } else if (size_ + tombstones_ + 1 >=
               entries_.size() - entries_.size() / 3) {
      size_t next = entries_.size();
      if (size_ >= entries_.size() / 2) {
        if (entries_.size() > SIZE_MAX / 2) return false;
        next = entries_.size() * 2;
      }
      if (!rehash(next)) return false;
    }
    const uintptr_t key = key_for(page->storage);
    size_t index = hash(key) & (entries_.size() - 1);
    size_t tombstone = entries_.size();
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.key == key) return entry.page == page;
      if (entry.key == kTombstone && tombstone == entries_.size())
        tombstone = index;
      if (entry.key == kEmpty) {
        if (tombstone != entries_.size()) {
          index = tombstone;
          --tombstones_;
        }
        entries_[index] = Entry{key, page};
        ++size_;
        return true;
      }
      index = (index + 1) & (entries_.size() - 1);
    }
    if (tombstone != entries_.size()) {
      entries_[tombstone] = Entry{key, page};
      --tombstones_;
      ++size_;
      return true;
    }
    return false;
  }

  void erase(GcPage* page) {
    if (!page || !page->storage || entries_.empty()) return;
    const uintptr_t key = key_for(page->storage);
    const size_t mask = entries_.size() - 1;
    size_t index = hash(key) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.key == kEmpty) return;
      if (entry.key == key) {
        if (entry.page != page) return;
        entry = Entry{kTombstone, nullptr};
        --size_;
        ++tombstones_;
        return;
      }
      index = (index + 1) & mask;
    }
  }

  GcPage* find(void* object) const {
    if (!object || entries_.empty()) return nullptr;
    const uintptr_t key = key_for(object);
    const size_t mask = entries_.size() - 1;
    size_t index = hash(key) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      const Entry& entry = entries_[index];
      if (entry.key == kEmpty) return nullptr;
      if (entry.key == key) return entry.page;
      index = (index + 1) & mask;
    }
    return nullptr;
  }

 private:
  static constexpr uintptr_t kEmpty = UINTPTR_MAX;
  static constexpr uintptr_t kTombstone = UINTPTR_MAX - 1;
  struct Entry {
    uintptr_t key = kEmpty;
    GcPage* page = nullptr;
  };

  static uintptr_t key_for(const void* address) {
    return reinterpret_cast<uintptr_t>(address) >> kGcPageShift;
  }

  static size_t hash(uintptr_t key) {
    uint64_t value = static_cast<uint64_t>(key);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<size_t>(value);
  }

  bool rehash(size_t capacity) {
    try {
      std::vector<Entry> replacement(capacity);
      for (const Entry& entry : entries_) {
        if (entry.key == kEmpty || entry.key == kTombstone) continue;
        const size_t mask = capacity - 1;
        size_t index = hash(entry.key) & mask;
        while (replacement[index].key != kEmpty)
          index = (index + 1) & mask;
        replacement[index] = entry;
      }
      entries_.swap(replacement);
      tombstones_ = 0;
      return true;
    } catch (...) {
      return false;
    }
  }

  std::vector<Entry> entries_;
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

bool test_object_start(const GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return false;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return false;
  return (page.object_starts[index / 64] >> (index % 64)) & 1;
}

void mark_object_start(GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return;
  page.object_starts[index / 64] |= uint64_t{1} << (index % 64);
}

void clear_object_start(GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return;
  page.object_starts[index / 64] &= ~(uint64_t{1} << (index % 64));
}

struct alignas(std::max_align_t) ObjectHeader {
  // An object is linked exactly once. While it is still in its owning mutator's
  // unpublished TLAB batch it uses `tlab_next`; once published it uses `next`.
  // The two lists never overlap, so the links share one pointer-sized slot.
  union {
    ObjectHeader* next;
    ObjectHeader* tlab_next;
  };
  ObjectHeader* mark_next = nullptr;
  GcPage* page = nullptr;
  // Wall-clock-free cycle epoch the object was born in; the sweep retains every
  // object born during the cycle it is collecting.
  uint64_t birth_epoch = 0;
  // How many elements the payload repeats. One for a fixed record; the only
  // per-instance thing an array has beyond its descriptor. The payload size
  // itself is `element_count * layout->payload_size`, so no instance stores it.
  uint32_t element_count = 1;
  // Everything that changes about an instance, packed into one word with
  // explicit masks rather than C++ bitfields: the collector reads one word and
  // updates one flag without touching a neighbour, and the encoding is defined by
  // the shifts below rather than by the compiler's allocation strategy.
  //
  // `young`, `remembered` and `age` share the word because they change together
  // in a sweep; only `marked` is written on its own, and it is still one
  // read-modify-write of a word the marker already has in cache.
  uint32_t state = kYoungBit;
  static constexpr uint32_t kMarkedBit = 1u << 0;
  static constexpr uint32_t kYoungBit = 1u << 1;
  static constexpr uint32_t kRememberedBit = 1u << 2;
  // Promotion age occupies the next byte; 255 collections without promotion is
  // past anything the policy reaches.
  static constexpr uint32_t kAgeShift = 8;
  static constexpr uint32_t kAgeMask = 0xFFu << kAgeShift;

  bool marked() const { return (state & kMarkedBit) != 0; }
  void set_marked(bool value) {
    state = value ? (state | kMarkedBit) : (state & ~kMarkedBit);
  }
  bool young() const { return (state & kYoungBit) != 0; }
  void set_young(bool value) {
    state = value ? (state | kYoungBit) : (state & ~kYoungBit);
  }
  bool remembered() const { return (state & kRememberedBit) != 0; }
  void set_remembered(bool value) {
    state = value ? (state | kRememberedBit) : (state & ~kRememberedBit);
  }
  uint32_t age() const { return (state & kAgeMask) >> kAgeShift; }
  // The encoding is a contract, so it is asserted rather than described: the
  // fields may not overlap, the age may not run past the word, and the whole
  // thing has to fit in the field the header gives it.
  static_assert((kMarkedBit & kYoungBit) == 0, "state fields overlap");
  static_assert((kMarkedBit & kRememberedBit) == 0, "state fields overlap");
  static_assert((kYoungBit & kRememberedBit) == 0, "state fields overlap");
  static_assert((kMarkedBit & kAgeMask) == 0, "state fields overlap");
  static_assert((kYoungBit & kAgeMask) == 0, "state fields overlap");
  static_assert((kRememberedBit & kAgeMask) == 0, "state fields overlap");
  static_assert(kAgeShift + 8 <= 32, "the age does not fit the state word");
  static_assert(kAgeMask <= 0xFFFFFFFFu, "the age mask leaves the word");
  static_assert(sizeof(uint32_t) * 8 >= kAgeShift + 8, "state word too narrow");
  // The whole word, for a transition that changes several fields at once: the
  // survivor path computes its next state locally and stores it once, instead of
  // three read-modify-writes of the same cache line.
  uint32_t state_word() const { return state; }
  void set_state(uint32_t value) { state = value; }
  void set_age(uint32_t value) {
    state = (state & ~kAgeMask) | ((value << kAgeShift) & kAgeMask);
  }
  // The shared, immutable description of this object's payload: its size and
  // shape, how to visit its references, and its custom trace callback. It is
  // declared last so the per-instance fields stay in the first cache line, and
  // it replaces the payload size, element size and trace callback each instance
  // used to carry.
  const nrq_object_layout* layout = nullptr;

  // Field-wise construction. Value-initialising the whole header and then
  // overwriting most of it touched both cache lines of the object twice, on a
  // path that runs once per managed object; this writes each member once. The
  // list link is left to the caller because the union member differs between the
  // published list and a thread-local batch.
  ObjectHeader(GcPage* owning_page, uint32_t elements,
               const nrq_object_layout* object_layout, bool is_young,
               uint64_t epoch)
      : next(nullptr),
        mark_next(nullptr),
        page(owning_page),
        birth_epoch(epoch),
        element_count(elements),
        state(is_young ? kYoungBit : 0),
        layout(object_layout) {}

  // The payload immediately follows the header. It is not stored: the page
  // bump allocator always places a header `sizeof(ObjectHeader)` bytes before
  // its object, so the address is pure arithmetic.
  void* payload() const {
    return const_cast<ObjectHeader*>(this) + 1;
  }
};

// Managed allocation cost includes one header per object. The reference map,
// payload size, element size and trace callback now live in the shared layout
// descriptor, so what remains is what genuinely differs per instance: one list
// link, the mark link, the owning page, the layout pointer, the element count,
// the flags and the birth epoch. A regression here raises the cost of every
// allocation, which is why the size is asserted rather than assumed.
static_assert(sizeof(ObjectHeader) == 48,
              "ObjectHeader metadata must stay within 64 bytes");
static_assert(alignof(ObjectHeader) == alignof(std::max_align_t),
              "ObjectHeader must keep payloads max_align_t aligned");
// The payload starts at `header + 1`, and object starts are spaced by
// `alignof(std::max_align_t)`, so the header's size is what keeps every payload
// aligned. A header size that is not a multiple of that alignment would misalign
// every payload on a target where it is 16, which is why the rule is asserted at
// the size rather than trusted at the call sites.
static_assert(sizeof(ObjectHeader) % alignof(std::max_align_t) == 0,
              "a header of this size misaligns payloads on this target");
// The payload size an object reports: its element count times the element size
// its shared descriptor names. One multiply, no per-instance size field, and it
// cannot disagree with the descriptor.
inline size_t object_payload_bytes(const ObjectHeader& header) {
  return static_cast<size_t>(header.element_count) * header.layout->payload_size;
}

// Storage a managed object occupies, header included, rounded up to the payload
// alignment the bump allocator uses.
size_t object_span_bytes(size_t payload_bytes) {
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw = sizeof(ObjectHeader) + payload_bytes;
  return (raw + alignment - 1) & ~(alignment - 1);
}

// Layout descriptors are interned by every property that changes how the
// payload is interpreted. Matching sizes alone is not enough: two records of the
// same size with reference words at different offsets, or with a different trace
// callback, must not share a descriptor.
struct LayoutKey {
  size_t payload_bytes;
  size_t element_bytes;
  nrq_gc_trace_fn trace;
  std::vector<uint32_t> offsets;

  bool operator==(const LayoutKey& other) const {
    return payload_bytes == other.payload_bytes &&
           element_bytes == other.element_bytes && trace == other.trace &&
           offsets == other.offsets;
  }
};

struct LayoutKeyHash {
  size_t operator()(const LayoutKey& key) const {
    size_t hash = key.payload_bytes * static_cast<size_t>(0x9E3779B97F4A7C15ull);
    hash ^= key.element_bytes * static_cast<size_t>(0xC2B2AE3D27D4EB4Full);
    hash ^= reinterpret_cast<size_t>(key.trace) >> 4;
    for (const uint32_t offset : key.offsets) hash = (hash * 31u) ^ offset;
    return hash;
  }
};

// Descriptors and their offset tables are never reclaimed, so a live object can
// always read the layout it was born with and pointer equality is a valid
// identity test. The table itself is only touched while a layout is created;
// allocation and tracing read the descriptor through a plain pointer.
std::mutex layout_mutex;
std::unordered_map<LayoutKey, const nrq_object_layout*, LayoutKeyHash> layout_pool;
// Bytes the published descriptors and their offset tables occupy. They are never
// freed, so this is the whole structural cost of sharing.
size_t layout_bytes_total = 0;

// Allocation must not take the pool lock. A thread remembers the last layout it
// asked for and proves the match by comparing the offset table itself, so a
// compiler-emitted constant table and a caller that rebuilds the same table on
// its stack both hit without touching the pool. Only a genuinely new layout
// reaches the interning table.
// The memo holds the interned descriptor and nothing else. Every field it checks
// comes from that descriptor, which the runtime owns for the life of the process,
// so a hit is decided without touching storage the caller owns.
//
// It must not cache the caller's offset table. The typed allocation entry points
// document that the collector copies the table, which means callers rebuild it on
// the stack and pass a pointer whose lifetime is the call: caching that pointer
// would read expired storage on the next allocation, and comparing two calls by
// it would accept a table that has since been rebuilt with different contents.
struct LayoutMemo {
  const nrq_object_layout* layout = nullptr;
};
thread_local LayoutMemo layout_memo;

bool layout_memo_matches(const LayoutMemo& memo, size_t bytes,
                         size_t element_size, nrq_gc_trace_fn trace,
                         const size_t* offsets, size_t count) {
  const nrq_object_layout* layout = memo.layout;
  if (!layout) return false;
  // The descriptor describes exactly this shape, or it does not: same element
  // size, same tracing, same reference count, same array/record kind.
  if (layout->payload_size != element_size) return false;
  if (layout->trace != trace) return false;
  if (layout->reference_count != count) return false;
  const bool describes_an_array =
      (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0;
  if (describes_an_array != (element_size != bytes)) return false;
  if (count == 0) return true;
  const uint32_t* owned = layout->reference_offsets;
  if (!owned || !offsets) return false;
  // Compared against the descriptor's table, which is immutable and outlives
  // every caller. The incoming table is read only while its caller is on the
  // stack, which is exactly the window in which it is alive.
  for (size_t index = 0; index < count; ++index) {
    if (static_cast<size_t>(owned[index]) != offsets[index]) return false;
  }
  return true;
}

// The descriptor a payload uses. A record passes its size as both bytes and
// element size; an array passes its element stride, which repeats. Payload
// offsets are always relative to the element, which is the whole record for a
// fixed layout.
const nrq_object_layout* layout_for_allocation(size_t bytes,
                                               size_t element_size,
                                               nrq_gc_trace_fn trace,
                                               const size_t* offsets,
                                               size_t count) {
  const size_t payload_bytes = element_size;
  const size_t element_bytes = element_size == bytes ? 0 : element_size;
  if (layout_memo_matches(layout_memo, bytes, element_size, trace, offsets,
                          count))
    return layout_memo.layout;
  const nrq_object_layout* layout = nrq_gc_layout_intern(
      payload_bytes, element_bytes, trace, offsets, count);
  if (layout) layout_memo.layout = layout;
  return layout;
}

// Reclaimed storage is tracked per page so later allocations can reuse it;
// defined with the page pool below, declared here because sweeping reaches them.
void release_span_locked(GcPage& page, size_t offset, size_t span);
GcPage* take_free_span_locked(size_t span, size_t& offset);

// Marking keeps a FIFO ring of pending headers. Contiguous storage avoids
// chasing the intrusive list through the heap and visits a graph level by
// level, which keeps related objects close in time. Objects beyond the ring
// capacity spill to the header's mark_next link, so the worklist never
// allocates auxiliary heap storage.
class MarkWorklist {
 public:
  static constexpr size_t kInlineCapacity = 256;

  bool empty() const { return count_ == 0 && overflow_ == nullptr; }

  void push(ObjectHeader* header) {
    if (!header) return;
    header->mark_next = nullptr;
    if (count_ < kInlineCapacity) {
      inline_[(head_ + count_) % kInlineCapacity] = header;
      ++count_;
      return;
    }
    header->mark_next = overflow_;
    overflow_ = header;
  }

  ObjectHeader* pop() {
    if (count_ != 0) {
      ObjectHeader* header = inline_[head_];
      head_ = (head_ + 1) % kInlineCapacity;
      --count_;
      return header;
    }
    if (!overflow_) return nullptr;
    ObjectHeader* header = overflow_;
    overflow_ = header->mark_next;
    header->mark_next = nullptr;
    return header;
  }

 private:
  std::array<ObjectHeader*, kInlineCapacity> inline_{};
  size_t head_ = 0;
  size_t count_ = 0;
  ObjectHeader* overflow_ = nullptr;
};

// The collector needs exact-address lookup while marking, but only allocation
// mutates this table. Open addressing keeps pointer metadata contiguous and
// avoids one node allocation per managed object.
class ObjectIndex {
 public:
  // Number of exact address entries. Small objects are found through their
  // page's start bitmap, so this only grows with large-object pages.
  size_t size() const { return size_; }

  ObjectHeader* find(void* object) const {
    if (!object || entries_.empty()) return nullptr;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      const Entry& entry = entries_[index];
      if (entry.object == nullptr) return nullptr;
      if (entry.object == object) return entry.header;
      index = (index + 1) & mask;
    }
    return nullptr;
  }

  bool insert(void* object, ObjectHeader* header) {
    if (!object || !header) return false;
    const bool needs_rehash = entries_.empty() ||
        size_ + tombstones_ + 1 >= entries_.size() - entries_.size() / 3;
    if (needs_rehash) {
      size_t next = 64;
      if (!entries_.empty()) {
        if (size_ >= entries_.size() / 2) {
          if (entries_.size() > SIZE_MAX / 2) return false;
          next = entries_.size() * 2;
        } else {
          next = entries_.size();
        }
      }
      if (!rehash(next)) return false;
    }
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    size_t tombstone = entries_.size();
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.object == object) return false;
      if (entry.object == tombstone_key() && tombstone == entries_.size())
        tombstone = index;
      if (entry.object == nullptr) {
        if (tombstone != entries_.size()) {
          index = tombstone;
          --tombstones_;
        }
        entries_[index] = Entry{object, header};
        ++size_;
        return true;
      }
      index = (index + 1) & mask;
    }
    if (tombstone != entries_.size()) {
      entries_[tombstone] = Entry{object, header};
      --tombstones_;
      ++size_;
      return true;
    }
    return false;
  }

  bool erase(void* object) {
    if (!object || entries_.empty()) return false;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.object == nullptr) return false;
      if (entry.object == object) {
        entry = Entry{tombstone_key(), nullptr};
        --size_;
        ++tombstones_;
        return true;
      }
      index = (index + 1) & mask;
    }
    return false;
  }

 private:
  struct Entry { void* object = nullptr; ObjectHeader* header = nullptr; };
  static void* tombstone_key() { return reinterpret_cast<void*>(uintptr_t{1}); }

  static size_t hash(void* object) {
    uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<size_t>(value);
  }

  bool rehash(size_t capacity) {
    try {
      std::vector<Entry> replacement(capacity);
      for (const Entry& entry : entries_) {
        if (!entry.object || entry.object == tombstone_key()) continue;
        const size_t mask = capacity - 1;
        size_t index = hash(entry.object) & mask;
        while (replacement[index].object != nullptr)
          index = (index + 1) & mask;
        replacement[index] = entry;
      }
      entries_.swap(replacement);
      tombstones_ = 0;
      return true;
    } catch (...) {
      return false;
    }
  }

  std::vector<Entry> entries_;
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

void destroy_object_storage(ObjectHeader* header) {
  if (!header) return;
  header->~ObjectHeader();
}

// The heap lock, instrumented with the one invariant the handshake depends on: no
// thread may hold it while waiting for a fast-barrier region to drain, because a
// region can itself need the lock to reserve a TLAB. That would be a deadlock, so
// wait_for_fast_barriers() checks the flag instead of trusting every caller.
thread_local bool heap_lock_held_by_this_thread = false;

class CheckedHeapMutex {
 public:
  void lock() {
    mutex_.lock();
    heap_lock_held_by_this_thread = true;
  }
  void unlock() {
    heap_lock_held_by_this_thread = false;
    mutex_.unlock();
  }
 private:
  std::mutex mutex_;
};

class HeapLockGuard {
 public:
  explicit HeapLockGuard(CheckedHeapMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  HeapLockGuard(const HeapLockGuard&) = delete;
  HeapLockGuard& operator=(const HeapLockGuard&) = delete;
  ~HeapLockGuard() { mutex_.unlock(); }
 private:
  CheckedHeapMutex& mutex_;
};

CheckedHeapMutex gc_mutex;
// Serializes complete collection cycles while leaving marking slices and the
// sweep open to concurrent mutator execution.
std::mutex collection_cycle_mutex;

enum class CollectionPhase { Idle, Preparing, Marking, Sweeping };
constexpr size_t kRememberedBufferCapacity = 64;
constexpr size_t kTransferPinBufferCapacity = 32;

// A pending transfer-pin adjustment. Managed returns hand one pin off per call;
// routing every one of them through the global pin table would take the
// collector mutex on every callee return. A registered mutator instead records a
// signed delta in its own buffer, and the collector folds the deltas into the
// table when it prepares a cycle. Deltas are matched per object, so a handoff
// that is released before the next cycle leaves no trace at all.
struct TransferPinDelta {
  void* object = nullptr;
  int64_t delta = 0;
};

struct MutatorState {
  size_t depth = 0;
  bool parked = false;
  GcPage* tlab_page = nullptr;
  unsigned char* tlab_cursor = nullptr;
  unsigned char* tlab_end = nullptr;
  uint64_t tlab_generation = 0;
  ObjectHeader* pending_objects = nullptr;
  // Depth of lock-free barrier regions this thread is currently inside. Each
  // mutator owns its own cache line, so a thread's store barrier never
  // invalidates another thread's copy; the collector reads the counters only
  // when it needs the handshake, and only over mutators that are registered.
  alignas(nirq::nrq_cache_line_bytes) std::atomic<uint32_t> barrier_depth{0};
  // Per-mutator allocation accounting. The owner updates these while
  // allocating; the collector reads them with gc_mutex held when publishing or
  // when reporting statistics, so the fast path never touches a global counter.
  std::atomic<size_t> pending_bytes{0};
  std::atomic<size_t> pending_count{0};
  // Owner-only mirrors of the two published counters. The stores below still
  // publish the exact value after every allocation; the loads that preceded them
  // are gone, because only this thread writes the counters and every clear
  // happens while the owner is parked or is the one clearing.
  size_t pending_bytes_local = 0;
  size_t pending_count_local = 0;
  size_t unflushed_bytes = 0;
  // Old-to-young stores buffer raw referents so ordinary stores do not need the
  // collector mutex. The owner drains this buffer at a safepoint/leave; the
  // collector drains every registered mutator after parking them.
  void* remembered[kRememberedBufferCapacity]{};
  size_t remembered_count = 0;
  // Transfer-pin deltas this thread has recorded but not yet folded into the
  // global pin table. Owner-only; the collector reads it after parking.
  TransferPinDelta transfer_pins[kTransferPinBufferCapacity]{};
  size_t transfer_pin_count = 0;
  // Short reclaimed runs this thread has taken but not handed out yet. A heap
  // whose free space is a scatter of few-object runs can only answer a refill
  // with a few cells, and paying the allocator lock once per few allocations
  // costs more than those cells are worth. The list is thread-owned, so taking a
  // run from it is a decrement, not a lock.
  //
  // It never survives a cycle boundary: prepare_cycle_locked returns whatever is
  // left to its page before the reclaimer runs. A page can only be recycled once
  // all of its space is either published or recorded as free, and a run held
  // here is neither.
  struct SpareRun {
    GcPage* page = nullptr;
    size_t offset = 0;
    size_t span = 0;
  };
  static constexpr size_t kSpareRunCapacity = 24;
  SpareRun spare_runs[kSpareRunCapacity];
  size_t spare_run_count = 0;
  // Unused reservation space this thread has given up, and the largest single
  // tail. Owner-only: only the owner can move its cursor, so no lock is needed,
  // and a thread that leaves folds its totals into the global counters.
  uint64_t tlab_tail_bytes = 0;
  uint64_t tlab_tail_max_bytes = 0;
  uint64_t tlab_tail_reclaimed_bytes = 0;
};
void drain_transfer_pins_locked(MutatorState& mutator);

std::mutex mutator_mutex;
// Mutators wait here for a collection request to clear.
std::condition_variable mutator_condition;
// The collector waits here for mutators to park. Keeping it separate from
// mutator_condition means a parking mutator wakes only the collector instead of
// every thread that is already parked or waiting to enter the registry.
std::condition_variable collector_condition;
std::vector<MutatorState*> mutators;
// Number of registered mutators, maintained with mutator_mutex. The collector
// compares it against the parked count instead of walking the registry each time
// the wait predicate is evaluated.
size_t registered_mutator_count = 0;
// Number of mutators currently parked. Written by the mutator as it parks and as
// it resumes, under mutator_mutex and published with release ordering.
std::atomic<size_t> parked_mutator_count{0};
// Read by every safepoint poll without a lock. Writers hold mutator_mutex and
// publish with release ordering; waiters re-check while holding that mutex.
std::atomic<bool> collection_requested{false};
thread_local bool thread_is_collector = false;
thread_local MutatorState thread_mutator;
// Objects whose page cannot be walked by a start bitmap, in publication order.
// Ordinary objects are found through their page instead, so allocation no longer
// links them into a heap-wide list and the sweep has no list to keep consistent
// while mutators allocate.
ObjectHeader* unindexed_objects = nullptr;
ObjectIndex object_index;
PageDirectory page_directory;

// Resolve a published object through page-local start metadata first. A hit
// validates the exact payload start, so interior and stale pointers still miss.
// Any page that is not indexed (large-object pages or an allocation failure)
// falls back to the exact ObjectIndex. Callers hold gc_mutex.
ObjectHeader* find_object_locked(void* object) {
  if (!object) return nullptr;
  GcPage* page = page_directory.find(object);
  if (page && page->object_starts) {
    const auto* address = static_cast<const unsigned char*>(object);
    if (address >= page->storage) {
      const size_t offset = static_cast<size_t>(address - page->storage);
      if (offset >= sizeof(ObjectHeader) && offset < page->capacity &&
          test_object_start(*page, offset)) {
        auto* header =
            reinterpret_cast<ObjectHeader*>(page->storage + offset) - 1;
        if (header->page == page && header->payload() == object)
          return header;
      }
    }
    // The address lies inside a page whose start bitmap is authoritative, so a
    // cleared slot is a decisive miss: interior pointers, stale addresses and
    // garbage all end here without paying for a hash probe of the global index.
    return nullptr;
  }
  return object_index.find(static_cast<void*>(object));
}

// count * element_size, reporting overflow through the return value and the
// product through `bytes`. The obvious guard divides by element_size first, and a
// 64-bit divide is one of the most expensive instructions on the path - it cost
// more than the rest of the entry point's arithmetic. The high half of the product
// answers the same question with one multiply.
bool multiply_size_overflowed(size_t count, size_t element_size, size_t& bytes) {
#if defined(_MSC_VER) && defined(_M_X64)
  unsigned __int64 high = 0;
  bytes = static_cast<size_t>(_umul128(count, element_size, &high));
  return high != 0;
#else
  if (element_size != 0 && count > SIZE_MAX / element_size) return true;
  bytes = count * element_size;
  return false;
#endif
}

constexpr size_t kGcTlabBytes = kGcPageBytes;
// The smallest reclaimed run worth a reservation. A short run still serves
// allocations, but taking one for a slab spends a lock acquisition on a few
// cells; see reserve_tlab_locked.
constexpr size_t kGcTlabMinSlabBytes = kGcTlabBytes / 8;
std::vector<std::unique_ptr<GcPage>> gc_pages;

// Visits every live object whose page is in the set, in page and address order,
// then the objects whose page has no walkable bitmap. Diagnostics use this
// instead of a heap-wide list, which allocation no longer maintains. Callers
// hold gc_mutex and are outside a sweep slice; objects detached into the sweep
// list are not visited, which matches what the old single list reported while a
// cycle was sweeping.
template <typename Visit>
void for_each_live_object_locked(Visit&& visit) {
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page || page_uses_index(page) || page->object_start_words == 0) continue;
    for (size_t word = 0; word < page->object_start_words; ++word) {
      uint64_t bits = page->object_starts[word];
      while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        const size_t payload_offset =
            (word * 64 + index) * kObjectStartAlignment;
        auto* header =
            reinterpret_cast<ObjectHeader*>(page->storage + payload_offset) - 1;
        visit(header);
      }
    }
  }
  for (ObjectHeader* header = unindexed_objects; header; header = header->next)
    visit(header);
}
// Pages that currently record at least one freed run. First-fit reuse used to
// scan every page in allocation order, so a refill cost grew with the heap even
// though almost every page had nothing to offer. The index holds only pages that
// can answer a request, and `GcPage::span_index` makes membership a swap-remove,
// so no path ever walks the page set to maintain it.
std::vector<GcPage*> span_pages;
// Number of pages marked `retired`, which are exactly the pages a cycle
// boundary can reclaim. The prepare step keeps this count so it can skip its
// page walk entirely when no page was emptied during the previous sweep.
size_t retired_page_count = 0;

// Join or leave the index of pages with reusable runs. Both operations are
// constant-time and neither allocates on the removal path. A page that cannot be
// indexed (the vector failed to grow) simply stays out of it: its runs remain
// recorded and are still merged, but the first-fit paths will not reach them,
// which is the behaviour the allocator had before the index existed.
void index_span_page_locked(GcPage& page) {
  if (page.span_index != GcPage::kNoSpanIndex) return;
  try {
    span_pages.push_back(&page);
    page.span_index = span_pages.size() - 1;
  } catch (...) {
    page.span_index = GcPage::kNoSpanIndex;
  }
}

// Pages with unused tail space. A bump allocator fills the pages it takes, so in
// a churn workload this index is empty most of the time - which is exactly the
// answer a refill needs, and the reverse scan could not give it without walking
// the entire page set. Membership is a swap-remove through `tail_index`.
std::vector<GcPage*> tail_pages;

// Upper bound on the tail space of any indexed page. A page whose tail cannot
// serve the request used to cost nothing because a reservation always charged
// the whole page; now that a tail is left behind, a large payload can face
// thousands of indexed pages that all hold too little. This answers "can any of
// them serve?" in one comparison, and a full scan that finds nothing recomputes
// it exactly, so the walk happens once per heap state rather than per refill.
size_t max_indexed_tail_bytes = 0;

size_t indexed_tail_bytes(const GcPage& page) {
  return page.capacity > page.used ? page.capacity - page.used : 0;
}

void index_tail_page_locked(GcPage& page) {
  if (page.tail_index != GcPage::kNoSpanIndex || page.retired) return;
  try {
    tail_pages.push_back(&page);
    page.tail_index = tail_pages.size() - 1;
  } catch (...) {
    page.tail_index = GcPage::kNoSpanIndex;
  }
}

void unindex_tail_page_locked(GcPage& page) {
  if (page.tail_index == GcPage::kNoSpanIndex) return;
  GcPage* last = tail_pages.back();
  tail_pages[page.tail_index] = last;
  last->tail_index = page.tail_index;
  tail_pages.pop_back();
  page.tail_index = GcPage::kNoSpanIndex;
}

// Keep the index in step with the one field it describes: a page leaves it the
// moment its bump cursor reaches the end of the page.
void note_page_tail_locked(GcPage& page) {
  if (page.used >= page.capacity) {
    unindex_tail_page_locked(page);
  } else {
    index_tail_page_locked(page);
    const size_t remaining = indexed_tail_bytes(page);
    if (remaining > max_indexed_tail_bytes) max_indexed_tail_bytes = remaining;
  }
}

// A page the sweep emptied stays in the page set until the next cycle boundary -
// the first moment its storage may go back to the recycle pool, because until then
// a mutator that has not reached a safepoint may still hold a cursor into it. The
// space it freed is not off limits, though: the sweep already decided about every
// object it covered, and an object materialised afterwards carries a birth epoch
// past the cycle, so handing that space out again is ordinary reuse inside a page
// the heap already owns. Clearing the mark is what tells the boundary the page is
// occupied again and must not be reclaimed.
void unretire_page_locked(GcPage& page) {
  if (!page.retired) return;
  page.retired = false;
  if (retired_page_count != 0) --retired_page_count;
}

void unindex_span_page_locked(GcPage& page) {
  if (page.span_index == GcPage::kNoSpanIndex) return;
  GcPage* last = span_pages.back();
  span_pages[page.span_index] = last;
  last->span_index = page.span_index;
  span_pages.pop_back();
  page.span_index = GcPage::kNoSpanIndex;
}
// Empty pages keep their backing storage across cycles. Reusing them avoids an
// operator new / operator delete pair for every page a churn-heavy workload
// frees at each cycle boundary. The pool is bounded so idle programs do not
// retain an unbounded amount of address space.
std::vector<std::unique_ptr<GcPage>> recycled_pages;
// One run's worth of shells. The pool exists to skip the page-struct and start
// bitmap setup for a heap that is about to grow again, not to hold a heap's worth
// of storage for a program that has stopped allocating: a page that leaves the pool
// goes back to its run and its commit charge goes back to the OS.
constexpr size_t kMaxRecycledPages = 128;
// Large-object pages can be much bigger than kGcPageBytes. Retaining a pool of
// them would make the page-count bound meaningless, so only ordinary-sized
// pages are recycled and oversized pages return to the process heap.
constexpr size_t kMaxRecycledPageBytes = 2 * kGcPageBytes;
uint64_t recycled_page_reuse_count = 0;
// Counts thread-local slab reservations, the allocation call's slow path. A heap
// that cannot hand out a useful slab turns every few objects into a refill, so the
// count is the direct measure of that cost.
uint64_t tlab_refill_count = 0;
// Counts allocations served from space freed by an earlier sweep rather than
// from a page's untouched tail.
uint64_t recycled_span_reuse_count = 0;
size_t total_free_spans = 0;// AO-06 observation. `reused_span_bytes` is how much reclaimed space has been
// handed back out, which with the allocated total gives the reuse hit rate;
// `tlab_tail_bytes_total` folds in the reservations of threads that have left
// the registry, so the tail counters stay per-mutator and race-free.
uint64_t reused_span_bytes = 0;
uint64_t tlab_tail_bytes_total = 0;
uint64_t tlab_tail_max_bytes_total = 0;
// Reservation space returned to its page at a safepoint, which is what turns an
// abandoned slab tail from stranded bytes into reusable free space.
uint64_t tlab_tail_reclaimed_bytes_total = 0;
// Number of pages whose largest recorded run can host a TLAB slab. A refill asks
// this one word before searching the run index, so a fragmented heap answers
// "no slab anywhere" in constant time instead of proving it by walking every
// page once per few allocations. Maintained under gc_mutex with the page maxima.
size_t pages_with_slab_run = 0;
// Cache the current bump page per allocating thread. Collection increments the
// generation so a thread that resumes after another thread reclaimed its page
// discards the stale pointer before dereferencing it.
uint64_t gc_page_generation = 1;
thread_local GcPage* thread_allocation_page = nullptr;
thread_local uint64_t thread_allocation_generation = 0;
struct RootInfo {
  size_t references = 0;
  size_t object_size = 0;
  size_t repeated_count = 0;
  // Roots describe layouts exactly like objects do, so they share the same
  // immutable descriptor instead of carrying their own offset copy. A repeated
  // array root is an array layout whose payload is one element.
  const nrq_object_layout* layout = nullptr;
};
std::unordered_map<void*, RootInfo> roots;
// Thread-local shadow stacks hold ordinary roots. Array/external roots stay in
// `roots`; ordinary roots never touch gc_mutex after the stack is registered.
struct ShadowRoot {
  void* slot = nullptr;
  RootInfo info;
};
// Per-slot bookkeeping for an active shadow root. `count` preserves
// duplicate-registration semantics: a slot stays indexed while any frame still
// owns a registration for it. `layout` is the validated descriptor shared by
// every registration of that slot.
struct SlotState {
  size_t count = 0;
  size_t object_size = 0;
  // The descriptor of the first registration. A repeat must agree on the
  // pointer itself, which is one compare instead of an offset walk.
  const nrq_object_layout* layout = nullptr;
  bool pointer_root = false;
};
// Thread-local index of active root slots. It answers the three questions the
// shadow stack needs -- how many registrations a slot currently has, whether a
// repeated registration agrees with the first one, and whether the slot is a
// plain pointer root -- without allocating a hash node per slot. See
// slot_index.h for the table's rehash policy and the PO-27 tombstone fix.

// Direct-mapped record of the slots this thread has already proven to be pointer
// roots. A rooted allocation validates its slot on every call; the hash index
// answers that in one probe, and this answers it in one compare for the slots a
// construction loop reuses. An entry is retired together with the registration
// it came from, so a hit can never outlive the slot state it describes.
struct PointerRootCache {
  static constexpr size_t kCapacity = 8;
  void* slots[kCapacity]{};

  static size_t index_of(void* slot) {
    return (reinterpret_cast<uintptr_t>(slot) >> 4) & (kCapacity - 1);
  }
  bool contains(void* slot) const { return slots[index_of(slot)] == slot; }
  void insert(void* slot) { slots[index_of(slot)] = slot; }
  void forget(void* slot) {
    void*& entry = slots[index_of(slot)];
    if (entry == slot) entry = nullptr;
  }
};

struct ShadowStack {
  std::mutex mutex;
  std::vector<ShadowRoot> roots;
  struct Frame {
    uint64_t marker;
    size_t root_depth;
  };
  std::vector<Frame> frames;
  uint64_t next_marker = 1;
  // O(1) index of active root slots. Maintained by the owning thread only and
  // never read by the collector. It replaces both the old full-root scan used
  // for duplicate-layout validation and the pointer-root set that was rebuilt
  // on every frame end.
  SlotIndex<SlotState> slot_states;
  PointerRootCache pointer_roots;
};
std::vector<ShadowStack*> shadow_stacks;
thread_local ShadowStack* thread_shadow_stack = nullptr;

// Cache of frame root descriptors this thread has already validated. Keying it
// by address alone would be enough for the compiler's immutable tables, but the
// entry point is a public C ABI: a caller may hand over a descriptor on the
// stack, and two activations can reuse that address with different content. A
// hit therefore requires identical words as well, so a cached verdict can only
// ever be reused for the descriptor it was computed from - the validation is
// deduplicated, never skipped. Comparing the words costs two vector compares for
// a typical frame, far less than the walk it replaces.
struct ValidatedDescriptorCache {
  static constexpr size_t kCapacity = 16;
  static constexpr size_t kMaxWords = 16;
  struct Entry {
    const size_t* descriptor = nullptr;
    size_t word_count = 0;
    size_t words[kMaxWords]{};
  };
  Entry entries[kCapacity]{};
  size_t count = 0;

  bool matches(const size_t* desc) const noexcept {
    const size_t word_count = desc[1];
    // A table wider than the memo is validated on every activation instead of
    // being stored truncated.
    if (word_count < 2 || word_count > kMaxWords) return false;
    for (size_t i = 0; i < count; ++i) {
      const Entry& entry = entries[i];
      if (entry.descriptor != desc) continue;
      return entry.word_count == word_count &&
             std::memcmp(entry.words, desc, word_count * sizeof(size_t)) == 0;
    }
    return false;
  }

  void insert(const size_t* desc) noexcept {
    const size_t word_count = desc[1];
    if (word_count < 2 || word_count > kMaxWords) return;
    for (size_t i = 0; i < count; ++i) {
      if (entries[i].descriptor != desc) continue;
      entries[i].word_count = word_count;
      std::memcpy(entries[i].words, desc, word_count * sizeof(size_t));
      return;
    }
    Entry& entry = count < kCapacity ? entries[count++] : entries[0];
    entry.descriptor = desc;
    entry.word_count = word_count;
    std::memcpy(entry.words, desc, word_count * sizeof(size_t));
  }
};
thread_local ValidatedDescriptorCache thread_validated_descriptors;
// Cleans up thread-local GC state when a thread terminates without unwinding
// its mutator/root frames. Without this the collector could later enumerate a
// shadow stack whose owning thread is gone and whose slots are dangling, and a
// thread-local allocation batch could be lost.
struct ThreadGcStateCleanup {
  bool armed = false;
  void arm() { armed = true; }
  ~ThreadGcStateCleanup();
};
thread_local ThreadGcStateCleanup thread_gc_cleanup;
std::unordered_map<void*, size_t> pinned_objects;
// Pinning is a diagnostic facility rather than a concurrency primitive: a
// reasonable program pins an object while a native call borrows it and unpins it
// immediately afterwards. Repeating the same pin far more often than that means
// the program is leaking pins (they are never released by the collector), so the
// runtime rejects further pins for that object and counts the rejection instead
// of failing the process.
constexpr size_t kMaxPinsPerObject = 16;
uint64_t pin_violations = 0;
// Reference diagnostics is the only reader that can look at a shadow stack while
// the collector is idle: during a collection every registered mutator is parked
// before roots are enumerated, so a running mutator's stack cannot be read. The
// fast paths therefore take the stack mutex only when a diagnostic query is
// possible, and plain, owner-only access otherwise.
std::atomic<bool> reference_diagnostics_enabled{false};

bool diagnostics_reading_roots() {
  return reference_diagnostics_enabled.load(std::memory_order_acquire);
}
// Collection is an explicit opt-in policy. Programs using the GC domain may
// call gc.enable() to request automatic triggers; explicit gc.collect() always
// works regardless of this state.
std::atomic<bool> automatic_collection_enabled{false};
std::atomic<bool> generational_collection_enabled{true};
// Every GC allocation updates these counters, from every mutator thread, so
// they are the hottest shared state in the runtime. They sit together on one
// cache line because they are updated together, and that line is isolated from
// the collector's own fields so a mutator's accounting does not invalidate the
// line a collection is reading (false sharing). The residual contention is true
// sharing: several threads do update these same words, which per-mutator
// accounting rather than layout has to remove.
// Every GC allocation used to update global atomics. Now each mutator batches
// its unflushed byte count locally and contributes it to this line only when a
// TLAB is published or a batch of this size accumulates. The counters therefore
// stay on one isolated line without becoming a per-allocation rendezvous point.
constexpr size_t kAccountingBatchBytes = 16 * 1024;
struct alignas(nirq::nrq_cache_line_bytes) AllocationAccounting {
  std::atomic<size_t> bytes_since_collection{0};
  std::atomic<size_t> collection_threshold{1024 * 1024};
};
AllocationAccounting allocation_accounting;
// Adaptive heap-growth policy. The trigger is not a constant: after every cycle
// the collector knows how much of the heap survived, so it can set the next
// trigger from the live set instead of collecting on a fixed byte count that is
// either wasteful for a small heap or far too eager for a large one. The policy
// is disabled as soon as a program sets its own threshold.
constexpr size_t kMinimumCollectionHeadroom = 512 * 1024;
struct CollectionPolicy {
  bool adaptive = true;
  size_t last_reclaimed_bytes = 0;
  size_t adaptations = 0;
};
CollectionPolicy collection_policy;
// Live bytes of the published heap, maintained incrementally by publication and
// reclamation. finish_cycle used to recompute this by walking every live object.
size_t live_heap_bytes = 0;
// Live bytes at the start of the cycle in progress, used to measure survival.
size_t cycle_start_live_bytes = 0;
nrq_gc_stats statistics{};
uint64_t minor_collections = 0;
uint64_t major_collections = 0;

// A collection is split into short mark slices. Mutators continue executing
// between slices; barriers below preserve the snapshot-at-the-beginning
// invariant. The sweep phase serializes GC API calls with gc_mutex but does not
// park mutators that are only executing ordinary code.
// Slice budgets are expressed as bytes of heap touched, not as a fixed object
// count. A constant count makes the number of lock rounds proportional to the
// heap: 2 M live objects at 64 objects per slice is 31 000 acquisitions of
// gc_mutex, each of which a refilling mutator can also be waiting on. A byte
// budget keeps a slice short in wall-clock terms for any object size while the
// number of rounds follows the bytes actually processed. Marking touches less
// per object than sweeping does, so its budget is the smaller of the two.
constexpr size_t kMarkSliceBytes = 64 * 1024;
constexpr size_t kSweepSliceBytes = 256 * 1024;
// A byte budget alone does not bound a sweep chunk: a page holding one live
// object contributes one span to the budget but still costs its whole start
// bitmap to examine, and an empty page costs a visit. Charge bitmap words as
// work too, so a sparse heap cannot hold the collector lock for a scan bounded
// only by its size. Dense pages reach the byte budget first (a 64 KiB page of
// 120-byte objects is ~512 words for 2184 objects); 4096 words bounds the sparse
// case at ~32 pages per chunk.
constexpr size_t kSweepBitmapWordsPerChunk = 4096;
constexpr uint8_t kPromotionAge = 2;
// A full collection is worth its cost only when it can reclaim something a young
// collection cannot: unreachable old objects. Those are invisible to minors, so
// they show up as heap growth the program did not ask for. The collector
// therefore runs a full cycle once the live heap has grown by half since the
// last one, with a floor so consecutive full cycles cannot happen and a ceiling
// so a heap that never grows is still checked periodically.
constexpr size_t kMinimumMinorInterval = 2;
constexpr size_t kMaximumMinorInterval = 32;
alignas(nirq::nrq_cache_line_bytes) std::atomic<CollectionPhase> collection_phase{
    CollectionPhase::Idle};
bool active_collection_is_minor = false;
// Live bytes observed after the most recent full collection, and the kind of
// cycle the next automatic collection should run. Both are planned while the
// heap lock is held and published for the triggered collection to consume.
size_t live_heap_bytes_at_last_major = 0;
std::atomic<bool> next_cycle_is_major{false};
uint64_t active_cycle_epoch = 0;
std::atomic<uint64_t> next_cycle_epoch{1};
std::atomic<size_t> minor_collections_since_major{0};
// A trace callback may visit many references. Resolve them in a short
// contiguous batch after the callback returns instead of interleaving a page
// lookup with every callback invocation. The inline array keeps the hot path
// allocation-free; an oversized fan-out resolves the overflow immediately.
constexpr size_t kReferenceBatchCapacity = 128;
struct MarkContext {
  MarkWorklist worklist;
  bool young_only = false;
  // The cycle's birth epoch, copied from active_cycle_epoch when the context is
  // prepared. Every reference resolution compares against this copy instead of
  // the global, which keeps the marking path off shared state entirely.
  uint64_t epoch = 0;
  std::array<void*, kReferenceBatchCapacity> references{};
  size_t reference_count = 0;
};
MarkContext active_mark;
// During the sweep phase the snapshot list is detached from the allocation
// list. New allocations are therefore never inserted into a list the sweeper
// is traversing, and GC barriers can publish new marks between sweep slices.
ObjectHeader* sweep_list = nullptr;
// Old objects are not traced during a minor collection. This list records young
// objects referenced by old objects; the root-store/root-copy barriers append
// here, and a minor collection treats the list as additional roots. Entries are
// unique because an object's `remembered` flag gates insertion, so the list
// needs no hashing on the barrier path and scans one contiguous array.
std::vector<ObjectHeader*> remembered_young;
// A thread may buffer a reference to an object that is still in another
// mutator's unpublished TLAB. These raw addresses are retried after all TLABs
// are published at the next cycle boundary; they are never dereferenced.
std::unordered_set<void*> unresolved_remembered;
// While every live managed object is young, a young-to-young assignment cannot
// create an old-to-young edge, so the write barrier can skip remembered-set
// work. `generational_barrier_active` is raised for the duration of every
// collection cycle (because a concurrent promotion can make an object old while
// mutators run) and lowered after a cycle that leaves no old objects. This
// keeps the idle fast path allocation-only for young workloads without
// weakening generational tracking once an old generation exists.
std::atomic<bool> generational_barrier_active{false};
// Single published switch for the store barrier: true exactly when generational
// tracking is enabled and remembered-set recording is required. Mutators test
// this one acquire load instead of the two flags it is derived from. It is
// rewritten by refresh_store_barrier() wherever either input changes, and by the
// mutator that allocates the first object of an old generation.
std::atomic<bool> store_barrier_active{false};
std::atomic<size_t> old_object_count{0};

// Every writer of either input publishes the combined switch around its own flag
// store: raised before `generational_barrier_active`, lowered after it, and
// raised before generational collection is enabled. A mutator that observes a
// stale value therefore records remembered-set entries it did not have to, never
// skips one it needed.
void raise_store_barrier() {
  store_barrier_active.store(true, std::memory_order_release);
  generational_barrier_active.store(true, std::memory_order_release);
}

void lower_store_barrier() {
  generational_barrier_active.store(false, std::memory_order_release);
  store_barrier_active.store(false, std::memory_order_release);
}

uint64_t saturated_add(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

// `bytes_since_collection` has exactly one writer-path class - the collection
// and allocation code that runs under gc_mutex - so the update is a plain
// saturating load and store rather than a compare-exchange loop that can never
// retry. The trigger reads it without the lock and only needs a snapshot, which
// a relaxed store publishes.
void add_since_collection_locked(size_t amount) {
  const size_t current =
      allocation_accounting.bytes_since_collection.load(std::memory_order_relaxed);
  allocation_accounting.bytes_since_collection.store(
      amount > SIZE_MAX - current ? SIZE_MAX : current + amount,
      std::memory_order_relaxed);
}

// Fold a mutator's locally batched allocation bytes into the shared trigger
// counter. Callers hold gc_mutex.
void flush_mutator_accounting_locked(MutatorState& mutator) {
  if (mutator.unflushed_bytes == 0) return;
  add_since_collection_locked(mutator.unflushed_bytes);
  mutator.unflushed_bytes = 0;
}

void publish_tlab_locked(MutatorState& mutator) {
  ObjectHeader* header = mutator.pending_objects;
  while (header) {
    ObjectHeader* next = header->tlab_next;
    const bool indexed = page_uses_index(header->page);
    if (indexed) {
      if (!object_index.insert(header->payload(), header))
        nrq_runtime_fatal("GC failed to publish a thread-local allocation");
    } else {
      const size_t offset = static_cast<size_t>(
          static_cast<unsigned char*>(header->payload()) -
          header->page->storage);
      mark_object_start(*header->page, offset);
    }
    // The published state does not use the batch link: the pending list is
    // dropped as a whole, and only an object on a page without a start bitmap is
    // linked into `unindexed_objects`. Clearing the link here used to dirty one
    // header cache line per object, so the traversal paid a write-back for every
    // line it read; reuse is safe without it because the header is constructed
    // again when its storage is handed out.
    if (indexed) {
      header->next = unindexed_objects;
      unindexed_objects = header;
    }
    if (!header->page || header->page->live_objects == SIZE_MAX)
      nrq_runtime_fatal("GC page live-object count overflow");
    ++header->page->live_objects;
    if (header->young()) {
      ++header->page->young_objects;
    } else {
      // Objects become visible here, so this is where the generational policy's
      // old-object count is updated - once per published batch instead of once
      // per allocation, which keeps a locked instruction off the fast path.
      old_object_count.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t payload_bytes = object_payload_bytes(*header);
    statistics.allocated_bytes =
        saturated_add(statistics.allocated_bytes, payload_bytes);
    statistics.live_objects = saturated_add(statistics.live_objects, 1);
    live_heap_bytes = saturated_add(live_heap_bytes, payload_bytes);
    header = next;
  }
  flush_mutator_accounting_locked(mutator);
  mutator.pending_objects = nullptr;
  mutator.pending_bytes_local = 0;
  mutator.pending_bytes.store(0, std::memory_order_relaxed);
  mutator.pending_count_local = 0;
  mutator.pending_count.store(0, std::memory_order_relaxed);
}

bool collection_is_requested() {
  return collection_requested.load(std::memory_order_acquire);
}

void wait_until_not_requested(std::unique_lock<std::mutex>& lock) {
  mutator_condition.wait(lock, [] {
    return !collection_requested.load(std::memory_order_acquire);
  });
}

// Park the calling mutator and wait for the request to clear. The caller holds
// mutator_mutex through `lock`, and the count it publishes is what the collector's
// wait predicate observes; parking mutators do not wake each other.
void park_mutator(std::unique_lock<std::mutex>& lock) {
  thread_mutator.parked = true;
  parked_mutator_count.fetch_add(1, std::memory_order_release);
  collector_condition.notify_one();
  wait_until_not_requested(lock);
  parked_mutator_count.fetch_sub(1, std::memory_order_release);
  thread_mutator.parked = false;
}

bool all_mutators_parked() {
  const size_t self = thread_mutator.depth != 0 ? 1 : 0;
  const size_t required =
      registered_mutator_count >= self ? registered_mutator_count - self : 0;
  return parked_mutator_count.load(std::memory_order_acquire) >= required;
}

void stop_mutators() {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) {
    if (thread_mutator.depth != 0) {
      park_mutator(lock);
    } else {
      wait_until_not_requested(lock);
    }
  }
  // Mutators observe the request at their next safepoint poll, so raising it only
  // has to wake this collector once each mutator reports in.
  collection_requested.store(true, std::memory_order_release);
  collector_condition.wait(lock, [] { return all_mutators_parked(); });
}

void resume_mutators() {
  std::lock_guard<std::mutex> lock(mutator_mutex);
  collection_requested.store(false, std::memory_order_release);
  mutator_condition.notify_all();
}

// Enter a lock-free barrier region. Only a registered mutator can be counted and
// therefore only a registered mutator may run the fast path; a thread that never
// entered the runtime is invisible to the handshake, so it has to use the paths
// guarded by gc_mutex. The increment is on the calling thread's own line, which
// turns what used to be a contended read-modify-write on shared state into a
// private one.
// Both sides of a barrier region are two relaxed stores and one acquire load.
// Out of line they cost a call and a register save on each side of every rooted
// allocation, which is a large fraction of a bump allocation; inlined, the
// caller sees that the region only publishes the depth around its own work.
__forceinline bool begin_fast_barrier() {
  if (thread_mutator.depth == 0) return false;
  // The counter lives on this thread's own cache line: only its owner writes it
  // and only the collector reads it, so no other thread can race the update.
  // A relaxed store plus an acquire-release fence is therefore equivalent to the
  // read-modify-write it replaces - the increment is ordered before everything
  // the region does, and the region's work is ordered before the decrement -
  // without a locked instruction. On x64 the fence is a compiler barrier only,
  // so entering a region costs a plain store instead of `lock xadd`.
  const uint32_t previous =
      thread_mutator.barrier_depth.load(std::memory_order_relaxed);
  thread_mutator.barrier_depth.store(previous + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acq_rel);
  if (collection_phase.load(std::memory_order_acquire) ==
      CollectionPhase::Idle)
    return true;
  std::atomic_thread_fence(std::memory_order_acq_rel);
  thread_mutator.barrier_depth.store(previous, std::memory_order_relaxed);
  return false;
}

__forceinline void end_fast_barrier() {
  const uint32_t previous =
      thread_mutator.barrier_depth.load(std::memory_order_relaxed);
  // Release the region's stores before the decrement becomes visible, so a
  // collector that observes a zero count has also observed the work.
  std::atomic_thread_fence(std::memory_order_acq_rel);
  thread_mutator.barrier_depth.store(previous - 1, std::memory_order_relaxed);
}

// Wait until every other registered mutator has left its barrier regions. The
// collector publishes Preparing first, so a mutator that observes the phase
// change retreats to the serialized path and never enters a new region; the loop
// only has to wait out the regions that were already in flight, and a mutator
// inside one finishes it before it can reach a safepoint.
//
// The calling thread's own entry is skipped. It can only be non-zero when the caller
// is itself inside a region, and waiting for it would spin until the caller's own
// region ended - which cannot happen, because the caller is this thread and it is
// busy waiting. Its region cannot be in flight against the enumeration the caller is
// about to perform either: one thread runs its stores and its enumeration in
// sequence, and the region ends before the next store.
void wait_for_fast_barriers() {
  if (heap_lock_held_by_this_thread)
    nrq_runtime_fatal(
        "GC lock order violated: heap lock held while waiting for barrier regions");
  for (;;) {
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(mutator_mutex);
      for (const MutatorState* mutator : mutators) {
        if (mutator && mutator != &thread_mutator &&
            mutator->barrier_depth.load(std::memory_order_acquire) != 0) {
          busy = true;
          break;
        }
      }
    }
    if (!busy) return;
    std::this_thread::yield();
  }
}

void trace_children(ObjectHeader* header, nrq_gc_visit_fn visit,
                    void* context) {
  // The descriptor is read once and holds the whole tracing contract, so the
  // common path is one dependent load rather than one per object field.
  const nrq_object_layout* layout = header->layout;
  if (!layout) nrq_runtime_fatal("GC object has no layout descriptor");
  const size_t payload_bytes = object_payload_bytes(*header);
  if (layout->trace)
    layout->trace(header->payload(), payload_bytes, visit, context);
  // The reference map lives in the descriptor: one table for all instances,
  // already proven to fit one element, so the walk needs one bounds test per
  // base rather than one per offset.
  if (layout->reference_count == 0) return;
  const size_t stride = (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY)
                            ? layout->payload_size
                            : payload_bytes;
  if (stride == 0) return;
  const auto* bytes = static_cast<const unsigned char*>(header->payload());
  for (size_t base = 0; base + stride <= payload_bytes; base += stride) {
    for (uint32_t offset_index = 0; offset_index < layout->reference_count;
         ++offset_index) {
      const size_t offset = layout->reference_offsets[offset_index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* child = nullptr;
      std::memcpy(&child, bytes + base + offset, sizeof(child));
      visit(child, context);
    }
  }
}

void enqueue_header_locked(ObjectHeader* header, MarkContext& context) {
  if (!header) return;
  // One read of the state word answers both flag questions. The word is read
  // before the epoch test because the compile-time filters are the common case
  // for an already-marked object, and the store below only happens once the
  // object is known to be entering the worklist.
  const uint32_t state = header->state_word();
  if ((state & ObjectHeader::kMarkedBit) != 0) return;
  // Objects born during this cycle are retained by the sweep's birth-epoch
  // check. Marking them would leave a mark bit that the snapshot never clears.
  // The epoch comes from the context, so the hot path never loads the shared
  // cycle state.
  if (header->birth_epoch >= context.epoch) return;
  if (context.young_only && (state & ObjectHeader::kYoungBit) == 0) return;
  header->set_marked(true);
  context.worklist.push(header);
}

void resolve_reference_locked(void* object, MarkContext& context) {
  if (!object) return;
  ObjectHeader* found = find_object_locked(object);
  // Roots may refer to static/native objects. Only collector-owned objects
  // enter the intrusive mark stack, so marking never allocates auxiliary heap.
  enqueue_header_locked(found, context);
}

void flush_reference_batch_locked(MarkContext& context) {
  for (size_t index = 0; index < context.reference_count; ++index)
    resolve_reference_locked(context.references[index], context);
  context.reference_count = 0;
}

void enqueue_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* context = static_cast<MarkContext*>(opaque_context);
  if (context->reference_count < kReferenceBatchCapacity) {
    context->references[context->reference_count++] = object;
    return;
  }
  resolve_reference_locked(object, *context);
}

void mark_value_locked(void* object) {
  // Barriers resolve immediately: they run between marking slices and must be
  // visible before the collector can decide that marking is complete.
  if (!object || collection_phase.load(std::memory_order_relaxed) ==
                     CollectionPhase::Idle)
    return;
  resolve_reference_locked(object, active_mark);
}

void mark_root_values_locked(void* storage, const RootInfo& root) {
  const nrq_object_layout* layout = root.layout;
  if (!layout || layout->reference_count == 0) return;
  auto* bytes = static_cast<unsigned char*>(storage);
  const size_t stride = layout->payload_size;
  const size_t elements = root.repeated_count != 0 ? root.repeated_count : 1;
  for (size_t element = 0; element < elements; ++element) {
    for (uint32_t index = 0; index < layout->reference_count; ++index) {
      const size_t offset = layout->reference_offsets[index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* reference = nullptr;
      std::memcpy(&reference, bytes + element * stride + offset,
                  sizeof(reference));
      mark_value_locked(reference);
    }
  }
}

struct ReferenceCountTarget {
  void* object = nullptr;
  uint64_t count = 0;
};

// The diagnostic API asks about one object at a time. Comparing each visited
// reference against that target avoids building a map of every object in the
// heap and keeps the walk allocation-free.
void count_reference_to_target(void* object, void* opaque_context) {
  if (!object) return;
  auto* target = static_cast<ReferenceCountTarget*>(opaque_context);
  if (object == target->object)
    target->count = saturated_add(target->count, 1);
}

void enqueue_root_locked(void* root_storage, const RootInfo& root,
                         MarkContext& context) {
  const nrq_object_layout* layout = root.layout;
  if (!layout || layout->reference_count == 0) return;
  auto* bytes = static_cast<const unsigned char*>(root_storage);
  const size_t stride = layout->payload_size;
  const size_t elements = root.repeated_count != 0 ? root.repeated_count : 1;
  for (size_t element = 0; element < elements; ++element) {
    const unsigned char* element_bytes = bytes + element * stride;
    for (uint32_t index = 0; index < layout->reference_count; ++index) {
      const size_t offset = layout->reference_offsets[index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* reference = nullptr;
      std::memcpy(&reference, element_bytes + offset, sizeof(reference));
      enqueue_reference(reference, &context);
    }
  }
}

void enqueue_roots_locked(MarkContext& context) {
  for (const auto& root : roots)
    enqueue_root_locked(root.first, root.second, context);
  // Local roots are owned by their threads. The mutex guards the vector while
  // the collector copies a consistent view; ordinary registration does not take
  // gc_mutex, so collection never blocks the whole world to add a stack root.
  for (ShadowStack* stack : shadow_stacks) {
    if (!stack) continue;
    std::lock_guard<std::mutex> stack_lock(stack->mutex);
    for (const ShadowRoot& root : stack->roots)
      enqueue_root_locked(root.slot, root.info, context);
  }
  for (const auto& pinned : pinned_objects) {
    if (pinned.second != 0) enqueue_reference(pinned.first, &context);
  }
  if (context.young_only) {
    for (ObjectHeader* header : remembered_young)
      enqueue_header_locked(header, context);
  }
  flush_reference_batch_locked(context);
}

// Append a young object to the remembered set unless its flag says it is
// already there. The flag is what makes the flat list a set, so a repeated
// old-to-young store costs one bool test instead of a hash probe.
void add_remembered_locked(ObjectHeader* header) {
  if (!header) return;
  // The flag is the deduplication test, so it is one masked read of the word.
  if ((header->state_word() & ObjectHeader::kRememberedBit) != 0) return;
  header->set_remembered(true);
  try {
    remembered_young.push_back(header);
  } catch (...) {
    header->set_remembered(false);
    nrq_runtime_fatal("GC remembered-set insertion failed");
  }
}

// Drop entries whose object was reclaimed or promoted since insertion. The list
// is only traversed between this point and the end of the cycle, so a stale
// entry is never dereferenced; compaction also releases the vector's capacity
// when the remembered set shrinks permanently.
void compact_remembered_locked() {
  remembered_young.erase(
      std::remove_if(remembered_young.begin(), remembered_young.end(),
                     [](ObjectHeader* header) {
                       return !header || !header->remembered();
                     }),
      remembered_young.end());
  if (remembered_young.capacity() > 1024 &&
      remembered_young.size() * 4 < remembered_young.capacity())
    std::vector<ObjectHeader*>(remembered_young.begin(), remembered_young.end())
        .swap(remembered_young);
}

void remember_reference(void* object, void* opaque_context) {
  (void)opaque_context;
  if (!object) return;
  ObjectHeader* found = find_object_locked(object);
  if (!found || !found->young()) return;
  add_remembered_locked(found);
}

void remember_young_locked(void* object) {
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      !generational_barrier_active.load(std::memory_order_acquire) ||
      !object)
    return;
  ObjectHeader* found = find_object_locked(object);
  if (found && found->young()) add_remembered_locked(found);
}

void remember_children_locked(ObjectHeader* header) {
  trace_children(header, remember_reference, nullptr);
}

void defer_remembered_locked(void* object) {
  if (!object || !generational_barrier_active.load(std::memory_order_acquire))
    return;
  ObjectHeader* found = find_object_locked(object);
  if (found && found->young()) {
    add_remembered_locked(found);
    return;
  }
  if (!found) {
    try {
      unresolved_remembered.insert(object);
    } catch (...) {
      nrq_runtime_fatal("GC remembered-set overflow failed");
    }
  }
}

void drain_remembered_locked(MutatorState& mutator, bool defer_unresolved) {
  for (size_t index = 0; index < mutator.remembered_count; ++index) {
    void* object = mutator.remembered[index];
    if (!object) continue;
    ObjectHeader* found = find_object_locked(object);
    if (found && found->young()) {
      add_remembered_locked(found);
    } else if (!found && defer_unresolved) {
      try {
        unresolved_remembered.insert(object);
      } catch (...) {
        nrq_runtime_fatal("GC remembered-set overflow failed");
      }
    }
  }
  mutator.remembered_count = 0;
}

ThreadGcStateCleanup::~ThreadGcStateCleanup() {
  if (!armed) return;
  ShadowStack* stack = thread_shadow_stack;
  // Match the mutator_leave lock order (mutator_mutex then gc_mutex) so a
  // thread that exits while another thread is leaving cannot deadlock.
  std::lock_guard<std::mutex> mutator_lock(mutator_mutex);
  if (thread_mutator.depth != 0) {
    {
      HeapLockGuard heap_lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
      drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
      drain_transfer_pins_locked(thread_mutator);
    }
    auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
    if (found != mutators.end()) {
      mutators.erase(found);
      --registered_mutator_count;
      collector_condition.notify_all();
    }
    thread_mutator.depth = 0;
    thread_mutator.parked = false;
    mutator_condition.notify_all();
  }
  if (stack) {
    // Remove the stack under gc_mutex: enqueue_roots_locked enumerates the
    // registry under the same lock, so the collector can no longer reach it.
    HeapLockGuard heap_lock(gc_mutex);
    auto found = std::find(shadow_stacks.begin(), shadow_stacks.end(), stack);
    if (found != shadow_stacks.end()) shadow_stacks.erase(found);
    thread_shadow_stack = nullptr;
  }
  delete stack;
}

void resolve_unresolved_remembered_locked() {
  for (void* object : unresolved_remembered) {
    ObjectHeader* found = find_object_locked(object);
    if (!found || !found->young()) continue;
    add_remembered_locked(found);
  }
  unresolved_remembered.clear();
}

void buffer_remembered(void* object);

// Append a referent to the thread's remembered buffer. The caller has already
// checked the published barrier switch, so this is the only work a barrier does.
void append_remembered(void* object) {
  if (!object) return;
  MutatorState& mutator = thread_mutator;
  if (mutator.remembered_count < kRememberedBufferCapacity) {
    mutator.remembered[mutator.remembered_count++] = object;
    return;
  }
  HeapLockGuard lock(gc_mutex);
  publish_tlab_locked(mutator);
  drain_remembered_locked(mutator, /*defer_unresolved=*/true);
  defer_remembered_locked(object);
}

void buffer_pointer_words(void* storage, size_t bytes) {
  // One acquire load covers the whole store: the switch is read here instead of
  // once per machine word. A switch that drops mid-loop only leaves extra
  // entries, which the next drain discards unless they name young objects.
  if (!store_barrier_active.load(std::memory_order_acquire)) return;
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    append_remembered(reference);
  }
}

void buffer_remembered(void* object) {
  if (!store_barrier_active.load(std::memory_order_acquire)) return;
  append_remembered(object);
}

// The ordinary record carries one or two pointer fields and a single element, and
// its layout is already in the header. For that shape the trace callback adds
// nothing: it walks the same two words the marker can read here, through an
// indirect call per object. Anything else - a callback, a typed array, a wider
// layout - still goes through trace_children.
void enqueue_narrow_children_locked(ObjectHeader* header, MarkContext& context) {
  const auto* bytes = static_cast<const unsigned char*>(header->payload());
  const nrq_object_layout* layout = header->layout;
  for (uint32_t index = 0; index < layout->reference_count; ++index) {
    const size_t offset = layout->reference_offsets[index];
    if (offset > object_payload_bytes(*header) ||
        sizeof(void*) > object_payload_bytes(*header) - offset)
      continue;
    void* child = nullptr;
    std::memcpy(&child, bytes + offset, sizeof(child));
    if (child) enqueue_reference(child, &context);
  }
}

bool mark_slice_locked(size_t budget_bytes) {
  size_t touched = 0;
  while (!active_mark.worklist.empty() && touched < budget_bytes) {
    ObjectHeader* header = active_mark.worklist.pop();
    touched += object_payload_bytes(*header) + sizeof(ObjectHeader);
    if (active_mark.young_only && !header->young()) continue;
    const size_t reference_count = header->layout->reference_count;
    if (!header->layout->trace && reference_count != 0 && reference_count <= 2 &&
        (header->layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) {
      enqueue_narrow_children_locked(header, active_mark);
    } else {
      trace_children(header, enqueue_reference, &active_mark);
    }
    flush_reference_batch_locked(active_mark);
  }
  flush_reference_batch_locked(active_mark);
  return active_mark.worklist.empty() && active_mark.reference_count == 0;
}

// Sweep state. A cycle reclaims along the page set instead of following one
// global list: pages are visited in allocation order and each page's start
// bitmap in address order, which is also the order a page's spans want to be
// released in, so each freed run extends the recorded one with no ordered
// insert. Objects whose page has no walkable bitmap - a large-object page, or a
// page the directory refused - keep the detached list and are swept after the
// pages.
struct SweepCursor {
  size_t page = 0;
  size_t page_limit = 0;
  size_t word = 0;
  // Set bits of `word` that this sweep has not examined yet. A bit position is
  // not enough: resuming mid-word by re-testing positions would visit every
  // zero bit between object starts, which is 94 % of a full page's bitmap.
  uint64_t mask = 0;
};
SweepCursor sweep_cursor;

bool sweep_chunk_locked(size_t budget_bytes) {
  size_t steps = 0;
  size_t touched = 0;
  size_t bitmap_words = 0;
  size_t empty_words = 0;
  std::chrono::steady_clock::time_point chunk_start;
  if constexpr (nirq::runtime_debug_enabled)
    chunk_start = std::chrono::steady_clock::now();
  // Promotion and reclamation both move objects between generations. The
  // generation counters used to be updated with one atomic per object, which
  // meant two shared read-modify-write operations for every survivor and one for
  // every reclaimed old object. They are accumulated here and applied once per
  // chunk; every other reader of these counters holds gc_mutex, which this
  // function holds for the whole chunk, so no one can observe the intermediate
  // value.
  size_t reclaimed_old = 0;
  size_t promoted_young = 0;
  bool promotion_raised_barrier = false;
  // Reclaims one object, or leaves it for the next cycle. Returns true when the
  // object was freed, so the caller cannot go on to treat it as a survivor.
  auto sweep_object = [&](ObjectHeader* header) -> bool {
    const bool collectable = !active_collection_is_minor || header->young();
    if (!header->marked() && collectable) {
      const size_t object_offset = static_cast<size_t>(
          static_cast<unsigned char*>(header->payload()) -
          header->page->storage);
      if (page_uses_index(header->page)) {
        object_index.erase(header->payload());
      } else {
        clear_object_start(*header->page, object_offset);
      }
      // The object's storage becomes reusable space in its own page instead of
      // being lost to the bump cursor.
      release_span_locked(*header->page,
                          object_offset - sizeof(ObjectHeader),
                          object_span_bytes(object_payload_bytes(*header)));
      if (!header->young()) ++reclaimed_old;
      statistics.collected_bytes = saturated_add(
          statistics.collected_bytes,
          static_cast<uint64_t>(object_payload_bytes(*header)));
      if (statistics.live_objects != 0) --statistics.live_objects;
      const size_t payload_bytes = object_payload_bytes(*header);
      live_heap_bytes = live_heap_bytes > payload_bytes
                            ? live_heap_bytes - payload_bytes
                            : 0;
      if (!header->page || header->page->live_objects == 0)
        nrq_runtime_fatal("GC page live-object count underflow");
      --header->page->live_objects;
      if (header->young()) --header->page->young_objects;
      if (header->page->live_objects == 0) {
        header->page->retired = true;
        ++retired_page_count;
      }
      destroy_object_storage(header);
      return true;
    }

    if (header->marked()) {
      // Everything the survivor needs to become one word: the mark is always
      // cleared, and an aging or promoting object also rewrites its age. The
      // fields left untouched keep their bits, which a mask guarantees and a
      // sequence of separate flag stores cannot.
      uint32_t survived = header->state_word() & ~ObjectHeader::kMarkedBit;
      const bool was_young =
          (survived & ObjectHeader::kYoungBit) != 0;
      if (active_collection_is_minor) {
        if (was_young) {
          uint32_t age = (survived & ObjectHeader::kAgeMask) >>
                         ObjectHeader::kAgeShift;
          if (age < UINT8_MAX) ++age;
          if (age >= kPromotionAge) {
            survived &= ~(ObjectHeader::kYoungBit | ObjectHeader::kAgeMask |
                          ObjectHeader::kRememberedBit);
            header->set_state(survived);
            --header->page->young_objects;
            ++promoted_young;
            promotion_raised_barrier = true;
            remember_children_locked(header);
            debug_emit(NRQ_GC_EVENT_PROMOTE, 0, 0,
                       static_cast<void*>(header + 1));
            return false;
          }
          survived = (survived & ~ObjectHeader::kAgeMask) |
                     (age << ObjectHeader::kAgeShift);
        }
        header->set_state(survived);
      } else {
        if (was_young) {
          --header->page->young_objects;
          ++promoted_young;
          promotion_raised_barrier = true;
        }
        survived &= ~(ObjectHeader::kYoungBit | ObjectHeader::kAgeMask |
                      ObjectHeader::kRememberedBit);
        header->set_state(survived);
        if (was_young)
          debug_emit(NRQ_GC_EVENT_PROMOTE, 0, 0, static_cast<void*>(header + 1));
      }
    }
    return false;
  };

  // Phase 1: pages that can be walked by their own start bitmap. Only set bits
  // are object starts, so each iteration consumes one set bit; zero bits are
  // never visited. The remaining mask is part of the cursor, which is what
  // makes a chunk that stops mid-word resume without re-testing anything.
  while (sweep_cursor.page < sweep_cursor.page_limit &&
         touched < budget_bytes &&
         bitmap_words < kSweepBitmapWordsPerChunk) {
    GcPage* page = gc_pages[sweep_cursor.page].get();
    if (!page || page_uses_index(page) || page->object_start_words == 0 ||
        page->live_objects == 0 ||
        (active_collection_is_minor && page->young_objects == 0)) {
      // A page transition is work even when it examines no object: an empty
      // page still has to be reached, tested and stepped over. Charge its whole
      // bitmap so a run of empty pages cannot outlast the word budget.
      bitmap_words += (page && page->object_start_words != 0)
                          ? page->object_start_words
                          : size_t{1};
      ++sweep_cursor.page;
      sweep_cursor.word = 0;
      sweep_cursor.mask = 0;
      continue;
    }
    if (sweep_cursor.mask == 0) {
      if (sweep_cursor.word >= page->object_start_words) {
        bitmap_words += page->object_start_words;
        ++sweep_cursor.page;
        sweep_cursor.word = 0;
        continue;
      }
      // Snapshot the word. A mutator may set a start bit between slices, but
      // that object's birth epoch is past this cycle and it is skipped below.
      // Only this sweep clears start bits, and only for objects it has already
      // consumed, so the snapshot cannot resurrect or lose one of those.
      sweep_cursor.mask = page->object_starts[sweep_cursor.word];
      ++bitmap_words;
      if (sweep_cursor.mask == 0) {
        ++empty_words;
        ++sweep_cursor.word;
        continue;
      }
    }
    const size_t word_index = sweep_cursor.word;
    const uint64_t mask = sweep_cursor.mask;
    const unsigned index = static_cast<unsigned>(std::countr_zero(mask));
    // Consume the bit before the object can clear it, and advance the word as
    // soon as it is empty. The cursor must be committed before any `continue`.
    sweep_cursor.mask = mask & (mask - uint64_t{1});
    if (sweep_cursor.mask == 0) ++sweep_cursor.word;
    const size_t slot = word_index * 64 + index;
    const size_t payload_offset = slot * kObjectStartAlignment;
    auto* header =
        reinterpret_cast<ObjectHeader*>(page->storage + payload_offset) - 1;
    ++steps;
    touched += object_span_bytes(object_payload_bytes(*header));
    // An object allocated after the cycle started is outside the snapshot's
    // reachability analysis. It stays where it is for this cycle - there is no
    // list to keep it out of - and the next cycle considers it.
    if (header->birth_epoch >= active_cycle_epoch) {
      header->set_marked(false);
      continue;
    }
    sweep_object(header);
  }

  // Phase 2: objects whose page has no walkable start bitmap.
  while (sweep_list && touched < budget_bytes) {
    ObjectHeader* header = sweep_list;
    sweep_list = header->next;
    ++steps;
    touched += object_span_bytes(object_payload_bytes(*header));
    if (header->birth_epoch >= active_cycle_epoch) {
      header->set_marked(false);
      header->next = unindexed_objects;
      unindexed_objects = header;
      continue;
    }
    if (sweep_object(header)) continue;
    header->next = unindexed_objects;
    unindexed_objects = header;
  }

  if (reclaimed_old != 0) {
    const size_t current = old_object_count.load(std::memory_order_relaxed);
    old_object_count.store(current > reclaimed_old ? current - reclaimed_old : 0,
                           std::memory_order_relaxed);
  }
  if (promoted_young != 0) {
    const size_t current = old_object_count.load(std::memory_order_relaxed);
    old_object_count.store(
        current > SIZE_MAX - promoted_young ? SIZE_MAX : current + promoted_young,
        std::memory_order_relaxed);
  }
  // One release store per chunk is enough: the flag only has to be visible
  // before any mutator can publish an old-to-young edge, and mutators cannot run
  // new statements while this chunk holds the heap lock.
  if (promotion_raised_barrier) raise_store_barrier();
  debug_emit(NRQ_GC_EVENT_SWEEP, static_cast<uint32_t>(reclaimed_old), steps,
             nullptr);
  debug_emit(NRQ_GC_EVENT_SWEEP_WORK, static_cast<uint32_t>(steps),
             (static_cast<uint64_t>(bitmap_words) << 32) | empty_words,
             nullptr);
  if constexpr (nirq::runtime_debug_enabled) {
    const auto elapsed = std::chrono::steady_clock::now() - chunk_start;
    debug_emit(
        NRQ_GC_EVENT_SWEEP_CHUNK_NS, 0,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count()),
        nullptr);
  }
  return sweep_list == nullptr && sweep_cursor.page >= sweep_cursor.page_limit;
}

// Largest run currently recorded in a page, and how many runs have that size.
// The count lets a take keep the maximum exact in constant time while another
// run of the same size remains; only the last one forces a rescan. The debug
// build re-derives both so a missed update fails the test suite instead of
// silently sending refills down the wrong path.
size_t largest_free_run(const GcPage& page, size_t& count) {
  size_t largest = 0;
  count = 0;
  for (size_t index = 0; index < page.free_span_count; ++index) {
    const size_t span = page.free_spans[index].span;
    if (span > largest) {
      largest = span;
      count = 1;
    } else if (span == largest) {
      ++count;
    }
  }
  return largest;
}

void set_page_max_free_span_locked(GcPage& page, size_t span, size_t count) {
  if constexpr (nirq::runtime_debug_enabled) {
    size_t actual_count = 0;
    const size_t actual = largest_free_run(page, actual_count);
    if (actual != span || actual_count != count)
      nrq_runtime_fatal("GC free-run size index is inconsistent");
  }
  const bool was_slab = page.max_free_span >= kGcTlabMinSlabBytes;
  page.max_free_span = span;
  page.max_free_span_count = count;
  const bool is_slab = span >= kGcTlabMinSlabBytes;
  if (was_slab == is_slab) return;
  if (is_slab) {
    ++pages_with_slab_run;
  } else {
    if (pages_with_slab_run == 0)
      nrq_runtime_fatal("GC slab-run page count underflow");
    --pages_with_slab_run;
  }
}

void refresh_page_max_free_span_locked(GcPage& page) {
  size_t count = 0;
  const size_t largest = largest_free_run(page, count);
  set_page_max_free_span_locked(page, largest, count);
}

// Account for one run changing from `before` bytes to `after` bytes (0 means it
// was inserted or removed). Maintaining the count of maximum-sized runs is what
// keeps this O(1): only the disappearance of the last run of the maximum size
// needs the run list walked again.
void adjust_page_max_free_span_locked(GcPage& page, size_t before,
                                      size_t after) {
  const size_t maximum = page.max_free_span;
  if (before == maximum && before != 0) {
    if (after > maximum) {
      set_page_max_free_span_locked(page, after, 1);
    } else if (after < maximum) {
      if (page.max_free_span_count > 1) {
        set_page_max_free_span_locked(page, maximum,
                                      page.max_free_span_count - 1);
      } else {
        refresh_page_max_free_span_locked(page);
      }
    }
    return;
  }
  // The changed run was below the maximum, so it can only raise it or join it.
  if (after > maximum) {
    set_page_max_free_span_locked(page, after, 1);
  } else if (after != 0 && after == maximum) {
    set_page_max_free_span_locked(page, maximum, page.max_free_span_count + 1);
  }
}

// Record space an object used to occupy so a later allocation can reuse it.
// Callers hold gc_mutex. A page whose span list is full keeps the space
// unusable, which is exactly the behaviour of the bump allocator.
//
// INVARIANT: `free_spans` is ascending by offset, runs never overlap, and
// `free_span_bytes` is exactly the sum of the runs. Every operation in this file
// depends on it - insertion finds its position by offset, first-fit scans from
// the lowest offset, and the run taken for a TLAB is picked from the head. A
// removal therefore has to close the hole by shifting the tail, never by moving
// the last entry into it: the last entry has the highest offset, and putting it
// in the middle leaves the array unsorted, after which adjacent runs stop
// merging and the page fragments until it can no longer serve a reservation.
void release_span_locked(GcPage& page, size_t offset, size_t span) {
  if (span == 0) return;
  // A run the representation cannot describe would be recorded as a negative
  // length; such a page simply keeps behaving like the bump allocator it was.
  if (offset > UINT32_MAX || span > UINT32_MAX) return;
  // Runs are kept ordered by offset and always merged with a neighbour they
  // touch. Sweeping visits objects in list order, not address order, so a run
  // that is freed in pieces has to coalesce back into one region; otherwise the
  // largest reusable block in a page would be a single object.
  size_t index = 0;
  while (index < page.free_span_count && page.free_spans[index].offset < offset)
    ++index;
  if (index > 0) {
    GcPage::FreeSpan& previous = page.free_spans[index - 1];
    if (previous.offset + previous.span == offset) {
      const size_t previous_before = previous.span;
      size_t next_before = 0;
      previous.span += span;
      page.free_span_bytes += span;
      if (index < page.free_span_count &&
          previous.offset + previous.span == page.free_spans[index].offset) {
        next_before = page.free_spans[index].span;
        previous.span += next_before;
        for (size_t move = index; move + 1 < page.free_span_count; ++move)
          page.free_spans[move] = page.free_spans[move + 1];
        --page.free_span_count;
        --total_free_spans;
      }
      adjust_page_max_free_span_locked(page, previous_before, previous.span);
      if (next_before != 0)
        adjust_page_max_free_span_locked(page, next_before, 0);
      return;
    }
  }
  if (index < page.free_span_count &&
      offset + span == page.free_spans[index].offset) {
    GcPage::FreeSpan& next = page.free_spans[index];
    const size_t before = next.span;
    next.offset = offset;
    next.span += span;
    page.free_span_bytes += span;
    adjust_page_max_free_span_locked(page, before, next.span);
    return;
  }
  if (page.free_span_count >= GcPage::kMaxFreeSpans) return;
  for (size_t move = page.free_span_count; move > index; --move)
    page.free_spans[move] = page.free_spans[move - 1];
  page.free_spans[index] = GcPage::FreeSpan{
      static_cast<uint32_t>(offset), static_cast<uint32_t>(span)};
  ++page.free_span_count;
  ++total_free_spans;
  page.free_span_bytes += span;
  adjust_page_max_free_span_locked(page, 0, span);
  index_span_page_locked(page);
}

// A reservation is charged to its page in full, so ending it part way through an
// object leaves a remainder that no allocation of that size can use - up to a
// whole slab for a payload close to the slab size. Grants are therefore cut to a
// whole number of objects, and everything past the last one stays free space in
// the page, where a smaller object can still have it.
size_t whole_object_grant(size_t grant, size_t span) {
  if (span == 0) return grant;
  const size_t whole = (grant / span) * span;
  return whole != 0 ? whole : grant;
}
// First-fit reuse of reclaimed space. The scan visits pages in allocation order
// and only pages that currently hold a free run, so a heap with nothing to reuse
// costs one branch per page. Callers hold gc_mutex.
GcPage* take_free_span_locked(size_t span, size_t& offset) {
  if (span == 0 || total_free_spans == 0) return nullptr;
  for (GcPage* page : span_pages) {
    if (!page || page->free_span_count == 0 || page->max_free_span < span)
      continue;
    for (size_t index = 0; index < page->free_span_count; ++index) {
      GcPage::FreeSpan& candidate = page->free_spans[index];
      if (candidate.span < span) continue;
      const size_t candidate_before = candidate.span;
      unretire_page_locked(*page);
      offset = candidate.offset;
      page->free_span_bytes -= span;
      if (candidate.span > span) {
        // The remainder stays a run in the same page. `span` is a multiple of
        // the payload alignment, so the new start keeps that alignment.
        candidate.offset += span;
        candidate.span -= span;
        adjust_page_max_free_span_locked(*page, candidate_before,
                                         candidate.span);
      } else {
        // Runs are ordered by offset, so removal shifts the tail down instead of
        // swapping the last run into the hole.
        for (size_t move = index; move + 1 < page->free_span_count; ++move)
          page->free_spans[move] = page->free_spans[move + 1];
        --page->free_span_count;
        --total_free_spans;
        // A page with no recorded run can never answer a request again until
        // something is freed into it, so it leaves the index with its last run.
        if (page->free_span_count == 0) unindex_span_page_locked(*page);
        adjust_page_max_free_span_locked(*page, candidate_before, 0);
      }
      recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
      reused_span_bytes = saturated_add(reused_span_bytes, span);
      return page;
    }
  }
  return nullptr;
}

// First-fit reuse for a thread-local reservation. `take_free_span_locked` takes
// exactly the span it is asked for, which is right for one object and wrong for a
// TLAB: a sweep merges a dead page into one large run, and carving it into 4 KB
// pieces re-fragmented the page, forced a reservation every few dozen objects,
// and left later reservations to open fresh pages - which grew the heap cycle
// after cycle and made sweeping cost more each round. This takes the whole run
// when it is no larger than `desired`, so one refill consumes one page's run
// instead of sixteen, and it still accepts a shorter run as long as it can serve
// one object. `granted` reports the length actually taken.
// Callers hold gc_mutex.
GcPage* take_free_run_locked(size_t desired, size_t minimum, size_t multiple,
                             size_t& offset, size_t& granted) {
  if (desired == 0 || minimum == 0 || minimum > desired ||
      total_free_spans == 0)
    return nullptr;
  for (GcPage* page : span_pages) {
    if (!page || page->free_span_count == 0 || page->max_free_span < minimum)
      continue;
    for (size_t index = 0; index < page->free_span_count; ++index) {
      GcPage::FreeSpan& candidate = page->free_spans[index];
      if (candidate.span < minimum) continue;
      const size_t candidate_before = candidate.span;
      const size_t span = whole_object_grant(
          std::min(static_cast<size_t>(candidate.span), desired), multiple);
      if (span < minimum) continue;
      unretire_page_locked(*page);
      offset = candidate.offset;
      page->free_span_bytes -= span;
      if (candidate.span > span) {
        // `span` is a multiple of the payload alignment, so the remainder keeps
        // both its alignment and its position in the ordered run list.
        candidate.offset += span;
        candidate.span -= span;
        adjust_page_max_free_span_locked(*page, candidate_before,
                                         candidate.span);
      } else {
        for (size_t move = index; move + 1 < page->free_span_count; ++move)
          page->free_spans[move] = page->free_spans[move + 1];
        --page->free_span_count;
        --total_free_spans;
        if (page->free_span_count == 0) unindex_span_page_locked(*page);
        adjust_page_max_free_span_locked(*page, candidate_before, 0);
      }
      recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
      reused_span_bytes = saturated_add(reused_span_bytes, span);
      granted = span;
      return page;
    }
  }
  return nullptr;
}
// Reserves a batch of runs from the first page in the index that can serve the
// request, in one pass over that page's run list. Taking runs one at a time made
// the list maintenance the cost of a fragmented refill: every take closed its gap
// by shifting the tail of an array that can hold 96 runs, so handing out a page's
// holes one by one moved ~24x more entries than it handed out bytes. Here the
// runs come off the top of the list, a partial take only moves its own start, and
// the removals are compacted in a single pass afterwards.
//
// Callers hold gc_mutex. `out` must have room for `max_runs` entries. Returns how
// many runs were written and, in `page`, the page they came from - a batch is one
// page's holes, which keeps the space a thread reserves in one place.
size_t take_free_runs_locked(size_t minimum, size_t byte_budget, size_t max_runs,
                             size_t multiple, GcPage*& page,
                             GcPage::FreeSpan* out) {
  page = nullptr;
  if (minimum == 0 || max_runs == 0 || total_free_spans == 0) return 0;
  GcPage* target = nullptr;
  for (GcPage* candidate : span_pages) {
    if (!candidate || candidate->free_span_count == 0 ||
        candidate->max_free_span < minimum)
      continue;
    target = candidate;
    break;
  }
  if (!target) return 0;
  unretire_page_locked(*target);

  size_t taken = 0;
  size_t taken_bytes = 0;
  bool removed_any = false;
  size_t index = target->free_span_count;
  while (index != 0 && taken < max_runs) {
    --index;
    GcPage::FreeSpan& run = target->free_spans[index];
    if (run.span < minimum) continue;
    const size_t grant = whole_object_grant(
        std::min(static_cast<size_t>(run.span), kGcTlabBytes), multiple);
    if (grant < minimum) continue;
    if (taken != 0 && taken_bytes + grant > byte_budget) break;
    // A run is taken from its top, so a partial take only moves the run's own
    // start and the entry keeps its place in the ascending list.
    out[taken].offset = run.offset + (run.span - grant);
    out[taken].span = grant;
    ++taken;
    taken_bytes += grant;
    target->free_span_bytes -= grant;
    recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
    if (grant == run.span) {
      run.span = 0;  // tombstone: compacted below, never observed elsewhere
      removed_any = true;
      --total_free_spans;
    } else {
      run.span -= grant;
    }
  }
  if (removed_any) {
    size_t write = 0;
    for (size_t read = 0; read < target->free_span_count; ++read) {
      if (target->free_spans[read].span == 0) continue;
      if (write != read) target->free_spans[write] = target->free_spans[read];
      ++write;
    }
    target->free_span_count = write;
    if (write == 0) unindex_span_page_locked(*target);
  }
  if (taken != 0) refresh_page_max_free_span_locked(*target);
  page = target;
  return taken;
}

// Records the unused remainder of the reservation this thread is leaving behind.
// The space stays charged to its page until the page is recycled, so this is the
// only measurement of what an abandoned slab costs.
void account_tlab_tail(MutatorState& mutator) {
  if (!mutator.tlab_cursor || mutator.tlab_end <= mutator.tlab_cursor) return;
  const size_t tail = static_cast<size_t>(mutator.tlab_end - mutator.tlab_cursor);
  mutator.tlab_tail_bytes = saturated_add(mutator.tlab_tail_bytes, tail);
  if (tail > mutator.tlab_tail_max_bytes) mutator.tlab_tail_max_bytes = tail;
}

// Returns the unused end of this thread's reservation to its page. The space was
// charged to the page when the reservation was taken and nothing was ever
// published in it, so at a safepoint it is ordinary free space again: leaving it
// charged kept most of a slab unusable until the whole page was recycled, which
// is the one cost a larger reservation would otherwise not pay for.
//
// Callers hold gc_mutex and this mutator is parked or finished. The cursor is
// cleared so the tail is never released twice.
void release_tlab_tail_locked(MutatorState& mutator) {
  if (mutator.tlab_page && mutator.tlab_cursor &&
      mutator.tlab_end > mutator.tlab_cursor) {
    const size_t offset =
        static_cast<size_t>(mutator.tlab_cursor - mutator.tlab_page->storage);
    const size_t span = static_cast<size_t>(mutator.tlab_end - mutator.tlab_cursor);
    mutator.tlab_tail_reclaimed_bytes =
        saturated_add(mutator.tlab_tail_reclaimed_bytes, span);
    release_span_locked(*mutator.tlab_page, offset, span);
  }
  mutator.tlab_page = nullptr;
  mutator.tlab_cursor = nullptr;
  mutator.tlab_end = nullptr;
}
// Installs a new thread-local cursor, accounting whatever the thread is giving
// up. Every path that starts a reservation goes through here, so a tail is
// never replaced without being counted.
void install_tlab(MutatorState& mutator, GcPage* page, size_t offset, size_t span) {
  account_tlab_tail(mutator);
  mutator.tlab_page = page;
  mutator.tlab_cursor = page->storage + offset;
  mutator.tlab_end = mutator.tlab_cursor + span;
  mutator.tlab_generation = gc_page_generation;
  thread_allocation_page = page;
  thread_allocation_generation = gc_page_generation;
}
// Returns every run this thread reserved and did not hand out. The caller holds
// gc_mutex and this mutator is not running: the cycle boundary flushes before it
// reclaims empty pages, and a thread leaving the registry flushes so the space it
// reserved is not lost with the mutator.
void flush_spare_runs_locked(MutatorState& mutator) {
  while (mutator.spare_run_count != 0) {
    const MutatorState::SpareRun spare =
        mutator.spare_runs[--mutator.spare_run_count];
    if (!spare.page) continue;
    release_span_locked(*spare.page, spare.offset, spare.span);
  }
}

// Pops a reserved run into the thread-local cursor. No lock: the list is owned by
// this thread and every entry was already removed from its page under the lock,
// so no other thread can be handed the same cells. A run too small for this
// request stays on the list for a smaller object.
bool take_spare_run(MutatorState& mutator, size_t minimum_span) {
  size_t index = mutator.spare_run_count;
  while (index != 0) {
    --index;
    const MutatorState::SpareRun spare = mutator.spare_runs[index];
    if (spare.span < minimum_span) continue;
    mutator.spare_runs[index] = mutator.spare_runs[--mutator.spare_run_count];
    install_tlab(mutator, spare.page, spare.offset, spare.span);
    return true;
  }
  return false;
}

// Reserves short runs in one pass and leaves them on the thread's list. Each
// reservation is the same first-fit take the single-run path made, so the page's
// run list, its largest-run record and the index stay exact; what changes is that
// one lock acquisition now answers the next `kSpareRunCapacity` refills. The batch
// is bounded by bytes as well as by count, so a heap that still has medium runs
// cannot have a large slice reserved by a thread that may not need it.
bool reserve_spare_runs_locked(MutatorState& mutator, size_t minimum_span) {
  constexpr size_t byte_budget = kGcTlabBytes * 2;
  GcPage::FreeSpan runs[MutatorState::kSpareRunCapacity];
  GcPage* page = nullptr;
  const size_t taken = take_free_runs_locked(
      minimum_span, byte_budget, MutatorState::kSpareRunCapacity, minimum_span,
      page, runs);
  if (taken == 0) return false;
  for (size_t index = 0; index < taken; ++index) {
    MutatorState::SpareRun& spare = mutator.spare_runs[mutator.spare_run_count];
    spare.page = page;
    spare.offset = runs[index].offset;
    spare.span = runs[index].span;
    ++mutator.spare_run_count;
  }
  return true;
}

// Drops a cursor the cycle boundary invalidated. Reserved runs are returned by
// that boundary too, so a non-empty list here means that step was skipped and the
// pages it points into may already have been recycled: debug builds trap it, a
// release build abandons the list rather than touch storage that may be gone.
void discard_stale_tlab(MutatorState& mutator) {
  account_tlab_tail(mutator);
  mutator.tlab_page = nullptr;
  mutator.tlab_cursor = nullptr;
  mutator.tlab_end = nullptr;
  mutator.tlab_generation = gc_page_generation;
  if (mutator.spare_run_count != 0) {
    if constexpr (nirq::runtime_debug_enabled)
      nrq_runtime_fatal("GC reserved runs outlived a cycle boundary");
    mutator.spare_run_count = 0;
  }
}

// Take a page of at least `capacity` bytes from the recycle pool, or create a
// fresh one. The caller inherits storage ownership and must either publish the
// page into gc_pages or release the storage explicitly.
// Page storage comes straight from the OS. A GC page needs kGcPageBytes
// alignment, and VirtualAlloc returns exactly that: it hands back a 64 KiB-aligned
// region and commits only the bytes requested. The over-aligned `operator new` it
// replaced has to reserve room to align the block itself, so every 64 KiB page
// carried up to another 64 KiB of reserved and committed space - a measured 2x
// private commit for a heap of ordinary pages. Release goes back through
// VirtualFree so the accounting stays symmetric.
//
// Ordinary pages are carved from runs instead of asking the OS for one page at a
// time. A growing heap used to pay a reservation and, later, a release per 64 KiB
// page; a run pays one reservation per 8 MiB and one commit per MiB. A page
// released while the heap is still busy goes back to its run's free list and is
// handed out again without a system call. The free lists are bounded, so a heap
// that shrinks returns its commit charge through MEM_DECOMMIT instead of holding
// it.
constexpr size_t kPageRunBytes = size_t{8} << 20;
constexpr size_t kPageRunCommitBytes = size_t{1} << 20;
// Commit granularity of the OS. A carve's extent is the commit extent as well.
constexpr size_t kPageCommitGranularity = size_t{4} << 10;

struct PageRun {
  unsigned char* base = nullptr;
  // Bytes committed from `base`, kept as a high-water mark: a carve is handed out
  // once, so nothing below the mark is ever un-committed here.
  size_t committed = 0;
  // Bytes carved from `base`. An ordinary page starts on a kGcPageBytes boundary
  // and occupies exactly one page; a larger page is carved at whatever 4 KiB
  // boundary comes next and occupies its own capacity, so a page that is not a
  // multiple of 64 KiB does not pay for the rounding.
  size_t carved = 0;
};
// A deque, so the carving cursor below stays valid when another run is recorded.
std::deque<PageRun> page_runs;
// The run carving continues from. Only it can have un-carved slots, so the
// allocation path never has to look at the other runs.
PageRun* active_page_run = nullptr;
// Storage handed back and ready to be used again. One entry is one carve: a single
// slot for an ordinary page, a contiguous group of slots for a page larger than
// 64 KiB. Entries are matched by exact size, which keeps every page's capacity and
// its mapping in step.
struct FreePageStorage {
  unsigned char* storage = nullptr;
  size_t bytes = 0;
};
std::vector<FreePageStorage> free_page_storage;
// Storage whose commit charge went back to the OS but whose address space the run
// still owns. It is recommitted when the heap needs it again, so a program that
// peaks and shrinks does not lose the addresses it already reserved.
std::vector<FreePageStorage> released_page_storage;
// One run's worth of bytes, so a busy heap never holds more than this much
// committed-but-unused storage in the free list. Anything beyond it is decommitted
// immediately.
constexpr size_t kMaxRetainedBytes = kPageRunBytes;
size_t retained_page_bytes = 0;
// The largest page a run will carve. A single object can be far larger than a run,
// and that case still needs the direct mapping.
constexpr size_t kPageRunMaxCarveBytes = kPageRunBytes / 2;

PageRun* find_page_run(unsigned char* storage) {
  for (PageRun& run : page_runs) {
    if (storage >= run.base && storage < run.base + kPageRunBytes) return &run;
  }
  return nullptr;
}

// Commit enough of a run to cover `end` bytes. Returns false when the OS refuses.
bool commit_run_bytes(PageRun& run, size_t end) {
  if (end <= run.committed) return true;
  size_t target = run.committed + kPageRunCommitBytes;
  if (target < end) target = end;
  if (target > kPageRunBytes) target = kPageRunBytes;
  if (!::VirtualAlloc(run.base + run.committed, target - run.committed,
                      MEM_COMMIT, PAGE_READWRITE))
    return false;
  run.committed = target;
  return true;
}

// Hand back a released carve of exactly `bytes`, recommitting it when its commit
// charge had already been returned.
unsigned char* take_released_page_storage(size_t bytes) {
  for (auto entry = free_page_storage.rbegin(); entry != free_page_storage.rend();
       ++entry) {
    if (entry->bytes != bytes) continue;
    unsigned char* storage = entry->storage;
    *entry = free_page_storage.back();
    free_page_storage.pop_back();
    if (retained_page_bytes >= bytes) retained_page_bytes -= bytes;
    return storage;
  }
  for (auto entry = released_page_storage.rbegin();
       entry != released_page_storage.rend(); ++entry) {
    if (entry->bytes != bytes) continue;
    if (!::VirtualAlloc(entry->storage, bytes, MEM_COMMIT, PAGE_READWRITE))
      return nullptr;
    unsigned char* storage = entry->storage;
    *entry = released_page_storage.back();
    released_page_storage.pop_back();
    return storage;
  }
  return nullptr;
}

// Carve a page of `bytes` from a run. Ordinary pages keep the 64 KiB alignment
// the page directory keys on; a larger page needs no such alignment (it is never
// indexed by address) and is carved at its own size.
unsigned char* carve_page_run(size_t bytes) {
  const size_t extent =
      (bytes + kPageCommitGranularity - 1) & ~(kPageCommitGranularity - 1);
  for (;;) {
    if (!active_page_run) {
      try {
        PageRun run;
        run.base = static_cast<unsigned char*>(
            ::VirtualAlloc(nullptr, kPageRunBytes, MEM_RESERVE, PAGE_READWRITE));
        if (!run.base) return nullptr;
        page_runs.push_back(std::move(run));
      } catch (...) {
        return nullptr;
      }
      active_page_run = &page_runs.back();
    }
    size_t offset = active_page_run->carved;
    if (bytes <= kGcPageBytes)
      offset = (offset + kGcPageBytes - 1) & ~(kGcPageBytes - 1);
    if (offset >= kPageRunBytes || extent > kPageRunBytes - offset) {
      // A run that cannot serve this request is closed; the next request opens a
      // new one instead of retrying this one forever.
      active_page_run->carved = kPageRunBytes;
      active_page_run = nullptr;
      continue;
    }
    if (!commit_run_bytes(*active_page_run, offset + extent)) {
      active_page_run->carved = kPageRunBytes;
      active_page_run = nullptr;
      continue;
    }
    active_page_run->carved = offset + extent;
    return active_page_run->base + offset;
  }
}

// Storage for one page: a released carve of the same size, a fresh carve from a
// run, or a direct mapping when the page is too large for a run or the runs cannot
// serve the request.
unsigned char* commit_page_storage(size_t bytes) {
  if (bytes <= kPageRunMaxCarveBytes) {
    if (unsigned char* storage = take_released_page_storage(bytes))
      return storage;
    if (unsigned char* storage = carve_page_run(bytes)) return storage;
  }
  return static_cast<unsigned char*>(
      ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
}

// Return storage to the run that owns it, or to the OS when it was a direct
// mapping. Carves are kept for reuse until the free list is full.
void release_page_storage(unsigned char* storage, size_t bytes) {
  if (!storage) return;
  if (find_page_run(storage)) {
    if (retained_page_bytes + bytes <= kMaxRetainedBytes) {
      try {
        free_page_storage.push_back(FreePageStorage{storage, bytes});
        retained_page_bytes += bytes;
        return;
      } catch (...) {
      }
    }
    if (::VirtualFree(storage, bytes, MEM_DECOMMIT)) {
      try {
        released_page_storage.push_back(FreePageStorage{storage, bytes});
      } catch (...) {
      }
    }
    return;
  }
  ::VirtualFree(storage, 0, MEM_RELEASE);
}

std::unique_ptr<GcPage> acquire_page(size_t capacity) {
  for (auto entry = recycled_pages.begin(); entry != recycled_pages.end();
       ++entry) {
    if ((*entry)->capacity < capacity) continue;
    std::unique_ptr<GcPage> page = std::move(*entry);
    recycled_pages.erase(entry);
    page->used = 0;
    page->live_objects = 0;
    page->retired = false;
    if (page->free_span_count != 0) {
      unindex_span_page_locked(*page);
      total_free_spans -= page->free_span_count;
      page->free_span_count = 0;
      page->free_span_bytes = 0;
    }
    set_page_max_free_span_locked(*page, 0, 0);
    page->young_objects = 0;
    if (page->object_starts && page->object_start_words != 0) {
      std::memset(page->object_starts.get(), 0,
                  page->object_start_words * sizeof(uint64_t));
    }
    recycled_page_reuse_count = saturated_add(recycled_page_reuse_count, 1);
    return page;
  }
  unsigned char* storage = commit_page_storage(capacity);
  if (!storage) return nullptr;
  try {
    auto page = std::make_unique<GcPage>();
    page->storage = storage;
    page->capacity = capacity;
    if (capacity <= kGcPageBytes) {
      const size_t slots = capacity / kObjectStartAlignment;
      const size_t words = (slots + 63) / 64;
      page->object_starts.reset(new (std::nothrow) uint64_t[words]());
      if (page->object_starts) page->object_start_words = words;
    }
    return page;
  } catch (...) {
    release_page_storage(storage, capacity);
    return nullptr;
  }
}

// Return an empty page to the bounded recycle pool, or release its storage when
// the pool is full. Must be called with gc_mutex held at a cycle boundary, when
// no mutator can still hold a cursor into the page.
void recycle_page(std::unique_ptr<GcPage> page) {
  if (!page) return;
  unindex_span_page_locked(*page);
  if (page->free_span_count != 0) {
    total_free_spans -= page->free_span_count;
    page->free_span_count = 0;
    page->free_span_bytes = 0;
  }
  set_page_max_free_span_locked(*page, 0, 0);
  if (recycled_pages.size() >= kMaxRecycledPages ||
      page->capacity < kGcPageBytes ||
      page->capacity > kMaxRecycledPageBytes) {
    release_page_storage(page->storage, page->capacity);
    return;
  }
  page->used = 0;
  page->live_objects = 0;
  page->young_objects = 0;
  page->retired = false;
  page->free_span_count = 0;
  page->free_span_bytes = 0;
  GcPage* raw = page.get();
  try {
    recycled_pages.push_back(std::move(page));
  } catch (...) {
    release_page_storage(raw->storage, raw->capacity);
  }
}

// Choose the next automatic-collection trigger from what the finished cycle
// observed. The trigger is an allocation budget, so it is expressed as the live
// heap plus headroom: no headroom would make the collector run again as soon as
// anything is allocated, and unbounded headroom would grow the heap forever.
// Headroom is at least a fixed minimum and otherwise half the live heap, so the
// budget scales with the program's actual working set, and a cycle that barely
// reclaimed anything (a mostly live heap) gets more room next time instead of
// being repeated for almost no gain.
void adapt_collection_policy_locked() {
  const size_t live = live_heap_bytes;
  const size_t before = cycle_start_live_bytes;
  collection_policy.last_reclaimed_bytes =
      before > live ? before - live : 0;
  if (!collection_policy.adaptive) return;

  size_t headroom = std::max(kMinimumCollectionHeadroom, live / 2);
  const bool heap_is_mostly_live = before != 0 && live > (before / 8) * 7;
  if (heap_is_mostly_live) headroom += live / 2;
  size_t threshold = live + headroom;
  if (threshold < live) threshold = live;  // saturating add of a huge heap

  allocation_accounting.collection_threshold.store(threshold,
                                                  std::memory_order_relaxed);
  ++collection_policy.adaptations;
}

ObjectHeader* find_pending_header(MutatorState& mutator, void* object);

// An entry is active only while it has both an object and a nonzero delta. A
// handoff that is cancelled before the next drain leaves nothing to fold, so the
// slot has to stop matching: a zero-delta entry that still matched would absorb
// later pins and keep its slot forever, which is how the buffer used to fill with
// entries that describe nothing.
TransferPinDelta* find_transfer_delta(MutatorState& mutator, void* object) {
  if (!object) return nullptr;
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    const TransferPinDelta& entry = mutator.transfer_pins[index];
    if (entry.object == object && entry.delta != 0)
      return &mutator.transfer_pins[index];
  }
  return nullptr;
}

// Releases a delta entry that has cancelled out. Keeping the object pointer would
// leave a slot that looks occupied to every later scan.
void clear_transfer_delta(TransferPinDelta& entry) {
  entry.object = nullptr;
  entry.delta = 0;
}

// Returns an empty slot, reusing a retired entry before extending the logical
// high-water count. Without reuse a workload that returns and immediately releases
// distinct objects would fill all 32 entries with retired slots and push every
// later handoff onto the global pin table for the rest of the process.
TransferPinDelta* acquire_transfer_delta_slot(MutatorState& mutator) {
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    TransferPinDelta& entry = mutator.transfer_pins[index];
    if (!entry.object) {
      entry.delta = 0;
      return &entry;
    }
  }
  if (mutator.transfer_pin_count >= kTransferPinBufferCapacity) return nullptr;
  TransferPinDelta& entry = mutator.transfer_pins[mutator.transfer_pin_count];
  ++mutator.transfer_pin_count;
  entry.object = nullptr;
  entry.delta = 0;
  return &entry;
}

// Handoffs that found no reusable slot. Zero here means the buffered fast path
// carried every handoff; a rising count means the buffer is genuinely full of live
// deltas, which is the only case that should reach the global pin table.
std::atomic<uint64_t> transfer_pin_fallbacks{0};

// Fold a mutator's buffered transfer-pin deltas into the global pin table. Called
// with gc_mutex held, either by the collector after parking the mutator or by the
// mutator itself, so the buffer cannot be written while it is being read.
void drain_transfer_pins_locked(MutatorState& mutator) {
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    const TransferPinDelta& entry = mutator.transfer_pins[index];
    if (!entry.object || entry.delta == 0) continue;
    if (entry.delta > 0) {
      // A pin of a not-yet-published object is legitimate: cycle preparation
      // publishes every TLAB before this drain runs. Anything else is a pin of a
      // pointer the heap does not own, which the buffered path can only detect
      // here, so it is recorded as a rejection instead of being counted.
      if (!find_object_locked(entry.object) &&
          !find_pending_header(mutator, entry.object)) {
        pin_violations =
            saturated_add(pin_violations, static_cast<uint64_t>(entry.delta));
        continue;
      }
      try {
        size_t& count = pinned_objects[entry.object];
        const uint64_t total =
            static_cast<uint64_t>(count) + static_cast<uint64_t>(entry.delta);
        if (total > static_cast<uint64_t>(SIZE_MAX))
          nrq_runtime_fatal("GC transfer pin count overflow");
        count = static_cast<size_t>(total);
      } catch (...) {
        nrq_runtime_fatal("GC transfer pin registration failed");
      }
      continue;
    }
    auto found = pinned_objects.find(entry.object);
    if (found == pinned_objects.end()) continue;  // nothing left to release
    const uint64_t release = static_cast<uint64_t>(-entry.delta);
    if (release >= static_cast<uint64_t>(found->second))
      pinned_objects.erase(found);
    else
      found->second -= static_cast<size_t>(release);
  }
  mutator.transfer_pin_count = 0;
}

void prepare_cycle_locked(bool young_only) {
  for (MutatorState* mutator : mutators)
    if (mutator) publish_tlab_locked(*mutator);
  // Every reserved-but-unused run goes back to its page here, before the
  // generation stamp moves and before the reclaimer decides which pages are
  // empty. A page can only be recycled once all of its space is either published
  // or recorded as free, and a run sitting on a mutator's list is neither.
  for (MutatorState* mutator : mutators)
    if (mutator) flush_spare_runs_locked(*mutator);
  // The unused end of each live reservation is the same argument one level
  // closer: it holds no published object, so at this safepoint it is free space
  // and the next reservation in that page can use it.
  for (MutatorState* mutator : mutators)
    if (mutator) release_tlab_tail_locked(*mutator);
  // A fast old-to-young store may have filled its buffer without taking the
  // collector lock. Publish every mutator's TLAB first, then drain, so buffered
  // references to objects that were still thread-local become visible. Any
  // reference deferred by a thread exit is resolved after all TLABs are live.
  for (MutatorState* mutator : mutators)
    if (mutator) drain_remembered_locked(*mutator, /*defer_unresolved=*/false);
  resolve_unresolved_remembered_locked();
  // Buffered transfer pins are invisible until they reach the table, so fold them
  // in before the cycle enumerates pinned roots.
  for (MutatorState* mutator : mutators)
    if (mutator) drain_transfer_pins_locked(*mutator);

  // No mutator can be inside an allocator while stop_mutators() has everyone
  // parked. Invalidate stale TLAB cursors before reclaiming empty pages.
  //
  // `retired_page_count` is the number of pages the previous sweep emptied, so
  // the page set is only walked when there is something to reclaim. Every page
  // that becomes empty is counted where it becomes empty: the sweep that frees
  // its last object, or an allocation that reserved space and then failed.
  ++gc_page_generation;
  if (retired_page_count != 0) {
    for (auto page = gc_pages.begin(); page != gc_pages.end();) {
      if ((*page)->live_objects == 0) {
        if ((*page)->retired) --retired_page_count;
        unindex_tail_page_locked(**page);
        page_directory.erase(page->get());
        auto empty_page = std::move(*page);
        page = gc_pages.erase(page);
        recycle_page(std::move(empty_page));
      } else {
        (*page)->retired = false;
        ++page;
      }
    }
    retired_page_count = 0;
  }

  sweep_list = nullptr;
  sweep_cursor = SweepCursor{};
  cycle_start_live_bytes = live_heap_bytes;
  active_cycle_epoch = next_cycle_epoch.fetch_add(1) + 1;
  active_collection_is_minor = young_only;
  active_mark = MarkContext{};
  active_mark.young_only = young_only;
  active_mark.epoch = active_cycle_epoch;
  // Reclaimed and promoted objects cleared their own flag during the previous
  // cycle. Drop those entries now, before anything traverses the list, so the
  // traversal never observes an address whose object is gone.
  compact_remembered_locked();
  enqueue_roots_locked(active_mark);
  collection_phase.store(CollectionPhase::Marking, std::memory_order_release);
}

// Decide whether the next automatic collection must be a full cycle. Young
// objects that die are reclaimed by minors, so the only signal a minor cannot
// act on is old garbage, which keeps the live heap growing without the program
// retaining more data. Callers hold gc_mutex.
void plan_next_cycle_locked() {
  if (!generational_collection_enabled.load(std::memory_order_relaxed)) {
    next_cycle_is_major.store(true, std::memory_order_relaxed);
    return;
  }
  const size_t minors = minor_collections_since_major.load(std::memory_order_relaxed);
  bool major = false;
  if (minors >= kMaximumMinorInterval) {
    major = true;
  } else if (minors >= kMinimumMinorInterval) {
    const size_t baseline = live_heap_bytes_at_last_major;
    const size_t growth = live_heap_bytes > baseline ? live_heap_bytes - baseline : 0;
    const size_t growth_limit =
        std::max(kMinimumCollectionHeadroom, baseline / 2);
    major = growth >= growth_limit;
  }
  next_cycle_is_major.store(major, std::memory_order_relaxed);
}

void begin_cycle(bool young_only) {
  debug_emit(NRQ_GC_EVENT_CYCLE_BEGIN, young_only ? 1u : 0u, 0, nullptr);
  // Publish the transition before parking so new fast-path stores fall back to
  // the synchronized barrier. In-flight fast stores finish before the root
  // snapshot; the per-mutator barrier depth is the handshake that makes that
  // visible.
  {
    HeapLockGuard lock(gc_mutex);
    // A concurrent promotion can turn a reachable object old while mutators
    // run, so enable remembered-set recording for the whole cycle. The flag is
    // lowered again if the cycle finishes with no old objects.
    raise_store_barrier();
    collection_phase.store(CollectionPhase::Preparing,
                           std::memory_order_release);
  }
  stop_mutators();
  wait_for_fast_barriers();
  {
    HeapLockGuard lock(gc_mutex);
    prepare_cycle_locked(young_only);
  }
  resume_mutators();
}

void finish_cycle_locked(bool young_only) {
  statistics.collections = saturated_add(statistics.collections, 1);
  if (young_only) {
    minor_collections = saturated_add(minor_collections, 1);
    minor_collections_since_major.store(
        minor_collections_since_major.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
  } else {
    major_collections = saturated_add(major_collections, 1);
    minor_collections_since_major.store(0, std::memory_order_relaxed);
  }
  // The live heap size is maintained by publication and reclamation, so this
  // reports it directly instead of walking every live object each cycle.
  statistics.allocated_bytes = live_heap_bytes;
  adapt_collection_policy_locked();
  debug_emit(NRQ_GC_EVENT_CYCLE_END, young_only ? 1u : 0u,
             collection_policy.last_reclaimed_bytes, nullptr);
  if (!young_only) live_heap_bytes_at_last_major = live_heap_bytes;
  plan_next_cycle_locked();
  // Do not clear the remembered set here. A major collection erases promoted
  // survivors as it sweeps, while young objects allocated during the cycle
  // remain young and may be referenced only by old objects; their remembered
  // entries must survive into the next minor collection.
  // The barrier is only useful while a minor can run: with generations off they
  // never do, and with no old objects there is no old-to-young edge for the
  // remembered set to record. Leaving it raised in either case would put the
  // write barrier back on every store of a program that does not need it.
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      old_object_count.load(std::memory_order_relaxed) == 0)
    lower_store_barrier();
  active_mark = MarkContext{};
  collection_phase.store(CollectionPhase::Idle, std::memory_order_release);
  allocation_accounting.bytes_since_collection.store(0, std::memory_order_relaxed);
}

// A registered mutator may request a collection while another thread is already
// collecting. Waiting on collection_cycle_mutex without parking would deadlock
// the active collector, which waits for every mutator to reach a safepoint. Park
// before blocking on the cycle lock, then resume once the previous cycle exits.
std::unique_lock<std::mutex> acquire_collection_cycle() {
  std::unique_lock<std::mutex> cycle_lock(collection_cycle_mutex, std::defer_lock);
  if (cycle_lock.try_lock()) return cycle_lock;
  if (thread_mutator.depth == 0) {
    cycle_lock.lock();
    return cycle_lock;
  }
  {
    std::unique_lock<std::mutex> lock(mutator_mutex);
    thread_mutator.parked = true;
    parked_mutator_count.fetch_add(1, std::memory_order_release);
    collector_condition.notify_one();
  }
  cycle_lock.lock();
  {
    std::lock_guard<std::mutex> lock(mutator_mutex);
    parked_mutator_count.fetch_sub(1, std::memory_order_release);
    thread_mutator.parked = false;
  }
  return cycle_lock;
}

struct CollectorThreadGuard {
  bool previous = thread_is_collector;
  CollectorThreadGuard() { thread_is_collector = true; }
  ~CollectorThreadGuard() { thread_is_collector = previous; }
};

void run_collection(bool young_only) {
  CollectorThreadGuard collector_guard;
  std::unique_lock<std::mutex> cycle_lock = acquire_collection_cycle();
  begin_cycle(young_only);
  bool sweeping = false;
  for (;;) {
    bool finished = false;
    bool made_progress = false;
    {
      HeapLockGuard lock(gc_mutex);
      if (!sweeping) {
        const bool marked = mark_slice_locked(kMarkSliceBytes);
        made_progress = true;
        if (marked) {
          // Freeze the unindexed list and capture the page set before releasing
          // the lock for sweep slices. Allocations after this point either land
          // in a fresh page (outside the captured set) or carry a birth epoch
          // past the cycle, so the sweeper never has to reason about them.
          sweep_list = unindexed_objects;
          unindexed_objects = nullptr;
          sweep_cursor = SweepCursor{};
          sweep_cursor.page_limit = gc_pages.size();
          collection_phase.store(CollectionPhase::Sweeping,
                                 std::memory_order_release);
          sweeping = true;
        }
      }
      if (sweeping) {
        // Drain barriers that ran between sweep slices before freeing the next
        // chunk. A pending marked parent may still enqueue its children.
        if (!mark_slice_locked(kMarkSliceBytes)) {
          made_progress = true;
        } else {
          made_progress = true;
          if (sweep_chunk_locked(kSweepSliceBytes)) {
            finish_cycle_locked(young_only);
            finished = true;
          }
        }
      }
    }
    if (finished) return;
    if (!made_progress) std::this_thread::yield();
  }
}

void maybe_collect_automatically() {
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle)
    return;
  const bool should_collect = automatic_collection_enabled.load(
                                  std::memory_order_relaxed) &&
      allocation_accounting.bytes_since_collection.load(std::memory_order_relaxed) >=
          allocation_accounting.collection_threshold.load(std::memory_order_relaxed);
  if (!should_collect) return;
  const bool young_only =
      generational_collection_enabled.load(std::memory_order_relaxed) &&
      !next_cycle_is_major.load(std::memory_order_relaxed);
  run_collection(young_only);
}

void* allocate_locked(size_t bytes, size_t element_size, size_t elements,
                      nrq_gc_trace_fn trace, const size_t* reference_offsets,
                      size_t reference_count,
                      const nrq_object_layout* resolved) {
  if (bytes == 0 || bytes > SIZE_MAX - sizeof(ObjectHeader) || element_size == 0)
    return nullptr;
  // A caller that already owns an immutable descriptor hands it over directly:
  // it is the identity the object stores, so this path neither interns nor
  // compares tables.
  const nrq_object_layout* layout =
      resolved ? resolved
               : layout_for_allocation(bytes, element_size, trace,
                                       reference_offsets, reference_count);
  if (!layout) return nullptr;
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw_span = sizeof(ObjectHeader) + bytes;
  if (raw_span > SIZE_MAX - (alignment - 1)) return nullptr;
  const size_t span = (raw_span + alignment - 1) & ~(alignment - 1);
  GcPage* page = nullptr;
  size_t header_offset = 0;
  bool reused_span = false;
  if (thread_allocation_generation != gc_page_generation) {
    thread_allocation_page = nullptr;
    thread_allocation_generation = gc_page_generation;
  }
  // Reclaimed space in an existing page comes first: reusing it keeps the
  // working set inside pages that are already resident.
  page = take_free_span_locked(span, header_offset);
  if (page) {
    reused_span = true;
  } else if (thread_allocation_page &&
      thread_allocation_page->used <= thread_allocation_page->capacity &&
      span <= thread_allocation_page->capacity - thread_allocation_page->used) {
    page = thread_allocation_page;
  } else {
    thread_allocation_page = nullptr;
    for (GcPage* candidate : tail_pages) {
      if (!candidate) continue;
      if (span <= candidate->capacity - candidate->used) {
        page = candidate;
        break;
      }
    }
  }
  if (!page) {
    const size_t capacity = std::max(kGcPageBytes, span);
    auto new_page = acquire_page(capacity);
    if (!new_page) return nullptr;
    page = new_page.get();
    try {
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      if (new_page) release_page_storage(new_page->storage, new_page->capacity);
      return nullptr;
    }
    // A page that cannot be indexed by address is still usable; its objects
    // are tracked through the exact object index instead.
    if (!page_directory.insert(page)) page->use_object_index = true;
    index_tail_page_locked(*page);
  }
  unretire_page_locked(*page);
  thread_allocation_page = page;
  thread_allocation_generation = gc_page_generation;
  if (!reused_span) {
    header_offset = page->used;
    page->used += span;
    note_page_tail_locked(*page);
  }
  void* header_storage = page->storage + header_offset;
  auto* header = new (header_storage) ObjectHeader(
      page, static_cast<uint32_t>(elements), layout,
      generational_collection_enabled.load(std::memory_order_relaxed),
      next_cycle_epoch.load(std::memory_order_relaxed));
  auto rollback_reservation = [&]() {
    destroy_object_storage(header);
    if (reused_span) {
      release_span_locked(*page, header_offset, span);
    } else {
      page->used -= span;
      note_page_tail_locked(*page);
    }
    // A page that reserved space and then could not publish its first object is
    // empty, not free for later use: mark it retired so the next cycle boundary
    // reclaims it instead of leaving it in the page set forever.
    if (page->live_objects == 0 && !page->retired) {
      page->retired = true;
      ++retired_page_count;
    }
  };
  void* object = static_cast<unsigned char*>(header_storage) + sizeof(ObjectHeader);
  std::memset(object, 0, bytes);
  try {
    // The reference map is the descriptor's, so an object with any number of
    // reference words costs no per-instance storage and no allocation.
    // Counted, not published: this allocation is already live, and the barrier
    // policy is the only thing that raises the store barrier (see allocate_tlab).
    if (!header->young())
      old_object_count.fetch_add(1, std::memory_order_relaxed);
    header->next = nullptr;
    if (page_uses_index(page)) {
      if (!object_index.insert(object, header)) {
        rollback_reservation();
        return nullptr;
      }
    } else {
      const size_t offset = static_cast<size_t>(
          static_cast<unsigned char*>(object) - page->storage);
      mark_object_start(*page, offset);
    }
  } catch (...) {
    rollback_reservation();
    return nullptr;
  }
  // Only a page that cannot be walked by a bitmap needs the object listed; every
  // other object is reachable from its page.
  if (page_uses_index(page)) {
    header->next = unindexed_objects;
    unindexed_objects = header;
  }
  ++page->live_objects;
  if (header->young()) ++page->young_objects;
  statistics.allocated_bytes = saturated_add(
      statistics.allocated_bytes, static_cast<uint64_t>(bytes));
  statistics.live_objects = saturated_add(statistics.live_objects, 1);
  live_heap_bytes = saturated_add(live_heap_bytes, bytes);
  add_since_collection_locked(bytes);
  debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, bytes, object);
  return object;
}

bool reserve_tlab_locked(MutatorState& mutator, size_t minimum_span) {
  tlab_refill_count = saturated_add(tlab_refill_count, 1);
  if (mutator.tlab_generation != gc_page_generation) discard_stale_tlab(mutator);
  // A merged run of reclaimed space can host a whole thread-local reservation.
  // Taking it here is what turns a fragmented page back into a usable bump
  // region instead of leaving it permanently half empty - and taking all of it,
  // rather than a fixed small slice, is what keeps a recycled heap from being
  // carved into pieces that later reservations cannot use.
  //
  // A slab-sized run is preferred, but only when one exists:
  // `pages_with_slab_run` answers that in one load. Proving the negative by
  // walking the run index made a fragmented heap pay a full page scan for every
  // few allocations. Short runs are still reused, after the committed page tails
  // below, which cost nothing to hand out.
  auto take_run = [&](size_t wanted_minimum) {
    size_t run_offset = 0;
    size_t run_span = 0;
    GcPage* reused = take_free_run_locked(kGcTlabBytes, wanted_minimum,
                                          minimum_span, run_offset, run_span);
    if (!reused) return false;
    install_tlab(mutator, reused, run_offset, run_span);
    return true;
  };
  if (minimum_span > kGcTlabMinSlabBytes) {
    // A single object larger than the slab minimum needs a run of its own size;
    // the slab counter cannot answer for that, so search directly. Such
    // allocations are rare, so the walk is not on the hot path.
    if (take_run(minimum_span)) return true;
  } else if (pages_with_slab_run != 0 && take_run(kGcTlabMinSlabBytes)) {
    // A page can host a whole reservation, so take it: one refill then serves a
    // full slab instead of a handful of cells.
    return true;
  }
  GcPage* page = nullptr;
  size_t available = 0;
  if (thread_allocation_generation != gc_page_generation) {
    thread_allocation_page = nullptr;
    thread_allocation_generation = gc_page_generation;
  }
  if (thread_allocation_page &&
      thread_allocation_page->used <= thread_allocation_page->capacity) {
    available = thread_allocation_page->capacity - thread_allocation_page->used;
    if (available >= minimum_span) page = thread_allocation_page;
  }
  if (!page && max_indexed_tail_bytes >= minimum_span) {
    // Newest first, as the reverse scan did: the most recently carved page is the
    // one most likely to still be resident and partly free.
    for (auto candidate = tail_pages.rbegin(); candidate != tail_pages.rend();
         ++candidate) {
      GcPage* possible = *candidate;
      if (!possible) continue;
      const size_t remaining = indexed_tail_bytes(*possible);
      if (remaining >= minimum_span) {
        page = possible;
        available = remaining;
        break;
      }
    }
    if (!page) {
      // Nothing indexed can serve this size. Record that exactly, so the next
      // refill of the same size skips the walk instead of repeating it.
      max_indexed_tail_bytes = 0;
      for (const GcPage* possible : tail_pages)
        if (possible) {
          const size_t remaining = indexed_tail_bytes(*possible);
          if (remaining > max_indexed_tail_bytes)
            max_indexed_tail_bytes = remaining;
        }
    }
  }
  // Now reuse short reclaimed runs. This is the path that serves a heap whose
  // largest free block is a few cells; it is tried only after the committed page
  // tails, which need no index maintenance. One take would hand back a few
  // hundred bytes and make the next two or three allocations pay this lock again,
  // so the batch is reserved here and handed out by `take_spare_run` for free.
  if (!page && reserve_spare_runs_locked(mutator, minimum_span) &&
      take_spare_run(mutator, minimum_span)) {
    return true;
  }
  if (!page) {
    const size_t capacity = std::max(kGcPageBytes, kGcTlabBytes);
    auto new_page = acquire_page(capacity);
    if (!new_page) return false;
    page = new_page.get();
    try {
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      if (new_page) release_page_storage(new_page->storage, new_page->capacity);
      return false;
    }
    // A page that cannot be indexed by address is still usable; its objects
    // are tracked through the exact object index instead.
    if (!page_directory.insert(page)) page->use_object_index = true;
    index_tail_page_locked(*page);
    available = page->capacity;
  }
  const size_t reserve =
      whole_object_grant(std::min(kGcTlabBytes, available), minimum_span);
  if (reserve < minimum_span) return false;
  unretire_page_locked(*page);
  install_tlab(mutator, page, page->used, reserve);
  page->used += reserve;
  note_page_tail_locked(*page);
  return true;
}

void account_tlab_allocation(MutatorState& mutator, size_t bytes) {
  // Both published counters are written only by this thread: publish_tlab_locked
  // clears them while the owner is parked (or is itself the publisher). A load
  // plus a store therefore has no lost update to guard against, and it keeps the
  // value immediately visible to a statistics query - unlike batching it, which
  // would make an in-flight allocation momentarily invisible. What it drops is
  // the locked read-modify-write, which cost more than the rest of this function.
  if (mutator.pending_bytes_local > SIZE_MAX - bytes)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  mutator.pending_bytes_local += bytes;
  mutator.pending_bytes.store(mutator.pending_bytes_local,
                              std::memory_order_relaxed);
  if (mutator.pending_count_local == SIZE_MAX)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  ++mutator.pending_count_local;
  mutator.pending_count.store(mutator.pending_count_local,
                              std::memory_order_relaxed);
  // The trigger only needs batch granularity, so the trackable total behind it
  // stays owner-only until the batch closes.
  if (bytes > SIZE_MAX - mutator.unflushed_bytes)
    nrq_runtime_fatal("GC thread-local batch accounting overflow");
  mutator.unflushed_bytes += bytes;
  const size_t threshold =
      allocation_accounting.collection_threshold.load(std::memory_order_relaxed);
  const size_t batch = std::min(kAccountingBatchBytes, threshold == 0 ? 1 : threshold);
  if (mutator.unflushed_bytes < batch) return;
  HeapLockGuard lock(gc_mutex);
  flush_mutator_accounting_locked(mutator);
}

// Thread-local bump allocation. The payload size has already been checked by the
// entry point that computed it, and `element_size` is only carried into the header,
// so the only precondition left is that this thread is a mutator with a usable
// TLAB. Keeping the checks at the boundary means the validated path and the slow
// path share one validation instead of each repeating it.
void* allocate_tlab(size_t bytes, size_t element_size, size_t elements,
                    nrq_gc_trace_fn trace, const size_t* reference_offsets,
                    size_t reference_count, bool& handled,
                    const nrq_object_layout* resolved) {
  handled = false;
  if (thread_mutator.depth == 0) return nullptr;
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw_span = sizeof(ObjectHeader) + bytes;
  if (raw_span > SIZE_MAX - (alignment - 1)) return nullptr;
  const size_t span = (raw_span + alignment - 1) & ~(alignment - 1);
  if (span > kGcTlabBytes) return nullptr;
  handled = true;
  const nrq_object_layout* layout =
      resolved ? resolved
               : layout_for_allocation(bytes, element_size, trace,
                                       reference_offsets, reference_count);
  if (!layout) return nullptr;

  if (thread_mutator.tlab_generation != gc_page_generation)
    discard_stale_tlab(thread_mutator);
  if (!thread_mutator.tlab_cursor || !thread_mutator.tlab_end ||
      thread_mutator.tlab_cursor > thread_mutator.tlab_end ||
      span > static_cast<size_t>(thread_mutator.tlab_end -
                                 thread_mutator.tlab_cursor)) {
    // A run reserved earlier answers this without the allocator lock: that is
    // what a fragmented heap needs, because its runs are only a few cells each.
    if (!take_spare_run(thread_mutator, span)) {
      HeapLockGuard lock(gc_mutex);
      if (!reserve_tlab_locked(thread_mutator, span)) return nullptr;
    }
  }

  unsigned char* header_storage = thread_mutator.tlab_cursor;
  thread_mutator.tlab_cursor += span;
  auto* header = new (header_storage) ObjectHeader(
      thread_mutator.tlab_page, static_cast<uint32_t>(elements), layout,
      generational_collection_enabled.load(std::memory_order_relaxed),
      next_cycle_epoch.load(std::memory_order_relaxed));
  void* object = header_storage + sizeof(ObjectHeader);
  std::memset(object, 0, bytes);
  // The reference map is the descriptor's, so an object with any number of
  // reference words costs no per-instance storage and no allocation.
  // A non-young object only has to be counted: the count feeds the generational
  // policy, and the store barrier is raised by the policy itself (`begin_cycle`
  // covers a cycle, `nrq_gc_set_generational` covers the switch). Raising it here
  // as well would undo `nrq_gc_set_generational(0)` on the next allocation and
  // make every later store record a remembered entry that no minor can use, and
  // the counter update would be a locked instruction on the allocation path. The
  // count is folded in where the batch is published.
  header->tlab_next = thread_mutator.pending_objects;
  thread_mutator.pending_objects = header;
  account_tlab_allocation(thread_mutator, bytes);

  // During a cycle an unpublished young object is invisible to the marker and
  // to the root barrier. Publish it immediately so a barrier can identify it.
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle) {
    HeapLockGuard lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
  }
  debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, bytes, object);
  return object;
}

void store_root_value_locked(void** slot, void* value) {
  const CollectionPhase phase =
      collection_phase.load(std::memory_order_relaxed);
  if (phase != CollectionPhase::Idle) {
    // Snapshot-at-the-beginning: retain the old referent as well as the new
    // one while marking. Objects reachable at the snapshot are already marked
    // once sweeping starts, so only newly installed referents need marking.
    if (phase == CollectionPhase::Marking) mark_value_locked(*slot);
    mark_value_locked(value);
  }
  *slot = value;
  defer_remembered_locked(value);
}

ShadowStack& ensure_shadow_stack() {
  if (thread_shadow_stack) return *thread_shadow_stack;
  thread_gc_cleanup.arm();
  auto* stack = new (std::nothrow) ShadowStack();
  if (!stack) nrq_runtime_fatal("GC shadow-stack allocation failed");
  {
    HeapLockGuard lock(gc_mutex);
    try {
      shadow_stacks.push_back(stack);
    } catch (...) {
      delete stack;
      nrq_runtime_fatal("GC shadow-stack registration failed");
    }
  }
  thread_shadow_stack = stack;
  return *stack;
}

bool is_pointer_root(const RootInfo& root) {
  return root.repeated_count == 0 && root.object_size == sizeof(void*) &&
         root.layout && root.layout->reference_count == 1 &&
         root.layout->reference_offsets[0] == 0;
}

// The descriptor for a root registration: the same interning path objects use.
// A repeated array root is described by one element, so the stride and the
// offset bounds are the element's; a single record is described by its size.
const nrq_object_layout* root_layout(size_t object_size, size_t element_size,
                                     size_t repeated_count,
                                     const size_t* reference_offsets,
                                     size_t reference_count) {
  // Registration runs once per root per frame entry, so it uses the same
  // thread-local memo as allocation: a slot with the layout this thread saw last
  // resolves without the interning lock, and only a genuinely new shape pays for
  // the table. The compiler's tables are module constants, so the memo compares
  // them by pointer.
  if (repeated_count != 0)
    return layout_for_allocation(object_size, element_size, nullptr,
                                 reference_offsets, reference_count);
  return layout_for_allocation(object_size, object_size, nullptr,
                               reference_offsets, reference_count);
}

void add_slot_state(ShadowStack& stack, void* slot, const RootInfo& info,
                    bool pointer_root) {
  SlotState* state = stack.slot_states.insert(slot);
  if (!state)
    nrq_runtime_fatal("GC root slot index failed to grow");
  if (state->count == 0) {
    state->count = 1;
    state->object_size = info.object_size;
    state->layout = info.layout;
    state->pointer_root = pointer_root;
  } else {
    // Descriptor identity is layout identity, so agreement is one compare.
    if (state->object_size != info.object_size ||
        state->layout != info.layout ||
        state->pointer_root != pointer_root)
      nrq_runtime_fatal(
          "GC root slot was registered with an incompatible layout");
    ++state->count;
  }
  // A slot that is a pointer root stays one while it is registered, so the
  // allocation path can answer its check from here instead of the index.
  if (pointer_root) stack.pointer_roots.insert(slot);
}

void remove_slot_state(ShadowStack& stack, void* slot, uint32_t count = 1) {
  bool fully_released = false;
  if (!stack.slot_states.release(slot, count, &fully_released))
    nrq_runtime_fatal("GC root slot registration bookkeeping is inconsistent");
  // The last registration of a slot takes its pointer-root memo with it, so a
  // later registration of the same address with another layout is re-validated.
  if (fully_released) stack.pointer_roots.forget(slot);
}

// Frame ends usually release a handful of roots, and a frame can name the same
// slot more than once. Counting them first means each distinct slot probes the
// index once for the whole frame. Frames larger than this batch fall back to one
// release per registration, which is still a single probe chain each.
constexpr size_t kMaxBatchedSlotRemovals = 32;

struct PendingSlotRemoval {
  void* slot = nullptr;
  uint32_t count = 0;
};

void remove_frame_slots(ShadowStack& stack, size_t first, size_t end) {
  PendingSlotRemoval batch[kMaxBatchedSlotRemovals];
  size_t batch_size = 0;
  bool overflowed = false;
  for (size_t index = first; index < end; ++index) {
    void* slot = stack.roots[index].slot;
    size_t position = 0;
    while (position < batch_size && batch[position].slot != slot) ++position;
    if (position == batch_size) {
      if (batch_size == kMaxBatchedSlotRemovals) {
        overflowed = true;
        break;
      }
      batch[batch_size].slot = slot;
      batch[batch_size].count = 0;
      ++batch_size;
    }
    ++batch[position].count;
  }
  if (overflowed) {
    for (size_t index = first; index < end; ++index)
      remove_slot_state(stack, stack.roots[index].slot);
    return;
  }
  for (size_t position = 0; position < batch_size; ++position)
    remove_slot_state(stack, batch[position].slot, batch[position].count);
}

__forceinline bool shadow_has_pointer_root(ShadowStack& stack, void* slot) {
  // The memo answers for the slots a loop reuses; anything else falls back to
  // one probe in the per-thread index. Neither is ever read by the collector.
  if (stack.pointer_roots.contains(slot)) return true;
  const SlotState* found = stack.slot_states.find(slot);
  return found != nullptr && found->pointer_root;
}

ObjectHeader* find_pending_header(MutatorState& mutator, void* object) {
  for (ObjectHeader* header = mutator.pending_objects; header;
       header = header->tlab_next) {
    if (header->payload() == object) return header;
  }
  return nullptr;
}

}  // namespace

extern "C" {

void* nrq_gc_allocate(size_t bytes, nrq_gc_trace_fn trace) {
  if (bytes == 0 || bytes > SIZE_MAX - sizeof(ObjectHeader)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, bytes, 1, trace, nullptr, 0, handled, nullptr);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, bytes, 1, trace, nullptr, 0, nullptr);
}

void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace) {
  if (count == 0 || element_size == 0) return nullptr;
  // The instance records its element count in 32 bits, so a longer array is
  // rejected rather than truncated.
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, element_size, count, trace, nullptr, 0, handled,
                     nullptr);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, element_size, count, trace, nullptr, 0, nullptr);
}

void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count) {
  if (count == 0 || element_size == 0) return nullptr;
  // The instance records its element count in 32 bits, so a longer array is
  // rejected rather than truncated.
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, element_size, count, nullptr,
                               reference_offsets, reference_count, handled,
                               nullptr);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, element_size, count, nullptr,
                         reference_offsets, reference_count, nullptr);
}

void* allocate_array_rooted_impl(void** root_slot, size_t count,
                                 size_t element_size,
                                 const size_t* reference_offsets,
                                 size_t reference_count,
                                 const nrq_object_layout* resolved);

// A descriptor the compiler emits is read-only data, so the production
// configuration trusts it: no validation runs on an allocation path. The
// development configuration proves every field, because a table that disagrees
// with its payload would corrupt tracing rather than fail an allocation.
//
// It proves a caller-supplied descriptor on every call. That descriptor's
// address is not a stable identity to cache: the contract asks for storage
// that outlives the objects, but an address can be reused by a different
// descriptor once the first one is gone, and a pointer key would then skip the
// check that exists to catch exactly that. Only the interned pool has
// addresses whose identity the runtime owns.
bool layout_is_consistent(const nrq_object_layout& layout) {
  if (layout.payload_size == 0) return false;
  if ((layout.flags & ~static_cast<uint32_t>(NRQ_OBJECT_LAYOUT_ARRAY)) != 0)
    return false;
  if (layout.reference_count == 0) return true;
  if (!layout.reference_offsets) return false;
  for (uint32_t index = 0; index < layout.reference_count; ++index) {
    const size_t offset = layout.reference_offsets[index];
    if (offset > layout.payload_size ||
        sizeof(void*) > layout.payload_size - offset)
      return false;
  }
  return true;
}

void* nrq_gc_allocate_layout(const nrq_object_layout* layout) {
  if (!layout || (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  const size_t bytes = layout->payload_size;
  if (bytes == 0 || bytes > SIZE_MAX - sizeof(ObjectHeader)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object =
      allocate_tlab(bytes, bytes, 1, layout->trace, nullptr, 0, handled,
                    layout);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, bytes, 1, layout->trace, nullptr, 0, layout);
}

void* nrq_gc_allocate_array_layout(const nrq_object_layout* layout,
                                   size_t count) {
  if (!layout || count == 0) return nullptr;
  if ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, layout->payload_size, bytes))
    return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, layout->payload_size, count, layout->trace,
                              nullptr, 0, handled, layout);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, layout->payload_size, count, layout->trace,
                         nullptr, 0, layout);
}

void* nrq_gc_allocate_array_layout_rooted(void** root_slot,
                                          const nrq_object_layout* layout,
                                          size_t count) {
  if (!root_slot || !layout || count == 0) return nullptr;
  if ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  return allocate_array_rooted_impl(root_slot, count, layout->payload_size,
                                    nullptr, 0, layout);
}
// Rooted allocation shared by the offset-table and the descriptor entry points:
// the root slot is published inside the same barrier region as the allocation,
// so a collection can never see the object before the root names it.
void* allocate_array_rooted_impl(void** root_slot, size_t count,
                                 size_t element_size,
                                 const size_t* reference_offsets,
                                 size_t reference_count,
                                 const nrq_object_layout* resolved) {
  if (!root_slot || count == 0 || element_size == 0) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  if (thread_mutator.depth != 0) {
    ShadowStack& stack = ensure_shadow_stack();
    // O(1) root slot validation through the per-thread slot index.
    if (!shadow_has_pointer_root(stack, root_slot))
      nrq_runtime_fatal(
          "rooted GC allocation requires a registered pointer root slot");
    // The reader count spans allocation and root publication. The collector
    // cannot take its root snapshot in that window, so the object becomes
    // reachable before any snapshot can decide it is garbage.
    if (begin_fast_barrier()) {
      bool handled = false;
      void* object = allocate_tlab(bytes, element_size, count, nullptr,
                                   reference_offsets, reference_count, handled,
                                   resolved);
      if (handled) {
        *root_slot = object;
        end_fast_barrier();
        return object;
      }
      end_fast_barrier();
    }
  }

  HeapLockGuard lock(gc_mutex);
  if (thread_shadow_stack &&
      !shadow_has_pointer_root(*thread_shadow_stack, root_slot))
    nrq_runtime_fatal(
        "rooted GC allocation requires a registered pointer root slot");
  void* object = allocate_locked(bytes, element_size, count, nullptr,
                                 reference_offsets, reference_count, resolved);
  store_root_value_locked(root_slot, object);
  return object;
}

void* nrq_gc_allocate_array_typed_rooted(
    void** root_slot, size_t count, size_t element_size,
    const size_t* reference_offsets, size_t reference_count) {
  return allocate_array_rooted_impl(root_slot, count, element_size,
                                    reference_offsets, reference_count,
                                    nullptr);
}

void nrq_gc_root_register(void** slot) {
  static constexpr size_t pointer_offset = 0;
  nrq_gc_root_register_typed(slot, sizeof(void*), &pointer_offset, 1);
}

void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count) {
  if (!slot || object_size == 0 || reference_count == 0 || !reference_offsets)
    nrq_runtime_fatal("GC root registration received an invalid root descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > object_size || sizeof(void*) > object_size - offset)
      nrq_runtime_fatal("GC root registration contains an out-of-range pointer offset");
  }
  RootInfo info;
  info.object_size = object_size;
  info.references = reference_count;
  info.layout = root_layout(object_size, object_size, 0, reference_offsets,
                            reference_count);
  if (!info.layout)
    nrq_runtime_fatal("GC root registration received an undescribable layout");

  ShadowStack& stack = ensure_shadow_stack();
  auto push_root = [&]() {
    try {
      const bool is_ptr_root = is_pointer_root(info);
      stack.roots.push_back(ShadowRoot{slot, std::move(info)});
      // Validate and count this slot in O(1). A slot may be registered more
      // than once (nested frames or repeated registration); the per-slot count
      // keeps the pointer-root index alive until the last owner exits.
      add_slot_state(stack, slot, stack.roots.back().info, is_ptr_root);
    } catch (...) {
      nrq_runtime_fatal("GC root registration failed to track the current thread");
    }
  };

  // Fast path: no collector mutex, and no stack mutex unless a diagnostic query
  // could be reading this stack right now.
  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      push_root();
    } else {
      push_root();
    }
    end_fast_barrier();
    return;
  }

  HeapLockGuard heap_lock(gc_mutex);
  {
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    // Re-read under gc_mutex. If the collector snapshotted before this push,
    // prepare_cycle has already advanced the phase to Marking; if it has not,
    // the root will be part of the snapshot.
    const CollectionPhase phase =
        collection_phase.load(std::memory_order_acquire);
    if (phase == CollectionPhase::Marking ||
        phase == CollectionPhase::Sweeping)
      mark_root_values_locked(slot, info);
    push_root();
  }
}

// Word 0 is the slot count and word 1 is the table's own length.
constexpr size_t kFrameRootHeaderWords = 2;

// Shared body of the two frame-registration entry points. `validate` is a
// compile-time constant at both call sites, so the development configuration
// keeps every check and the production configuration for a compiler-built table
// contains none of them.
static void register_frame_roots(void* const* slots, size_t slot_count,
                                 const size_t* descriptor, bool validate) {
  if (slot_count == 0) return;
  if (!slots || !descriptor)
    nrq_runtime_fatal("GC frame root registration received an invalid descriptor");

  if (validate) {
    if (descriptor[0] != slot_count)
      nrq_runtime_fatal(
          "GC frame root registration descriptor does not match its slot count");

    for (size_t index = 0; index < slot_count; ++index) {
      if (!slots[index])
        nrq_runtime_fatal("GC frame root registration contains a null slot");
    }
  }

  if (validate && !thread_validated_descriptors.matches(descriptor)) {
    // The table states its own length, so a truncated descriptor is rejected
    // before the walk can read past it. A table with no room for its own header or
    // for the roots it claims is malformed in the same way.
    const size_t word_count = descriptor[1];
    if (word_count < 2 || word_count < 2 + 2 * slot_count)
      nrq_runtime_fatal(
          "GC frame root registration descriptor is shorter than its roots");

    // Validate the entire table before publishing any root: a malformed
    // descriptor must fail with the frame untouched, not halfway registered.
    size_t cursor = 2;
    for (size_t index = 0; index < slot_count; ++index) {
      const size_t object_size = descriptor[cursor++];
      const size_t reference_count = descriptor[cursor++];
      if (object_size == 0 || reference_count == 0)
        nrq_runtime_fatal("GC frame root registration contains an empty root");
      for (size_t reference = 0; reference < reference_count; ++reference) {
        const size_t offset = descriptor[cursor + reference];
        if (offset > object_size || sizeof(void*) > object_size - offset)
          nrq_runtime_fatal(
              "GC frame root registration contains an out-of-range pointer offset");
      }
      cursor += reference_count;
    }
    // The words the roots described have to account for the whole table.
    if (cursor != word_count)
      nrq_runtime_fatal(
          "GC frame root registration descriptor length does not match its roots");
    thread_validated_descriptors.insert(descriptor);
  }

  ShadowStack& stack = ensure_shadow_stack();
  // One reservation for the frame keeps a wide activation from growing the
  // vector root by root; the per-root metadata itself stays allocation-free for
  // every layout that fits the inline offset storage.
  auto reserve_roots = [&]() {
    try {
      stack.roots.reserve(stack.roots.size() + slot_count);
    } catch (...) {
      nrq_runtime_fatal("GC frame root registration failed to reserve roots");
    }
  };
  // Reads one slot's metadata out of the descriptor, advancing the cursor, and
  // pushes it. The cursor walks the table once, so a wide frame costs one pass.
  auto push_root = [&](void* slot, size_t& cursor) {
    RootInfo info;
    info.object_size = descriptor[cursor++];
    const size_t reference_count = descriptor[cursor++];
    info.references = reference_count;
    info.layout = root_layout(info.object_size, info.object_size, 0,
                              descriptor + cursor, reference_count);
    if (!info.layout)
      nrq_runtime_fatal("GC root registration received an undescribable layout");
    cursor += reference_count;
    const bool is_ptr_root = is_pointer_root(info);
    try {
      stack.roots.push_back(ShadowRoot{slot, std::move(info)});
      add_slot_state(stack, slot, stack.roots.back().info, is_ptr_root);
    } catch (...) {
      nrq_runtime_fatal("GC root registration failed to track the current thread");
    }
  };

  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      reserve_roots();
      size_t cursor = kFrameRootHeaderWords;
      for (size_t index = 0; index < slot_count; ++index)
        push_root(slots[index], cursor);
    } else {
      reserve_roots();
      size_t cursor = kFrameRootHeaderWords;
      for (size_t index = 0; index < slot_count; ++index)
        push_root(slots[index], cursor);
    }
    end_fast_barrier();
    return;
  }

  HeapLockGuard heap_lock(gc_mutex);
  {
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    const CollectionPhase phase =
        collection_phase.load(std::memory_order_acquire);
    reserve_roots();
    size_t cursor = kFrameRootHeaderWords;
    for (size_t index = 0; index < slot_count; ++index) {
      // Mark exactly the roots the single-slot path marks: a root pushed while a
      // snapshot is already running has to behave as if it had been part of it.
      if (phase == CollectionPhase::Marking ||
          phase == CollectionPhase::Sweeping) {
        RootInfo info;
        info.object_size = descriptor[cursor];
        const size_t reference_count = descriptor[cursor + 1];
        info.layout = root_layout(info.object_size, info.object_size, 0,
                                  descriptor + cursor + 2, reference_count);
        if (!info.layout)
          nrq_runtime_fatal(
              "GC root registration received an undescribable layout");
        mark_root_values_locked(slots[index], info);
      }
      push_root(slots[index], cursor);
    }
  }
}

// Dynamic entry point: the descriptor is supplied by the caller, so it is
// validated on every activation (memoised only for an identical table).
void nrq_gc_root_register_frame(void* const* slots, size_t slot_count,
                                const size_t* descriptor) {
  register_frame_roots(slots, slot_count, descriptor, /*validate=*/true);
}

// Static-descriptor entry point. Only the lowering emits this, and only with a
// table it built itself: `nrq_gc_roots_*` is a constant private global whose
// words the compiler derives from the slots it is about to publish, so the table
// cannot disagree with the frame. The development configuration still validates
// it, because a bug in that derivation must be caught where it is cheap to
// diagnose; the production configuration skips the walk entirely, which is the
// whole point of a separate entry point.
void nrq_gc_root_register_frame_static(void* const* slots, size_t slot_count,
                                       const size_t* descriptor) {
  register_frame_roots(slots, slot_count, descriptor,
                       /*validate=*/nirq::runtime_debug_enabled);
}

void nrq_gc_root_register_array(void* data, size_t count, size_t element_size,
                                const size_t* reference_offsets,
                                size_t reference_count) {
  if (!data || count == 0) return;
  if (element_size == 0 || reference_count == 0 || !reference_offsets ||
      count > SIZE_MAX / element_size)
    nrq_runtime_fatal("GC array-root registration received an invalid descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > element_size || sizeof(void*) > element_size - offset)
      nrq_runtime_fatal("GC array-root registration contains an invalid pointer offset");
  }
  HeapLockGuard lock(gc_mutex);
  auto found = roots.find(data);
  if (found != roots.end()) {
    nrq_runtime_fatal("GC array-root storage address is already registered");
  }
  try {
    RootInfo root;
    root.references = 1;
    root.object_size = count * element_size;
    root.repeated_count = count;
    root.layout = root_layout(root.object_size, element_size, count,
                              reference_offsets, reference_count);
    if (!root.layout)
      nrq_runtime_fatal(
          "GC array-root registration received an undescribable layout");
    roots.emplace(data, std::move(root));
  } catch (...) {
    nrq_runtime_fatal("GC array-root registration failed to allocate metadata");
  }
}

void nrq_gc_root_clear_array_references(void* data, size_t count,
                                        size_t element_size,
                                        const size_t* reference_offsets,
                                        size_t reference_count) {
  if (!data || count == 0) return;
  if (element_size == 0 || reference_count == 0 || !reference_offsets ||
      count > SIZE_MAX / element_size)
    nrq_runtime_fatal("GC array-root initialization received an invalid descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > element_size || sizeof(void*) > element_size - offset)
      nrq_runtime_fatal("GC array-root initialization contains an invalid pointer offset");
  }
  HeapLockGuard lock(gc_mutex);
  if (roots.find(data) != roots.end())
    nrq_runtime_fatal("GC array-root initialization requires unregistered storage");
  auto* bytes = static_cast<unsigned char*>(data);
  for (size_t element = 0; element < count; ++element) {
    unsigned char* element_bytes = bytes + element * element_size;
    for (size_t index = 0; index < reference_count; ++index)
      std::memset(element_bytes + reference_offsets[index], 0, sizeof(void*));
  }
}

void nrq_gc_root_unregister(void** slot) {
  if (!slot) return;
  if (thread_shadow_stack) {
    ShadowStack& stack = *thread_shadow_stack;
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    for (auto entry = stack.roots.rbegin(); entry != stack.roots.rend();
         ++entry) {
      if (entry->slot != slot) continue;
      remove_slot_state(stack, slot);
      stack.roots.erase(std::next(entry).base());
      return;
    }
  }
  // Array/external roots live in the global table and may be released by their
  // owner from any thread.
  HeapLockGuard lock(gc_mutex);
  auto found = roots.find(slot);
  // Compiler cleanup is emitted for owned arrays even when their element type
  // contains no managed references and no root region was registered. Preserve
  // the historical no-op for an unknown external root.
  if (found == roots.end()) return;
  if (found->second.references == 0)
    nrq_runtime_fatal("GC root unregistration count underflow");
  if (--found->second.references == 0) roots.erase(found);
}

void nrq_gc_root_store(void** slot, void* value) {
  if (!slot) return;
  // Rewriting the referent a root already holds creates no new edge, so the store
  // is not observable by the collector and needs neither the barrier nor the
  // handshake. A snapshot-at-the-beginning marker would only re-mark a value the
  // slot never stopped holding.
  if (*slot == value) return;
  // With no cycle in progress the store cannot be observed by any collector: a
  // cycle only snapshots this thread's roots after the thread reaches a
  // safepoint and parks, and parking happens after the store. Nothing needs
  // marking and nothing can be swept, so the handshake buys nothing here; the
  // old-to-young buffer, when the barrier is raised, is append-only and is
  // drained under the heap lock at the next cycle. This is the common case for
  // a mutator that collects only at its own safepoints.
  if (thread_mutator.depth != 0 &&
      collection_phase.load(std::memory_order_acquire) ==
          CollectionPhase::Idle) {
    *slot = value;
    buffer_remembered(value);
    return;
  }
  // Language-generated stores execute inside a registered mutator. The fast
  // path is just a pointer store plus a reader-count handshake; the collector
  // waits for in-flight readers before snapshotting roots.
  if (thread_mutator.depth != 0 && begin_fast_barrier()) {
    *slot = value;
    // Clearing a root cannot create an old-to-young edge, so only a non-null
    // referent is worth recording.
    if (value) buffer_remembered(value);
    end_fast_barrier();
    return;
  }
  HeapLockGuard lock(gc_mutex);
  store_root_value_locked(slot, value);
}

void scan_pointer_words_locked(void* storage, size_t bytes, bool mark) {
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    if (!reference) continue;
    if (mark) mark_value_locked(reference);
    defer_remembered_locked(reference);
  }
}

void nrq_gc_root_copy(void* destination, const void* source, size_t bytes) {
  if (!destination || !source || bytes == 0) return;
  if (thread_mutator.depth != 0 && begin_fast_barrier()) {
    std::memcpy(destination, source, bytes);
    buffer_pointer_words(destination, bytes);
    end_fast_barrier();
    return;
  }
  HeapLockGuard lock(gc_mutex);
  const CollectionPhase phase =
      collection_phase.load(std::memory_order_relaxed);
  if (phase == CollectionPhase::Marking)
    scan_pointer_words_locked(destination, bytes, /*mark=*/true);
  std::memcpy(destination, source, bytes);
  // The destination may now reference young objects even when the containing
  // object is old; record them for the next minor collection. During sweep,
  // marking new referents prevents the next slice from reclaiming them.
  scan_pointer_words_locked(destination, bytes,
                            phase != CollectionPhase::Idle);
}

static uint64_t start_root_frame() {
  // stack.frames, next_marker, and the root-depth checkpoint are thread-local
  // bookkeeping only. The collector never reads stack.frames; it only reads
  // stack.roots under stack.mutex. No mutex is required here.
  ShadowStack& stack = ensure_shadow_stack();
  if (stack.next_marker == UINT64_MAX)
    nrq_runtime_fatal("GC root frame marker space exhausted");
  const uint64_t marker = stack.next_marker++;
  try {
    stack.frames.push_back(ShadowStack::Frame{marker, stack.roots.size()});
  } catch (...) {
    nrq_runtime_fatal("GC root frame registration failed to allocate thread metadata");
  }
  debug_emit(NRQ_GC_EVENT_ROOT_FRAME, 1, marker, nullptr);
  return marker;
}

uint64_t nrq_gc_root_frame_begin(void) { return start_root_frame(); }

// The trusted fused entry. Generated code always opens a frame immediately
// before publishing that frame's static roots, so doing both here removes a
// second call and a second shadow-stack re-entry per activation. The frame
// record is pushed first, exactly as the split pair did; a registration failure
// terminates the process before the caller can observe anything, so a frame is
// never visible to the collector with only part of its roots published.
// `descriptor` must be the compiler-emitted table for exactly these slots -
// the same precondition `nrq_gc_root_register_frame_static` documents.
uint64_t nrq_gc_root_frame_open(void* const* slots, size_t slot_count,
                                const size_t* descriptor) {
  const uint64_t marker = start_root_frame();
  register_frame_roots(slots, slot_count, descriptor,
                       /*validate=*/nirq::runtime_debug_enabled);
  return marker;
}

void nrq_gc_root_frame_end(uint64_t marker) {
  if (!thread_shadow_stack)
    nrq_runtime_fatal("GC root frame ended without a shadow stack");
  ShadowStack& stack = *thread_shadow_stack;
  // stack.frames is thread-private; validate without a lock.
  if (stack.frames.empty() || stack.frames.back().marker != marker ||
      stack.frames.back().root_depth > stack.roots.size())
    nrq_runtime_fatal("GC root frames must end in LIFO order with a valid marker");
  const size_t target_depth = stack.frames.back().root_depth;
  stack.frames.pop_back();
  debug_emit(NRQ_GC_EVENT_ROOT_FRAME, 0, marker, nullptr);

  // Remove exactly the registrations owned by this frame. Complexities of this
  // path are proportional to the number of roots the frame leaves behind, not
  // to all roots still active on the thread.
  auto remove_owned_roots = [&]() {
    remove_frame_slots(stack, target_depth, stack.roots.size());
    stack.roots.resize(target_depth);
  };

  // Shrinking stack.roots requires coordination with the collector, which reads
  // roots under stack.mutex. Use the fast barrier when idle so the collector
  // cannot begin a snapshot between the resize and this function returning.
  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> lock(stack.mutex);
      remove_owned_roots();
    } else {
      remove_owned_roots();
    }
    end_fast_barrier();
    return;
  }
  // Collector is active; hold stack.mutex while shrinking roots so the
  // collector sees a consistent view during its enumeration.
  std::lock_guard<std::mutex> lock(stack.mutex);
  remove_owned_roots();
}

#if NRQ_DEBUG_ENABLED
size_t nrq_gc_debug_snapshot_roots(nrq_gc_debug_root* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  // A fast-path root push does not take the stack mutex, so reading a stack
  // needs the same handshake the reference-count query uses: publish that a
  // diagnostic reader is active, which makes every later push synchronize, then
  // drain the pushes that entered the lock-free path before the flag became
  // visible. Only then is every stack this function walks either quiescent or
  // mutex-protected.
  const bool previous = reference_diagnostics_enabled.exchange(
      true, std::memory_order_acq_rel);
  struct RestoreFlag {
    bool previous;
    ~RestoreFlag() {
      reference_diagnostics_enabled.store(previous, std::memory_order_release);
    }
  } restore{previous};
  wait_for_fast_barriers();

  size_t written = 0;
  auto record = [&](void* slot, const RootInfo& info) {
    if (written == capacity) return;
    nrq_gc_debug_root& entry = out[written++];
    entry = nrq_gc_debug_root{};
    entry.slot = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slot));
    const nrq_object_layout* layout = info.layout;
    entry.reference_count = layout ? layout->reference_count : 0u;
    entry.element_size = static_cast<uint32_t>(layout ? layout->payload_size : 0);
    entry.element_count = info.repeated_count != 0
                             ? static_cast<uint32_t>(info.repeated_count)
                             : 1u;
    entry.is_pointer_root = is_pointer_root(info) ? 1u : 0u;
    if (!slot || !layout || layout->reference_count == 0) return;
    // Report the object the slot currently holds at its first reference offset.
    // For an array root that is the first element's first reference.
    const size_t span = layout->payload_size;
    const size_t offset = layout->reference_offsets[0];
    if (offset > span || sizeof(void*) > span - offset) return;
    void* referent = nullptr;
    std::memcpy(&referent, static_cast<const unsigned char*>(slot) + offset,
                sizeof(referent));
    entry.referent = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(referent));
  };

  {
    HeapLockGuard lock(gc_mutex);
    for (const auto& root : roots) record(root.first, root.second);
    for (ShadowStack* stack : shadow_stacks) {
      if (!stack) continue;
      std::lock_guard<std::mutex> stack_lock(stack->mutex);
      for (const ShadowRoot& root : stack->roots) record(root.slot, root.info);
    }
  }
  return written;
}
#endif  // NRQ_DEBUG_ENABLED

void nrq_gc_enable(void) {
  automatic_collection_enabled.store(true, std::memory_order_relaxed);
}

void nrq_gc_disable(void) {
  automatic_collection_enabled.store(false, std::memory_order_relaxed);
}

void nrq_gc_collect(void) {
  run_collection(/*young_only=*/false);
}

void nrq_gc_collect_minor(void) {
  // With generations disabled every object is old, so a young-only cycle could
  // not reclaim anything. Treat the explicit request as a full collection.
  run_collection(
      generational_collection_enabled.load(std::memory_order_relaxed));
}

void nrq_gc_set_generational(int32_t enabled) {
  if (enabled) {
    // Raise the derived switch first: a mutator that still sees the switch raised
    // after generations are turned off only records extra entries.
    store_barrier_active.store(true, std::memory_order_release);
    generational_collection_enabled.store(true, std::memory_order_relaxed);
    return;
  }
  store_barrier_active.store(false, std::memory_order_release);
  generational_collection_enabled.store(false, std::memory_order_relaxed);
}

int32_t nrq_gc_is_generational(void) {
  return generational_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

uint64_t nrq_gc_get_minor_collection_count(void) {
  HeapLockGuard lock(gc_mutex);
  return minor_collections;
}

uint64_t nrq_gc_get_major_collection_count(void) {
  HeapLockGuard lock(gc_mutex);
  return major_collections;
}

void nrq_gc_mutator_enter(void) {
  if (thread_mutator.depth == SIZE_MAX)
    nrq_runtime_fatal("GC mutator nesting depth overflow");
  thread_gc_cleanup.arm();
  if (thread_mutator.depth++ != 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) wait_until_not_requested(lock);
  try {
    mutators.push_back(&thread_mutator);
    ++registered_mutator_count;
  } catch (...) {
    thread_mutator.depth = 0;
    nrq_runtime_fatal("GC mutator registration failed");
  }
}

void nrq_gc_mutator_leave(void) {
  if (thread_mutator.depth == 0)
    nrq_runtime_fatal("GC mutator leave without matching enter");
  if (--thread_mutator.depth != 0) return;

  std::lock_guard<std::mutex> lock(mutator_mutex);
  {
    HeapLockGuard heap_lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
    // This thread is never visited by cycle preparation again, so the space it
    // reserved has to be returned to its pages here.
    flush_spare_runs_locked(thread_mutator);
    release_tlab_tail_locked(thread_mutator);
    tlab_tail_bytes_total =
        saturated_add(tlab_tail_bytes_total, thread_mutator.tlab_tail_bytes);
    if (thread_mutator.tlab_tail_max_bytes > tlab_tail_max_bytes_total)
      tlab_tail_max_bytes_total = thread_mutator.tlab_tail_max_bytes;
    tlab_tail_reclaimed_bytes_total = saturated_add(
        tlab_tail_reclaimed_bytes_total,
        thread_mutator.tlab_tail_reclaimed_bytes);
    drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
    // A thread that leaves the registry is never visited by cycle preparation
    // again, so its buffered handoffs have to be settled here.
    drain_transfer_pins_locked(thread_mutator);
  }
  thread_mutator.parked = false;
  auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
  if (found == mutators.end())
    nrq_runtime_fatal("GC mutator registry lost an active thread");
  mutators.erase(found);
  --registered_mutator_count;
  // The collector's park predicate compares against the registry size, so a
  // shrinking registry has to wake it.
  collector_condition.notify_all();
  mutator_condition.notify_all();
}

void nrq_gc_safepoint(void) {
  // The common path is a single acquire load. Only threads that observe a
  // request pay for the mutex and condition-variable protocol.
  if (!collection_is_requested()) return;
  if (thread_is_collector || thread_mutator.depth == 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  if (!collection_is_requested()) return;
  park_mutator(lock);
}

void nrq_gc_set_threshold(size_t bytes) {
  // An explicit threshold is a policy decision by the program, so automatic
  // adaptation stops here until it is turned back on.
  HeapLockGuard lock(gc_mutex);
  collection_policy.adaptive = false;
  collection_policy.adaptations = 0;
  allocation_accounting.collection_threshold.store(bytes == 0 ? 1 : bytes,
                                                   std::memory_order_relaxed);
}

size_t nrq_gc_get_threshold(void) {
  HeapLockGuard lock(gc_mutex);
  return allocation_accounting.collection_threshold.load(
      std::memory_order_relaxed);
}

void nrq_gc_set_adaptive(int32_t enabled) {
  HeapLockGuard lock(gc_mutex);
  collection_policy.adaptive = enabled != 0;
  // Re-enabling starts from the current live set instead of waiting for the next
  // cycle to correct a threshold the program may have pinned long ago.
  if (collection_policy.adaptive) adapt_collection_policy_locked();
}

int32_t nrq_gc_policy_is_adaptive(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.adaptive ? 1 : 0;
}

uint64_t nrq_gc_policy_adaptation_count(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.adaptations;
}

uint64_t nrq_gc_last_reclaimed_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.last_reclaimed_bytes;
}

size_t nrq_gc_object_header_bytes(void) { return sizeof(ObjectHeader); }

const nrq_object_layout* nrq_gc_layout_intern(
    size_t payload_bytes, size_t element_bytes, nrq_gc_trace_fn trace,
    const size_t* reference_offsets, size_t reference_count) {
  // The descriptor stores these as 32-bit fields; a payload larger than that is
  // left to a custom trace callback rather than described by a table.
  if (payload_bytes == 0 || payload_bytes > UINT32_MAX) return nullptr;
  if (reference_count > UINT32_MAX) return nullptr;
  // An array element and its payload are the same bytes: `element_bytes` only
  // says whether the payload repeats.
  if (element_bytes != 0 && element_bytes != payload_bytes) return nullptr;
  if (reference_count != 0 && !reference_offsets) return nullptr;
  LayoutKey key{payload_bytes, element_bytes, trace, {}};
  key.offsets.reserve(reference_count);
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    // A reference word must fit inside the record (or one element, which is the
    // payload of an array). Rejecting it here keeps every reader able to load a
    // pointer without a bounds test.
    if (offset > payload_bytes || sizeof(void*) > payload_bytes - offset)
      return nullptr;
    key.offsets.push_back(static_cast<uint32_t>(offset));
  }

  std::lock_guard<std::mutex> lock(layout_mutex);
  const auto existing = layout_pool.find(key);
  if (existing != layout_pool.end()) return existing->second;

  auto* offsets = new (std::nothrow) uint32_t[key.offsets.size()];
  if (!offsets && !key.offsets.empty()) return nullptr;
  for (size_t index = 0; index < key.offsets.size(); ++index)
    offsets[index] = key.offsets[index];
  auto* descriptor = new (std::nothrow) nrq_object_layout{
      static_cast<uint32_t>(payload_bytes),
      static_cast<uint32_t>(key.offsets.size()),
      element_bytes != 0 ? static_cast<uint32_t>(NRQ_OBJECT_LAYOUT_ARRAY) : 0u,
      offsets, trace};
  if (!descriptor) {
    delete[] offsets;
    return nullptr;
  }
  try {
    layout_bytes_total += sizeof(nrq_object_layout) + key.offsets.size() * sizeof(uint32_t);
    layout_pool.emplace(std::move(key), descriptor);
  } catch (...) {
    delete descriptor;
    delete[] offsets;
    return nullptr;
  }
  return descriptor;
}

size_t nrq_gc_layout_count(void) {
  std::lock_guard<std::mutex> lock(layout_mutex);
  return layout_pool.size();
}
size_t nrq_gc_layout_bytes(void) {
  std::lock_guard<std::mutex> lock(layout_mutex);
  return layout_bytes_total;
}

nrq_gc_allocation_facts nrq_gc_get_allocation_facts(void) {
  nrq_gc_allocation_facts facts{};
  facts.object_header_bytes = sizeof(ObjectHeader);
  // Measured from the layout rather than restated, so the split cannot drift
  // away from the struct it describes; the layout pointer is declared last.
  facts.object_instance_bytes = offsetof(ObjectHeader, layout);
  facts.object_shared_bytes = sizeof(ObjectHeader) - offsetof(ObjectHeader, layout);
  facts.payload_alignment = kObjectStartAlignment;
  facts.page_bytes = kGcPageBytes;
  facts.tlab_bytes = kGcTlabBytes;
  facts.tlab_min_slab_bytes = kGcTlabMinSlabBytes;
  facts.spare_run_capacity = MutatorState::kSpareRunCapacity;
  facts.layout_descriptors = nrq_gc_layout_count();
  facts.layout_bytes = nrq_gc_layout_bytes();
  return facts;
}

int32_t nrq_gc_get_allocation_profile(size_t payload_bytes,
                                  nrq_gc_allocation_profile* out) {
  if (!out) return 0;
  if (payload_bytes > SIZE_MAX - sizeof(ObjectHeader) - kObjectStartAlignment)
    return 0;
  const size_t raw_span = sizeof(ObjectHeader) + payload_bytes;
  const size_t span =
      (raw_span + kObjectStartAlignment - 1) & ~(kObjectStartAlignment - 1);
  // A page is at least as large as the span it must hold, exactly as
  // take_span_locked sizes it.
  const size_t capacity = std::max(kGcPageBytes, span);
  const size_t slots = capacity / span;
  nrq_gc_allocation_profile profile{};
  profile.payload_bytes = payload_bytes;
  profile.span_bytes = span;
  profile.slots_per_page = slots;
  profile.page_slack_bytes = capacity - slots * span;
  profile.page_bytes = capacity;
  // A reservation never spans a page boundary, so one slab serves the objects a
  // page holds - or one, when a single object fills the page.
  profile.objects_per_refill = slots;
  *out = profile;
  return 1;
}

size_t nrq_gc_page_count(void) {
  HeapLockGuard lock(gc_mutex);
  return gc_pages.size();
}

size_t nrq_gc_recycled_page_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_pages.size();
}

uint64_t nrq_gc_page_reuse_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_page_reuse_count;
}

uint64_t nrq_gc_span_reuse_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_span_reuse_count;
}

size_t nrq_gc_object_index_entries(void) {
  HeapLockGuard lock(gc_mutex);
  return object_index.size();
}

int32_t nrq_gc_pointer_is_managed(void* object) {
  HeapLockGuard lock(gc_mutex);
  return find_object_locked(object) ? 1 : 0;
}

const nrq_object_layout* nrq_gc_object_layout(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectHeader* header = find_object_locked(object);
  return header ? header->layout : nullptr;
}

uint32_t nrq_gc_object_state_bits(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectHeader* header = find_object_locked(object);
  return header ? header->state_word() : 0;
}

int32_t nrq_gc_store_barrier_active(void) {
  return store_barrier_active.load(std::memory_order_acquire) ? 1 : 0;
}

uint64_t nrq_gc_tlab_refill_count(void) {
  HeapLockGuard lock(gc_mutex);
  return tlab_refill_count;
}
uint64_t nrq_gc_tlab_tail_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t total = tlab_tail_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator) total = saturated_add(total, mutator->tlab_tail_bytes);
  return total;
}

uint64_t nrq_gc_tlab_tail_max_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t largest = tlab_tail_max_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator && mutator->tlab_tail_max_bytes > largest)
      largest = mutator->tlab_tail_max_bytes;
  return largest;
}

uint64_t nrq_gc_tlab_tail_reclaimed_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t total = tlab_tail_reclaimed_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator) total = saturated_add(total, mutator->tlab_tail_reclaimed_bytes);
  return total;
}
size_t nrq_gc_free_run_count(void) {
  HeapLockGuard lock(gc_mutex);
  return total_free_spans;
}

size_t nrq_gc_page_with_runs_count(void) {
  HeapLockGuard lock(gc_mutex);
  return span_pages.size();
}

uint64_t nrq_gc_reused_span_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  return reused_span_bytes;
}

nrq_gc_stats nrq_gc_get_stats(void) {
  // The registered-mutator vector is mutated under mutator_mutex. Take it
  // before gc_mutex to match the enter/leave lock order; taking only gc_mutex
  // would race a concurrent registration.
  std::lock_guard<std::mutex> registry_lock(mutator_mutex);
  HeapLockGuard lock(gc_mutex);
  nrq_gc_stats result = statistics;
  // Unpublished TLAB objects are not in `statistics` yet, but they are live.
  // Sum the per-mutator batches instead of maintaining a global pending pair
  // that every allocation would have to update.
  uint64_t pending_bytes = 0;
  uint64_t pending_count = 0;
  for (const MutatorState* mutator : mutators) {
    if (!mutator) continue;
    pending_bytes = saturated_add(
        pending_bytes, mutator->pending_bytes.load(std::memory_order_relaxed));
    pending_count = saturated_add(
        pending_count, mutator->pending_count.load(std::memory_order_relaxed));
  }
  result.allocated_bytes = saturated_add(result.allocated_bytes, pending_bytes);
  result.live_objects = saturated_add(result.live_objects, pending_count);
  return result;
}

uint64_t nrq_gc_get_allocated_bytes(void) {
  return nrq_gc_get_stats().allocated_bytes;
}

uint64_t nrq_gc_get_collected_bytes(void) {
  return nrq_gc_get_stats().collected_bytes;
}

uint64_t nrq_gc_get_collection_count(void) {
  return nrq_gc_get_stats().collections;
}

uint64_t nrq_gc_get_live_objects(void) {
  return nrq_gc_get_stats().live_objects;
}

void* nrq_gc_pin(void* object) {
  if (!object) return nullptr;
  HeapLockGuard lock(gc_mutex);
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!find_object_locked(object)) return nullptr;
  if (collection_phase.load(std::memory_order_relaxed) !=
      CollectionPhase::Idle)
    mark_value_locked(object);
  auto existing = pinned_objects.find(object);
  if (existing != pinned_objects.end() && existing->second >= kMaxPinsPerObject) {
    pin_violations = saturated_add(pin_violations, 1);
    return nullptr;
  }
  try {
    ++pinned_objects[object];
    return object;
  } catch (...) {
    return nullptr;
  }
}

uint64_t nrq_gc_pin_count(void) {
  HeapLockGuard lock(gc_mutex);
  // Fold in this thread's buffered deltas so a query made right after a managed
  // return reflects it. Other threads' buffers stay private until the next cycle;
  // they are only ever folded while their owner is parked or holding this lock.
  drain_transfer_pins_locked(thread_mutator);
  return static_cast<uint64_t>(pinned_objects.size());
}

uint64_t nrq_gc_transfer_pin_fallbacks(void) {
  return transfer_pin_fallbacks.load(std::memory_order_relaxed);
}

uint64_t nrq_gc_pin_violations(void) {
  HeapLockGuard lock(gc_mutex);
  return pin_violations;
}

int32_t nrq_gc_is_enabled(void) {
  return automatic_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static void unpin_value(void* object);

void nrq_gc_unpin(void* object) {
  unpin_value(object);
}

// Publishes a pinned managed result into a root slot or field and releases the
// pin in one call, publication first. The release runs in this translation unit,
// so it is inlined into this entry instead of being a second call; one call
// boundary is what this saves. Both routines it calls still open their own
// fast-barrier region, so this entry enters two regions, not one.
void nrq_gc_root_store_release(void** slot, void* value) {
  if (slot) nrq_gc_root_store(slot, value);
  unpin_value(value);
}

static void unpin_value(void* object) {
  if (!object) return;
  bool buffer_fell_back = false;
  // Releasing a pin this thread buffered cancels it here, and releasing a pin the
  // table already knows about is queued as a negative delta. Either way the
  // collector mutex stays out of the path.
  if (begin_fast_barrier()) {
    MutatorState& mutator = thread_mutator;
    TransferPinDelta* pending = find_transfer_delta(mutator, object);
    if (pending && pending->delta > 0) {
      // A buffered pin cancelled before the drain has no effect on the pin table,
      // so the entry stops being active as soon as its delta reaches zero.
      if (--pending->delta == 0) clear_transfer_delta(*pending);
      end_fast_barrier();
      return;
    }
    if (!pending) {
      if (TransferPinDelta* slot = acquire_transfer_delta_slot(mutator)) {
        slot->object = object;
        slot->delta = -1;
        end_fast_barrier();
        return;
      }
      transfer_pin_fallbacks.fetch_add(1, std::memory_order_relaxed);
      // The buffer holds this thread's earlier deltas. Applying this release to the
      // table first would let an earlier buffered pin arrive afterwards and leave the
      // object pinned with nothing left to release, so the buffer is settled in order
      // before the new operation.
      buffer_fell_back = true;
    }
    end_fast_barrier();
  }
  HeapLockGuard lock(gc_mutex);
  if (buffer_fell_back) drain_transfer_pins_locked(thread_mutator);
  auto found = pinned_objects.find(object);
  if (found == pinned_objects.end()) return;
  if (--found->second == 0) pinned_objects.erase(found);
}

void* nrq_gc_pin_transfer(void* object) {
  if (!object) return nullptr;
  bool buffer_fell_back = false;
  // Registered mutators record the handoff in their own buffer instead of taking
  // the collector mutex. The buffer is folded into the pin table during cycle
  // preparation, before pinned roots are enumerated, so a buffered pin protects
  // its referent for exactly the collections a table pin would. Validity of the
  // pointer is checked at that point; code generation only ever transfers managed
  // results.
  if (begin_fast_barrier()) {
    MutatorState& mutator = thread_mutator;
    TransferPinDelta* pending = find_transfer_delta(mutator, object);
    if (pending) {
      // A pin cancels a buffered release, which is the same net effect a table pin
      // followed by the queued release would have had.
      if (++pending->delta == 0) clear_transfer_delta(*pending);
      end_fast_barrier();
      return object;
    }
    if (TransferPinDelta* slot = acquire_transfer_delta_slot(mutator)) {
      slot->object = object;
      slot->delta = 1;
      end_fast_barrier();
      return object;
    }
    transfer_pin_fallbacks.fetch_add(1, std::memory_order_relaxed);
    // Every slot holds a live delta. Leave the barrier region before taking the
    // collector mutex, because a collector holding it waits for barrier regions to
    // drain.
    buffer_fell_back = true;
    end_fast_barrier();
  }
  HeapLockGuard lock(gc_mutex);
  if (buffer_fell_back) drain_transfer_pins_locked(thread_mutator);
  // A managed return usually refers to an object that is still in this
  // thread's unpublished TLAB. Publishing the whole pending batch here would
  // defeat TLAB batching for every helper that returns a managed pointer. The
  // object is already protected: cycle preparation publishes every registered
  // mutator's TLAB before enumerating pinned roots, so a pending pin becomes
  // visible for exactly the collection that needs it.
  if (!find_object_locked(object) &&
      !find_pending_header(thread_mutator, object))
    return nullptr;
  if (collection_phase.load(std::memory_order_relaxed) !=
      CollectionPhase::Idle)
    mark_value_locked(object);
  try {
    size_t& count = pinned_objects[object];
    if (count == SIZE_MAX)
      nrq_runtime_fatal("GC transfer pin count overflow");
    ++count;
    return object;
  } catch (...) {
    nrq_runtime_fatal("GC transfer pin registration failed");
  }
}

void nrq_gc_ref_count_start(void) {
  // Mutators check this before touching their shadow stack without its mutex, so
  // it has to be published before the next query reads any stack.
  reference_diagnostics_enabled.store(true, std::memory_order_release);
}

void nrq_gc_ref_count_stop(void) {
  reference_diagnostics_enabled.store(false, std::memory_order_release);
}

int64_t nrq_gc_get_ref_count(void* object) {
  // Diagnostics are off in normal execution, so a disabled query touches nothing.
  if (!diagnostics_reading_roots()) return -1;
  // The query reads shadow stacks, and a mutator that entered its root fast path
  // before the flag became visible is still inside a lock-free region. Drain
  // those regions first: mutators that observe the flag afterwards take the
  // stack mutex themselves. The mutator registry is taken before the heap lock
  // to keep the documented lock order.
  wait_for_fast_barriers();
  HeapLockGuard lock(gc_mutex);
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!object || !find_object_locked(object)) return 0;
  try {
    ReferenceCountTarget target{object, 0};
    // The diagnostic walks exactly what the marker walks, through the same
    // descriptor, so a reference count can never disagree with marking.
    for_each_live_object_locked([&](ObjectHeader* header) {
      trace_children(header, count_reference_to_target, &target);
    });
    // Counts exactly what marking reads: the same descriptor, the same walk.
    auto count_root = [&](const void* storage, const RootInfo& info) {
      const nrq_object_layout* layout = info.layout;
      if (!layout || layout->reference_count == 0) return;
      const auto* bytes = static_cast<const unsigned char*>(storage);
      const size_t stride = layout->payload_size;
      const size_t elements = info.repeated_count != 0 ? info.repeated_count : 1;
      for (size_t element = 0; element < elements; ++element) {
        for (uint32_t index = 0; index < layout->reference_count; ++index) {
          const size_t offset = layout->reference_offsets[index];
          if (offset > stride || sizeof(void*) > stride - offset) continue;
          void* reference = nullptr;
          std::memcpy(&reference, bytes + element * stride + offset,
                      sizeof(reference));
          count_reference_to_target(reference, &target);
        }
      }
    };
    for (const auto& root : roots) count_root(root.first, root.second);
    for (ShadowStack* stack : shadow_stacks) {
      if (!stack) continue;
      std::lock_guard<std::mutex> stack_lock(stack->mutex);
      for (const ShadowRoot& root : stack->roots)
        count_root(root.slot, root.info);
    }
    auto pinned = pinned_objects.find(object);
    if (pinned != pinned_objects.end())
      target.count = saturated_add(target.count, pinned->second);
    return static_cast<int64_t>(target.count);
  } catch (...) {
    return -1;
  }
}

}  // extern "C"
