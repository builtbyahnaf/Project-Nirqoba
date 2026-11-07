// gc_debug.cpp — the observation layer of the development runtime.
//
// This file is part of both runtime libraries and compiles to a single function
// in the production one: `nrq_runtime_debug_enabled` reporting 0. Everything
// else here needs NRQ_DEBUG_ENABLED, because these entry points observe the heap
// and have no meaning in a build that does not record anything.
//
// Design notes
//
//  * Recording must never change what the collector does, so nothing in this
//    file holds the collector lock, allocates on behalf of the heap, or waits on
//    a mutator. Each thread owns a fixed ring and appends to it without
//    cooperating with anyone.
//  * A ring that fills drops records and counts the loss instead of growing or
//    blocking. A trace is a sample of execution, and a bounded one says so.
//  * Rings are registered in a leaked registry. Thread-local destructors can run
//    after namespace-scope statics are gone; leaking the registry keeps
//    unregistration valid until the process exits.

#include <nirqoba/runtime/gc_debug.h>

#include "runtime_config.h"

#include <stdint.h>

int32_t nrq_runtime_debug_enabled(void) { return NRQ_DEBUG_ENABLED ? 1 : 0; }

#if NRQ_DEBUG_ENABLED

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "gc_debug_internal.h"

namespace {

constexpr size_t kEventRingCapacity = 1024;
constexpr size_t kTagRingCapacity = 512;

// Sequence numbers and the clock are process-wide: a consumer merges rings and
// needs one order and one time base to do it.
std::atomic<uint64_t> next_sequence{1};

uint64_t clock_origin = 0;
uint64_t clock_frequency = 0;
std::once_flag clock_once;

void init_clock() {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER counter{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  clock_frequency = frequency.QuadPart ? static_cast<uint64_t>(frequency.QuadPart) : 1;
  clock_origin = static_cast<uint64_t>(counter.QuadPart);
}

uint64_t now_nanos() {
  std::call_once(clock_once, init_clock);
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart) - clock_origin;
  // Multiply before dividing so a microsecond-resolution clock still reports
  // nanoseconds without truncating the whole tick.
  return (ticks / clock_frequency) * 1000000000ull +
         ((ticks % clock_frequency) * 1000000000ull) / clock_frequency;
}

// A single-producer / single-consumer ring. The owning thread appends; the
// draining thread removes. `produced` and `consumed` are monotonic totals, so a
// full ring is a subtraction rather than a wrap-around comparison.
template <typename Record, size_t Capacity>
struct Ring {
  Record entries[Capacity];
  std::atomic<size_t> produced{0};
  std::atomic<size_t> consumed{0};
  std::atomic<uint64_t> dropped{0};

  void push(const Record& record) {
    const size_t write = produced.load(std::memory_order_relaxed);
    if (write - consumed.load(std::memory_order_relaxed) >= Capacity) {
      dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    entries[write % Capacity] = record;
    produced.store(write + 1, std::memory_order_release);
  }

  size_t drain(Record* out, size_t capacity) {
    size_t read = consumed.load(std::memory_order_relaxed);
    const size_t write = produced.load(std::memory_order_acquire);
    size_t copied = 0;
    while (read < write && copied < capacity) {
      out[copied++] = entries[read % Capacity];
      ++read;
    }
    consumed.store(read, std::memory_order_release);
    return copied;
  }

  uint64_t dropped_count() const {
    return dropped.load(std::memory_order_relaxed);
  }
};

struct ThreadRings {
  Ring<nrq_gc_event, kEventRingCapacity> events;
  Ring<nrq_gc_debug_tag, kTagRingCapacity> tags;
  ThreadRings* next = nullptr;
};

// Registry of live thread rings. Leaked on purpose; see the file comment.
struct Registry {
  std::mutex mutex;
  ThreadRings* head = nullptr;
};

Registry& registry() {
  static Registry* instance = new Registry();
  return *instance;
}

// Each thread attaches its rings lazily, on its first record, and detaches them
// when it exits. A thread that never records never appears.
struct ThreadAttachment {
  ThreadRings rings;

  ThreadAttachment() {
    Registry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    rings.next = state.head;
    state.head = &rings;
  }

  ~ThreadAttachment() {
    Registry& state = registry();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (ThreadRings** link = &state.head; *link; link = &(*link)->next) {
      if (*link == &rings) {
        *link = rings.next;
        break;
      }
    }
  }
};

ThreadRings& thread_rings() {
  thread_local ThreadAttachment attachment;
  return attachment.rings;
}

// Declared allocation sites. The compiler emits a declaration at each allocation
// site it lowers, so it runs once per execution of that expression: the table
// has to accept an insert with no lock and no allocation. It is a fixed, open-
// addressed table of insert-only records; a program with more distinct sites
// than it holds loses the surplus mappings, which costs a line number in a
// trace, not correctness.
constexpr size_t kSiteTableCapacity = 1024;
constexpr size_t kMaxSitePathBytes = 176;

struct SiteSlot {
  // 0 when free, 1 while a writer is filling the slot, 2 when readable.
  std::atomic<uint32_t> state{0};
  std::atomic<uint64_t> id{0};
  uint32_t line = 0;
  char file[kMaxSitePathBytes] = {};
};

SiteSlot* site_slots() {
  static SiteSlot* slots = new SiteSlot[kSiteTableCapacity]();
  return slots;
}

constexpr size_t kSiteTableMask = kSiteTableCapacity - 1;

}  // namespace

namespace nirq::gc::detail {

void debug_record_event(uint32_t kind, uint32_t aux, uint64_t value,
                        const void* address) {
  nrq_gc_event event{};
  event.sequence = next_sequence.fetch_add(1, std::memory_order_relaxed);
  event.timestamp_nanos = now_nanos();
  event.address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(address));
  event.value = value;
  event.kind = kind;
  event.aux = aux;
  thread_rings().events.push(event);
}

void debug_record_tag(uint64_t site, const void* object) {
  nrq_gc_debug_tag tag{};
  tag.site = site;
  tag.object = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
  thread_rings().tags.push(tag);
}

}  // namespace nirq::gc::detail

extern "C" {

void nrq_gc_debug_set_event_mask(uint32_t mask) {
  nirq::gc::detail::debug_event_mask().store(mask, std::memory_order_relaxed);
}

uint32_t nrq_gc_debug_event_mask(void) {
  return nirq::gc::detail::debug_event_mask().load(std::memory_order_relaxed);
}

size_t nrq_gc_debug_drain_events(nrq_gc_event* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  Registry& state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  size_t copied = 0;
  for (ThreadRings* rings = state.head; rings; rings = rings->next) {
    if (copied == capacity) break;
    copied += rings->events.drain(out + copied, capacity - copied);
  }
  return copied;
}

size_t nrq_gc_debug_drain_tags(nrq_gc_debug_tag* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  Registry& state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  size_t copied = 0;
  for (ThreadRings* rings = state.head; rings; rings = rings->next) {
    if (copied == capacity) break;
    copied += rings->tags.drain(out + copied, capacity - copied);
  }
  return copied;
}

uint64_t nrq_gc_debug_dropped_events(void) {
  Registry& state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  uint64_t total = 0;
  for (ThreadRings* rings = state.head; rings; rings = rings->next)
    total += rings->events.dropped_count();
  return total;
}

uint64_t nrq_gc_debug_dropped_tags(void) {
  Registry& state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  uint64_t total = 0;
  for (ThreadRings* rings = state.head; rings; rings = rings->next)
    total += rings->tags.dropped_count();
  return total;
}

void nrq_gc_debug_declare_site(uint64_t site, const char* file,
                              size_t file_length, uint32_t line) {
  if (site == 0) return;
  SiteSlot* slots = site_slots();
  size_t index = static_cast<size_t>(site) & kSiteTableMask;
  for (size_t probes = 0; probes < kSiteTableCapacity; ++probes,
              index = (index + 1) & kSiteTableMask) {
    SiteSlot& slot = slots[index];
    const uint64_t current = slot.id.load(std::memory_order_relaxed);
    if (current == site &&
        slot.state.load(std::memory_order_acquire) == 2)
      return;  // Already declared; entering the site again costs one load.
    if (current != 0) continue;  // Another site owns this slot.
    uint32_t expected = 0;
    if (!slot.state.compare_exchange_strong(expected, 1,
                                            std::memory_order_acq_rel)) {
      // Another thread is publishing a site here. Leave its slot alone and keep
      // probing; the next free slot answers these declarations.
      continue;
    }
    const size_t length = file && file_length < kMaxSitePathBytes - 1
                              ? file_length
                              : (file ? kMaxSitePathBytes - 1 : 0);
    if (length != 0) std::memcpy(slot.file, file, length);
    slot.file[length] = '\0';
    slot.line = line;
    slot.id.store(site, std::memory_order_relaxed);
    slot.state.store(2, std::memory_order_release);
    return;
  }
}

int32_t nrq_gc_debug_describe_site(uint64_t site, char* file, size_t capacity,
                                   uint32_t* line) {
  if (site == 0) return 0;
  SiteSlot* slots = site_slots();
  // The table is small and lookups belong to the consumer, which asks about a
  // handful of sites after draining a trace.
  for (size_t index = 0; index < kSiteTableCapacity; ++index) {
    const SiteSlot& slot = slots[index];
    if (slot.state.load(std::memory_order_acquire) != 2) continue;
    if (slot.id.load(std::memory_order_relaxed) != site) continue;
    if (line) *line = slot.line;
    if (file && capacity != 0) {
      const size_t length = std::strlen(slot.file);
      const size_t keep = length < capacity - 1 ? length : capacity - 1;
      std::memcpy(file, slot.file, keep);
      file[keep] = '\0';
    }
    return 1;
  }
  return 0;
}

void nrq_gc_debug_tag_allocation(void* object, uint64_t site) {
  if (!object) return;
  nirq::gc::detail::debug_record_tag(site, object);
}

}  // extern "C"

#endif  // NRQ_DEBUG_ENABLED
