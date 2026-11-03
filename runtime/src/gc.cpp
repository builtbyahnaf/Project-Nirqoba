// Precise non-moving mark-and-sweep collector for explicitly described objects.
// The runtime owns a linked allocation registry, while roots are addresses of
// pointer slots registered by generated code or native callers. Marking follows
// only trace callbacks, so integer data that happens to resemble an address is
// never treated as a reference.

#include "nirqoba/runtime/gc.h"
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

struct GcPage {
  unsigned char* storage = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  size_t live_objects = 0;
  // Pages emptied by a concurrent sweep are retained until the next
  // stop-the-world cycle boundary, when no mutator can still be inside an
  // allocator or holding a TLAB cursor into the page.
  bool retired = false;
};

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

 private:
  std::array<size_t, 2> inline_offsets_{};
  std::unique_ptr<std::vector<size_t>> overflow_offsets_;
  size_t size_ = 0;
};

struct alignas(std::max_align_t) ObjectHeader {
  ObjectHeader* next = nullptr;
  ObjectHeader* mark_next = nullptr;
  ObjectHeader* tlab_next = nullptr;
  GcPage* page = nullptr;
  void* object = nullptr;
  size_t size = 0;
  size_t element_size = 0;
  nrq_gc_trace_fn trace = nullptr;
  bool marked = false;
  bool young = true;
  uint8_t age = 0;
  uint64_t birth_epoch = 0;
  ReferenceLayout reference_offsets;
};

// The collector needs exact-address lookup while marking, but only allocation
// mutates this table. Open addressing keeps pointer metadata contiguous and
// avoids one node allocation per managed object.
class ObjectIndex {
 public:
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

  void erase(void* object) {
    if (!object || entries_.empty()) return;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.object == nullptr) return;
      if (entry.object == object) {
        entry = Entry{tombstone_key(), nullptr};
        --size_;
        ++tombstones_;
        return;
      }
      index = (index + 1) & mask;
    }
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

enum class CollectionPhase { Idle, Marking, Sweeping };
struct MutatorState {
  size_t depth = 0;
  bool parked = false;
  GcPage* tlab_page = nullptr;
  unsigned char* tlab_cursor = nullptr;
  unsigned char* tlab_end = nullptr;
  uint64_t tlab_generation = 0;
  ObjectHeader* pending_objects = nullptr;
  size_t pending_bytes = 0;
  size_t pending_count = 0;
};
std::mutex mutator_mutex;
std::condition_variable mutator_condition;
std::vector<MutatorState*> mutators;
bool collection_requested = false;
std::thread::id collector_thread;
thread_local MutatorState thread_mutator;
ObjectHeader* objects = nullptr;
ObjectIndex object_index;
std::vector<std::unique_ptr<GcPage>> gc_pages;
constexpr size_t kGcPageBytes = 64 * 1024;
constexpr size_t kGcTlabBytes = 4 * 1024;
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
  std::vector<size_t> reference_offsets;
};
std::unordered_map<void*, RootInfo> roots;
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
thread_local std::vector<void*> thread_roots;
struct RootFrame {
  uint64_t marker;
  size_t root_depth;
};
thread_local std::vector<RootFrame> thread_root_frames;
std::atomic<uint64_t> next_root_frame_marker{1};
// Collection is an explicit opt-in policy. Programs using the GC domain may
// call gc.enable() to request automatic triggers; explicit gc.collect() always
// works regardless of this state.
std::atomic<bool> automatic_collection_enabled{false};
std::atomic<bool> generational_collection_enabled{true};
std::atomic<size_t> collection_threshold{1024 * 1024};
std::atomic<size_t> bytes_since_collection{0};
std::atomic<uint64_t> pending_allocation_bytes{0};
std::atomic<uint64_t> pending_allocation_count{0};
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
constexpr size_t kMajorCollectionInterval = 8;
std::atomic<CollectionPhase> collection_phase{CollectionPhase::Idle};
bool active_collection_is_minor = false;
uint64_t active_cycle_epoch = 0;
uint64_t next_cycle_epoch = 1;
size_t minor_collections_since_major = 0;
struct MarkContext {
  ObjectHeader* pending = nullptr;
  bool young_only = false;
};
MarkContext active_mark;
// During the sweep phase the snapshot list is detached from the allocation
// list. New allocations are therefore never inserted into a list the sweeper
// is traversing, and GC barriers can publish new marks between sweep slices.
ObjectHeader* sweep_list = nullptr;
// Old objects are not traced during a minor collection. This set records young
// objects referenced by old objects; the root-store/root-copy barriers insert
// here, and a minor collection treats the set as additional roots.
std::unordered_set<ObjectHeader*> remembered_young;

uint64_t saturated_add(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

void atomic_saturating_add_size(std::atomic<size_t>& target, size_t amount) {
  size_t current = target.load(std::memory_order_relaxed);
  while (!target.compare_exchange_weak(
      current, amount > SIZE_MAX - current ? SIZE_MAX : current + amount,
      std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

void atomic_saturating_add(std::atomic<uint64_t>& target, uint64_t amount) {
  uint64_t current = target.load(std::memory_order_relaxed);
  while (!target.compare_exchange_weak(
      current, saturated_add(current, amount), std::memory_order_relaxed,
      std::memory_order_relaxed)) {}
}

void publish_tlab_locked(MutatorState& mutator) {
  ObjectHeader* header = mutator.pending_objects;
  while (header) {
    ObjectHeader* next = header->tlab_next;
    if (!object_index.insert(header->object, header))
      nrq_runtime_fatal("GC failed to publish a thread-local allocation");
    header->tlab_next = nullptr;
    header->next = objects;
    objects = header;
    if (!header->page || header->page->live_objects == SIZE_MAX)
      nrq_runtime_fatal("GC page live-object count overflow");
    ++header->page->live_objects;
    statistics.allocated_bytes = saturated_add(
        statistics.allocated_bytes, static_cast<uint64_t>(header->size));
    statistics.live_objects = saturated_add(statistics.live_objects, 1);
    header = next;
  }
  if (mutator.pending_bytes != 0)
    pending_allocation_bytes.fetch_sub(mutator.pending_bytes,
                                       std::memory_order_relaxed);
  if (mutator.pending_count != 0)
    pending_allocation_count.fetch_sub(mutator.pending_count,
                                       std::memory_order_relaxed);
  mutator.pending_objects = nullptr;
  mutator.pending_bytes = 0;
  mutator.pending_count = 0;
}

void stop_mutators() {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_requested) {
    if (thread_mutator.depth != 0) {
      thread_mutator.parked = true;
      mutator_condition.notify_all();
      mutator_condition.wait(lock, [] { return !collection_requested; });
      thread_mutator.parked = false;
    } else {
      mutator_condition.wait(lock, [] { return !collection_requested; });
    }
  }
  collection_requested = true;
  collector_thread = std::this_thread::get_id();
  mutator_condition.notify_all();
  mutator_condition.wait(lock, [] {
    for (const MutatorState* mutator : mutators)
      if (mutator != &thread_mutator && !mutator->parked) return false;
    return true;
  });
}

void resume_mutators() {
  std::lock_guard<std::mutex> lock(mutator_mutex);
  collection_requested = false;
  collector_thread = std::thread::id{};
  mutator_condition.notify_all();
}

void trace_children(ObjectHeader* header, nrq_gc_visit_fn visit,
                    void* context) {
  if (header->trace)
    header->trace(header->object, header->size, visit, context);
  if (header->reference_offsets.empty() || header->element_size == 0) return;
  const auto* bytes = static_cast<const unsigned char*>(header->object);
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
  // Objects born during this cycle are outside the snapshot and are retained
  // unconditionally by the sweep. Marking them would leave a mark bit that the
  // detached snapshot never clears.
  if (collection_phase.load(std::memory_order_relaxed) !=
          CollectionPhase::Idle &&
      header->birth_epoch == active_cycle_epoch)
    return;
  if (context.young_only && !header->young) return;
  header->marked = true;
  header->mark_next = context.pending;
  context.pending = header;
}

void enqueue_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* context = static_cast<MarkContext*>(opaque_context);
  ObjectHeader* found = object_index.find(object);
  // Roots may refer to static/native objects. Only collector-owned objects
  // enter the intrusive mark stack, so marking never allocates auxiliary heap.
  enqueue_header_locked(found, *context);
}

void mark_value_locked(void* object) {
  if (!object || collection_phase.load(std::memory_order_relaxed) ==
                     CollectionPhase::Idle)
    return;
  enqueue_reference(object, &active_mark);
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

void count_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* counts = static_cast<std::unordered_map<void*, uint64_t>*>(opaque_context);
  if (object_index.find(object)) {
    uint64_t& count = (*counts)[object];
    count = saturated_add(count, 1);
  }
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
  for (const auto& pinned : pinned_objects) {
    if (pinned.second != 0) enqueue_reference(pinned.first, &context);
  }
  if (context.young_only) {
    for (ObjectHeader* header : remembered_young)
      enqueue_header_locked(header, context);
  }
}

void remember_reference(void* object, void* opaque_context) {
  (void)opaque_context;
  if (!object) return;
  ObjectHeader* found = object_index.find(object);
  if (found && found->young) remembered_young.insert(found);
}

void remember_young_locked(void* object) {
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      !object)
    return;
  ObjectHeader* found = object_index.find(object);
  if (found && found->young) {
    try {
      remembered_young.insert(found);
    } catch (...) {
      nrq_runtime_fatal("GC remembered-set insertion failed");
    }
  }
}

void remember_children_locked(ObjectHeader* header) {
  trace_children(header, remember_reference, nullptr);
}

bool mark_slice_locked(size_t budget) {
  size_t steps = 0;
  while (active_mark.pending && steps < budget) {
    ObjectHeader* header = active_mark.pending;
    active_mark.pending = header->mark_next;
    header->mark_next = nullptr;
    if (active_mark.young_only && !header->young) continue;
    trace_children(header, enqueue_reference, &active_mark);
    ++steps;
  }
  return active_mark.pending == nullptr;
}

bool sweep_chunk_locked(size_t budget) {
  size_t steps = 0;
  while (sweep_list && steps < budget) {
    ObjectHeader* header = sweep_list;
    sweep_list = header->next;
    ++steps;

    const bool collectable = !active_collection_is_minor || header->young;
    if (!header->marked && collectable) {
      object_index.erase(header->object);
      remembered_young.erase(header);
      statistics.collected_bytes = saturated_add(
          statistics.collected_bytes, static_cast<uint64_t>(header->size));
      if (statistics.live_objects != 0) --statistics.live_objects;
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
            remember_children_locked(header);
            remembered_young.erase(header);
          }
        }
      } else {
        header->young = false;
        header->age = 0;
      }
    }
  }
  return sweep_list == nullptr;
}

void prepare_cycle_locked(bool young_only) {
  for (MutatorState* mutator : mutators)
    if (mutator) publish_tlab_locked(*mutator);

  // No mutator can be inside an allocator while stop_mutators() has everyone
  // parked. Invalidate stale TLAB cursors before reclaiming empty pages.
  ++gc_page_generation;
  for (auto page = gc_pages.begin(); page != gc_pages.end();) {
    if ((*page)->live_objects == 0) {
      ::operator delete((*page)->storage);
      page = gc_pages.erase(page);
    } else {
      (*page)->retired = false;
      ++page;
    }
  }

  sweep_list = nullptr;
  active_cycle_epoch = ++next_cycle_epoch;
  active_collection_is_minor = young_only;
  active_mark = MarkContext{};
  active_mark.young_only = young_only;
  enqueue_roots_locked(active_mark);
  collection_phase.store(CollectionPhase::Marking, std::memory_order_release);
}

void begin_cycle(bool young_only) {
  stop_mutators();
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
    ++minor_collections_since_major;
  } else {
    major_collections = saturated_add(major_collections, 1);
    minor_collections_since_major = 0;
  }
  statistics.allocated_bytes = 0;
  for (ObjectHeader* header = objects; header; header = header->next) {
    statistics.allocated_bytes = saturated_add(
        statistics.allocated_bytes, static_cast<uint64_t>(header->size));
  }
  if (!young_only) remembered_young.clear();
  active_mark = MarkContext{};
  collection_phase.store(CollectionPhase::Idle, std::memory_order_release);
  bytes_since_collection.store(0, std::memory_order_relaxed);
}

void run_collection(bool young_only) {
  std::lock_guard<std::mutex> cycle_lock(collection_cycle_mutex);
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
      bytes_since_collection.load(std::memory_order_relaxed) >=
          collection_threshold.load(std::memory_order_relaxed);
  if (!should_collect) return;
  const bool young_only =
      generational_collection_enabled.load(std::memory_order_relaxed) &&
      minor_collections_since_major < kMajorCollectionInterval;
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
  if (thread_allocation_generation != gc_page_generation) {
    thread_allocation_page = nullptr;
    thread_allocation_generation = gc_page_generation;
  }
  if (thread_allocation_page && !thread_allocation_page->retired &&
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
    void* storage = ::operator new(capacity, std::nothrow);
    if (!storage) return nullptr;
    try {
      auto new_page = std::make_unique<GcPage>();
      new_page->storage = static_cast<unsigned char*>(storage);
      new_page->capacity = capacity;
      page = new_page.get();
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      ::operator delete(storage);
      return nullptr;
    }
  }
  thread_allocation_page = page;
  thread_allocation_generation = gc_page_generation;
  void* header_storage = page->storage + page->used;
  page->used += span;
  auto* header = new (header_storage) ObjectHeader();
  auto rollback_reservation = [&]() {
    destroy_object_storage(header);
    page->used -= span;
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
    header->object = object;
    header->size = bytes;
    header->element_size = element_size;
    header->trace = trace;
    header->young = generational_collection_enabled.load(std::memory_order_relaxed);
    header->age = 0;
    header->birth_epoch = next_cycle_epoch;
    header->next = objects;
      if (!object_index.insert(object, header)) {
        rollback_reservation();
        return nullptr;
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
  atomic_saturating_add_size(bytes_since_collection, bytes);
  return object;
}

bool reserve_tlab_locked(MutatorState& mutator, size_t minimum_span) {
  if (mutator.tlab_generation != gc_page_generation) {
    mutator.tlab_page = nullptr;
    mutator.tlab_cursor = nullptr;
    mutator.tlab_end = nullptr;
    mutator.tlab_generation = gc_page_generation;
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
    void* storage = ::operator new(capacity, std::nothrow);
    if (!storage) return false;
    try {
      auto new_page = std::make_unique<GcPage>();
      new_page->storage = static_cast<unsigned char*>(storage);
      new_page->capacity = capacity;
      page = new_page.get();
      gc_pages.push_back(std::move(new_page));
      available = capacity;
    } catch (...) {
      ::operator delete(storage);
      return false;
    }
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
  header->object = object;
  header->size = bytes;
  header->element_size = element_size;
  header->trace = trace;
  header->young = generational_collection_enabled.load(std::memory_order_relaxed);
  header->age = 0;
  header->birth_epoch = next_cycle_epoch;
  header->tlab_next = thread_mutator.pending_objects;
  thread_mutator.pending_objects = header;
  if (bytes > SIZE_MAX - thread_mutator.pending_bytes ||
      thread_mutator.pending_count == SIZE_MAX)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  thread_mutator.pending_bytes += bytes;
  ++thread_mutator.pending_count;
  atomic_saturating_add(pending_allocation_bytes,
                        static_cast<uint64_t>(bytes));
  atomic_saturating_add(pending_allocation_count, uint64_t{1});
  atomic_saturating_add_size(bytes_since_collection, bytes);

  // During a cycle an unpublished young object is invisible to the marker and
  // to the root barrier. Publish it immediately so a barrier can identify it.
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle) {
    std::lock_guard<std::mutex> lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
  }
  return object;
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
  if (thread_mutator.depth != 0) {
    {
      std::lock_guard<std::mutex> lock(gc_mutex);
      const auto root = roots.find(root_slot);
      if (root == roots.end() || root->second.repeated_count != 0 ||
          root->second.object_size != sizeof(void*) ||
          root->second.reference_offsets.size() != 1 ||
          root->second.reference_offsets[0] != 0)
        nrq_runtime_fatal(
            "rooted GC allocation requires a registered pointer root slot");
    }
    bool handled = false;
    void* object = allocate_tlab(count * element_size, element_size, nullptr,
                                 reference_offsets, reference_count, handled);
    if (!handled) {
      std::lock_guard<std::mutex> lock(gc_mutex);
      object = allocate_locked(count * element_size, element_size, nullptr,
                               reference_offsets, reference_count);
    }
    std::lock_guard<std::mutex> lock(gc_mutex);
    const auto root = roots.find(root_slot);
    if (root == roots.end())
      nrq_runtime_fatal("rooted GC allocation lost its registered pointer root");
    *root_slot = object;
    return object;
  }
  std::lock_guard<std::mutex> lock(gc_mutex);
  const auto root = roots.find(root_slot);
  if (root == roots.end() || root->second.repeated_count != 0 ||
      root->second.object_size != sizeof(void*) ||
      root->second.reference_offsets.size() != 1 ||
      root->second.reference_offsets[0] != 0)
    nrq_runtime_fatal("rooted GC allocation requires a registered pointer root slot");
  void* object = allocate_locked(count * element_size, element_size, nullptr,
                                 reference_offsets, reference_count);
  *root_slot = object;
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
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = roots.find(slot);
  bool inserted = false;
  if (found == roots.end()) {
    try {
      RootInfo root;
      root.object_size = object_size;
      root.reference_offsets.assign(reference_offsets,
                                    reference_offsets + reference_count);
      auto result = roots.emplace(slot, std::move(root));
      found = result.first;
      inserted = result.second;
    } catch (...) {
      nrq_runtime_fatal("GC root registration failed to allocate root metadata");
    }
  } else if (found->second.object_size != object_size ||
             found->second.reference_offsets.size() != reference_count ||
             !std::equal(found->second.reference_offsets.begin(),
                         found->second.reference_offsets.end(), reference_offsets)) {
    nrq_runtime_fatal("GC root slot was registered with an incompatible layout");
  }
  if (found->second.references == SIZE_MAX) {
    if (inserted) roots.erase(found);
    nrq_runtime_fatal("GC root registration count overflow");
  }
  try {
    thread_roots.push_back(slot);
  } catch (...) {
    if (inserted) roots.erase(found);
    nrq_runtime_fatal("GC root registration failed to track the current thread");
  }
  ++found->second.references;
  if (collection_phase.load(std::memory_order_relaxed) !=
      CollectionPhase::Idle)
    mark_root_values_locked(slot, found->second);
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
    root.reference_offsets.assign(reference_offsets,
                                  reference_offsets + reference_count);
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
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = roots.find(slot);
  if (found != roots.end()) {
    auto thread_root = thread_roots.rend();
    for (auto item = thread_roots.rbegin(); item != thread_roots.rend(); ++item) {
      if (*item == slot) {
        thread_root = item;
        break;
      }
    }
    // Array roots are registered as external storage rather than as a stack
    // root on the calling thread. Ordinary root slots must be unregistered by
    // a thread that owns one of their registrations; otherwise a cross-thread
    // unregister could leave a stale TLS entry that later affects a reused slot.
    if (thread_root == thread_roots.rend() && found->second.repeated_count == 0)
      nrq_runtime_fatal("GC root unregistration must match a registration on the current thread");
    if (found->second.references == 0)
      nrq_runtime_fatal("GC root unregistration count underflow");
    if (thread_root != thread_roots.rend())
      thread_roots.erase(std::next(thread_root).base());
    if (--found->second.references == 0) roots.erase(found);
  }
}

void nrq_gc_root_store(void** slot, void* value) {
  if (!slot) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  const CollectionPhase phase =
      collection_phase.load(std::memory_order_relaxed);
  if (phase != CollectionPhase::Idle) {
    // Snapshot-at-the-beginning: retain the old referent as well as the new
    // one. During sweep, the new referent is retained before the next slice;
    // the old referent is already part of the snapshot decision.
    if (phase == CollectionPhase::Marking) mark_value_locked(*slot);
    mark_value_locked(value);
  }
  *slot = value;
  remember_young_locked(value);
}

void scan_pointer_words_locked(void* storage, size_t bytes, bool mark) {
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    if (!reference) continue;
    if (mark) mark_value_locked(reference);
    remember_young_locked(reference);
  }
}

void nrq_gc_root_copy(void* destination, const void* source, size_t bytes) {
  if (!destination || !source || bytes == 0) return;
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
  uint64_t marker = next_root_frame_marker.load(std::memory_order_relaxed);
  for (;;) {
    if (marker == 0 || marker == UINT64_MAX)
      nrq_runtime_fatal("GC root frame marker space exhausted");
    if (next_root_frame_marker.compare_exchange_weak(
            marker, marker + 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) break;
  }
  try {
    thread_root_frames.push_back(RootFrame{marker, thread_roots.size()});
  } catch (...) {
    nrq_runtime_fatal("GC root frame registration failed to allocate thread metadata");
  }
  return marker;
}

void nrq_gc_root_frame_end(uint64_t marker) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  if (thread_root_frames.empty() || thread_root_frames.back().marker != marker ||
      thread_root_frames.back().root_depth > thread_roots.size())
    nrq_runtime_fatal("GC root frames must end in LIFO order with a valid marker");
  const size_t target = thread_root_frames.back().root_depth;
  thread_root_frames.pop_back();
  while (thread_roots.size() > target) {
    void* slot = thread_roots.back();
    thread_roots.pop_back();
    auto found = roots.find(slot);
    if (found != roots.end()) {
      if (found->second.references == 0)
        nrq_runtime_fatal("GC root frame count underflow");
      if (--found->second.references == 0) roots.erase(found);
    }
  }
}

void nrq_gc_enable(void) {
  automatic_collection_enabled.store(true, std::memory_order_relaxed);
}

void nrq_gc_disable(void) {
  automatic_collection_enabled.store(false, std::memory_order_relaxed);
}

void nrq_gc_collect(void) { run_collection(/*young_only=*/false); }

void nrq_gc_collect_minor(void) { run_collection(/*young_only=*/true); }

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
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_requested) {
    if (thread_mutator.depth != 0) {
      thread_mutator.parked = true;
      mutator_condition.notify_all();
      mutator_condition.wait(lock, [] { return !collection_requested; });
      thread_mutator.parked = false;
    } else {
      mutator_condition.wait(lock, [] { return !collection_requested; });
    }
  }
  if (thread_mutator.depth == SIZE_MAX)
    nrq_runtime_fatal("GC mutator nesting depth overflow");
  if (thread_mutator.depth++ == 0) {
    try {
      mutators.push_back(&thread_mutator);
    } catch (...) {
      thread_mutator.depth = 0;
      nrq_runtime_fatal("GC mutator registration failed");
    }
  }
}

void nrq_gc_mutator_leave(void) {
  std::lock_guard<std::mutex> lock(mutator_mutex);
  if (thread_mutator.depth == 0)
    nrq_runtime_fatal("GC mutator leave without matching enter");
  if (--thread_mutator.depth == 0) {
    {
      std::lock_guard<std::mutex> heap_lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
    }
    thread_mutator.parked = false;
    auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
    if (found == mutators.end())
      nrq_runtime_fatal("GC mutator registry lost an active thread");
    mutators.erase(found);
    mutator_condition.notify_all();
  }
}

void nrq_gc_safepoint(void) {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  if (!collection_requested || collector_thread == std::this_thread::get_id() ||
      thread_mutator.depth == 0)
    return;
  thread_mutator.parked = true;
  mutator_condition.notify_all();
  mutator_condition.wait(lock, [] { return !collection_requested; });
  thread_mutator.parked = false;
}

void nrq_gc_set_threshold(size_t bytes) {
  collection_threshold.store(bytes == 0 ? 1 : bytes,
                             std::memory_order_relaxed);
}

nrq_gc_stats nrq_gc_get_stats(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  nrq_gc_stats result = statistics;
  result.allocated_bytes = saturated_add(
      result.allocated_bytes,
      pending_allocation_bytes.load(std::memory_order_relaxed));
  result.live_objects = saturated_add(
      result.live_objects,
      pending_allocation_count.load(std::memory_order_relaxed));
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
  if (!object_index.find(object)) return nullptr;
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
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!object_index.find(object)) return nullptr;
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
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!reference_diagnostics_enabled) return -1;
  if (!object || !object_index.find(object)) return 0;
  try {
    std::unordered_map<void*, uint64_t> counts;
    for (ObjectHeader* header = objects; header; header = header->next) {
      if (header->trace)
        header->trace(header->object, header->size, count_reference, &counts);
      if (!header->reference_offsets.empty() && header->element_size != 0) {
        const auto* bytes = static_cast<const unsigned char*>(header->object);
        for (size_t base = 0; base + header->element_size <= header->size;
             base += header->element_size) {
          for (size_t offset_index = 0;
               offset_index < header->reference_offsets.size(); ++offset_index) {
            const size_t offset = header->reference_offsets[offset_index];
            void* reference = nullptr;
            std::memcpy(&reference, bytes + base + offset, sizeof(reference));
            count_reference(reference, &counts);
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
            count_reference(reference, &counts);
          }
        }
      } else {
        for (size_t offset : root.second.reference_offsets) {
          if (offset > root.second.object_size ||
              sizeof(void*) > root.second.object_size - offset) continue;
          void* reference = nullptr;
          std::memcpy(&reference, bytes + offset, sizeof(reference));
          count_reference(reference, &counts);
        }
      }
    }
    for (const auto& pinned : pinned_objects) {
      uint64_t& count = counts[pinned.first];
      count = saturated_add(count, static_cast<uint64_t>(pinned.second));
    }
    auto found = counts.find(object);
    return found == counts.end() ? 0 : static_cast<int64_t>(found->second);
  } catch (...) {
    return -1;
  }
}

}  // extern "C"
