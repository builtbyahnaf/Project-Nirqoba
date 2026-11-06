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

#include <algorithm>
#include <array>
#include <atomic>
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

using namespace std;

namespace {

constexpr size_t kGcPageShift = 16;
constexpr size_t kGcPageBytes = size_t{1} << kGcPageShift;
constexpr size_t kObjectStartAlignment = alignof(std::max_align_t);

struct GcPage {
  unsigned char* storage = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  size_t live_objects = 0;
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
  struct FreeSpan {
    size_t offset = 0;
    size_t span = 0;
  };
  static constexpr size_t kMaxFreeSpans = 96;
  std::array<FreeSpan, kMaxFreeSpans> free_spans{};
  size_t free_span_count = 0;
  size_t free_span_bytes = 0;
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

// Most managed records contain only one or two GC references. Store those
// offsets in the object header itself so traced objects do not need a separate
// heap allocation for layout metadata. Unusually wide records keep an overflow
// vector without changing the exact tracing representation.
class ReferenceLayout {
 public:
  void assign(const size_t* offsets, size_t count) {
    if (count <= inline_offsets_.size()) {
      for (size_t index = 0; index < count; ++index)
        inline_offsets_[index] = offsets[index];
      overflow_offsets_.reset();
      size_ = count;
      return;
    }
    overflow_offsets_ = std::make_unique<std::vector<size_t>>(
        offsets, offsets + count);
    size_ = count;
  }

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  size_t operator[](size_t index) const {
    return size_ <= inline_offsets_.size()
        ? inline_offsets_[index] : (*overflow_offsets_)[index];
  }
  const size_t* begin() const {
    return size_ <= inline_offsets_.size()
        ? inline_offsets_.data() : overflow_offsets_->data();
  }
  const size_t* end() const { return begin() + size_; }
  bool matches(const size_t* offsets, size_t count) const {
    if (count != size_) return false;
    for (size_t index = 0; index < count; ++index)
      if ((*this)[index] != offsets[index]) return false;
    return true;
  }

 private:
  std::array<size_t, 2> inline_offsets_{};
  std::unique_ptr<std::vector<size_t>> overflow_offsets_;
  size_t size_ = 0;
};

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
  size_t size = 0;
  size_t element_size = 0;
  nrq_gc_trace_fn trace = nullptr;
  bool marked = false;
  bool young = true;
  // Set while this object is listed in the remembered set. Deduplication is a
  // flag test instead of a hash lookup, and the entry itself lives in one
  // contiguous vector that minor collections scan in address order of insertion.
  bool remembered = false;
  uint8_t age = 0;
  uint64_t birth_epoch = 0;
  ReferenceLayout reference_offsets;

  // The payload immediately follows the header. It is not stored: the page
  // bump allocator always places a header `sizeof(ObjectHeader)` bytes before
  // its object, so the address is pure arithmetic.
  void* payload() const {
    return const_cast<ObjectHeader*>(this) + 1;
  }
};

// Managed allocation cost includes one header per object. Keep the header at
// six pointer words plus the flags/epoch and reference layout; a regression
// here silently raises the cost of every small allocation.
static_assert(sizeof(ObjectHeader) == 96,
              "ObjectHeader metadata must stay within 96 bytes");
static_assert(alignof(ObjectHeader) == alignof(std::max_align_t),
              "ObjectHeader must keep payloads max_align_t aligned");
// Storage a managed object occupies, header included, rounded up to the payload
// alignment the bump allocator uses.
size_t object_span_bytes(size_t payload_bytes) {
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw = sizeof(ObjectHeader) + payload_bytes;
  return (raw + alignment - 1) & ~(alignment - 1);
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

std::mutex gc_mutex;
// Serializes complete collection cycles while leaving marking slices and the
// sweep open to concurrent mutator execution.
std::mutex collection_cycle_mutex;

enum class CollectionPhase { Idle, Preparing, Marking, Sweeping };
constexpr size_t kRememberedBufferCapacity = 64;
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
  size_t unflushed_bytes = 0;
  // Old-to-young stores buffer raw referents so ordinary stores do not need the
  // collector mutex. The owner drains this buffer at a safepoint/leave; the
  // collector drains every registered mutator after parking them.
  void* remembered[kRememberedBufferCapacity]{};
  size_t remembered_count = 0;
};
std::mutex mutator_mutex;
std::condition_variable mutator_condition;
std::vector<MutatorState*> mutators;
// Read by every safepoint poll without a lock. Writers hold mutator_mutex and
// publish with release ordering; waiters re-check while holding that mutex.
std::atomic<bool> collection_requested{false};
thread_local bool thread_is_collector = false;
thread_local MutatorState thread_mutator;
ObjectHeader* objects = nullptr;
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

constexpr size_t kGcTlabBytes = 4 * 1024;
std::vector<std::unique_ptr<GcPage>> gc_pages;
// Empty pages keep their backing storage across cycles. Reusing them avoids an
// operator new / operator delete pair for every page a churn-heavy workload
// frees at each cycle boundary. The pool is bounded so idle programs do not
// retain an unbounded amount of address space.
std::vector<std::unique_ptr<GcPage>> recycled_pages;
constexpr size_t kMaxRecycledPages = 64;
// Large-object pages can be much bigger than kGcPageBytes. Retaining a pool of
// them would make the page-count bound meaningless, so only ordinary-sized
// pages are recycled and oversized pages return to the process heap.
constexpr size_t kMaxRecycledPageBytes = 2 * kGcPageBytes;
uint64_t recycled_page_reuse_count = 0;
// Counts allocations served from space freed by an earlier sweep rather than
// from a page's untouched tail.
uint64_t recycled_span_reuse_count = 0;
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
  size_t element_size = 0;
  ReferenceLayout reference_offsets;
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
  ReferenceLayout layout;
  bool pointer_root = false;
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
  std::unordered_map<void*, SlotState> slot_states;
};
std::vector<ShadowStack*> shadow_stacks;
thread_local ShadowStack* thread_shadow_stack = nullptr;
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
bool reference_diagnostics_enabled = false;
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
constexpr size_t kMarkBudget = 64;
constexpr size_t kSweepBudget = 64;
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
std::atomic<size_t> old_object_count{0};

uint64_t saturated_add(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

void atomic_saturating_add_size(std::atomic<size_t>& target, size_t amount) {
  size_t current = target.load(std::memory_order_relaxed);
  while (!target.compare_exchange_weak(
      current, amount > SIZE_MAX - current ? SIZE_MAX : current + amount,
      std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

// Fold a mutator's locally batched allocation bytes into the shared trigger
// counter. Callers hold gc_mutex.
void flush_mutator_accounting_locked(MutatorState& mutator) {
  if (mutator.unflushed_bytes == 0) return;
  atomic_saturating_add_size(allocation_accounting.bytes_since_collection,
                             mutator.unflushed_bytes);
  mutator.unflushed_bytes = 0;
}

void publish_tlab_locked(MutatorState& mutator) {
  ObjectHeader* header = mutator.pending_objects;
  while (header) {
    ObjectHeader* next = header->tlab_next;
    if (page_uses_index(header->page)) {
      if (!object_index.insert(header->payload(), header))
        nrq_runtime_fatal("GC failed to publish a thread-local allocation");
    } else {
      const size_t offset = static_cast<size_t>(
          static_cast<unsigned char*>(header->payload()) -
          header->page->storage);
      mark_object_start(*header->page, offset);
    }
    header->tlab_next = nullptr;
    header->next = objects;
    objects = header;
    if (!header->page || header->page->live_objects == SIZE_MAX)
      nrq_runtime_fatal("GC page live-object count overflow");
    ++header->page->live_objects;
    statistics.allocated_bytes = saturated_add(
        statistics.allocated_bytes, static_cast<uint64_t>(header->size));
    statistics.live_objects = saturated_add(statistics.live_objects, 1);
    live_heap_bytes = saturated_add(live_heap_bytes, header->size);
    header = next;
  }
  flush_mutator_accounting_locked(mutator);
  mutator.pending_objects = nullptr;
  mutator.pending_bytes.store(0, std::memory_order_relaxed);
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

void stop_mutators() {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) {
    if (thread_mutator.depth != 0) {
      thread_mutator.parked = true;
      mutator_condition.notify_all();
      wait_until_not_requested(lock);
      thread_mutator.parked = false;
    } else {
      wait_until_not_requested(lock);
    }
  }
  collection_requested.store(true, std::memory_order_release);
  mutator_condition.notify_all();
  mutator_condition.wait(lock, [] {
    for (const MutatorState* mutator : mutators)
      if (mutator != &thread_mutator && !mutator->parked) return false;
    return true;
  });
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
bool begin_fast_barrier() {
  if (thread_mutator.depth == 0) return false;
  thread_mutator.barrier_depth.fetch_add(1, std::memory_order_acq_rel);
  if (collection_phase.load(std::memory_order_acquire) ==
      CollectionPhase::Idle)
    return true;
  thread_mutator.barrier_depth.fetch_sub(1, std::memory_order_release);
  return false;
}

void end_fast_barrier() {
  thread_mutator.barrier_depth.fetch_sub(1, std::memory_order_release);
}

// Wait until every registered mutator has left its barrier regions. The
// collector publishes Preparing first, so a mutator that observes the phase
// change retreats to the serialized path and never enters a new region; the loop
// only has to wait out the regions that were already in flight, and a mutator
// inside one finishes it before it can reach a safepoint.
void wait_for_fast_barriers() {
  for (;;) {
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(mutator_mutex);
      for (const MutatorState* mutator : mutators) {
        if (mutator &&
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
  if (header->trace)
    header->trace(header->payload(), header->size, visit, context);
  if (header->reference_offsets.empty() || header->element_size == 0) return;
  const auto* bytes = static_cast<const unsigned char*>(header->payload());
  for (size_t base = 0; base + header->element_size <= header->size;
       base += header->element_size) {
    for (size_t offset_index = 0;
         offset_index < header->reference_offsets.size(); ++offset_index) {
      const size_t offset = header->reference_offsets[offset_index];
      if (offset > header->element_size ||
          sizeof(void*) > header->element_size - offset)
        continue;
      void* child = nullptr;
      std::memcpy(&child, bytes + base + offset, sizeof(child));
      visit(child, context);
    }
  }
}

void enqueue_header_locked(ObjectHeader* header, MarkContext& context) {
  if (!header || header->marked) return;
  // Objects born during this cycle are retained by the sweep's birth-epoch
  // check. Marking them would leave a mark bit that the snapshot never clears.
  // The epoch comes from the context, so the hot path never loads the shared
  // cycle state.
  if (header->birth_epoch >= context.epoch) return;
  if (context.young_only && !header->young) return;
  header->marked = true;
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
  if (root.repeated_count != 0) {
    auto* bytes = static_cast<unsigned char*>(storage);
    for (size_t element = 0; element < root.repeated_count; ++element) {
      for (size_t offset : root.reference_offsets) {
        void* reference = nullptr;
        std::memcpy(&reference, bytes + element * root.element_size + offset,
                    sizeof(reference));
        mark_value_locked(reference);
      }
    }
    return;
  }
  auto* bytes = static_cast<unsigned char*>(storage);
  for (size_t offset : root.reference_offsets) {
    if (offset > root.object_size ||
        sizeof(void*) > root.object_size - offset)
      continue;
    void* reference = nullptr;
    std::memcpy(&reference, bytes + offset, sizeof(reference));
    mark_value_locked(reference);
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
  auto* bytes = static_cast<const unsigned char*>(root_storage);
  if (root.reference_offsets.empty()) return;
  if (root.repeated_count != 0) {
    for (size_t element = 0; element < root.repeated_count; ++element) {
      const unsigned char* element_bytes = bytes + element * root.element_size;
      for (size_t offset : root.reference_offsets) {
        void* reference = nullptr;
        std::memcpy(&reference, element_bytes + offset, sizeof(reference));
        enqueue_reference(reference, &context);
      }
    }
  } else {
    for (size_t offset : root.reference_offsets) {
      if (offset > root.object_size ||
          sizeof(void*) > root.object_size - offset)
        continue;
      void* reference = nullptr;
      std::memcpy(&reference, bytes + offset, sizeof(reference));
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
  if (!header || header->remembered) return;
  header->remembered = true;
  try {
    remembered_young.push_back(header);
  } catch (...) {
    header->remembered = false;
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
                       return !header || !header->remembered;
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
  if (!found || !found->young) return;
  add_remembered_locked(found);
}

void remember_young_locked(void* object) {
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      !generational_barrier_active.load(std::memory_order_acquire) ||
      !object)
    return;
  ObjectHeader* found = find_object_locked(object);
  if (found && found->young) add_remembered_locked(found);
}

void remember_children_locked(ObjectHeader* header) {
  trace_children(header, remember_reference, nullptr);
}

void defer_remembered_locked(void* object) {
  if (!object || !generational_barrier_active.load(std::memory_order_acquire))
    return;
  ObjectHeader* found = find_object_locked(object);
  if (found && found->young) {
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
    if (found && found->young) {
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
      std::lock_guard<std::mutex> heap_lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
      drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
    }
    auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
    if (found != mutators.end()) mutators.erase(found);
    thread_mutator.depth = 0;
    thread_mutator.parked = false;
    mutator_condition.notify_all();
  }
  if (stack) {
    // Remove the stack under gc_mutex: enqueue_roots_locked enumerates the
    // registry under the same lock, so the collector can no longer reach it.
    std::lock_guard<std::mutex> heap_lock(gc_mutex);
    auto found = std::find(shadow_stacks.begin(), shadow_stacks.end(), stack);
    if (found != shadow_stacks.end()) shadow_stacks.erase(found);
    thread_shadow_stack = nullptr;
  }
  delete stack;
}

void resolve_unresolved_remembered_locked() {
  for (void* object : unresolved_remembered) {
    ObjectHeader* found = find_object_locked(object);
    if (!found || !found->young) continue;
    add_remembered_locked(found);
  }
  unresolved_remembered.clear();
}

void buffer_remembered(void* object);

void buffer_pointer_words(void* storage, size_t bytes) {
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      !generational_barrier_active.load(std::memory_order_acquire))
    return;
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    buffer_remembered(reference);
  }
}

void buffer_remembered(void* object) {
  if (!object ||
      !generational_collection_enabled.load(std::memory_order_relaxed) ||
      !generational_barrier_active.load(std::memory_order_acquire))
    return;
  MutatorState& mutator = thread_mutator;
  if (mutator.remembered_count < kRememberedBufferCapacity) {
    mutator.remembered[mutator.remembered_count++] = object;
    return;
  }
  std::lock_guard<std::mutex> lock(gc_mutex);
  publish_tlab_locked(mutator);
  drain_remembered_locked(mutator, /*defer_unresolved=*/true);
  defer_remembered_locked(object);
}

bool mark_slice_locked(size_t budget) {
  size_t steps = 0;
  while (!active_mark.worklist.empty() && steps < budget) {
    ObjectHeader* header = active_mark.worklist.pop();
    ++steps;
    if (active_mark.young_only && !header->young) continue;
    trace_children(header, enqueue_reference, &active_mark);
    flush_reference_batch_locked(active_mark);
  }
  flush_reference_batch_locked(active_mark);
  return active_mark.worklist.empty() && active_mark.reference_count == 0;
}

bool sweep_chunk_locked(size_t budget) {
  size_t steps = 0;
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
  while (sweep_list && steps < budget) {
    ObjectHeader* header = sweep_list;
    sweep_list = header->next;
    ++steps;

    // Objects allocated after the cycle started are outside the snapshot's
    // reachability analysis. Retain them for this cycle and consider them in
    // the next one; marking them would leave a bit the snapshot never clears.
    if (header->birth_epoch >= active_cycle_epoch) {
      header->marked = false;
      header->next = objects;
      objects = header;
      continue;
    }

    const bool collectable = !active_collection_is_minor || header->young;
    if (!header->marked && collectable) {
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
                          object_span_bytes(header->size));
      header->remembered = false;
      if (!header->young) ++reclaimed_old;
      statistics.collected_bytes = saturated_add(
          statistics.collected_bytes, static_cast<uint64_t>(header->size));
      if (statistics.live_objects != 0) --statistics.live_objects;
      live_heap_bytes = live_heap_bytes > header->size
                            ? live_heap_bytes - header->size
                            : 0;
      if (!header->page || header->page->live_objects == 0)
        nrq_runtime_fatal("GC page live-object count underflow");
      --header->page->live_objects;
      if (header->page->live_objects == 0) header->page->retired = true;
      destroy_object_storage(header);
      continue;
    }

    // Survivors rejoin the allocation list. New objects allocated while the
    // sweep is in progress are already at the head of this list, so this also
    // keeps them out of the detached snapshot.
    header->next = objects;
    objects = header;
    if (header->marked) {
      header->marked = false;
      if (active_collection_is_minor) {
        if (header->young) {
          if (header->age < UINT8_MAX) ++header->age;
          if (header->age >= kPromotionAge) {
            header->young = false;
            header->age = 0;
            ++promoted_young;
            promotion_raised_barrier = true;
            remember_children_locked(header);
            header->remembered = false;
          }
        }
      } else {
        if (header->young) {
          header->young = false;
          ++promoted_young;
          promotion_raised_barrier = true;
        }
        header->age = 0;
        header->remembered = false;
      }
    }
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
  if (promotion_raised_barrier)
    generational_barrier_active.store(true, std::memory_order_release);
  return sweep_list == nullptr;
}

// Record space an object used to occupy so a later allocation can reuse it.
// Callers hold gc_mutex. A page whose span list is full keeps the space
// unusable, which is exactly the behaviour of the bump allocator.
void release_span_locked(GcPage& page, size_t offset, size_t span) {
  if (span == 0) return;
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
      previous.span += span;
      page.free_span_bytes += span;
      if (index < page.free_span_count &&
          previous.offset + previous.span == page.free_spans[index].offset) {
        previous.span += page.free_spans[index].span;
        page.free_spans[index] = page.free_spans[--page.free_span_count];
      }
      return;
    }
  }
  if (index < page.free_span_count &&
      offset + span == page.free_spans[index].offset) {
    page.free_spans[index].offset = offset;
    page.free_spans[index].span += span;
    page.free_span_bytes += span;
    return;
  }
  if (page.free_span_count >= GcPage::kMaxFreeSpans) return;
  for (size_t move = page.free_span_count; move > index; --move)
    page.free_spans[move] = page.free_spans[move - 1];
  page.free_spans[index] = GcPage::FreeSpan{offset, span};
  ++page.free_span_count;
  page.free_span_bytes += span;
}

// First-fit reuse of reclaimed space. The scan visits pages in allocation order
// and only pages that currently hold a free run, so a heap with nothing to reuse
// costs one branch per page. Callers hold gc_mutex.
GcPage* take_free_span_locked(size_t span, size_t& offset) {
  if (span == 0) return nullptr;
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page || page->retired || page->free_span_count == 0 ||
        page->free_span_bytes < span)
      continue;
    for (size_t index = 0; index < page->free_span_count; ++index) {
      GcPage::FreeSpan& candidate = page->free_spans[index];
      if (candidate.span < span) continue;
      offset = candidate.offset;
      page->free_span_bytes -= span;
      if (candidate.span > span) {
        // The remainder stays a run in the same page. `span` is a multiple of
        // the payload alignment, so the new start keeps that alignment.
        candidate.offset += span;
        candidate.span -= span;
      } else {
        // Runs are ordered by offset, so removal shifts the tail down instead of
        // swapping the last run into the hole.
        for (size_t move = index; move + 1 < page->free_span_count; ++move)
          page->free_spans[move] = page->free_spans[move + 1];
        --page->free_span_count;
      }
      recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
      return page;
    }
  }
  return nullptr;
}

// Take a page of at least `capacity` bytes from the recycle pool, or create a
// fresh one. The caller inherits storage ownership and must either publish the
// page into gc_pages or release the storage explicitly.
std::unique_ptr<GcPage> acquire_page(size_t capacity) {
  for (auto entry = recycled_pages.begin(); entry != recycled_pages.end();
       ++entry) {
    if ((*entry)->capacity < capacity) continue;
    std::unique_ptr<GcPage> page = std::move(*entry);
    recycled_pages.erase(entry);
    page->used = 0;
    page->live_objects = 0;
    page->retired = false;
    page->free_span_count = 0;
    page->free_span_bytes = 0;
    if (page->object_starts && page->object_start_words != 0) {
      std::memset(page->object_starts.get(), 0,
                  page->object_start_words * sizeof(uint64_t));
    }
    recycled_page_reuse_count = saturated_add(recycled_page_reuse_count, 1);
    return page;
  }
  void* storage =
      ::operator new(capacity, std::align_val_t{kGcPageBytes}, std::nothrow);
  if (!storage) return nullptr;
  try {
    auto page = std::make_unique<GcPage>();
    page->storage = static_cast<unsigned char*>(storage);
    page->capacity = capacity;
    if (capacity <= kGcPageBytes) {
      const size_t slots = capacity / kObjectStartAlignment;
      const size_t words = (slots + 63) / 64;
      page->object_starts.reset(new (std::nothrow) uint64_t[words]());
      if (page->object_starts) page->object_start_words = words;
    }
    return page;
  } catch (...) {
    ::operator delete(storage, std::align_val_t{kGcPageBytes});
    return nullptr;
  }
}

// Return an empty page to the bounded recycle pool, or release its storage when
// the pool is full. Must be called with gc_mutex held at a cycle boundary, when
// no mutator can still hold a cursor into the page.
void recycle_page(std::unique_ptr<GcPage> page) {
  if (!page) return;
  if (recycled_pages.size() >= kMaxRecycledPages ||
      page->capacity < kGcPageBytes ||
      page->capacity > kMaxRecycledPageBytes) {
    ::operator delete(page->storage, std::align_val_t{kGcPageBytes});
    return;
  }
  page->used = 0;
  page->live_objects = 0;
  page->retired = false;
  page->free_span_count = 0;
  page->free_span_bytes = 0;
  GcPage* raw = page.get();
  try {
    recycled_pages.push_back(std::move(page));
  } catch (...) {
    ::operator delete(raw->storage, std::align_val_t{kGcPageBytes});
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

void prepare_cycle_locked(bool young_only) {
  for (MutatorState* mutator : mutators)
    if (mutator) publish_tlab_locked(*mutator);
  // A fast old-to-young store may have filled its buffer without taking the
  // collector lock. Publish every mutator's TLAB first, then drain, so buffered
  // references to objects that were still thread-local become visible. Any
  // reference deferred by a thread exit is resolved after all TLABs are live.
  for (MutatorState* mutator : mutators)
    if (mutator) drain_remembered_locked(*mutator, /*defer_unresolved=*/false);
  resolve_unresolved_remembered_locked();

  // No mutator can be inside an allocator while stop_mutators() has everyone
  // parked. Invalidate stale TLAB cursors before reclaiming empty pages.
  ++gc_page_generation;
  for (auto page = gc_pages.begin(); page != gc_pages.end();) {
    if ((*page)->live_objects == 0) {
      page_directory.erase(page->get());
      auto empty_page = std::move(*page);
      page = gc_pages.erase(page);
      recycle_page(std::move(empty_page));
    } else {
      (*page)->retired = false;
      ++page;
    }
  }

  sweep_list = nullptr;
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
  // Publish the transition before parking so new fast-path stores fall back to
  // the synchronized barrier. In-flight fast stores finish before the root
  // snapshot; the per-mutator barrier depth is the handshake that makes that
  // visible.
  {
    std::lock_guard<std::mutex> lock(gc_mutex);
    // A concurrent promotion can turn a reachable object old while mutators
    // run, so enable remembered-set recording for the whole cycle. The flag is
    // lowered again if the cycle finishes with no old objects.
    generational_barrier_active.store(true, std::memory_order_release);
    collection_phase.store(CollectionPhase::Preparing,
                           std::memory_order_release);
  }
  stop_mutators();
  wait_for_fast_barriers();
  {
    std::lock_guard<std::mutex> lock(gc_mutex);
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
  if (!young_only) live_heap_bytes_at_last_major = live_heap_bytes;
  plan_next_cycle_locked();
  // Do not clear the remembered set here. A major collection erases promoted
  // survivors as it sweeps, while young objects allocated during the cycle
  // remain young and may be referenced only by old objects; their remembered
  // entries must survive into the next minor collection.
  if (generational_collection_enabled.load(std::memory_order_relaxed) &&
      old_object_count.load(std::memory_order_relaxed) == 0)
    generational_barrier_active.store(false, std::memory_order_release);
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
    std::lock_guard<std::mutex> lock(mutator_mutex);
    thread_mutator.parked = true;
    mutator_condition.notify_all();
  }
  cycle_lock.lock();
  {
    std::lock_guard<std::mutex> lock(mutator_mutex);
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
      std::lock_guard<std::mutex> lock(gc_mutex);
      if (!sweeping) {
        const bool marked = mark_slice_locked(kMarkBudget);
        made_progress = true;
        if (marked) {
          // Freeze the snapshot list before releasing the lock for sweep
          // slices. Allocations after this point go to the ordinary list and
          // are outside the snapshot, so the sweeper never sees a moving list.
          sweep_list = objects;
          objects = nullptr;
          collection_phase.store(CollectionPhase::Sweeping,
                                 std::memory_order_release);
          sweeping = true;
        }
      }
      if (sweeping) {
        // Drain barriers that ran between sweep slices before freeing the next
        // chunk. A pending marked parent may still enqueue its children.
        if (!mark_slice_locked(kMarkBudget)) {
          made_progress = true;
        } else {
          made_progress = true;
          if (sweep_chunk_locked(kSweepBudget)) {
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

void* allocate_locked(size_t bytes, size_t element_size, nrq_gc_trace_fn trace,
                      const size_t* reference_offsets, size_t reference_count) {
  if (bytes == 0 || bytes > SIZE_MAX - sizeof(ObjectHeader) || element_size == 0)
    return nullptr;
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
  } else if (thread_allocation_page && !thread_allocation_page->retired &&
      thread_allocation_page->used <= thread_allocation_page->capacity &&
      span <= thread_allocation_page->capacity - thread_allocation_page->used) {
    page = thread_allocation_page;
  } else {
    thread_allocation_page = nullptr;
    for (auto candidate = gc_pages.rbegin(); candidate != gc_pages.rend(); ++candidate) {
      if (!(*candidate)->retired &&
          (*candidate)->used <= (*candidate)->capacity &&
          span <= (*candidate)->capacity - (*candidate)->used) {
        page = candidate->get();
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
      if (new_page)
        ::operator delete(new_page->storage,
                          std::align_val_t{kGcPageBytes});
      return nullptr;
    }
    // A page that cannot be indexed by address is still usable; its objects
    // are tracked through the exact object index instead.
    if (!page_directory.insert(page)) page->use_object_index = true;
  }
  thread_allocation_page = page;
  thread_allocation_generation = gc_page_generation;
  if (!reused_span) {
    header_offset = page->used;
    page->used += span;
  }
  void* header_storage = page->storage + header_offset;
  auto* header = new (header_storage) ObjectHeader();
  auto rollback_reservation = [&]() {
    destroy_object_storage(header);
    if (reused_span) {
      release_span_locked(*page, header_offset, span);
    } else {
      page->used -= span;
    }
  };
  header->page = page;
  void* object = static_cast<unsigned char*>(header_storage) + sizeof(ObjectHeader);
  std::memset(object, 0, bytes);
  try {
    if (reference_count != 0) {
      if (!reference_offsets || reference_count > SIZE_MAX / sizeof(size_t)) {
        rollback_reservation();
        return nullptr;
      }
      header->reference_offsets.assign(reference_offsets, reference_count);
      for (size_t index = 0; index < header->reference_offsets.size(); ++index) {
        const size_t offset = header->reference_offsets[index];
        if (offset > element_size || sizeof(void*) > element_size - offset) {
          rollback_reservation();
          return nullptr;
        }
      }
    }
    header->size = bytes;
    header->element_size = element_size;
    header->trace = trace;
    header->young = generational_collection_enabled.load(std::memory_order_relaxed);
    if (!header->young) {
      old_object_count.fetch_add(1, std::memory_order_relaxed);
      generational_barrier_active.store(true, std::memory_order_release);
    }
    header->age = 0;
    header->birth_epoch = next_cycle_epoch.load(std::memory_order_relaxed);
    header->next = objects;
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
  objects = header;
  ++page->live_objects;
  statistics.allocated_bytes = saturated_add(
      statistics.allocated_bytes, static_cast<uint64_t>(bytes));
  statistics.live_objects = saturated_add(statistics.live_objects, 1);
  live_heap_bytes = saturated_add(live_heap_bytes, bytes);
  atomic_saturating_add_size(allocation_accounting.bytes_since_collection, bytes);
  return object;
}

bool reserve_tlab_locked(MutatorState& mutator, size_t minimum_span) {
  if (mutator.tlab_generation != gc_page_generation) {
    mutator.tlab_page = nullptr;
    mutator.tlab_cursor = nullptr;
    mutator.tlab_end = nullptr;
    mutator.tlab_generation = gc_page_generation;
  }
  // A merged run of reclaimed space can host a whole thread-local reservation.
  // Taking it here is what turns a fragmented page back into a usable bump
  // region instead of leaving it permanently half empty.
  {
    size_t run_offset = 0;
    if (GcPage* reused = take_free_span_locked(kGcTlabBytes, run_offset)) {
      mutator.tlab_page = reused;
      mutator.tlab_cursor = reused->storage + run_offset;
      mutator.tlab_end = mutator.tlab_cursor + kGcTlabBytes;
      mutator.tlab_generation = gc_page_generation;
      thread_allocation_page = reused;
      thread_allocation_generation = gc_page_generation;
      return true;
    }
  }
  GcPage* page = nullptr;
  size_t available = 0;
  if (thread_allocation_generation != gc_page_generation) {
    thread_allocation_page = nullptr;
    thread_allocation_generation = gc_page_generation;
  }
  if (thread_allocation_page && !thread_allocation_page->retired &&
      thread_allocation_page->used <= thread_allocation_page->capacity) {
    available = thread_allocation_page->capacity - thread_allocation_page->used;
    if (available >= minimum_span) page = thread_allocation_page;
  }
  if (!page) {
    for (auto candidate = gc_pages.rbegin(); candidate != gc_pages.rend(); ++candidate) {
      GcPage* possible = candidate->get();
      if (possible->retired || possible->used > possible->capacity) continue;
      const size_t remaining = possible->capacity - possible->used;
      if (remaining >= minimum_span) {
        page = possible;
        available = remaining;
        break;
      }
    }
  }
  if (!page) {
    const size_t capacity = std::max(kGcPageBytes, kGcTlabBytes);
    auto new_page = acquire_page(capacity);
    if (!new_page) return false;
    page = new_page.get();
    try {
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      if (new_page)
        ::operator delete(new_page->storage,
                          std::align_val_t{kGcPageBytes});
      return false;
    }
    // A page that cannot be indexed by address is still usable; its objects
    // are tracked through the exact object index instead.
    if (!page_directory.insert(page)) page->use_object_index = true;
    available = page->capacity;
  }
  const size_t reserve = std::min(kGcTlabBytes, available);
  if (reserve < minimum_span) return false;
  mutator.tlab_page = page;
  mutator.tlab_cursor = page->storage + page->used;
  page->used += reserve;
  mutator.tlab_end = mutator.tlab_cursor + reserve;
  mutator.tlab_generation = gc_page_generation;
  thread_allocation_page = page;
  thread_allocation_generation = gc_page_generation;
  return true;
}

void account_tlab_allocation(MutatorState& mutator, size_t bytes) {
  const size_t previous = mutator.pending_bytes.fetch_add(
      bytes, std::memory_order_relaxed);
  if (previous > SIZE_MAX - bytes ||
      mutator.pending_count.fetch_add(1, std::memory_order_relaxed) == SIZE_MAX)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  if (bytes > SIZE_MAX - mutator.unflushed_bytes)
    nrq_runtime_fatal("GC thread-local batch accounting overflow");
  mutator.unflushed_bytes += bytes;
  const size_t threshold =
      allocation_accounting.collection_threshold.load(std::memory_order_relaxed);
  const size_t batch = std::min(kAccountingBatchBytes, threshold == 0 ? 1 : threshold);
  if (mutator.unflushed_bytes < batch) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  flush_mutator_accounting_locked(mutator);
}

void* allocate_tlab(size_t bytes, size_t element_size, nrq_gc_trace_fn trace,
                    const size_t* reference_offsets, size_t reference_count,
                    bool& handled) {
  handled = false;
  if (thread_mutator.depth == 0 || bytes == 0 || element_size == 0 ||
      bytes > SIZE_MAX - sizeof(ObjectHeader))
    return nullptr;
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw_span = sizeof(ObjectHeader) + bytes;
  if (raw_span > SIZE_MAX - (alignment - 1)) return nullptr;
  const size_t span = (raw_span + alignment - 1) & ~(alignment - 1);
  if (span > kGcTlabBytes) return nullptr;
  handled = true;

  if (thread_mutator.tlab_generation != gc_page_generation) {
    thread_mutator.tlab_page = nullptr;
    thread_mutator.tlab_cursor = nullptr;
    thread_mutator.tlab_end = nullptr;
    thread_mutator.tlab_generation = gc_page_generation;
  }
  if (!thread_mutator.tlab_cursor || !thread_mutator.tlab_end ||
      thread_mutator.tlab_cursor > thread_mutator.tlab_end ||
      span > static_cast<size_t>(thread_mutator.tlab_end -
                                 thread_mutator.tlab_cursor)) {
    std::lock_guard<std::mutex> lock(gc_mutex);
    if (!reserve_tlab_locked(thread_mutator, span)) return nullptr;
  }

  unsigned char* header_storage = thread_mutator.tlab_cursor;
  thread_mutator.tlab_cursor += span;
  auto* header = new (header_storage) ObjectHeader();
  header->page = thread_mutator.tlab_page;
  void* object = header_storage + sizeof(ObjectHeader);
  std::memset(object, 0, bytes);
  try {
    if (reference_count != 0) {
      if (!reference_offsets || reference_count > SIZE_MAX / sizeof(size_t)) {
        header->~ObjectHeader();
        return nullptr;
      }
      header->reference_offsets.assign(reference_offsets, reference_count);
      for (size_t index = 0; index < header->reference_offsets.size(); ++index) {
        const size_t offset = header->reference_offsets[index];
        if (offset > element_size || sizeof(void*) > element_size - offset) {
          header->~ObjectHeader();
          return nullptr;
        }
      }
    }
  } catch (...) {
    header->~ObjectHeader();
    return nullptr;
  }
  header->size = bytes;
  header->element_size = element_size;
  header->trace = trace;
  header->young = generational_collection_enabled.load(std::memory_order_relaxed);
  if (!header->young) {
    old_object_count.fetch_add(1, std::memory_order_relaxed);
    generational_barrier_active.store(true, std::memory_order_release);
  }
  header->age = 0;
  header->birth_epoch = next_cycle_epoch.load(std::memory_order_relaxed);
  header->tlab_next = thread_mutator.pending_objects;
  thread_mutator.pending_objects = header;
  account_tlab_allocation(thread_mutator, bytes);

  // During a cycle an unpublished young object is invisible to the marker and
  // to the root barrier. Publish it immediately so a barrier can identify it.
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle) {
    std::lock_guard<std::mutex> lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
  }
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
    std::lock_guard<std::mutex> lock(gc_mutex);
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
         root.reference_offsets.size() == 1 && root.reference_offsets[0] == 0;
}

void add_slot_state(ShadowStack& stack, void* slot, const RootInfo& info,
                    bool pointer_root) {
  auto found = stack.slot_states.find(slot);
  if (found == stack.slot_states.end()) {
    SlotState state;
    state.count = 1;
    state.object_size = info.object_size;
    state.layout.assign(info.reference_offsets.begin(),
                        info.reference_offsets.size());
    state.pointer_root = pointer_root;
    stack.slot_states.emplace(slot, std::move(state));
    return;
  }
  if (found->second.object_size != info.object_size ||
      !found->second.layout.matches(info.reference_offsets.begin(),
                                    info.reference_offsets.size()) ||
      found->second.pointer_root != pointer_root)
    nrq_runtime_fatal(
        "GC root slot was registered with an incompatible layout");
  ++found->second.count;
}

void remove_slot_state(ShadowStack& stack, void* slot) {
  auto found = stack.slot_states.find(slot);
  if (found == stack.slot_states.end())
    nrq_runtime_fatal("GC root slot registration bookkeeping is inconsistent");
  if (--found->second.count == 0) stack.slot_states.erase(found);
}

bool shadow_has_pointer_root(ShadowStack& stack, void* slot) {
  // O(1) lookup in the per-thread slot index. Never read by the collector.
  auto found = stack.slot_states.find(slot);
  return found != stack.slot_states.end() && found->second.pointer_root;
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
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, bytes, trace, nullptr, 0, handled);
  if (handled) return object;
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(bytes, bytes, trace, nullptr, 0);
}

void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(count * element_size, element_size, trace,
                               nullptr, 0, handled);
  if (handled) return object;
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(count * element_size, element_size, trace, nullptr, 0);
}

void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(count * element_size, element_size, nullptr,
                               reference_offsets, reference_count, handled);
  if (handled) return object;
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(count * element_size, element_size, nullptr,
                         reference_offsets, reference_count);
}

void* nrq_gc_allocate_array_typed_rooted(
    void** root_slot, size_t count, size_t element_size,
    const size_t* reference_offsets, size_t reference_count) {
  if (!root_slot || count == 0 || element_size == 0 ||
      count > SIZE_MAX / element_size)
    return nullptr;
  maybe_collect_automatically();
  const size_t bytes = count * element_size;
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
      void* object = allocate_tlab(bytes, element_size, nullptr,
                                   reference_offsets, reference_count, handled);
      if (handled) {
        *root_slot = object;
        end_fast_barrier();
        return object;
      }
      end_fast_barrier();
    }
  }

  std::lock_guard<std::mutex> lock(gc_mutex);
  if (thread_shadow_stack &&
      !shadow_has_pointer_root(*thread_shadow_stack, root_slot))
    nrq_runtime_fatal(
        "rooted GC allocation requires a registered pointer root slot");
  void* object = allocate_locked(bytes, element_size, nullptr,
                                 reference_offsets, reference_count);
  store_root_value_locked(root_slot, object);
  return object;
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
  try {
    info.reference_offsets.assign(reference_offsets, reference_count);
  } catch (...) {
    nrq_runtime_fatal("GC root registration failed to allocate root metadata");
  }

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

  // Fast path: no collector mutex. The reader count keeps a snapshot from
  // starting between the root being published and the phase check.
  if (begin_fast_barrier()) {
    {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      push_root();
    }
    end_fast_barrier();
    return;
  }

  std::lock_guard<std::mutex> heap_lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = roots.find(data);
  if (found != roots.end()) {
    nrq_runtime_fatal("GC array-root storage address is already registered");
  }
  try {
    RootInfo root;
    root.references = 1;
    root.object_size = count * element_size;
    root.repeated_count = count;
    root.element_size = element_size;
    root.reference_offsets.assign(reference_offsets, reference_count);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  // Language-generated stores execute inside a registered mutator. The fast
  // path is just a pointer store plus a reader-count handshake; the collector
  // waits for in-flight readers before snapshotting roots.
  if (thread_mutator.depth != 0 && begin_fast_barrier()) {
    *slot = value;
    buffer_remembered(value);
    end_fast_barrier();
    return;
  }
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
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

uint64_t nrq_gc_root_frame_begin(void) {
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

  // Remove exactly the registrations owned by this frame. Complexities of this
  // path are proportional to the number of roots the frame leaves behind, not
  // to all roots still active on the thread.
  auto remove_owned_roots = [&]() {
    for (size_t index = target_depth; index < stack.roots.size(); ++index)
      remove_slot_state(stack, stack.roots[index].slot);
    stack.roots.resize(target_depth);
  };

  // Shrinking stack.roots requires coordination with the collector, which reads
  // roots under stack.mutex. Use the fast barrier when idle so the collector
  // cannot begin a snapshot between the resize and this function returning.
  if (begin_fast_barrier()) {
    std::lock_guard<std::mutex> lock(stack.mutex);
    remove_owned_roots();
    end_fast_barrier();
    return;
  }
  // Collector is active; hold stack.mutex while shrinking roots so the
  // collector sees a consistent view during its enumeration.
  std::lock_guard<std::mutex> lock(stack.mutex);
  remove_owned_roots();
}

void nrq_gc_enable(void) {
  automatic_collection_enabled.store(true, std::memory_order_relaxed);
}

void nrq_gc_disable(void) {
  automatic_collection_enabled.store(false, std::memory_order_relaxed);
}

void nrq_gc_collect(void) { run_collection(/*young_only=*/false); }

void nrq_gc_collect_minor(void) {
  // With generations disabled every object is old, so a young-only cycle could
  // not reclaim anything. Treat the explicit request as a full collection.
  run_collection(
      generational_collection_enabled.load(std::memory_order_relaxed));
}

void nrq_gc_set_generational(int32_t enabled) {
  generational_collection_enabled.store(enabled != 0,
                                        std::memory_order_relaxed);
}

int32_t nrq_gc_is_generational(void) {
  return generational_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

uint64_t nrq_gc_get_minor_collection_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return minor_collections;
}

uint64_t nrq_gc_get_major_collection_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
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
    std::lock_guard<std::mutex> heap_lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
    drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
  }
  thread_mutator.parked = false;
  auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
  if (found == mutators.end())
    nrq_runtime_fatal("GC mutator registry lost an active thread");
  mutators.erase(found);
  mutator_condition.notify_all();
}

void nrq_gc_safepoint(void) {
  // The common path is a single acquire load. Only threads that observe a
  // request pay for the mutex and condition-variable protocol.
  if (!collection_is_requested()) return;
  if (thread_is_collector || thread_mutator.depth == 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  if (!collection_is_requested()) return;
  thread_mutator.parked = true;
  mutator_condition.notify_all();
  wait_until_not_requested(lock);
  thread_mutator.parked = false;
}

void nrq_gc_set_threshold(size_t bytes) {
  // An explicit threshold is a policy decision by the program, so automatic
  // adaptation stops here until it is turned back on.
  std::lock_guard<std::mutex> lock(gc_mutex);
  collection_policy.adaptive = false;
  collection_policy.adaptations = 0;
  allocation_accounting.collection_threshold.store(bytes == 0 ? 1 : bytes,
                                                   std::memory_order_relaxed);
}

size_t nrq_gc_get_threshold(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocation_accounting.collection_threshold.load(
      std::memory_order_relaxed);
}

void nrq_gc_set_adaptive(int32_t enabled) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  collection_policy.adaptive = enabled != 0;
  // Re-enabling starts from the current live set instead of waiting for the next
  // cycle to correct a threshold the program may have pinned long ago.
  if (collection_policy.adaptive) adapt_collection_policy_locked();
}

int32_t nrq_gc_policy_is_adaptive(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return collection_policy.adaptive ? 1 : 0;
}

uint64_t nrq_gc_policy_adaptation_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return collection_policy.adaptations;
}

uint64_t nrq_gc_last_reclaimed_bytes(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return collection_policy.last_reclaimed_bytes;
}

size_t nrq_gc_object_header_bytes(void) { return sizeof(ObjectHeader); }

size_t nrq_gc_page_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return gc_pages.size();
}

size_t nrq_gc_recycled_page_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return recycled_pages.size();
}

uint64_t nrq_gc_page_reuse_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return recycled_page_reuse_count;
}

uint64_t nrq_gc_span_reuse_count(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return recycled_span_reuse_count;
}

size_t nrq_gc_object_index_entries(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return object_index.size();
}

int32_t nrq_gc_pointer_is_managed(void* object) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return find_object_locked(object) ? 1 : 0;
}

nrq_gc_stats nrq_gc_get_stats(void) {
  // The registered-mutator vector is mutated under mutator_mutex. Take it
  // before gc_mutex to match the enter/leave lock order; taking only gc_mutex
  // would race a concurrent registration.
  std::lock_guard<std::mutex> registry_lock(mutator_mutex);
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
  return static_cast<uint64_t>(pinned_objects.size());
}

uint64_t nrq_gc_pin_violations(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return pin_violations;
}

int32_t nrq_gc_is_enabled(void) {
  return automatic_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

void nrq_gc_unpin(void* object) {
  if (!object) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = pinned_objects.find(object);
  if (found == pinned_objects.end()) return;
  if (--found->second == 0) pinned_objects.erase(found);
}

void* nrq_gc_pin_transfer(void* object) {
  if (!object) return nullptr;
  std::lock_guard<std::mutex> lock(gc_mutex);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
  reference_diagnostics_enabled = true;
}

void nrq_gc_ref_count_stop(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  reference_diagnostics_enabled = false;
}

int64_t nrq_gc_get_ref_count(void* object) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  // Diagnostics are off in normal execution. Return before publishing a TLAB
  // batch or scanning the heap, so a disabled query stays a single lock.
  if (!reference_diagnostics_enabled) return -1;
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!object || !find_object_locked(object)) return 0;
  try {
    ReferenceCountTarget target{object, 0};
    for (ObjectHeader* header = objects; header; header = header->next) {
      if (header->trace)
        header->trace(header->payload(), header->size, count_reference_to_target,
                      &target);
      if (!header->reference_offsets.empty() && header->element_size != 0) {
        const auto* bytes = static_cast<const unsigned char*>(header->payload());
        for (size_t base = 0; base + header->element_size <= header->size;
             base += header->element_size) {
          for (size_t offset_index = 0;
               offset_index < header->reference_offsets.size(); ++offset_index) {
            const size_t offset = header->reference_offsets[offset_index];
            void* reference = nullptr;
            std::memcpy(&reference, bytes + base + offset, sizeof(reference));
            count_reference_to_target(reference, &target);
          }
        }
      }
    }
    for (const auto& root : roots) {
      auto* bytes = static_cast<const unsigned char*>(root.first);
      if (root.second.repeated_count != 0) {
        for (size_t element = 0; element < root.second.repeated_count; ++element) {
          const unsigned char* element_bytes =
              bytes + element * root.second.element_size;
          for (size_t offset : root.second.reference_offsets) {
            void* reference = nullptr;
            std::memcpy(&reference, element_bytes + offset, sizeof(reference));
            count_reference_to_target(reference, &target);
          }
        }
      } else {
        for (size_t offset : root.second.reference_offsets) {
          if (offset > root.second.object_size ||
              sizeof(void*) > root.second.object_size - offset) continue;
          void* reference = nullptr;
          std::memcpy(&reference, bytes + offset, sizeof(reference));
          count_reference_to_target(reference, &target);
        }
      }
    }
    for (ShadowStack* stack : shadow_stacks) {
      if (!stack) continue;
      std::lock_guard<std::mutex> stack_lock(stack->mutex);
      for (const ShadowRoot& root : stack->roots) {
        auto* bytes = static_cast<const unsigned char*>(root.slot);
        if (root.info.repeated_count != 0) {
          for (size_t element = 0; element < root.info.repeated_count;
               ++element) {
            const unsigned char* element_bytes =
                bytes + element * root.info.element_size;
            for (size_t offset : root.info.reference_offsets) {
              void* reference = nullptr;
              std::memcpy(&reference, element_bytes + offset, sizeof(reference));
              count_reference_to_target(reference, &target);
            }
          }
        } else {
          for (size_t offset : root.info.reference_offsets) {
            if (offset > root.info.object_size ||
                sizeof(void*) > root.info.object_size - offset)
              continue;
            void* reference = nullptr;
            std::memcpy(&reference, bytes + offset, sizeof(reference));
            count_reference_to_target(reference, &target);
          }
        }
      }
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
