// Windows explicit allocator implementations. Every strategy is owned by an
// opaque runtime handle; the common allocation registry records which handle
// owns each returned pointer so frees cannot cross allocator domains.

#ifndef _WIN32
#  error "allocator_windows.cpp is the Windows-only allocator implementation."
#endif

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/cdmm.h"
#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/fatal.h"
#include "nirqoba/runtime/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <shared_mutex>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

namespace {

constexpr size_t kDefaultRegionBytes = 1024 * 1024;
constexpr size_t kMinimumBlockBytes = 16;
// Alignment used when `memory.reallocate` is asked to grow a null pointer, i.e.
// when the call is really a fresh allocation with no prior alignment to reuse.
constexpr size_t kDefaultAlignmentBytes = 16;
// Smallest chunk either explicit strategy commits.
constexpr size_t kCdmmMinimumChunkBytes = 4096;
// Pool chunk size when the caller does not choose a slot count. It matches the
// collector's page size so both metadata systems are arranged the same way.
constexpr size_t kCdmmPoolChunkBytes = 64 * 1024;

// --- CDMM explicit-strategy metadata -----------------------------------------
//
// Neither explicit strategy puts a header on an object. An arena derives the
// layout of a block from the bump pointer of the chunk that contains it; a pool
// derives the slot size and liveness from the chunk as well. What remains is one
// destructor record per destructible arena object, one liveness bit per pool
// slot, and the optional generation word. The range table below is what lets an
// outside caller ask which strategy owns an address without reading the object.

struct DestructorRecord {
  DestructorRecord* previous = nullptr;
  NrqCdmmDestructor destructor = nullptr;
  void* context = nullptr;
  void* object = nullptr;
};

struct CdmmRange {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  const void* owner = nullptr;
  NrqCdmmStrategy strategy = NRQ_CDMM_STRATEGY_NONE;
};

std::mutex cdmm_range_mutex;
std::vector<CdmmRange> cdmm_ranges;

void cdmm_register_range(const void* owner, uintptr_t begin, size_t bytes,
                         NrqCdmmStrategy strategy);
void cdmm_unregister_range(const void* owner, uintptr_t begin);
bool cdmm_lookup_range(uintptr_t address, CdmmRange& result);

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

bool checked_align_up(size_t value, size_t alignment, size_t& result) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      value > std::numeric_limits<size_t>::max() - (alignment - 1))
    return false;
  result = align_up(value, alignment);
  return true;
}

bool checked_align_address(uintptr_t value, size_t alignment,
                           uintptr_t& result) {
  static_assert(sizeof(size_t) == sizeof(uintptr_t));
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      value > std::numeric_limits<uintptr_t>::max() -
                  static_cast<uintptr_t>(alignment - 1))
    return false;
  const uintptr_t mask = static_cast<uintptr_t>(alignment - 1);
  result = (value + mask) & ~mask;
  return true;
}

size_t next_power_of_two(size_t value) {
  if (value <= 1) return 1;
  --value;
  for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
    value |= value >> shift;
  return value + 1;
}

// The Windows process heap already returns max_align_t-aligned storage. A
// request within that alignment therefore needs no prefix, slack or saved raw
// pointer: the returned address is the heap block itself. Only over-aligned
// requests pay for a header and the alignment adjustment.
constexpr size_t kHeapNaturalAlignment = alignof(std::max_align_t);
static_assert(kHeapNaturalAlignment >= 8,
              "the process heap must satisfy fundamental alignment");

void* heap_alloc_aligned(size_t bytes, size_t alignment) {
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return nullptr;
  if (alignment <= kHeapNaturalAlignment)
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
  if (alignment > std::numeric_limits<size_t>::max() - sizeof(void*) ||
      bytes > std::numeric_limits<size_t>::max() - alignment - sizeof(void*))
    return nullptr;
  const size_t total = bytes + alignment - 1 + sizeof(void*);
  void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
  if (!raw) return nullptr;
  const uintptr_t start = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
  const uintptr_t address = (start + alignment - 1) & ~(alignment - 1);
  void* result = reinterpret_cast<void*>(address);
  reinterpret_cast<void**>(result)[-1] = raw;
  return result;
}

void heap_free_aligned(void* pointer) {
  if (pointer) HeapFree(GetProcessHeap(), 0, reinterpret_cast<void**>(pointer)[-1]);
}

void heap_free_pointer(void* pointer, size_t alignment) {
  if (!pointer) return;
  if (alignment <= kHeapNaturalAlignment) {
    HeapFree(GetProcessHeap(), 0, pointer);
    return;
  }
  heap_free_aligned(pointer);
}

class Allocator {
 public:
  explicit Allocator(size_t capacity) : capacity_(capacity) {}
  virtual ~Allocator() = default;
  virtual void* allocate(size_t bytes, size_t alignment) = 0;
  virtual void deallocate(void* pointer) = 0;
  // General storage needs the recorded alignment to know whether the block has
  // a raw-pointer prefix. Other strategies ignore it and keep their existing
  // deallocate implementation.
  virtual void deallocate_aligned(void* pointer, size_t alignment) {
    (void)alignment;
    deallocate(pointer);
  }
  virtual bool can_deallocate(void*) const { return true; }
  virtual bool reset() = 0;
  virtual bool can_destroy() const { return true; }
  /**
   * Stateful region allocators and user callbacks are serialized by default.
   * Implementations may opt out only when their underlying allocation API is
   * safe for concurrent calls and they keep no mutable per-instance state.
   */
  virtual bool requires_operation_serialization() const { return true; }
  virtual NrqAllocatorKind kind() const { return NRQ_ALLOCATOR_GENERAL; }
  // Strategy reported by `nrq_cdmm_strategy_of`. Only the explicit strategies
  // in this file answer with a name; every other allocator reports none, which
  // is also what keeps stack and collector storage out of the range table.
  virtual NrqCdmmStrategy cdmm_strategy() const { return NRQ_CDMM_STRATEGY_NONE; }
  // Fills the page-level facts for `object`, which is inside a chunk this
  // allocator owns. The base answer is the common one: no per-object header.
  virtual void cdmm_describe(const void* object, NrqCdmmFacts& facts) const {
    (void)object;
    facts.header_bytes = 0;
  }
  // Runs `function` under the allocator's operation lock. Entry points that do
  // more than one step (a destructor record plus its object) use it so the pair
  // is serialized as one operation.
  template <typename Function>
  auto serialize_operation(Function&& function) -> decltype(function()) {
    if (!requires_operation_serialization()) return function();
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return function();
  }
  size_t capacity() const { return capacity_; }

  bool try_pin_operation() {
    size_t current = in_flight_operations_.load(std::memory_order_relaxed);
    while (current != SIZE_MAX) {
      if (in_flight_operations_.compare_exchange_weak(
              current, current + 1, std::memory_order_acquire,
              std::memory_order_relaxed)) return true;
    }
    return false;
  }
  void unpin_operation() {
    const size_t previous =
        in_flight_operations_.fetch_sub(1, std::memory_order_release);
    if (previous == 0)
      nrq_runtime_fatal("allocator operation pin count underflow");
  }
  size_t in_flight_operations() const {
    return in_flight_operations_.load(std::memory_order_acquire);
  }

  void* allocate_serialized(size_t bytes, size_t alignment) {
    if (!requires_operation_serialization()) return allocate(bytes, alignment);
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return allocate(bytes, alignment);
  }
  void deallocate_serialized(void* pointer, size_t alignment) {
    if (!requires_operation_serialization()) {
      deallocate_aligned(pointer, alignment);
      return;
    }
    std::lock_guard<std::mutex> lock(operation_mutex_);
    deallocate_aligned(pointer, alignment);
  }
  bool try_deallocate_serialized(void* pointer, size_t alignment) {
    if (!requires_operation_serialization()) {
      if (!can_deallocate(pointer)) return false;
      deallocate_aligned(pointer, alignment);
      return true;
    }
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!can_deallocate(pointer)) return false;
    deallocate_aligned(pointer, alignment);
    return true;
  }
  bool reset_serialized() {
    if (!requires_operation_serialization()) return reset();
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return reset();
  }
  void* reallocate_serialized(void* pointer, size_t old_bytes, size_t bytes,
                              size_t alignment) {
    if (!requires_operation_serialization())
      return reallocate(pointer, old_bytes, bytes, alignment);
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return reallocate(pointer, old_bytes, bytes, alignment);
  }
  void free_statistics_serialized(size_t& free_bytes,
                                  size_t& largest_free_block) const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    free_statistics(free_bytes, largest_free_block);
  }

  // Reports how many bytes the allocator can still serve and the largest single
  // request it can satisfy. Strategies that cannot enumerate their free space
  // (the general-purpose and custom allocators) inherit these zeros, which the
  // profiler reports as "not measurable" rather than as "no free space".
  virtual void free_statistics(size_t& free_bytes,
                              size_t& largest_free_block) const {
    free_bytes = 0;
    largest_free_block = 0;
  }

  // Resizes `pointer` from `old_bytes` to `bytes`, keeping the allocator
  // identity and the alignment recorded when the block was created.
  //
  // The default implementation allocates a replacement, copies the surviving
  // bytes and then releases the original, so a null result means the original
  // allocation is still live and still owned by the caller. Strategies with
  // stricter lifetime rules override this: a stack allocator must resize in
  // place, and an arena can extend its bump pointer when the block is the most
  // recent one.
  virtual void* reallocate(void* pointer, size_t old_bytes, size_t bytes,
                          size_t alignment) {
    void* replacement = allocate(bytes, alignment);
    if (!replacement) return nullptr;
    std::memcpy(replacement, pointer, std::min(old_bytes, bytes));
    deallocate_aligned(pointer, alignment);
    return replacement;
  }

 protected:
  size_t capacity_;

 private:
  mutable std::mutex operation_mutex_;
  std::atomic<size_t> in_flight_operations_{0};
};

class GeneralAllocator final : public Allocator {
 public:
  GeneralAllocator() : Allocator(0) {}
  /**
   * This implementation intentionally uses the Windows process heap. HeapAlloc
   * and HeapFree provide their own synchronization, so adding this runtime's
   * per-allocator mutex would serialize independent callers unnecessarily.
   * The allocation registry still validates ownership and allocator identity.
   * Platform abstraction or replacement is future work, not part of this
   * Phase 6 Upgrade change.
   */
  bool requires_operation_serialization() const override { return false; }
  void* allocate(size_t bytes, size_t alignment) override {
    return heap_alloc_aligned(bytes, alignment);
  }
  void deallocate(void*) override {
    nrq_runtime_fatal("general allocation requires its recorded alignment");
  }
  void deallocate_aligned(void* pointer, size_t alignment) override {
    heap_free_pointer(pointer, alignment);
  }
  bool reset() override { return true; }
};

class CustomAllocator final : public Allocator {
 public:
  explicit CustomAllocator(const NrqCustomAllocatorVTable& callbacks)
      : Allocator(0), callbacks_(callbacks) {}

  ~CustomAllocator() override {
    for (const auto& allocation : live_) {
      try {
        callbacks_.deallocate(callbacks_.context, allocation.first,
                              allocation.second.bytes, allocation.second.alignment);
      } catch (...) {
      }
    }
    if (callbacks_.destroy) {
      try { callbacks_.destroy(callbacks_.context); } catch (...) {}
    }
  }

  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    void* pointer = callbacks_.allocate(callbacks_.context, bytes, alignment);
    if (!pointer) return nullptr;
    try {
      auto [entry, inserted] = live_.emplace(pointer, Request{bytes, alignment});
      (void)entry;
      if (!inserted) return nullptr;
    } catch (...) {
      try { callbacks_.deallocate(callbacks_.context, pointer, bytes, alignment); }
      catch (...) {}
      return nullptr;
    }
    return pointer;
  }

  void deallocate(void* pointer) override {
    auto found = live_.find(pointer);
    if (found == live_.end())
      nrq_runtime_fatal("custom allocator received an unknown allocation");
    try {
      callbacks_.deallocate(callbacks_.context, pointer,
                            found->second.bytes, found->second.alignment);
    } catch (...) {
      nrq_runtime_fatal("custom allocator deallocation callback threw an exception");
    }
    live_.erase(found);
  }

  bool reset() override { return false; }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_CUSTOM; }

 private:
  struct Request { size_t bytes; size_t alignment; };
  NrqCustomAllocatorVTable callbacks_;
  std::unordered_map<void*, Request> live_;
};

struct PoolBucket {
  size_t stride = 0;
  size_t alignment = 0;
  std::vector<void*> free_slots;
  std::vector<void*> raw_chunks;
  std::unordered_map<void*, size_t> slot_chunk;
};

class FixedPoolAllocator final : public Allocator {
 public:
  FixedPoolAllocator(size_t capacity, bool slab)
      : Allocator(std::max<size_t>(capacity, 16)), slab_(slab) {}
  ~FixedPoolAllocator() override { reset(); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        (!slab_ && bytes > capacity_)) return nullptr;
    size_t size_class = next_power_of_two(
        slab_ ? std::max(bytes, alignment) : std::max(capacity_, alignment));
    if (size_class == 0) return nullptr;
    if (slab_) size_class = std::max<size_t>(16, size_class);
    const auto key = std::make_pair(size_class, alignment);
    PoolBucket& bucket = buckets_[key];
    if (bucket.stride == 0) {
      bucket.alignment = std::max<size_t>(alignment, 16);
      if (!checked_align_up(size_class, bucket.alignment, bucket.stride))
        return nullptr;
    }
    if (bucket.free_slots.empty() && !add_chunk(bucket)) return nullptr;
    void* result = bucket.free_slots.back();
    bucket.free_slots.pop_back();
    return result;
  }

  void deallocate(void* pointer) override {
    for (auto& pair : buckets_) {
      auto found = pair.second.slot_chunk.find(pointer);
      if (found != pair.second.slot_chunk.end()) {
        pair.second.free_slots.push_back(pointer);
        return;
      }
    }
  }

  bool reset() override {
    for (auto& pair : buckets_) {
      for (void* raw : pair.second.raw_chunks)
        HeapFree(GetProcessHeap(), 0, raw);
    }
    buckets_.clear();
    return true;
  }
  NrqAllocatorKind kind() const override {
    return slab_ ? NRQ_ALLOCATOR_SLAB : NRQ_ALLOCATOR_POOL;
  }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = 0;
    largest_free_block = 0;
    for (const auto& pair : buckets_) {
      const PoolBucket& bucket = pair.second;
      if (bucket.free_slots.empty()) continue;
      // Each free slot can serve one request no larger than the slot stride.
      free_bytes += bucket.free_slots.size() * bucket.stride;
      largest_free_block = std::max(largest_free_block, bucket.stride);
    }
  }

 private:
  bool add_chunk(PoolBucket& bucket) {
    const size_t old_free_count = bucket.free_slots.size();
    const size_t old_chunk_count = bucket.raw_chunks.size();
    const size_t slots = std::max<size_t>(8, 64 * 1024 / bucket.stride);
    if (slots > (std::numeric_limits<size_t>::max() - bucket.alignment) /
                    bucket.stride) return false;
    if (slots > std::numeric_limits<size_t>::max() - old_free_count ||
        slots > std::numeric_limits<size_t>::max() - old_chunk_count ||
        slots > std::numeric_limits<size_t>::max() - bucket.slot_chunk.size())
      return false;
    const size_t total = slots * bucket.stride + bucket.alignment - 1;
    try {
      bucket.free_slots.reserve(old_free_count + slots);
      bucket.raw_chunks.reserve(old_chunk_count + 1);
      bucket.slot_chunk.reserve(bucket.slot_chunk.size() + slots);
    } catch (...) {
      return false;
    }
    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!raw) return false;
    uintptr_t base = 0;
    if (!checked_align_address(reinterpret_cast<uintptr_t>(raw),
                               bucket.alignment, base)) {
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    try {
      bucket.raw_chunks.push_back(raw);
      for (size_t index = 0; index < slots; ++index) {
        void* slot = reinterpret_cast<void*>(base + index * bucket.stride);
        bucket.free_slots.push_back(slot);
        bucket.slot_chunk.emplace(slot, old_chunk_count);
      }
    } catch (...) {
      for (size_t index = old_free_count; index < bucket.free_slots.size(); ++index)
        bucket.slot_chunk.erase(bucket.free_slots[index]);
      bucket.free_slots.resize(old_free_count);
      bucket.raw_chunks.resize(old_chunk_count);
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    return true;
  }

  bool slab_;
  std::map<std::pair<size_t, size_t>, PoolBucket> buckets_;
};

// Fixed-slot pool for explicit CDMM allocation.
//
// One slot size per pool, one liveness bit per slot, and a free list threaded
// through the free slots themselves. A slot carries no header and keeps no side
// record unless the pool was created with generations, so an allocator that
// hands out a million 32-byte objects pays one bit per object rather than an
// object record. Chunk size is the pool's page: layout, stride and liveness all
// come from the chunk, which is the same reasoning behind the headerless GC
// representation.
class CdmmPool final : public Allocator {
 public:
  CdmmPool(size_t slot_bytes, size_t alignment, size_t slots_per_chunk,
           bool generations, NrqCdmmDestructor destructor, void* context)
      : Allocator(slot_bytes),
        slot_bytes_(slot_bytes),
        alignment_(alignment),
        generations_(generations),
        destructor_(destructor),
        context_(context) {
    const size_t minimum = std::max(slot_bytes_, sizeof(size_t));
    stride_ = align_up(minimum, alignment_);
    slots_per_chunk_ = slots_per_chunk != 0
                           ? slots_per_chunk
                           : std::max<size_t>(8, kCdmmPoolChunkBytes / stride_);
  }
  ~CdmmPool() override { release_chunks(); }

  NrqCdmmStrategy cdmm_strategy() const override { return NRQ_CDMM_STRATEGY_POOL; }

  // Commits the first chunk at creation so the pool either exists fully formed
  // or not at all, and so `committed` is meaningful before the first allocation.
  bool reserve_chunk() { return add_chunk(); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || bytes > slot_bytes_ || alignment == 0 ||
        alignment > alignment_ || (alignment & (alignment - 1)) != 0)
      return nullptr;
    return allocate_slot();
  }
  void deallocate(void* pointer) override { release_slot(pointer, false, 0); }

  bool release_serialized_slot(void* slot, bool checked, uint64_t generation) {
    return serialize_operation(
        [&] { return release_slot(slot, checked, generation); });
  }

  // Scope path: the scope owns the pool for its lifetime, so a release needs no
  // operation lock. The liveness and generation checks are identical, so a
  // double release or a stale generation is still refused.
  bool release_unserialized(void* slot, bool checked, uint64_t generation) {
    return release_slot(slot, checked, generation);
  }

  bool reset() override {
    free_slot_count_ = 0;
    for (size_t chunk_index = 0; chunk_index < chunks_.size(); ++chunk_index) {
      Chunk& chunk = chunks_[chunk_index];
      if (chunk.live == 0) {
        // Its free list is already complete; rebuilding it would be work
        // proportional to a pool the caller is not using.
        free_slot_count_ += chunk.free_count;
        continue;
      }
      // Only the live slots have to move. Every other slot is already on this
      // chunk's free list, so reset costs the live set plus a bitmap scan, not a
      // walk over the whole chunk.
      size_t released = 0;
      for (size_t word = 0; word < chunk.live_bits.size(); ++word) {
        uint64_t bits = chunk.live_bits[word];
        chunk.live_bits[word] = 0;
        while (bits != 0) {
          const size_t bit_index = static_cast<size_t>(std::countr_zero(bits));
          bits &= bits - 1;
          const size_t local = word * 64 + bit_index;
          if (local >= chunk.slot_count) break;
          void* slot = reinterpret_cast<void*>(chunk.base + local * stride_);
          if (destructor_) destructor_(slot, context_);
          std::memset(slot, 0xDD, stride_);
          *static_cast<size_t*>(slot) = chunk.free_head;
          chunk.free_head = local;
          ++released;
        }
      }
      chunk.live = 0;
      chunk.free_count += released;
      free_slot_count_ += chunk.free_count;
      if (!chunk.in_free_set) {
        chunk.in_free_set = true;
        chunks_with_free_.push_back(chunk_index);
      }
    }
    live_total_ = 0;
    return true;
  }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_POOL; }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = free_slot_count_ * stride_;
    // A free slot can serve exactly one slot-sized request.
    largest_free_block = free_slot_count_ == 0 ? 0 : stride_;
  }

  void cdmm_describe(const void* object, NrqCdmmFacts& facts) const override {
    facts.header_bytes = 0;
    facts.object_bytes = slot_bytes_;
    facts.generation = 0;
    facts.metadata_bytes = 0;
    size_t chunk_index = 0;
    size_t local = 0;
    if (!locate(object, chunk_index, local)) return;
    const Chunk& chunk = chunks_[chunk_index];
    if (generations_) facts.generation = chunk.generations[local];
    facts.metadata_bytes = chunk.slot_count == 0 ? 0
                                                 : chunk.metadata_bytes / chunk.slot_count;
  }

  uint64_t slot_generation(const void* slot) const {
    size_t chunk_index = 0;
    size_t local = 0;
    if (!generations_ || !locate(slot, chunk_index, local)) return 0;
    return chunks_[chunk_index].generations[local];
  }

#if defined(NRQ_RUNTIME_DEBUG_ENABLED) && NRQ_RUNTIME_DEBUG_ENABLED
  bool seed_generation(const void* slot, uint32_t generation) {
    size_t chunk_index = 0;
    size_t local = 0;
    if (!generations_ || !locate(slot, chunk_index, local)) return false;
    chunks_[chunk_index].generations[local] = generation;
    return true;
  }
#endif

  uint64_t live_count() const { return live_total_; }
  uint64_t committed_bytes() const { return committed_; }
  uint64_t slot_bytes() const { return slot_bytes_; }
  bool contains(const void* pointer) const {
    size_t chunk_index = 0;
    size_t local = 0;
    return locate(pointer, chunk_index, local);
  }

 private:
  static constexpr size_t kNoSlot = std::numeric_limits<size_t>::max();

  struct Chunk {
    void* raw = nullptr;
    uintptr_t base = 0;
    size_t slot_count = 0;
    size_t live = 0;
    // Local free list: a free slot holds the local index of the next free slot in
    // this chunk, and the chunk holds the head. Keeping the list per chunk is what
    // makes a reset cost the chunks it touches instead of the whole pool.
    size_t free_head = kNoSlot;
    size_t free_count = 0;
    bool in_free_set = false;
    size_t metadata_bytes = 0;
    std::vector<uint64_t> live_bits;
    std::vector<uint32_t> generations;
  };

  // Maps an address to its chunk and local slot, or returns false when the
  // address is not the start of a slot in this pool. An interior address is
  // rejected rather than rounded: releasing one would corrupt its neighbour.
  bool locate(const void* pointer, size_t& chunk_index, size_t& local) const {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    // Binary search over chunk bases. The pool grows by allocating chunks, so a
    // linear scan costs one comparison per chunk and dominated the release path
    // once a pool held a few hundred of them.
    auto position = std::upper_bound(
        chunk_order_.begin(), chunk_order_.end(), address,
        [this](uintptr_t value, size_t index) {
          return value < chunks_[index].base;
        });
    if (position == chunk_order_.begin()) return false;
    const size_t candidate = *(position - 1);
    const Chunk& chunk = chunks_[candidate];
    const uintptr_t offset = address - chunk.base;
    if (offset >= chunk.slot_count * stride_) return false;
    if (offset % stride_ != 0) return false;
    chunk_index = candidate;
    local = static_cast<size_t>(offset / stride_);
    return true;
  }

  void* slot_address(const Chunk& chunk, size_t local) const {
    return reinterpret_cast<void*>(chunk.base + local * stride_);
  }

  void link_new_chunk(size_t chunk_index) {
    Chunk& chunk = chunks_[chunk_index];
    chunk.free_head = kNoSlot;
    for (size_t local = chunk.slot_count; local-- > 0;) {
      void* slot = slot_address(chunk, local);
      *static_cast<size_t*>(slot) = chunk.free_head;
      chunk.free_head = local;
    }
    chunk.free_count = chunk.slot_count;
    chunk.in_free_set = true;
    chunks_with_free_.push_back(chunk_index);
    free_slot_count_ += chunk.slot_count;
  }

  void* allocate_slot() {
    while (!chunks_with_free_.empty()) {
      const size_t chunk_index = chunks_with_free_.back();
      Chunk& chunk = chunks_[chunk_index];
      if (chunk.free_count == 0) {
        chunks_with_free_.pop_back();
        chunk.in_free_set = false;
        continue;
      }
      const size_t local = chunk.free_head;
      void* slot = slot_address(chunk, local);
      chunk.free_head = *static_cast<size_t*>(slot);
      --chunk.free_count;
      --free_slot_count_;
      const uint64_t bit = uint64_t{1} << (local % 64);
      chunk.live_bits[local / 64] |= bit;
      ++chunk.live;
      ++live_total_;
      if (generations_) {
        // Zero means "no allocation", so a wrap skips it. With 2^32 values the
        // wrap is unreachable for any realistic reuse count.
        uint32_t generation = chunk.generations[local] + 1;
        if (generation == 0) generation = 1;
        chunk.generations[local] = generation;
      }
      if (chunk.free_count == 0) {
        chunks_with_free_.pop_back();
        chunk.in_free_set = false;
      }
      return slot;
    }
    if (!add_chunk()) return nullptr;
    return allocate_slot();
  }

  bool release_slot(void* pointer, bool checked, uint64_t expected) {
    if (!pointer) return false;
    size_t chunk_index = 0;
    size_t local = 0;
    if (!locate(pointer, chunk_index, local)) return false;
    Chunk& chunk = chunks_[chunk_index];
    const uint64_t bit = uint64_t{1} << (local % 64);
    // A bit that is already clear means a double release or an address that was
    // never handed out; both are refused without touching the slot.
    if ((chunk.live_bits[local / 64] & bit) == 0) return false;
    if (checked && generations_ &&
        chunk.generations[local] != static_cast<uint32_t>(expected))
      return false;
    chunk.live_bits[local / 64] &= ~bit;
    --chunk.live;
    --live_total_;
    if (destructor_) destructor_(pointer, context_);
    // Poison the payload so a stale read is visible in tests and a debugger, then
    // link the slot into this chunk's free list. The link overwrites the first
    // word, which is why the poison is written first.
    std::memset(pointer, 0xDD, stride_);
    *static_cast<size_t*>(pointer) = chunk.free_head;
    chunk.free_head = local;
    ++chunk.free_count;
    ++free_slot_count_;
    if (!chunk.in_free_set) {
      chunk.in_free_set = true;
      chunks_with_free_.push_back(chunk_index);
    }
    return true;
  }

  bool add_chunk() {
    const size_t slots = slots_per_chunk_;
    if (slots == 0 || slots > (std::numeric_limits<size_t>::max() - 64) / stride_)
      return false;
    const size_t bytes = stride_ * slots;
    const size_t alignment = std::max<size_t>(alignment_, 64);
    if (bytes > std::numeric_limits<size_t>::max() - alignment - 1) return false;
    const size_t total = bytes + alignment - 1;
    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!raw) return false;
    uintptr_t base = 0;
    if (!checked_align_address(reinterpret_cast<uintptr_t>(raw), alignment, base)) {
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    Chunk chunk;
    chunk.raw = raw;
    chunk.base = base;
    chunk.slot_count = slots;
    try {
      chunk.live_bits.assign((slots + 63) / 64, 0);
      if (generations_) chunk.generations.assign(slots, 0);
      chunk.metadata_bytes = sizeof(Chunk) +
                             chunk.live_bits.capacity() * sizeof(uint64_t) +
                             chunk.generations.capacity() * sizeof(uint32_t) +
                             (alignment - 1);
      chunks_.push_back(std::move(chunk));
      // Sorted chunk index, so a release finds its chunk without a scan. Chunk
      // indices stay valid: the vector only ever grows.
      const size_t index = chunks_.size() - 1;
      auto position = std::upper_bound(
          chunk_order_.begin(), chunk_order_.end(), base,
          [this](uintptr_t value, size_t candidate) {
            return value < chunks_[candidate].base;
          });
      chunk_order_.insert(position, index);
    } catch (...) {
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    const size_t index = chunks_.size() - 1;
    link_new_chunk(index);
    committed_ += total;
    cdmm_register_range(this, base, bytes, NRQ_CDMM_STRATEGY_POOL);
    return true;
  }

  void release_chunks() {
    for (const Chunk& chunk : chunks_) {
      cdmm_unregister_range(this, chunk.base);
      HeapFree(GetProcessHeap(), 0, chunk.raw);
    }
    chunks_.clear();
    chunk_order_.clear();
    chunks_with_free_.clear();
    free_slot_count_ = 0;
    live_total_ = 0;
    committed_ = 0;
  }

  size_t slot_bytes_ = 0;
  size_t alignment_ = 8;
  size_t stride_ = 0;
  size_t slots_per_chunk_ = 0;
  bool generations_ = false;
  NrqCdmmDestructor destructor_ = nullptr;
  void* context_ = nullptr;
  size_t live_total_ = 0;
  size_t free_slot_count_ = 0;
  size_t committed_ = 0;
  std::vector<Chunk> chunks_;
  // Chunk indices sorted by base address, and the set of chunks that still have
  // at least one free slot. Both hold indices, not pointers, so growing `chunks_`
  // never invalidates them.
  std::vector<size_t> chunk_order_;
  std::vector<size_t> chunks_with_free_;
};

struct ArenaChunk {
  void* raw = nullptr;
  uintptr_t base = 0;
  size_t size = 0;
  size_t used = 0;
};

class ArenaAllocator : public Allocator {
 public:
  explicit ArenaAllocator(size_t capacity) : Allocator(std::max(capacity, size_t{4096})) {}
  ~ArenaAllocator() override { release_chunks(); }

  void set_max_bytes(size_t max_bytes) { max_bytes_ = max_bytes; }
  // Commits the first chunk at creation, so a created arena either has its
  // initial capacity or does not exist, and `committed` is meaningful before the
  // first allocation.
  bool reserve_chunk() { return add_chunk(capacity_); }
  NrqCdmmStrategy cdmm_strategy() const override { return NRQ_CDMM_STRATEGY_ARENA; }
  uint64_t generation() const { return generation_; }
  uint64_t used_bytes() const {
    size_t total = 0;
    for (const ArenaChunk& chunk : chunks_) total += chunk.used;
    return total;
  }
  uint64_t committed_bytes() const { return committed_; }
  bool contains(const void* pointer) const {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    for (const ArenaChunk& chunk : chunks_) {
      if (address >= chunk.base && address < chunk.base + chunk.size) return true;
    }
    return false;
  }
  void cdmm_describe(const void* object, NrqCdmmFacts& facts) const override {
    (void)object;
    facts.header_bytes = 0;
    facts.generation = generation_;
    facts.metadata_bytes = 0;
  }

  // Allocates `bytes` and registers `destructor` so bulk release destroys the
  // object. The record is allocated inside the arena and is only paid for by
  // objects that actually need deterministic cleanup; the payload itself still
  // has no header.
  void* allocate_destructible(size_t bytes, size_t alignment,
                              NrqCdmmDestructor destructor, void* context) {
    if (!destructor) return allocate(bytes, alignment);
    const Checkpoint checkpoint = mark();
    void* record = allocate(sizeof(DestructorRecord), alignof(std::max_align_t));
    if (!record) return nullptr;
    void* object = allocate(bytes, alignment);
    if (!object) {
      rewind(checkpoint);
      return nullptr;
    }
    DestructorRecord* entry = static_cast<DestructorRecord*>(record);
    entry->previous = destructors_;
    entry->destructor = destructor;
    entry->context = context;
    entry->object = object;
    destructors_ = entry;
    return object;
  }

  void* allocate_destructible_serialized(size_t bytes, size_t alignment,
                                         NrqCdmmDestructor destructor,
                                         void* context) {
    return serialize_operation([&] {
      if (destroying_) return static_cast<void*>(nullptr);
      return allocate_destructible(bytes, alignment, destructor, context);
    });
  }

  void* allocate(size_t bytes, size_t alignment) override {
    if (destroying_) return nullptr;
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    if (chunks_.empty() || !fits(chunks_.back(), bytes, alignment)) {
      if (bytes > std::numeric_limits<size_t>::max() - (alignment - 1))
        return nullptr;
      const size_t request = bytes + alignment - 1;
      if (!add_chunk(std::max(capacity_, request))) return nullptr;
    }
    ArenaChunk& chunk = chunks_.back();
    uintptr_t aligned_address = 0;
    if (!checked_align_address(chunk.base + chunk.used, alignment,
                               aligned_address)) return nullptr;
    const size_t aligned_offset = static_cast<size_t>(aligned_address - chunk.base);
    if (aligned_offset > chunk.size || bytes > chunk.size - aligned_offset)
      return nullptr;
    void* result = reinterpret_cast<void*>(aligned_address);
    chunk.used = aligned_offset + bytes;
    return result;
  }

  void deallocate(void*) override {}

  bool reset() override {
    // Destructors first: they read the objects the chunks still hold. Running
    // them before the chunk walk also keeps a destructor from observing a
    // half-rewound arena.
    run_destructors();
    // Retain only the initial, capacity-bounded chunk. Growth chunks are freed
    // so a reset cannot pin an arbitrarily large peak working set; retaining
    // the first chunk lets repeated reset/reuse avoid a fresh HeapAlloc.
    const bool retain_first =
        !chunks_.empty() && chunks_.front().size <= capacity_;
    const size_t first_to_free = retain_first ? 1 : 0;
    for (size_t index = first_to_free; index < chunks_.size(); ++index) {
      cdmm_unregister_range(this, chunks_[index].base);
      committed_ -= chunks_[index].size;
      HeapFree(GetProcessHeap(), 0, chunks_[index].raw);
    }
    if (retain_first) {
      chunks_.resize(1);
      chunks_.front().used = 0;
    } else {
      chunks_.clear();
    }
    // A stale pointer plus the generation it was allocated under can be rejected
    // by the caller; a reset that cannot advance the generation would make the
    // pair meaningless.
    ++generation_;
    return true;
  }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_ARENA; }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = 0;
    largest_free_block = 0;
    for (const ArenaChunk& chunk : chunks_) {
      if (chunk.used > chunk.size) continue;
      const size_t available = chunk.size - chunk.used;
      free_bytes += available;
      largest_free_block = std::max(largest_free_block, available);
    }
  }

  // Growing the most recent allocation only moves the bump pointer; the start
  // address is unchanged, so the alignment recorded at allocation time still
  // holds. Anything else is copied into a fresh block, and the old bytes stay
  // inside the region — the documented arena behavior.
  void* reallocate(void* pointer, size_t old_bytes, size_t bytes,
                   size_t alignment) override {
    if (!chunks_.empty()) {
      ArenaChunk& chunk = chunks_.back();
      const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
      if (chunk.base <= address && address <= chunk.base + chunk.used &&
          address + old_bytes == chunk.base + chunk.used) {
        const size_t offset = static_cast<size_t>(address - chunk.base);
        if (offset <= chunk.size && bytes <= chunk.size - offset) {
          chunk.used = offset + bytes;
          return pointer;
        }
      }
    }
    return Allocator::reallocate(pointer, old_bytes, bytes, alignment);
  }

 protected:
  void release_chunks() {
    for (const ArenaChunk& chunk : chunks_) {
      cdmm_unregister_range(this, chunk.base);
      HeapFree(GetProcessHeap(), 0, chunk.raw);
    }
    chunks_.clear();
    committed_ = 0;
    // The records lived in the chunks just released.
    destructors_ = nullptr;
  }

  bool fits(const ArenaChunk& chunk, size_t bytes, size_t alignment) const {
    if (chunk.used > chunk.size ||
        chunk.base > std::numeric_limits<uintptr_t>::max() - chunk.used)
      return false;
    uintptr_t aligned_address = 0;
    if (!checked_align_address(chunk.base + chunk.used, alignment,
                               aligned_address) || aligned_address < chunk.base)
      return false;
    const size_t offset = static_cast<size_t>(aligned_address - chunk.base);
    return offset <= chunk.size && bytes <= chunk.size - offset;
  }

  bool add_chunk(size_t bytes) {
    if (bytes > std::numeric_limits<size_t>::max() - 63) return false;
    // A cap is a promise: an allocation that would cross it fails instead of
    // silently committing more storage.
    if (max_bytes_ != 0 &&
        (bytes > max_bytes_ || committed_ > max_bytes_ - bytes))
      return false;
    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + 63);
    if (!raw) return false;
    uintptr_t base = 0;
    if (!checked_align_address(reinterpret_cast<uintptr_t>(raw), 64, base)) {
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    try {
      chunks_.push_back(ArenaChunk{raw, base, bytes, 0});
    } catch (...) {
      HeapFree(GetProcessHeap(), 0, raw);
      return false;
    }
    committed_ += bytes;
    cdmm_register_range(this, base, bytes, cdmm_strategy());
    return true;
  }

 protected:
  struct Checkpoint {
    size_t chunk_count;
    size_t used;
    DestructorRecord* tail;
  };

  Checkpoint mark() const {
    return Checkpoint{chunks_.size(),
                      chunks_.empty() ? size_t{0} : chunks_.back().used,
                      destructors_};
  }

  // Undoes allocations made since `checkpoint`, including any chunk they forced
  // into existence. Used when the payload of a destructible allocation cannot be
  // served after its record already was.
  void rewind(const Checkpoint& checkpoint) {
    while (chunks_.size() > checkpoint.chunk_count) {
      const ArenaChunk& chunk = chunks_.back();
      cdmm_unregister_range(this, chunk.base);
      committed_ -= chunk.size;
      HeapFree(GetProcessHeap(), 0, chunk.raw);
      chunks_.pop_back();
    }
    if (!chunks_.empty()) chunks_.back().used = checkpoint.used;
    destructors_ = checkpoint.tail;
  }

  // Runs last-allocated-first, so an object that owns another arena object is
  // destroyed after the object it owns. Allocation is closed for the duration: a
  // destructor that allocated would push a record behind the one being walked.
  void run_destructors() {
    DestructorRecord* list = destructors_;
    destructors_ = nullptr;
    destroying_ = true;
    while (list) {
      DestructorRecord* previous = list->previous;
      list->destructor(list->object, list->context);
      list = previous;
    }
    destroying_ = false;
  }

  size_t max_bytes_ = 0;
  size_t committed_ = 0;
  uint64_t generation_ = 1;
  bool destroying_ = false;
  DestructorRecord* destructors_ = nullptr;
  std::vector<ArenaChunk> chunks_;
};

class StackAllocator final : public ArenaAllocator {
 public:
  explicit StackAllocator(size_t capacity) : ArenaAllocator(capacity) {}
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_STACK; }
  // A stack frame is not an arena: it cannot be reset in bulk, so it must not
  // answer as one to the explicit-strategy inspection API.
  NrqCdmmStrategy cdmm_strategy() const override { return NRQ_CDMM_STRATEGY_NONE; }
  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    if (chunks_.empty() || !fits(chunks_.back(), bytes, alignment)) {
      if (bytes > std::numeric_limits<size_t>::max() - (alignment - 1))
        return nullptr;
      const size_t request = bytes + alignment - 1;
      if (!add_chunk(std::max(capacity_, request))) return nullptr;
    }
    ArenaChunk& chunk = chunks_.back();
    const size_t previous = chunk.used;
    uintptr_t aligned_address = 0;
    if (!checked_align_address(chunk.base + previous, alignment,
                               aligned_address)) return nullptr;
    const size_t aligned_offset = static_cast<size_t>(aligned_address - chunk.base);
    if (aligned_offset > chunk.size || bytes > chunk.size - aligned_offset)
      return nullptr;
    void* result = reinterpret_cast<void*>(aligned_address);
    try {
      stack_.push_back({result, chunks_.size() - 1, previous});
    } catch (...) {
      return nullptr;
    }
    chunk.used = aligned_offset + bytes;
    return result;
  }
  void deallocate(void* pointer) override {
    if (stack_.empty() || stack_.back().pointer != pointer) return;
    const StackRecord record = stack_.back();
    stack_.pop_back();
    chunks_[record.chunk].used = record.previous_used;
    while (chunks_.size() > record.chunk + 1) {
      HeapFree(GetProcessHeap(), 0, chunks_.back().raw);
      chunks_.pop_back();
    }
  }
  bool can_deallocate(void* pointer) const override {
    return !stack_.empty() && stack_.back().pointer == pointer;
  }

  // Stack allocations form a strict LIFO chain, so only the most recent block
  // can be resized: anything else would either move a block below a live one or
  // leave a hole the strategy cannot describe. Returning null keeps the
  // original allocation valid, which the caller must honor.
  void* reallocate(void* pointer, size_t old_bytes, size_t bytes,
                   size_t alignment) override {
    (void)alignment;  // In-place growth keeps the original start address.
    if (stack_.empty() || stack_.back().pointer != pointer) return nullptr;
    const size_t chunk_index = stack_.back().chunk;
    if (chunk_index + 1 != chunks_.size()) return nullptr;
    ArenaChunk& chunk = chunks_[chunk_index];
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (chunk.base > address) return nullptr;
    const size_t offset = static_cast<size_t>(address - chunk.base);
    if (offset > chunk.size || bytes > chunk.size - offset) return nullptr;
    (void)old_bytes;
    chunk.used = offset + bytes;
    return pointer;
  }
  bool reset() override {
    stack_.clear();
    return ArenaAllocator::reset();
  }

 private:
  struct StackRecord { void* pointer; size_t chunk; size_t previous_used; };
  std::vector<StackRecord> stack_;
};

class BuddyAllocator final : public Allocator {
 public:
  explicit BuddyAllocator(size_t capacity)
      : Allocator(std::max<size_t>(4096, next_power_of_two(capacity))) {
    initialize();
  }
  ~BuddyAllocator() override { if (base_) _aligned_free(base_); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (!base_ || bytes == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 || alignment > capacity_) return nullptr;
    const size_t wanted = next_power_of_two(
        std::max({bytes, alignment, kMinimumBlockBytes}));
    if (wanted == 0) return nullptr;
    int order = order_for(wanted);
    if (order < minimum_order_ || order > maximum_order_) return nullptr;
    int found = order;
    while (found <= maximum_order_ && free_[found].empty()) ++found;
    if (found > maximum_order_) return nullptr;
    size_t offset = *free_[found].begin();
    free_[found].erase(free_[found].begin());
    while (found > order) {
      --found;
      const size_t buddy = offset + block_size(found);
      free_[found].insert(buddy);
    }
    void* result = static_cast<unsigned char*>(base_) + offset;
    live_[result] = order;
    return result;
  }

  void deallocate(void* pointer) override {
    auto found = live_.find(pointer);
    if (found == live_.end()) return;
    int order = found->second;
    size_t offset = static_cast<size_t>(
        static_cast<unsigned char*>(pointer) - static_cast<unsigned char*>(base_));
    live_.erase(found);
    while (order < maximum_order_) {
      const size_t buddy = offset ^ block_size(order);
      auto buddy_it = free_[order].find(buddy);
      if (buddy_it == free_[order].end()) break;
      free_[order].erase(buddy_it);
      offset = std::min(offset, buddy);
      ++order;
    }
    free_[order].insert(offset);
  }

  bool reset() override {
    live_.clear();
    initialize_free_lists();
    return base_ != nullptr;
  }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_BUDDY; }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = 0;
    largest_free_block = 0;
    for (int order = minimum_order_; order <= maximum_order_; ++order) {
      const size_t count = free_[static_cast<size_t>(order)].size();
      if (count == 0) continue;
      free_bytes += count * block_size(order);
      largest_free_block = std::max(largest_free_block, block_size(order));
    }
  }

 private:
  int order_for(size_t bytes) const {
    int order = 0;
    while ((size_t{1} << order) < bytes && order < 63) ++order;
    return order;
  }
  size_t block_size(int order) const { return size_t{1} << order; }
  void initialize() {
    base_ = _aligned_malloc(capacity_, capacity_);
    initialize_free_lists();
  }
  void initialize_free_lists() {
    minimum_order_ = order_for(kMinimumBlockBytes);
    maximum_order_ = order_for(capacity_);
    free_.clear();
    free_.resize(static_cast<size_t>(maximum_order_ + 1));
    if (base_) free_[maximum_order_].insert(0);
  }

  void* base_ = nullptr;
  int minimum_order_ = 0;
  int maximum_order_ = 0;
  std::vector<std::set<size_t>> free_;
  std::unordered_map<void*, int> live_;
};

struct FreeBlock {
  size_t offset = 0;
  size_t size = 0;
  bool free = true;
};

class FreeListAllocator : public Allocator {
 public:
  explicit FreeListAllocator(size_t capacity)
      : Allocator(std::max<size_t>(4096, next_power_of_two(capacity))) {
    reset();
  }
  ~FreeListAllocator() override { if (base_) _aligned_free(base_); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (!base_ || bytes == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 || alignment > capacity_) return nullptr;
    for (auto block = blocks_.begin(); block != blocks_.end(); ++block) {
      if (!block->free) continue;
      size_t aligned = 0;
      if (!checked_align_up(block->offset, alignment, aligned)) continue;
      const size_t prefix = aligned - block->offset;
      if (prefix > block->size || bytes > block->size - prefix) continue;
      const size_t original_end = block->offset + block->size;
      if (prefix != 0) {
        block = blocks_.insert(block, FreeBlock{block->offset, prefix, true});
        ++block;
        block->offset = aligned;
        block->size = original_end - aligned;
      }
      const size_t suffix = block->size - bytes;
      block->size = bytes;
      block->free = false;
      void* pointer = static_cast<unsigned char*>(base_) + aligned;
      live_[pointer] = block;
      if (suffix != 0)
        blocks_.insert(std::next(block), FreeBlock{aligned + bytes, suffix, true});
      return pointer;
    }
    return nullptr;
  }

  void deallocate(void* pointer) override {
    auto found = live_.find(pointer);
    if (found == live_.end()) return;
    auto block = found->second;
    live_.erase(found);
    block->free = true;
    if (block != blocks_.begin()) {
      auto previous = std::prev(block);
      if (previous->free) {
        previous->size += block->size;
        block = blocks_.erase(block);
        block = previous;
      }
    }
    auto next = std::next(block);
    if (next != blocks_.end() && next->free) {
      block->size += next->size;
      blocks_.erase(next);
    }
  }

  bool reset() override {
    live_.clear();
    blocks_.clear();
    if (!base_) base_ = _aligned_malloc(capacity_, capacity_);
    if (!base_) return false;
    blocks_.push_back(FreeBlock{0, capacity_, true});
    return true;
  }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_FREE_LIST; }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = 0;
    largest_free_block = 0;
    for (const FreeBlock& block : blocks_) {
      if (!block.free) continue;
      free_bytes += block.size;
      largest_free_block = std::max(largest_free_block, block.size);
    }
  }

 protected:
  struct BlockList : std::list<FreeBlock> {};
  void* base_ = nullptr;
  std::list<FreeBlock> blocks_;
  std::unordered_map<void*, std::list<FreeBlock>::iterator> live_;
};

class TlsfAllocator final : public Allocator {
 public:
  explicit TlsfAllocator(size_t capacity) : Allocator(capacity) { initialize(); }
  ~TlsfAllocator() override { if (base_) _aligned_free(base_); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (!base_ || bytes == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0 ||
        alignment > capacity_ ||
        bytes > std::numeric_limits<size_t>::max() - (alignment - 1)) return nullptr;
    const size_t required = bytes + alignment - 1;
    auto block = find_block(required);
    if (block == blocks_.end()) return nullptr;
    remove_bin(&*block);
    size_t aligned_offset = 0;
    if (!checked_align_up(block->offset, alignment, aligned_offset)) {
      insert_bin(&*block);
      return nullptr;
    }
    const size_t consumed_end = aligned_offset + bytes;
    const size_t original_end = block->offset + block->size;
    block->size = consumed_end - block->offset;
    block->free = false;
    void* pointer = static_cast<unsigned char*>(base_) + aligned_offset;
    live_[pointer] = block;
    if (consumed_end < original_end) {
      auto remainder = blocks_.insert(std::next(block),
          FreeBlock{consumed_end, original_end - consumed_end, true});
      block_positions_.emplace(&*remainder, remainder);
      insert_bin(&*remainder);
    }
    return pointer;
  }

  void deallocate(void* pointer) override {
    auto found = live_.find(pointer);
    if (found == live_.end()) return;
    auto block = found->second;
    live_.erase(found);
    block->free = true;
    if (block != blocks_.begin()) {
      auto previous = std::prev(block);
      if (previous->free) {
        remove_bin(&*previous);
        previous->size += block->size;
        block_positions_.erase(&*block);
        block = blocks_.erase(block);
        block = previous;
      }
    }
    auto next = std::next(block);
    if (next != blocks_.end() && next->free) {
      remove_bin(&*next);
      block->size += next->size;
      block_positions_.erase(&*next);
      blocks_.erase(next);
    }
    insert_bin(&*block);
  }

  bool reset() override {
    live_.clear();
    blocks_.clear();
    clear_bins();
    free_positions_.clear();
    block_positions_.clear();
    if (!base_) base_ = _aligned_malloc(capacity_, capacity_);
    if (!base_) return false;
    blocks_.push_back(FreeBlock{0, capacity_, true});
    block_positions_.emplace(&blocks_.front(), blocks_.begin());
    insert_bin(&blocks_.front());
    return true;
  }
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_TLSF; }

  void free_statistics(size_t& free_bytes,
                       size_t& largest_free_block) const override {
    free_bytes = 0;
    largest_free_block = 0;
    for (const FreeBlock& block : blocks_) {
      if (!block.free) continue;
      free_bytes += block.size;
      largest_free_block = std::max(largest_free_block, block.size);
    }
  }

 private:
  static constexpr size_t kSecondLevels = 8;
  static constexpr size_t kFirstLevels = 64;
  using BinList = std::list<FreeBlock*>;
  struct Bin { BinList blocks; };
  using Iterator = std::list<FreeBlock>::iterator;

  std::pair<size_t, size_t> mapping(size_t size) const {
    size_t first = 0;
    size_t value = size;
    while (value >>= 1) ++first;
    if (first >= kFirstLevels) first = kFirstLevels - 1;
    const size_t base = size_t{1} << first;
    const size_t unit = std::max<size_t>(1, base / kSecondLevels);
    const size_t second = std::min(kSecondLevels - 1, (size - base) / unit);
    return {first, second};
  }

  Iterator find_block(size_t required) {
    auto start = mapping(required);
    const size_t base = size_t{1} << start.first;
    const size_t unit = std::max<size_t>(1, base / kSecondLevels);
    const size_t bucket_lower_bound = base + start.second * unit;
    if (required > bucket_lower_bound && ++start.second == kSecondLevels) {
      start.second = 0;
      ++start.first;
    }
    for (size_t first = start.first; first < kFirstLevels; ++first) {
      const size_t first_second = first == start.first ? start.second : 0;
      for (size_t second = first_second; second < kSecondLevels; ++second) {
        const auto& bin = bins_[first * kSecondLevels + second].blocks;
        if (bin.empty()) continue;
        auto position = block_positions_.find(bin.front());
        if (position != block_positions_.end()) return position->second;
      }
    }
    return blocks_.end();
  }

  void insert_bin(FreeBlock* block) {
    const auto [first, second] = mapping(block->size);
    auto& bin = bins_[first * kSecondLevels + second].blocks;
    auto position = bin.insert(bin.end(), block);
    free_positions_.emplace(block, position);
  }

  void remove_bin(FreeBlock* block) {
    auto position = free_positions_.find(block);
    if (position == free_positions_.end()) return;
    const auto [first, second] = mapping(block->size);
    bins_[first * kSecondLevels + second].blocks.erase(position->second);
    free_positions_.erase(position);
  }

  void clear_bins() {
    for (Bin& bin : bins_) bin.blocks.clear();
  }

  void initialize() {
    capacity_ = std::max<size_t>(4096, next_power_of_two(capacity_));
    reset();
  }

  void* base_ = nullptr;
  std::list<FreeBlock> blocks_;
  std::array<Bin, kFirstLevels * kSecondLevels> bins_;
  std::unordered_map<void*, Iterator> live_;
  std::unordered_map<FreeBlock*, Iterator> block_positions_;
  std::unordered_map<FreeBlock*, BinList::iterator> free_positions_;
};

struct AllocationOwner {
  Allocator* allocator;
  size_t bytes;
  size_t alignment;
  bool operation_in_flight = false;
  bool shared = false;
};

// The explicit-allocation registry is split by pointer. Every allocation and
// release touches one shard, so independent threads no longer serialize on a
// single map mutex. Shards are always locked in index order when a diagnostic
// or allocator lifetime operation needs a consistent view of the whole
// registry. No path holds a shard while acquiring `allocator_mutex`.
constexpr size_t kAllocationShardCount = 8;

// Open-addressed ownership table. The registry used to allocate an
// unordered_map node for every live pointer and free it again on release, so a
// normal short-lived allocation paid the general heap allocator twice just to
// describe itself. The table keeps ownership records inline and uses tombstones
// for erased entries; it grows when live records exceed 70% of capacity and
// compacts back down when a mostly empty table would otherwise keep probing a
// large tombstone field.
class AllocationTable {
 public:
  struct Entry {
    void* key = nullptr;
    AllocationOwner owner{};
  };

  Entry* find(void* key) {
    if (capacity_ == 0) return nullptr;
    size_t index = hash(key);
    for (size_t probes = 0; probes < capacity_; ++probes) {
      Entry& entry = entries_[index];
      if (!entry.key) return nullptr;
      if (entry.key == key) return &entry;
      index = (index + 1) & (capacity_ - 1);
    }
    return nullptr;
  }

  bool insert(void* key, const AllocationOwner& owner) {
    if (capacity_ != 0 && tombstones_ > capacity_ / 2 &&
        size_ * 4 < capacity_) {
      size_t target = 16;
      while ((size_ + 1) * 10 > target * 7) target *= 2;
      rehash(target);
    }
    if (capacity_ == 0 || (size_ + 1) * 10 > capacity_ * 7)
      grow();

    size_t index = hash(key);
    size_t reusable = capacity_;
    for (size_t probes = 0; probes < capacity_; ++probes) {
      Entry& entry = entries_[index];
      if (!entry.key) {
        if (reusable == capacity_) reusable = index;
        entries_[reusable] = Entry{key, owner};
        ++size_;
        return true;
      }
      if (entry.key == tombstone()) {
        if (reusable == capacity_) reusable = index;
      } else if (entry.key == key) {
        return false;
      }
      index = (index + 1) & (capacity_ - 1);
    }
    if (reusable != capacity_) {
      entries_[reusable] = Entry{key, owner};
      ++size_;
      return true;
    }
    // A table below the load threshold always has a reusable slot; growing once
    // more also retires every tombstone, so this path is safe and bounded.
    grow();
    return insert(key, owner);
  }

  bool erase(void* key) {
    Entry* entry = find(key);
    if (!entry) return false;
    entry->key = tombstone();
    entry->owner = AllocationOwner{};
    --size_;
    ++tombstones_;
    return true;
  }

  template <typename Visitor>
  void for_each(Visitor&& visitor) const {
    for (const Entry& entry : entries_) {
      if (entry.key && entry.key != tombstone()) visitor(entry);
    }
  }

 private:
  static void* tombstone() {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(1));
  }

  size_t hash(void* key) const {
    uintptr_t value = reinterpret_cast<uintptr_t>(key) >> 4;
    value ^= value >> 16;
    return static_cast<size_t>(value) & (capacity_ - 1);
  }

  void place(std::vector<Entry>& entries, size_t capacity, void* key,
             const AllocationOwner& owner) {
    uintptr_t value = reinterpret_cast<uintptr_t>(key) >> 4;
    value ^= value >> 16;
    size_t index = static_cast<size_t>(value) & (capacity - 1);
    while (entries[index].key) index = (index + 1) & (capacity - 1);
    entries[index] = Entry{key, owner};
  }

  void grow() {
    const size_t capacity = capacity_ == 0 ? 16 : capacity_ * 2;
    rehash(capacity);
  }

  void rehash(size_t capacity) {
    std::vector<Entry> replacement(capacity);
    for (const Entry& entry : entries_) {
      if (entry.key && entry.key != tombstone())
        place(replacement, capacity, entry.key, entry.owner);
    }
    entries_.swap(replacement);
    capacity_ = capacity;
    tombstones_ = 0;
  }

  std::vector<Entry> entries_;
  size_t capacity_ = 0;
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

struct alignas(128) AllocationShard {
  std::mutex mutex;
  std::condition_variable changed;
  AllocationTable allocations;
};

std::array<AllocationShard, kAllocationShardCount> allocation_shards;

AllocationShard& shard_for(const void* pointer) {
  // Every allocator returns at least 16-byte-aligned storage, so the low four
  // bits carry no information. Shifting them out gives a cheap spread without
  // adding a multiply to every allocation and release.
  const uintptr_t value = reinterpret_cast<uintptr_t>(pointer) >> 4;
  return allocation_shards[value & (kAllocationShardCount - 1)];
}

template <typename Visitor>
void visit_all_allocations_locked(Visitor&& visitor) {
  std::array<std::unique_lock<std::mutex>, kAllocationShardCount> locks;
  for (size_t index = 0; index < kAllocationShardCount; ++index)
    locks[index] = std::unique_lock<std::mutex>(allocation_shards[index].mutex);
  for (const AllocationShard& shard : allocation_shards)
    shard.allocations.for_each(visitor);
}

// Removes every registry record owned by `allocator` after checking that no
// allocation operation is in flight. The caller must hold `allocator_mutex`.
// Bulk reset/destroy deliberately invalidates the region's storage; object-level
// cleanup is a compiler/language obligation, not something this path can infer.
bool erase_arena_allocations_locked(Allocator* allocator) {
  std::array<std::unique_lock<std::mutex>, kAllocationShardCount> locks;
  for (size_t index = 0; index < kAllocationShardCount; ++index)
    locks[index] = std::unique_lock<std::mutex>(allocation_shards[index].mutex);
  bool in_flight = false;
  for (const AllocationShard& shard : allocation_shards) {
    shard.allocations.for_each([&](const auto& entry) {
      if (entry.owner.allocator == allocator &&
          entry.owner.operation_in_flight)
        in_flight = true;
    });
  }
  if (in_flight) return false;
  for (AllocationShard& shard : allocation_shards) {
    std::vector<void*> released;
    shard.allocations.for_each([&](const auto& entry) {
      if (entry.owner.allocator == allocator) released.push_back(entry.key);
    });
    for (void* pointer : released) shard.allocations.erase(pointer);
    if (!released.empty()) shard.changed.notify_all();
  }
  return true;
}

std::mutex allocator_mutex;
std::unordered_map<void*, std::unique_ptr<Allocator>> allocator_handles;
std::unordered_map<Allocator*, size_t> selected_counts;
// Shared ownership uses one control per live pointer and pointer-selected
// shards. A retain or non-final release touches one shard lock and one atomic
// count; it no longer serializes with unrelated allocator traffic. The shard
// lock protects map/control lifetime, while the atomic count provides the
// overflow-safe 1->0 transition.
constexpr size_t kSharedShardCount = 64;
struct SharedControl {
  std::atomic<uint64_t> strong{1};
};
struct SharedShard {
  std::mutex mutex;
  std::unordered_map<void*, std::unique_ptr<SharedControl>> controls;
};
std::array<SharedShard, kSharedShardCount> shared_shards;

SharedShard& shared_shard_for(void* pointer) {
  const size_t index = std::hash<void*>{}(pointer) % kSharedShardCount;
  return shared_shards[index];
}

// Shared allocations whose explicit storage contains managed references need a
// root for as long as the last reference token exists. The runtime records the
// descriptor at allocation so final release can unregister it before freeing.
struct SharedRootMetadata {
  // Aggregate field layouts are usually short. Keep the common case inline so a
  // shared allocation with managed fields does not also allocate a separate
  // offsets vector; deeper layouts retain the vector fallback.
  static constexpr size_t kInlineOffsets = 8;
  size_t object_size = 0;
  std::array<size_t, kInlineOffsets> inline_offsets{};
  size_t inline_count = 0;
  std::vector<size_t> overflow;

  void assign(const size_t* values, size_t count) {
    if (count <= kInlineOffsets) {
      std::copy(values, values + count, inline_offsets.begin());
      inline_count = count;
      overflow.clear();
      return;
    }
    overflow.assign(values, values + count);
    inline_count = 0;
  }

  const size_t* data() const {
    return inline_count != 0 ? inline_offsets.data() : overflow.data();
  }

  size_t count() const {
    return inline_count != 0 ? inline_count : overflow.size();
  }
};
std::unordered_map<void*, SharedRootMetadata> shared_roots;
std::mutex shared_roots_mutex;
std::mutex memory_profile_mutex;
NrqMemoryProfile memory_profile{};
std::atomic<bool> memory_profiler_enabled{false};
// The generated code loads the whole report as an aggregate, so every field must
// stay a 64-bit integer and the order in `allocator.h` must match the field list
// the type checker publishes for `memory.profiler.capture()`.
static_assert(sizeof(NrqMemoryProfile) == 28 * sizeof(uint64_t),
              "NrqMemoryProfile must be 28 consecutive 64-bit fields");
GeneralAllocator default_allocator;
// Process-wide default for explicit allocations. It is applied when the calling
// thread has not selected an allocator, so a library can install a policy
// without touching every call site.
Allocator* policy_default_allocator = nullptr;
struct ThreadSelection {
  Allocator* allocator = nullptr;
  ~ThreadSelection() {
    if (!allocator) return;
    std::lock_guard<std::mutex> lock(allocator_mutex);
    auto found = selected_counts.find(allocator);
    if (found != selected_counts.end() && --found->second == 0)
      selected_counts.erase(found);
  }
};
thread_local ThreadSelection thread_selection;

// Small default-allocator blocks are recycled per thread. The general
// allocator is just the process heap, so a freed block has no allocator state
// to reset. Keeping a short thread-local free list avoids a HeapAlloc /
// HeapFree pair for the common short-lived allocation pattern without
// weakening the global ownership registry.
constexpr size_t kDefaultCacheMaxBytes = 1024;
constexpr size_t kDefaultCacheCapacity = 64;

class ThreadDefaultAllocationCache {
 public:
  ~ThreadDefaultAllocationCache() {
    for (size_t index = 0; index < size_; ++index)
      heap_free_pointer(blocks_[index].pointer, blocks_[index].alignment);
  }

  bool can_store(size_t bytes, size_t alignment) const {
    return size_ < kDefaultCacheCapacity && bytes != 0 &&
           bytes <= kDefaultCacheMaxBytes && alignment != 0;
  }

  void store(void* pointer, size_t bytes, size_t alignment) {
    blocks_[size_++] = CachedBlock{pointer, bytes, alignment};
  }

  void* take(size_t bytes, size_t alignment) {
    for (size_t index = size_; index > 0; --index) {
      CachedBlock& block = blocks_[index - 1];
      if (block.bytes != bytes || block.alignment != alignment) continue;
      void* pointer = block.pointer;
      blocks_[index - 1] = blocks_[--size_];
      return pointer;
    }
    return nullptr;
  }

 private:
  struct CachedBlock {
    void* pointer;
    size_t bytes;
    size_t alignment;
  };
  CachedBlock blocks_[kDefaultCacheCapacity]{};
  size_t size_ = 0;
};

thread_local ThreadDefaultAllocationCache thread_default_allocation_cache;
std::atomic<bool> default_general_cache_enabled{true};

Allocator* active_allocator() {
  if (thread_selection.allocator) return thread_selection.allocator;
  return policy_default_allocator ? policy_default_allocator : &default_allocator;
}

// Resolves an opaque handle to its allocator. Handle zero names the configured
// default, which is what `memory.allocate<T>(bytes, 0)` selects.
Allocator* resolve_allocator(uint64_t handle) {
  if (handle == 0)
    return policy_default_allocator ? policy_default_allocator : &default_allocator;
  auto found = allocator_handles.find(
      reinterpret_cast<void*>(static_cast<uintptr_t>(handle)));
  return found == allocator_handles.end() ? nullptr : found->second.get();
}

// Records the profiler counters shared by every successful allocation request.
// The size histogram bins are fixed so the report layout stays stable.
void record_allocation_profile(size_t bytes) {
  if (!memory_profiler_enabled.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(memory_profile_mutex);
  if (!memory_profiler_enabled.load(std::memory_order_relaxed)) return;
  memory_profile.current_bytes += bytes;
  memory_profile.total_bytes += bytes;
  memory_profile.peak_bytes = std::max(memory_profile.peak_bytes,
                                       memory_profile.current_bytes);
  ++memory_profile.allocations;
  if (bytes <= 64) ++memory_profile.allocations_le_64;
  else if (bytes <= 256) ++memory_profile.allocations_le_256;
  else if (bytes <= 1024) ++memory_profile.allocations_le_1024;
  else if (bytes <= 4096) ++memory_profile.allocations_le_4096;
  else if (bytes <= 65536) ++memory_profile.allocations_le_65536;
  else ++memory_profile.allocations_gt_65536;
  memory_profile.largest_allocation =
      std::max(memory_profile.largest_allocation, static_cast<uint64_t>(bytes));
}

void record_deallocation_profile(size_t bytes) {
  if (!memory_profiler_enabled.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(memory_profile_mutex);
  if (!memory_profiler_enabled.load(std::memory_order_relaxed)) return;
  memory_profile.current_bytes -= bytes;
  ++memory_profile.deallocations;
}

std::unique_ptr<Allocator> make_allocator(NrqAllocatorKind kind, size_t capacity) {
  switch (kind) {
    case NRQ_ALLOCATOR_GENERAL: return std::make_unique<GeneralAllocator>();
    case NRQ_ALLOCATOR_POOL: return std::make_unique<FixedPoolAllocator>(capacity, false);
    case NRQ_ALLOCATOR_ARENA: return std::make_unique<ArenaAllocator>(capacity);
    case NRQ_ALLOCATOR_STACK: return std::make_unique<StackAllocator>(capacity);
    case NRQ_ALLOCATOR_SLAB: return std::make_unique<FixedPoolAllocator>(capacity, true);
    case NRQ_ALLOCATOR_BUDDY:
      if (next_power_of_two(capacity) == 0) return nullptr;
      return std::make_unique<BuddyAllocator>(capacity);
    case NRQ_ALLOCATOR_FREE_LIST:
      if (next_power_of_two(capacity) == 0) return nullptr;
      return std::make_unique<FreeListAllocator>(capacity);
    case NRQ_ALLOCATOR_TLSF:
      if (next_power_of_two(capacity) == 0) return nullptr;
      return std::make_unique<TlsfAllocator>(capacity);
    case NRQ_ALLOCATOR_CUSTOM: return nullptr;
  }
  return nullptr;
}

// --- CDMM range table --------------------------------------------------------

void cdmm_register_range(const void* owner, uintptr_t begin, size_t bytes,
                         NrqCdmmStrategy strategy) {
  if (strategy == NRQ_CDMM_STRATEGY_NONE || bytes == 0) return;
  CdmmRange range;
  range.begin = begin;
  range.end = begin + bytes;
  range.owner = owner;
  range.strategy = strategy;
  std::lock_guard<std::mutex> lock(cdmm_range_mutex);
  auto position = std::lower_bound(
      cdmm_ranges.begin(), cdmm_ranges.end(), range.begin,
      [](const CdmmRange& entry, uintptr_t value) { return entry.begin < value; });
  cdmm_ranges.insert(position, range);
}

void cdmm_unregister_range(const void* owner, uintptr_t begin) {
  std::lock_guard<std::mutex> lock(cdmm_range_mutex);
  auto position = std::lower_bound(
      cdmm_ranges.begin(), cdmm_ranges.end(), begin,
      [](const CdmmRange& entry, uintptr_t value) { return entry.begin < value; });
  while (position != cdmm_ranges.end() && position->begin == begin) {
    if (position->owner == owner) {
      cdmm_ranges.erase(position);
      return;
    }
    ++position;
  }
}

bool cdmm_lookup_range(uintptr_t address, CdmmRange& result) {
  std::lock_guard<std::mutex> lock(cdmm_range_mutex);
  auto position = std::upper_bound(
      cdmm_ranges.begin(), cdmm_ranges.end(), address,
      [](uintptr_t value, const CdmmRange& entry) { return value < entry.begin; });
  if (position == cdmm_ranges.begin()) return false;
  --position;
  if (address >= position->end) return false;
  result = *position;
  return true;
}

// --- CDMM handle table -------------------------------------------------------
//
// Explicit strategies live here rather than in `allocator_handles` so the legacy
// allocator entry points cannot reset or destroy them behind the CDMM paths' back
// and the CDMM fast path never takes the global explicit-allocation lock. A
// shared lock covers lookup plus pinning; create and destroy take it uniquely.

std::shared_mutex cdmm_handle_mutex;
std::unordered_map<void*, std::unique_ptr<Allocator>> cdmm_handles;

Allocator* pin_cdmm_handle_locked(void* handle) {
  if (!handle) return nullptr;
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return nullptr;
  return found->second->try_pin_operation() ? found->second.get() : nullptr;
}

}  // namespace

extern "C" {

void* nrq_allocator_create(NrqAllocatorKind kind, size_t capacity) {
  try {
    auto allocator = make_allocator(kind, capacity == 0 ? kDefaultRegionBytes : capacity);
    if (!allocator) return nullptr;
    void* handle = allocator.get();
    std::lock_guard<std::mutex> lock(allocator_mutex);
    allocator_handles.emplace(handle, std::move(allocator));
    return handle;
  } catch (...) {
    return nullptr;
  }
}

void* nrq_allocator_create_custom(const NrqCustomAllocatorVTable* callbacks) {
  if (!callbacks || !callbacks->allocate || !callbacks->deallocate) return nullptr;
  try {
    auto allocator = std::make_unique<CustomAllocator>(*callbacks);
    void* handle = allocator.get();
    std::lock_guard<std::mutex> lock(allocator_mutex);
    auto [entry, inserted] = allocator_handles.emplace(handle, std::move(allocator));
    (void)entry;
    return inserted ? handle : nullptr;
  } catch (...) {
    return nullptr;
  }
}

int32_t nrq_allocator_destroy(void* handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  // An allocator that is still selected, still owns live allocations, or serves
  // as the process-wide default cannot be destroyed without invalidating
  // pointers the program may still reach.
  if (policy_default_allocator == allocator) return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  auto selected = selected_counts.find(allocator);
  if (selected != selected_counts.end() && selected->second != 0) return 0;
  bool owns_live_allocation = false;
  visit_all_allocations_locked([&](const auto& allocation) {
    if (allocation.owner.allocator == allocator) owns_live_allocation = true;
  });
  if (owns_live_allocation) return 0;
  allocator_handles.erase(found);
  return 1;
}

int32_t nrq_allocator_reset(void* handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->in_flight_operations() != 0) return 0;
  bool owns_live_allocation = false;
  visit_all_allocations_locked([&](const auto& allocation) {
    if (allocation.owner.allocator == allocator) owns_live_allocation = true;
  });
  if (owns_live_allocation) return 0;
  if (!allocator->reset_serialized()) return 0;
  return 1;
}

int32_t nrq_arena_reset(void* handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA &&
      allocator->kind() != NRQ_ALLOCATOR_POOL)
    return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  if (!erase_arena_allocations_locked(allocator)) return 0;
  return allocator->reset_serialized() ? 1 : 0;
}

int32_t nrq_arena_destroy(void* handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA &&
      allocator->kind() != NRQ_ALLOCATOR_POOL)
    return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  if (!erase_arena_allocations_locked(allocator)) return 0;
  allocator->reset_serialized();
  allocator_handles.erase(found);
  return 1;
}

int32_t nrq_memory_allocator_kind(uint64_t allocator_handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (allocator_handle == 0)
    return policy_default_allocator ? policy_default_allocator->kind()
                                    : NRQ_ALLOCATOR_GENERAL;
  void* handle = reinterpret_cast<void*>(static_cast<uintptr_t>(allocator_handle));
  auto found = allocator_handles.find(handle);
  return found == allocator_handles.end() ? -1 : found->second->kind();
}

void* nrq_memory_set_default_allocator(void* handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (handle && allocator_handles.find(handle) == allocator_handles.end())
    return nullptr;
  Allocator* previous = policy_default_allocator;
  policy_default_allocator = handle ? allocator_handles[handle].get() : nullptr;
  default_general_cache_enabled.store(policy_default_allocator == nullptr,
                                      std::memory_order_release);
  return previous;
}

void* nrq_memory_get_default_allocator(void) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  return policy_default_allocator;
}

void* nrq_memory_get_allocator(void) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  return thread_selection.allocator;
}

void* nrq_memory_use_allocator(void* handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (handle && allocator_handles.find(handle) == allocator_handles.end()) return nullptr;
  Allocator* previous = thread_selection.allocator;
  Allocator* next = handle ? allocator_handles[handle].get() : nullptr;
  if (next == previous) return previous;
  try {
    if (next && next != previous) ++selected_counts[next];
  } catch (...) {
    return nullptr;
  }
  if (previous) {
    auto count = selected_counts.find(previous);
    if (count != selected_counts.end() && --count->second == 0)
      selected_counts.erase(count);
  }
  thread_selection.allocator = next;
  return previous;
}

static void release_operation_pin_locked(Allocator* allocator) {
  allocator->unpin_operation();
}

static void release_operation_pin(Allocator* allocator) {
  release_operation_pin_locked(allocator);
}

static void* allocate_with_allocator(Allocator* allocator, size_t bytes,
                                     size_t alignment, bool pinned = true) {
  void* pointer = nullptr;
  try {
    pointer = allocator->allocate_serialized(bytes, alignment);
  } catch (...) {
    if (pinned) release_operation_pin(allocator);
    return nullptr;
  }
  if (!pointer) {
    if (pinned) release_operation_pin(allocator);
    return nullptr;
  }
  AllocationShard& shard = shard_for(pointer);
  try {
    std::unique_lock<std::mutex> lock(shard.mutex);
    auto* existing = shard.allocations.find(pointer);
    while (existing && existing->owner.operation_in_flight) {
      shard.changed.wait(lock);
      existing = shard.allocations.find(pointer);
    }
    if (!shard.allocations.insert(
            pointer, AllocationOwner{allocator, bytes, alignment}))
      nrq_runtime_fatal("allocator returned an address that is already live");
    record_allocation_profile(bytes);
  } catch (...) {
    allocator->deallocate_serialized(pointer, alignment);
    if (pinned) release_operation_pin(allocator);
    return nullptr;
  }
  if (pinned) release_operation_pin(allocator);
  return pointer;
}

static bool increment_operation_pin_locked(Allocator* allocator) {
  return allocator && allocator->try_pin_operation();
}

void* nrq_memory_allocate(size_t bytes, size_t alignment) {
  // The cache is only valid for the process default general allocator. A
  // thread-selected allocator and a process default override must both keep
  // their normal registry and pinning path. Read the thread selection and the
  // cache switch once: both are stable for the duration of this call, and the
  // previous fallthrough re-read them after every cache miss.
  const bool default_path =
      !thread_selection.allocator &&
      default_general_cache_enabled.load(std::memory_order_acquire);
  if (default_path) {
    if (void* cached =
            thread_default_allocation_cache.take(bytes, alignment)) {
      AllocationShard& shard = shard_for(cached);
      try {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (!shard.allocations.insert(
                cached, AllocationOwner{&default_allocator, bytes, alignment}))
          nrq_runtime_fatal("allocator cache returned an already-live address");
        record_allocation_profile(bytes);
        return cached;
      } catch (...) {
        heap_free_pointer(cached, alignment);
        return nullptr;
      }
    }
    // The process-default general allocator has static lifetime, so a normal
    // allocation needs neither the registry mutex nor an operation pin. Large
    // blocks bypass the cache but still take this lock-free path.
    return allocate_with_allocator(&default_allocator, bytes, alignment, false);
  }

  Allocator* allocator = nullptr;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    allocator = active_allocator();
    if (!increment_operation_pin_locked(allocator)) return nullptr;
  }
  return allocate_with_allocator(allocator, bytes, alignment);
}

void* nrq_memory_allocate_array(size_t count, size_t element_size,
                                size_t alignment) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  return nrq_memory_allocate(count * element_size, alignment);
}

void* nrq_memory_allocate_array_from(void* allocator, size_t count,
                                     size_t element_size, size_t alignment) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  return nrq_memory_allocate_from(allocator, count * element_size, alignment);
}

void nrq_memory_array_bounds_check(uint64_t index, uint64_t length) {
  if (index >= length)
    nrq_runtime_fatal("owned array index is outside the array bounds");
}

[[noreturn]] void nrq_memory_array_bounds_fail(void) {
  nrq_runtime_fatal("owned array index is outside the array bounds");
}

void nrq_memory_array_range_check(uint64_t start, uint64_t count, uint64_t length) {
  if (start > length || count > length - start)
    nrq_runtime_fatal("array view range is outside the source bounds");
}

void* nrq_memory_shared_allocate(size_t bytes, size_t alignment) {
  void* pointer = nrq_memory_allocate(bytes, alignment);
  if (!pointer) return nullptr;
  SharedShard& shared_shard = shared_shard_for(pointer);
  try {
    {
      std::lock_guard<std::mutex> lock(shared_shard.mutex);
      auto inserted = shared_shard.controls.emplace(
          pointer, std::make_unique<SharedControl>());
      if (!inserted.second)
        nrq_runtime_fatal("shared allocation already has a control block");
    }
    AllocationShard& shard = shard_for(pointer);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto* owner = shard.allocations.find(pointer);
    if (!owner) {
      std::lock_guard<std::mutex> control_lock(shared_shard.mutex);
      shared_shard.controls.erase(pointer);
      nrq_runtime_fatal("shared allocation disappeared from the registry");
    }
    owner->owner.shared = true;
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(shared_shard.mutex);
      shared_shard.controls.erase(pointer);
    }
    nrq_memory_deallocate_checked(pointer);
    return nullptr;
  }
  return pointer;
}

static bool increment_operation_pin_locked(Allocator* allocator);
static void release_operation_pin_locked(Allocator* allocator);

void* nrq_memory_shared_retain(void* pointer) {
  if (!pointer) return nullptr;
  SharedShard& shared_shard = shared_shard_for(pointer);
  std::lock_guard<std::mutex> lock(shared_shard.mutex);
  auto found = shared_shard.controls.find(pointer);
  if (found == shared_shard.controls.end()) return nullptr;
  SharedControl& control = *found->second;
  uint64_t current = control.strong.load(std::memory_order_relaxed);
  while (current != 0 && current != UINT64_MAX) {
    // The shard lock makes the control live, but the CAS still gives overflow
    // an explicit, race-free contract if the count is ever observed directly.
    if (control.strong.compare_exchange_weak(
            current, current + 1, std::memory_order_acq_rel,
            std::memory_order_relaxed))
      return pointer;
  }
  return nullptr;
}

int32_t nrq_memory_shared_release(void* pointer) {
  if (!pointer) return 0;
  SharedShard& shared_shard = shared_shard_for(pointer);
  std::unique_ptr<SharedControl> final_control;
  {
    std::lock_guard<std::mutex> lock(shared_shard.mutex);
    auto found = shared_shard.controls.find(pointer);
    if (found == shared_shard.controls.end()) return 0;
    SharedControl& control = *found->second;
    const uint64_t current = control.strong.load(std::memory_order_relaxed);
    if (current == 0) return 0;
    if (current > 1) {
      control.strong.fetch_sub(1, std::memory_order_acq_rel);
      return 1;
    }
    // Remove the control before deallocation. A concurrent retain then sees a
    // missing entry instead of resurrecting storage whose final release has
    // already begun.
    final_control = std::move(found->second);
    shared_shard.controls.erase(found);
    final_control->strong.store(0, std::memory_order_release);
  }

  SharedRootMetadata root_metadata;
  bool rooted = false;
  {
    std::lock_guard<std::mutex> lock(shared_roots_mutex);
    auto root = shared_roots.find(pointer);
    if (root != shared_roots.end()) {
      root_metadata = std::move(root->second);
      shared_roots.erase(root);
      rooted = true;
    }
  }

  AllocationShard& shard = shard_for(pointer);
  Allocator* allocator = nullptr;
  size_t alignment = 0;
  {
    std::lock_guard<std::mutex> shard_lock(shard.mutex);
    auto* owner = shard.allocations.find(pointer);
    if (!owner || owner->owner.operation_in_flight) {
      // A malformed call can reach a pointer whose allocator record is gone.
      // Restore the reference token rather than freeing or corrupting it.
      if (rooted) {
        nrq_gc_root_register_typed(pointer, root_metadata.object_size,
                                   root_metadata.data(),
                                   root_metadata.count());
        std::lock_guard<std::mutex> root_lock(shared_roots_mutex);
        shared_roots.emplace(pointer, std::move(root_metadata));
      }
      final_control->strong.store(1, std::memory_order_release);
      std::lock_guard<std::mutex> control_lock(shared_shard.mutex);
      shared_shard.controls.emplace(pointer, std::move(final_control));
      return 0;
    }
    allocator = owner->owner.allocator;
    alignment = owner->owner.alignment;
    // The live ownership record keeps the allocator alive until the callback
    // and record erase complete; destroy and reset scan that record and refuse
    // while it is in flight.
    owner->owner.operation_in_flight = true;
  }

  // Remove the collector-visible root before the storage can be freed. The
  // control is already absent, so no concurrent retain can resurrect it.
  if (rooted)
    nrq_gc_root_unregister(reinterpret_cast<void**>(pointer));

  bool released = false;
  bool callback_failed = false;
  try {
    released = allocator->try_deallocate_serialized(pointer, alignment);
  } catch (...) {
    callback_failed = true;
  }

  {
    std::lock_guard<std::mutex> shard_lock(shard.mutex);
    auto* owner = shard.allocations.find(pointer);
    if (!owner || !owner->owner.operation_in_flight)
      nrq_runtime_fatal("allocator ownership changed during shared release");
    if (released) {
      record_deallocation_profile(owner->owner.bytes);
      shard.allocations.erase(pointer);
      shard.changed.notify_all();
    } else {
      owner->owner.operation_in_flight = false;
      shard.changed.notify_all();
    }
  }
  if (!released && rooted) {
    // The allocator rejected the release (a stack allocator requires LIFO), so
    // the object is still live. Restore its root and token before reporting
    // the failure.
    nrq_gc_root_register_typed(pointer, root_metadata.object_size,
                               root_metadata.data(),
                               root_metadata.count());
    std::lock_guard<std::mutex> root_lock(shared_roots_mutex);
    shared_roots.emplace(pointer, std::move(root_metadata));
  }
  if (!released) {
    final_control->strong.store(1, std::memory_order_release);
    std::lock_guard<std::mutex> control_lock(shared_shard.mutex);
    shared_shard.controls.emplace(pointer, std::move(final_control));
  }
  if (callback_failed)
    nrq_runtime_fatal("allocator deallocation callback failed during shared release");
  if (!released)
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
  return 1;
}

void* nrq_memory_shared_allocate_rooted(size_t bytes, size_t alignment,
                                        size_t object_size,
                                        const size_t* reference_offsets,
                                        size_t reference_count) {
  if (object_size == 0 || reference_count == 0 || !reference_offsets)
    return nullptr;
  for (size_t index = 0; index < reference_count; ++index) {
    if (reference_offsets[index] > object_size ||
        sizeof(void*) > object_size - reference_offsets[index])
      return nullptr;
  }
  void* pointer = nrq_memory_shared_allocate(bytes, alignment);
  if (!pointer) return nullptr;
  SharedRootMetadata metadata;
  metadata.object_size = object_size;
  try {
    metadata.assign(reference_offsets, reference_count);
    nrq_gc_root_register_typed(pointer, object_size, metadata.data(),
                               metadata.count());
    std::lock_guard<std::mutex> lock(shared_roots_mutex);
    shared_roots.emplace(pointer, std::move(metadata));
  } catch (...) {
    nrq_gc_root_unregister(reinterpret_cast<void**>(pointer));
    nrq_memory_shared_release(pointer);
    return nullptr;
  }
  return pointer;
}

uint64_t nrq_memory_shared_count(void* pointer) {
  if (!pointer) return 0;
  SharedShard& shared_shard = shared_shard_for(pointer);
  std::lock_guard<std::mutex> lock(shared_shard.mutex);
  auto found = shared_shard.controls.find(pointer);
  return found == shared_shard.controls.end()
             ? 0
             : found->second->strong.load(std::memory_order_acquire);
}

void* nrq_memory_allocate_from(void* handle, size_t bytes, size_t alignment) {
  Allocator* target = nullptr;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    if (!handle) {
      target = policy_default_allocator ? policy_default_allocator
                                       : &default_allocator;
    } else {
      auto found = allocator_handles.find(handle);
      if (found == allocator_handles.end()) return nullptr;
      target = found->second.get();
    }
    if (!increment_operation_pin_locked(target)) return nullptr;
  }
  return allocate_with_allocator(target, bytes, alignment);
}

void* nrq_arena_allocate(void* handle, size_t bytes, size_t alignment) {
  if (!handle) return nullptr;
  Allocator* allocator = nullptr;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    auto found = allocator_handles.find(handle);
    if (found == allocator_handles.end()) return nullptr;
    allocator = found->second.get();
    if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return nullptr;
    if (!increment_operation_pin_locked(allocator)) return nullptr;
  }
  void* pointer = nullptr;
  try {
    pointer = allocator->allocate_serialized(bytes, alignment);
  } catch (...) {
    pointer = nullptr;
  }
  release_operation_pin(allocator);
  return pointer;
}

void nrq_memory_deallocate_checked(void* pointer) {
  if (!pointer) return;
  Allocator* allocator = nullptr;
  bool cached = false;
  size_t cached_bytes = 0;
  size_t cached_alignment = 0;
  size_t alignment = 0;
  AllocationShard& shard = shard_for(pointer);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto* found = shard.allocations.find(pointer);
    if (!found || found->owner.operation_in_flight)
      nrq_runtime_fatal("memory.deallocate received an invalid or already freed pointer");
    if (found->owner.shared)
      nrq_runtime_fatal("shared allocations must be released with memory.shared_release");
    if (found->owner.allocator == &default_allocator &&
        thread_default_allocation_cache.can_store(found->owner.bytes,
                                                  found->owner.alignment)) {
      cached = true;
      cached_bytes = found->owner.bytes;
      cached_alignment = found->owner.alignment;
      record_deallocation_profile(cached_bytes);
      // No waiter observes this erase. Allocation waits only while a record
      // exists with operation_in_flight set, and this cached path handles
      // records that were never marked in flight. Avoid a condition-variable
      // wakeup on every cached release.
      shard.allocations.erase(pointer);
    } else {
      allocator = found->owner.allocator;
      alignment = found->owner.alignment;
      found->owner.operation_in_flight = true;
    }
  }
  if (cached) {
    thread_default_allocation_cache.store(pointer, cached_bytes,
                                          cached_alignment);
    return;
  }

  // The live ownership record is the allocator lifetime reference here. Both
  // allocator destruction and reset scan every record while holding the shard
  // locks, and this record remains in flight until the callback has finished and
  // the record is erased. A separate operation pin would only repeat that
  // guarantee and would add the global allocator mutex to every explicit-handle
  // release.
  bool released = false;
  bool callback_failed = false;
  try {
    released = allocator->try_deallocate_serialized(pointer, alignment);
  } catch (...) {
    callback_failed = true;
  }

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto* found = shard.allocations.find(pointer);
    if (!found || !found->owner.operation_in_flight)
      nrq_runtime_fatal("allocator ownership changed during deallocation");
    if (released) {
      record_deallocation_profile(found->owner.bytes);
      shard.allocations.erase(pointer);
      shard.changed.notify_all();
    } else {
      found->owner.operation_in_flight = false;
      shard.changed.notify_all();
    }
  }
  if (callback_failed)
    nrq_runtime_fatal("allocator deallocation callback failed");
  if (!released)
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
}

void nrq_memory_deallocate(void* pointer, size_t bytes, size_t alignment) {
  (void)bytes;
  (void)alignment;
  nrq_memory_deallocate_checked(pointer);
}

void* nrq_memory_reallocate(void* pointer, size_t bytes) {
  if (!pointer) {
    if (bytes == 0) return nullptr;
    Allocator* allocator = nullptr;
    {
      std::lock_guard<std::mutex> lock(allocator_mutex);
      allocator = active_allocator();
      if (!increment_operation_pin_locked(allocator)) return nullptr;
    }
    return allocate_with_allocator(allocator, bytes, kDefaultAlignmentBytes);
  }
  if (bytes == 0) {
    // Zero bytes releases the block, matching the C realloc convention; the
    // caller cannot distinguish this from failure, which is why the language
    // documentation states that a zero-byte reallocation frees the block.
    nrq_memory_deallocate_checked(pointer);
    return nullptr;
  }

  Allocator* allocator = nullptr;
  size_t old_bytes = 0;
  size_t alignment = 0;
  AllocationShard& old_shard = shard_for(pointer);
  {
    std::lock_guard<std::mutex> shard_lock(old_shard.mutex);
    auto* found = old_shard.allocations.find(pointer);
    if (!found)
      nrq_runtime_fatal(
          "memory.reallocate received an invalid or already freed pointer");
    if (found->owner.shared)
      nrq_runtime_fatal("shared allocations cannot be resized");
    if (found->owner.operation_in_flight)
      nrq_runtime_fatal("memory.reallocate raced with another operation on the same pointer");
    allocator = found->owner.allocator;
    old_bytes = found->owner.bytes;
    alignment = found->owner.alignment;
    found->owner.operation_in_flight = true;
  }
  // The in-flight ownership record keeps this allocator alive for the same
  // reason as the deallocation path: destroy/reset must scan that record and
  // will refuse while the operation is in progress.
  void* replacement = nullptr;
  try {
    replacement = allocator->reallocate_serialized(pointer, old_bytes, bytes,
                                                     alignment);
  } catch (...) {
    replacement = nullptr;
  }

  if (!replacement) {
    {
      std::lock_guard<std::mutex> lock(old_shard.mutex);
      auto* found = old_shard.allocations.find(pointer);
      if (!found || !found->owner.operation_in_flight)
        nrq_runtime_fatal("allocator ownership changed during reallocation");
      found->owner.operation_in_flight = false;
      old_shard.changed.notify_all();
    }
    return nullptr;  // The original allocation is untouched.
  }

  AllocationShard& new_shard = shard_for(replacement);
  std::unique_lock<std::mutex> first(old_shard.mutex, std::defer_lock);
  std::unique_lock<std::mutex> second(new_shard.mutex, std::defer_lock);
  if (&old_shard == &new_shard) {
    first.lock();
  } else if (&old_shard < &new_shard) {
    first.lock();
    second.lock();
  } else {
    second.lock();
    first.lock();
  }

  auto* found = old_shard.allocations.find(pointer);
  if (!found || !found->owner.operation_in_flight)
    nrq_runtime_fatal("allocator ownership changed during reallocation");
  if (replacement == pointer) {
    found->owner = AllocationOwner{allocator, bytes, alignment};
  } else {
    old_shard.allocations.erase(pointer);
    if (!new_shard.allocations.insert(
            replacement, AllocationOwner{allocator, bytes, alignment}))
      nrq_runtime_fatal("allocator reallocation returned an already-live address");
  }
  if (memory_profiler_enabled.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> profile_lock(memory_profile_mutex);
    memory_profile.current_bytes = memory_profile.current_bytes - old_bytes + bytes;
    memory_profile.peak_bytes = std::max(memory_profile.peak_bytes,
                                         memory_profile.current_bytes);
    // Only growth requests additional bytes; a shrink is not an allocation.
    if (bytes > old_bytes) {
      memory_profile.total_bytes += bytes - old_bytes;
      ++memory_profile.allocations;
      if (bytes <= 64) ++memory_profile.allocations_le_64;
      else if (bytes <= 256) ++memory_profile.allocations_le_256;
      else if (bytes <= 1024) ++memory_profile.allocations_le_1024;
      else if (bytes <= 4096) ++memory_profile.allocations_le_4096;
      else if (bytes <= 65536) ++memory_profile.allocations_le_65536;
      else ++memory_profile.allocations_gt_65536;
      memory_profile.largest_allocation = std::max(
          memory_profile.largest_allocation, static_cast<uint64_t>(bytes));
    }
  }
  old_shard.changed.notify_all();
  if (&old_shard != &new_shard) new_shard.changed.notify_all();
  if (second.owns_lock()) second.unlock();
  first.unlock();

  return replacement;
}

static bool query_allocator_free_statistics(uint64_t allocator_handle,
                                            size_t& free_bytes,
                                            size_t& largest_free_block) {
  Allocator* allocator = nullptr;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    allocator = resolve_allocator(allocator_handle);
    if (!allocator || !increment_operation_pin_locked(allocator)) return false;
  }
  try {
    allocator->free_statistics_serialized(free_bytes, largest_free_block);
  } catch (...) {
    release_operation_pin(allocator);
    return false;
  }
  release_operation_pin(allocator);
  return true;
}

uint64_t nrq_memory_allocator_free_bytes(uint64_t allocator_handle) {
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  if (!query_allocator_free_statistics(allocator_handle, free_bytes,
                                       largest_free_block)) return 0;
  return free_bytes;
}

uint64_t nrq_memory_allocator_fragmentation_bytes(uint64_t allocator_handle) {
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  if (!query_allocator_free_statistics(allocator_handle, free_bytes,
                                       largest_free_block)) return 0;
  return free_bytes > largest_free_block ? free_bytes - largest_free_block : 0;
}

void nrq_memory_profiler_start(void) {
  std::array<std::unique_lock<std::mutex>, kAllocationShardCount> shard_locks;
  for (size_t index = 0; index < kAllocationShardCount; ++index)
    shard_locks[index] =
        std::unique_lock<std::mutex>(allocation_shards[index].mutex);
  std::lock_guard<std::mutex> profile_lock(memory_profile_mutex);
  memory_profile = {};
  for (const AllocationShard& shard : allocation_shards)
    shard.allocations.for_each([&](const auto& allocation) {
      memory_profile.current_bytes += allocation.owner.bytes;
    });
  memory_profile.peak_bytes = memory_profile.current_bytes;
  memory_profiler_enabled.store(true, std::memory_order_release);
}

void nrq_memory_profiler_stop(void) {
  std::array<std::unique_lock<std::mutex>, kAllocationShardCount> shard_locks;
  for (size_t index = 0; index < kAllocationShardCount; ++index)
    shard_locks[index] =
        std::unique_lock<std::mutex>(allocation_shards[index].mutex);
  std::lock_guard<std::mutex> profile_lock(memory_profile_mutex);
  memory_profiler_enabled.store(false, std::memory_order_release);
}

NrqMemoryProfile nrq_memory_profiler_capture(void) {
  NrqMemoryProfile profile{};
  std::vector<Allocator*> pinned_allocators;
  bool sample_allocator_space = true;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    try {
      pinned_allocators.reserve(allocator_handles.size());
    } catch (...) {
      sample_allocator_space = false;
    }
    if (sample_allocator_space) {
      for (const auto& handle : allocator_handles) {
        Allocator* allocator = handle.second.get();
        if (!increment_operation_pin_locked(allocator)) {
          sample_allocator_space = false;
          break;
        }
        pinned_allocators.push_back(allocator);
      }
    }
    if (!sample_allocator_space) {
      for (Allocator* allocator : pinned_allocators)
        release_operation_pin_locked(allocator);
      pinned_allocators.clear();
    }
  }

  {
    std::array<std::unique_lock<std::mutex>, kAllocationShardCount> shard_locks;
    for (size_t index = 0; index < kAllocationShardCount; ++index)
      shard_locks[index] =
          std::unique_lock<std::mutex>(allocation_shards[index].mutex);
    std::lock_guard<std::mutex> profile_lock(memory_profile_mutex);
    profile = memory_profile;

    // Live bytes per allocator kind describe the registry at this snapshot.
    for (const AllocationShard& shard : allocation_shards) {
      shard.allocations.for_each([&](const auto& allocation) {
        const uint64_t bytes = allocation.owner.bytes;
        switch (allocation.owner.allocator->kind()) {
          case NRQ_ALLOCATOR_GENERAL: profile.general_bytes += bytes; break;
          case NRQ_ALLOCATOR_POOL: profile.pool_bytes += bytes; break;
          case NRQ_ALLOCATOR_ARENA: profile.arena_bytes += bytes; break;
          case NRQ_ALLOCATOR_STACK: profile.stack_bytes += bytes; break;
          case NRQ_ALLOCATOR_SLAB: profile.slab_bytes += bytes; break;
          case NRQ_ALLOCATOR_BUDDY: profile.buddy_bytes += bytes; break;
          case NRQ_ALLOCATOR_FREE_LIST: profile.free_list_bytes += bytes; break;
          case NRQ_ALLOCATOR_TLSF: profile.tlsf_bytes += bytes; break;
          case NRQ_ALLOCATOR_CUSTOM: profile.custom_bytes += bytes; break;
        }
      });
    }
  }

  const nrq_gc_stats gc = nrq_gc_get_stats();
  profile.gc_allocated_bytes = gc.allocated_bytes;
  profile.gc_collected_bytes = gc.collected_bytes;
  profile.gc_collections = gc.collections;
  profile.gc_live_objects = gc.live_objects;

  // Occupancy is sampled rather than accumulated: allocator regions change
  // independently of the registry snapshot. Pins keep handles alive while
  // the per-allocator operation locks are taken outside the global lock.
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  auto sample = [&](const Allocator* allocator) {
    size_t allocator_free = 0;
    size_t allocator_largest = 0;
    try {
      allocator->free_statistics_serialized(allocator_free, allocator_largest);
      free_bytes += allocator_free;
      largest_free_block = std::max(largest_free_block, allocator_largest);
    } catch (...) {
      // Occupancy is optional diagnostic data; retain the remaining snapshot.
    }
  };
  sample(&default_allocator);
  if (sample_allocator_space)
    for (Allocator* allocator : pinned_allocators) sample(allocator);
  profile.free_bytes = free_bytes;
  profile.largest_free_block = largest_free_block;
  profile.fragmentation_bytes = free_bytes > largest_free_block
      ? free_bytes - largest_free_block
      : 0;

  if (!pinned_allocators.empty()) {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    for (Allocator* allocator : pinned_allocators)
      release_operation_pin_locked(allocator);
  }
  return profile;
}

void nrq_memory_profiler_capture_into(NrqMemoryProfile* output) {
  if (output) *output = nrq_memory_profiler_capture();
}

uint64_t nrq_memory_profiler_get_current_bytes(void) {
  return nrq_memory_profiler_capture().current_bytes;
}
uint64_t nrq_memory_profiler_get_total_bytes(void) {
  return nrq_memory_profiler_capture().total_bytes;
}
uint64_t nrq_memory_profiler_get_peak_bytes(void) {
  return nrq_memory_profiler_capture().peak_bytes;
}
uint64_t nrq_memory_profiler_get_allocations(void) {
  return nrq_memory_profiler_capture().allocations;
}
uint64_t nrq_memory_profiler_get_deallocations(void) {
  return nrq_memory_profiler_capture().deallocations;
}
uint64_t nrq_memory_profiler_get_gc_allocated_bytes(void) {
  return nrq_memory_profiler_capture().gc_allocated_bytes;
}
uint64_t nrq_memory_profiler_get_gc_collected_bytes(void) {
  return nrq_memory_profiler_capture().gc_collected_bytes;
}
uint64_t nrq_memory_profiler_get_gc_collections(void) {
  return nrq_memory_profiler_capture().gc_collections;
}
uint64_t nrq_memory_profiler_get_gc_live_objects(void) {
  return nrq_memory_profiler_capture().gc_live_objects;
}

void* nrq_memory_secure_allocate(size_t bytes, size_t alignment) {
  void* pointer = nrq_memory_allocate(bytes, alignment);
  if (pointer && bytes != 0) SecureZeroMemory(pointer, bytes);
  return pointer;
}

void nrq_memory_secure_zero(void* pointer, size_t bytes) {
  if (pointer && bytes != 0) SecureZeroMemory(pointer, bytes);
}

void nrq_memory_secure_release(void* pointer) {
  if (!pointer) return;
  Allocator* allocator = nullptr;
  size_t bytes = 0;
  size_t alignment = 0;
  AllocationShard& shard = shard_for(pointer);
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    std::lock_guard<std::mutex> shard_lock(shard.mutex);
    auto* found = shard.allocations.find(pointer);
    if (!found)
      nrq_runtime_fatal("memory.secure_release received an invalid pointer");
    if (found->owner.shared)
      nrq_runtime_fatal("shared allocations must be released with memory.shared_release");
    if (found->owner.operation_in_flight)
      nrq_runtime_fatal("memory.secure_release raced with another operation on the same pointer");
    allocator = found->owner.allocator;
    bytes = found->owner.bytes;
    alignment = found->owner.alignment;
    if (!increment_operation_pin_locked(allocator))
      nrq_runtime_fatal("memory.secure_release could not pin the owning allocator");
    found->owner.operation_in_flight = true;
  }

  SecureZeroMemory(pointer, bytes);
  bool released = false;
  bool callback_failed = false;
  try {
    released = allocator->try_deallocate_serialized(pointer, alignment);
  } catch (...) {
    callback_failed = true;
  }

  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    release_operation_pin_locked(allocator);
  }
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto* found = shard.allocations.find(pointer);
    if (!found || !found->owner.operation_in_flight)
      nrq_runtime_fatal("allocator ownership changed during secure release");
    if (released) {
      record_deallocation_profile(found->owner.bytes);
      shard.allocations.erase(pointer);
      shard.changed.notify_all();
    } else {
      found->owner.operation_in_flight = false;
      shard.changed.notify_all();
    }
  }
  if (callback_failed)
    nrq_runtime_fatal("allocator deallocation callback failed during secure release");
  if (!released)
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
}

uint64_t nrq_memory_available_bytes(void) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return 0;
  return static_cast<uint64_t>(status.ullAvailPhys);
}

// --- CDMM explicit strategies -----------------------------------------------

static void* cdmm_arena_entry(void* handle, size_t bytes, size_t alignment,
                              NrqCdmmDestructor destructor, void* context) {
  Allocator* allocator = nullptr;
  {
    // The shared lock covers lookup plus pinning, so a concurrent destroy cannot
    // reclaim the allocator between the two. The allocation itself runs outside
    // the lock, serialized only by the allocator's own operation lock.
    std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
    allocator = pin_cdmm_handle_locked(handle);
  }
  if (!allocator) return nullptr;
  void* pointer = nullptr;
  if (allocator->kind() == NRQ_ALLOCATOR_ARENA) {
    auto* arena = static_cast<ArenaAllocator*>(allocator);
    try {
      pointer = destructor ? arena->allocate_destructible_serialized(
                                 bytes, alignment, destructor, context)
                           : arena->allocate_serialized(bytes, alignment);
    } catch (...) {
      pointer = nullptr;
    }
  }
  allocator->unpin_operation();
  return pointer;
}

void* nrq_cdmm_arena_create(size_t initial_bytes, size_t max_bytes) {
  if (max_bytes != 0 && max_bytes < kCdmmMinimumChunkBytes)
    max_bytes = kCdmmMinimumChunkBytes;
  if (initial_bytes == 0) initial_bytes = kDefaultRegionBytes;
  if (initial_bytes < kCdmmMinimumChunkBytes)
    initial_bytes = kCdmmMinimumChunkBytes;
  if (max_bytes != 0 && initial_bytes > max_bytes) initial_bytes = max_bytes;
  try {
    auto arena = std::make_unique<ArenaAllocator>(initial_bytes);
    arena->set_max_bytes(max_bytes);
    if (!arena->reserve_chunk()) return nullptr;
    void* handle = arena.get();
    std::unique_lock<std::shared_mutex> lock(cdmm_handle_mutex);
    cdmm_handles.emplace(handle, std::move(arena));
    return handle;
  } catch (...) {
    return nullptr;
  }
}

int32_t nrq_cdmm_arena_reset(void* handle) {
  if (!handle) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return 0;
  // A scope pins the arena for its whole lifetime, so reset during a scope is
  // refused exactly like reset during a single allocation.
  if (allocator->in_flight_operations() != 0) return 0;
  return allocator->reset_serialized() ? 1 : 0;
}

int32_t nrq_cdmm_arena_destroy(void* handle) {
  if (!handle) return 0;
  std::unique_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  // Destructors run now, while every chunk is still mapped.
  allocator->reset_serialized();
  cdmm_handles.erase(found);
  return 1;
}

void* nrq_cdmm_arena_allocate(void* handle, size_t bytes, size_t alignment) {
  return cdmm_arena_entry(handle, bytes, alignment, nullptr, nullptr);
}

void* nrq_cdmm_arena_allocate_destructible(void* handle, size_t bytes,
                                           size_t alignment,
                                           NrqCdmmDestructor destructor,
                                           void* context) {
  return cdmm_arena_entry(handle, bytes, alignment, destructor, context);
}

size_t nrq_cdmm_arena_allocate_bulk(void* handle, size_t bytes,
                                    size_t alignment, size_t count,
                                    void** output) {
  if (!handle || !output) return 0;
  NrqCdmmArenaScope scope{};
  if (!nrq_cdmm_arena_begin(handle, &scope)) return 0;
  size_t written = 0;
  for (; written < count; ++written) {
    void* pointer = nrq_cdmm_arena_scope_allocate(&scope, bytes, alignment);
    if (!pointer) break;
    output[written] = pointer;
  }
  nrq_cdmm_arena_end(&scope);
  return written;
}

static uint64_t cdmm_arena_stat(const void* handle, bool committed) {
  if (!handle) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return 0;
  auto* arena = static_cast<ArenaAllocator*>(allocator);
  // Serialized against a concurrent reset, which rewrites the chunk list.
  return allocator->serialize_operation([&] {
    return committed ? arena->committed_bytes() : arena->used_bytes();
  });
}

uint64_t nrq_cdmm_arena_used(const void* handle) {
  return cdmm_arena_stat(handle, false);
}

uint64_t nrq_cdmm_arena_committed(const void* handle) {
  return cdmm_arena_stat(handle, true);
}

uint64_t nrq_cdmm_arena_generation(const void* handle) {
  if (!handle) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return 0;
  return static_cast<ArenaAllocator*>(allocator)->generation();
}

int32_t nrq_cdmm_arena_contains(const void* handle, const void* pointer) {
  if (!handle || !pointer) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) return 0;
  return static_cast<ArenaAllocator*>(allocator)->contains(pointer) ? 1 : 0;
}

int32_t nrq_cdmm_arena_begin(void* handle, NrqCdmmArenaScope* scope) {
  if (!handle || !scope) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  Allocator* allocator = pin_cdmm_handle_locked(handle);
  if (!allocator) return 0;
  if (allocator->kind() != NRQ_ALLOCATOR_ARENA) {
    allocator->unpin_operation();
    return 0;
  }
  scope->arena = handle;
  scope->state = allocator;
  scope->generation = static_cast<ArenaAllocator*>(allocator)->generation();
  return 1;
}

void* nrq_cdmm_arena_scope_allocate(NrqCdmmArenaScope* scope, size_t bytes,
                                    size_t alignment) {
  if (!scope || !scope->state) return nullptr;
  return static_cast<ArenaAllocator*>(scope->state)->allocate(bytes, alignment);
}

void* nrq_cdmm_arena_scope_allocate_destructible(NrqCdmmArenaScope* scope,
                                                 size_t bytes, size_t alignment,
                                                 NrqCdmmDestructor destructor,
                                                 void* context) {
  if (!scope || !scope->state) return nullptr;
  return static_cast<ArenaAllocator*>(scope->state)->allocate_destructible(
      bytes, alignment, destructor, context);
}

size_t nrq_cdmm_arena_scope_allocate_bulk(NrqCdmmArenaScope* scope, size_t bytes,
                                          size_t alignment, size_t count,
                                          void** output) {
  if (!scope || !scope->state || !output) return 0;
  size_t written = 0;
  for (; written < count; ++written) {
    void* pointer = nrq_cdmm_arena_scope_allocate(scope, bytes, alignment);
    if (!pointer) break;
    output[written] = pointer;
  }
  return written;
}

void nrq_cdmm_arena_end(NrqCdmmArenaScope* scope) {
  if (!scope || !scope->state) return;
  static_cast<Allocator*>(scope->state)->unpin_operation();
  scope->state = nullptr;
  scope->arena = nullptr;
}

void* nrq_cdmm_pool_create(size_t slot_bytes, size_t slot_alignment,
                           size_t slots_per_chunk, uint32_t flags,
                           NrqCdmmDestructor destructor, void* context) {
  if (slot_bytes == 0 || slot_alignment == 0 ||
      (slot_alignment & (slot_alignment - 1)) != 0)
    return nullptr;
  try {
    auto pool = std::make_unique<CdmmPool>(
        slot_bytes, slot_alignment, slots_per_chunk,
        (flags & NRQ_CDMM_POOL_GENERATIONS) != 0, destructor, context);
    if (!pool->reserve_chunk()) return nullptr;
    void* handle = pool.get();
    std::unique_lock<std::shared_mutex> lock(cdmm_handle_mutex);
    cdmm_handles.emplace(handle, std::move(pool));
    return handle;
  } catch (...) {
    return nullptr;
  }
}

// Looks up a pool and pins it, or returns null. The caller must hold the shared
// handle lock.
static CdmmPool* pin_cdmm_pool_locked(void* handle) {
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return nullptr;
  if (found->second->kind() != NRQ_ALLOCATOR_POOL) return nullptr;
  if (!found->second->try_pin_operation()) return nullptr;
  return static_cast<CdmmPool*>(found->second.get());
}

void* nrq_cdmm_pool_allocate(void* handle) {
  if (!handle) return nullptr;
  CdmmPool* pool = nullptr;
  {
    std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
    pool = pin_cdmm_pool_locked(handle);
  }
  if (!pool) return nullptr;
  void* slot = nullptr;
  try {
    slot = pool->allocate_serialized(1, 1);
  } catch (...) {
    slot = nullptr;
  }
  pool->unpin_operation();
  return slot;
}

size_t nrq_cdmm_pool_allocate_bulk(void* handle, size_t count, void** output) {
  if (!handle || !output) return 0;
  NrqCdmmPoolScope scope{};
  if (!nrq_cdmm_pool_begin(handle, &scope)) return 0;
  const size_t written = nrq_cdmm_pool_scope_allocate_bulk(&scope, count, output);
  nrq_cdmm_pool_end(&scope);
  return written;
}

static int32_t cdmm_pool_release_entry(void* handle, void* slot, bool checked,
                                       uint64_t generation) {
  if (!handle || !slot) return 0;
  CdmmPool* pool = nullptr;
  {
    std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
    pool = pin_cdmm_pool_locked(handle);
  }
  if (!pool) return 0;
  bool released = false;
  try {
    released = pool->release_serialized_slot(slot, checked, generation);
  } catch (...) {
    released = false;
  }
  pool->unpin_operation();
  return released ? 1 : 0;
}

int32_t nrq_cdmm_pool_release(void* handle, void* slot) {
  return cdmm_pool_release_entry(handle, slot, false, 0);
}

int32_t nrq_cdmm_pool_release_checked(void* handle, void* slot,
                                      uint64_t generation) {
  return cdmm_pool_release_entry(handle, slot, true, generation);
}

size_t nrq_cdmm_pool_release_bulk(void* handle, void* const* slots,
                                  size_t count) {
  if (!handle || !slots) return 0;
  NrqCdmmPoolScope scope{};
  if (!nrq_cdmm_pool_begin(handle, &scope)) return 0;
  const size_t released =
      nrq_cdmm_pool_scope_release_bulk(&scope, slots, count);
  nrq_cdmm_pool_end(&scope);
  return released;
}

#if defined(NRQ_RUNTIME_DEBUG_ENABLED) && NRQ_RUNTIME_DEBUG_ENABLED
int32_t nrq_cdmm_pool_seed_generation(void* handle, void* slot,
                                      uint32_t generation) {
  if (!handle || !slot) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return 0;
  if (found->second->kind() != NRQ_ALLOCATOR_POOL) return 0;
  return static_cast<CdmmPool*>(found->second.get())
                 ->seed_generation(slot, generation)
             ? 1
             : 0;
}
#endif

uint64_t nrq_cdmm_pool_generation(const void* handle, const void* slot) {
  if (!handle || !slot) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  if (found->second->kind() != NRQ_ALLOCATOR_POOL) return 0;
  return static_cast<const CdmmPool*>(found->second.get())->slot_generation(slot);
}

static uint64_t cdmm_pool_stat(const void* handle, int which) {
  if (!handle) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  if (found->second->kind() != NRQ_ALLOCATOR_POOL) return 0;
  const auto* pool = static_cast<const CdmmPool*>(found->second.get());
  switch (which) {
    case 0: return pool->live_count();
    case 1: return pool->committed_bytes();
    default: return pool->slot_bytes();
  }
}

uint64_t nrq_cdmm_pool_live(const void* handle) { return cdmm_pool_stat(handle, 0); }
uint64_t nrq_cdmm_pool_committed(const void* handle) {
  return cdmm_pool_stat(handle, 1);
}
uint64_t nrq_cdmm_pool_slot_bytes(const void* handle) {
  return cdmm_pool_stat(handle, 2);
}

int32_t nrq_cdmm_pool_contains(const void* handle, const void* pointer) {
  if (!handle || !pointer) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(const_cast<void*>(handle));
  if (found == cdmm_handles.end()) return 0;
  if (found->second->kind() != NRQ_ALLOCATOR_POOL) return 0;
  return static_cast<const CdmmPool*>(found->second.get())->contains(pointer) ? 1
                                                                             : 0;
}

int32_t nrq_cdmm_pool_reset(void* handle) {
  if (!handle) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_POOL) return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  return allocator->reset_serialized() ? 1 : 0;
}

int32_t nrq_cdmm_pool_destroy(void* handle) {
  if (!handle) return 0;
  std::unique_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  auto found = cdmm_handles.find(handle);
  if (found == cdmm_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (allocator->kind() != NRQ_ALLOCATOR_POOL) return 0;
  if (allocator->in_flight_operations() != 0) return 0;
  allocator->reset_serialized();
  cdmm_handles.erase(found);
  return 1;
}

int32_t nrq_cdmm_pool_begin(void* handle, NrqCdmmPoolScope* scope) {
  if (!handle || !scope) return 0;
  std::shared_lock<std::shared_mutex> lock(cdmm_handle_mutex);
  CdmmPool* pool = pin_cdmm_pool_locked(handle);
  if (!pool) return 0;
  scope->pool = handle;
  scope->state = pool;
  return 1;
}

void* nrq_cdmm_pool_scope_allocate(NrqCdmmPoolScope* scope) {
  if (!scope || !scope->state) return nullptr;
  return static_cast<CdmmPool*>(scope->state)->allocate(1, 1);
}

size_t nrq_cdmm_pool_scope_allocate_bulk(NrqCdmmPoolScope* scope, size_t count,
                                         void** output) {
  if (!scope || !scope->state || !output) return 0;
  auto* pool = static_cast<CdmmPool*>(scope->state);
  size_t written = 0;
  for (; written < count; ++written) {
    void* slot = pool->allocate(1, 1);
    if (!slot) break;
    output[written] = slot;
  }
  return written;
}

int32_t nrq_cdmm_pool_scope_release(NrqCdmmPoolScope* scope, void* slot) {
  if (!scope || !scope->state || !slot) return 0;
  auto* pool = static_cast<CdmmPool*>(scope->state);
  return pool->release_unserialized(slot, false, 0) ? 1 : 0;
}

int32_t nrq_cdmm_pool_scope_release_checked(NrqCdmmPoolScope* scope, void* slot,
                                            uint64_t generation) {
  if (!scope || !scope->state || !slot) return 0;
  auto* pool = static_cast<CdmmPool*>(scope->state);
  return pool->release_unserialized(slot, true, generation) ? 1 : 0;
}

size_t nrq_cdmm_pool_scope_release_bulk(NrqCdmmPoolScope* scope,
                                        void* const* slots, size_t count) {
  if (!scope || !scope->state || !slots) return 0;
  auto* pool = static_cast<CdmmPool*>(scope->state);
  size_t released = 0;
  for (size_t index = 0; index < count; ++index) {
    if (pool->release_unserialized(slots[index], false, 0)) ++released;
  }
  return released;
}

void nrq_cdmm_pool_end(NrqCdmmPoolScope* scope) {
  if (!scope || !scope->state) return;
  static_cast<Allocator*>(scope->state)->unpin_operation();
  scope->state = nullptr;
  scope->pool = nullptr;
}

NrqCdmmStrategy nrq_cdmm_strategy_of(const void* pointer) {
  if (!pointer) return NRQ_CDMM_STRATEGY_NONE;
  CdmmRange range;
  if (!cdmm_lookup_range(reinterpret_cast<uintptr_t>(pointer), range))
    return NRQ_CDMM_STRATEGY_NONE;
  return range.strategy;
}

int32_t nrq_cdmm_object_facts(const void* pointer, NrqCdmmFacts* output) {
  if (!pointer || !output) return 0;
  std::lock_guard<std::mutex> lock(cdmm_range_mutex);
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  auto position = std::upper_bound(
      cdmm_ranges.begin(), cdmm_ranges.end(), address,
      [](uintptr_t value, const CdmmRange& entry) { return value < entry.begin; });
  if (position == cdmm_ranges.begin()) return 0;
  --position;
  if (address >= position->end) return 0;
  const auto* owner = static_cast<const Allocator*>(position->owner);
  output->strategy = position->strategy;
  output->header_bytes = 0;
  output->object_bytes = 0;
  output->generation = 0;
  output->metadata_bytes = 0;
  // The range lock is held across the description so a chunk cannot be released
  // between the lookup and the call into the allocator that owns it.
  owner->cdmm_describe(pointer, *output);
  return 1;
}

}  // extern "C"
