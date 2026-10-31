// Windows explicit allocator implementations. Every strategy is owned by an
// opaque runtime handle; the common allocation registry records which handle
// owns each returned pointer so frees cannot cross allocator domains.

#ifndef _WIN32
#  error "allocator_windows.cpp is the Windows-only allocator implementation."
#endif

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/fatal.h"
#include "nirqoba/runtime/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr size_t kDefaultRegionBytes = 1024 * 1024;
constexpr size_t kMinimumBlockBytes = 16;

size_t align_up(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t next_power_of_two(size_t value) {
  if (value <= 1) return 1;
  --value;
  for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1)
    value |= value >> shift;
  return value + 1;
}

void* heap_alloc_aligned(size_t bytes, size_t alignment) {
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      alignment > std::numeric_limits<size_t>::max() - sizeof(void*) ||
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

class Allocator {
 public:
  explicit Allocator(size_t capacity) : capacity_(capacity) {}
  virtual ~Allocator() = default;
  virtual void* allocate(size_t bytes, size_t alignment) = 0;
  virtual void deallocate(void* pointer) = 0;
  virtual bool can_deallocate(void*) const { return true; }
  virtual bool reset() = 0;
  virtual bool can_destroy() const { return true; }
  size_t capacity() const { return capacity_; }

 protected:
  size_t capacity_;
};

class GeneralAllocator final : public Allocator {
 public:
  GeneralAllocator() : Allocator(0) {}
  void* allocate(size_t bytes, size_t alignment) override {
    return heap_alloc_aligned(bytes, alignment);
  }
  void deallocate(void* pointer) override { heap_free_aligned(pointer); }
  bool reset() override { return true; }
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
    const size_t size_class = slab_
        ? std::max<size_t>(16, next_power_of_two(std::max(bytes, alignment)))
        : next_power_of_two(std::max(capacity_, alignment));
    const auto key = std::make_pair(size_class, alignment);
    PoolBucket& bucket = buckets_[key];
    if (bucket.stride == 0) {
      bucket.alignment = std::max<size_t>(alignment, 16);
      bucket.stride = align_up(size_class, bucket.alignment);
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

 private:
  bool add_chunk(PoolBucket& bucket) {
    const size_t slots = std::max<size_t>(8, 64 * 1024 / bucket.stride);
    if (slots > (std::numeric_limits<size_t>::max() - bucket.alignment) /
                    bucket.stride) return false;
    const size_t total = slots * bucket.stride + bucket.alignment - 1;
    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!raw) return false;
    const uintptr_t base = align_up(
        reinterpret_cast<uintptr_t>(raw), bucket.alignment);
    bucket.raw_chunks.push_back(raw);
    for (size_t index = 0; index < slots; ++index) {
      void* slot = reinterpret_cast<void*>(base + index * bucket.stride);
      bucket.free_slots.push_back(slot);
      bucket.slot_chunk.emplace(slot, bucket.raw_chunks.size() - 1);
    }
    return true;
  }

  bool slab_;
  std::map<std::pair<size_t, size_t>, PoolBucket> buckets_;
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
  ~ArenaAllocator() override { reset(); }

  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    if (chunks_.empty() || !fits(chunks_.back(), bytes, alignment)) {
      const size_t request = bytes + alignment - 1;
      if (!add_chunk(std::max(capacity_, request))) return nullptr;
    }
    ArenaChunk& chunk = chunks_.back();
    const size_t aligned_offset = align_up(chunk.used, alignment);
    void* result = reinterpret_cast<void*>(chunk.base + aligned_offset);
    chunk.used = aligned_offset + bytes;
    return result;
  }

  void deallocate(void*) override {}

  bool reset() override {
    for (const ArenaChunk& chunk : chunks_)
      HeapFree(GetProcessHeap(), 0, chunk.raw);
    chunks_.clear();
    return true;
  }

 protected:
  bool fits(const ArenaChunk& chunk, size_t bytes, size_t alignment) const {
    const size_t offset = align_up(chunk.used, alignment);
    return offset <= chunk.size && bytes <= chunk.size - offset;
  }

  bool add_chunk(size_t bytes) {
    void* raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes + 63);
    if (!raw) return false;
    const uintptr_t base = align_up(reinterpret_cast<uintptr_t>(raw), 64);
    chunks_.push_back(ArenaChunk{raw, base, bytes, 0});
    return true;
  }

  std::vector<ArenaChunk> chunks_;
};

class StackAllocator final : public ArenaAllocator {
 public:
  explicit StackAllocator(size_t capacity) : ArenaAllocator(capacity) {}
  void* allocate(size_t bytes, size_t alignment) override {
    if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
      return nullptr;
    if (chunks_.empty() || !fits(chunks_.back(), bytes, alignment)) {
      const size_t request = bytes + alignment - 1;
      if (!add_chunk(std::max(capacity_, request))) return nullptr;
    }
    ArenaChunk& chunk = chunks_.back();
    const size_t previous = chunk.used;
    const size_t aligned_offset = align_up(previous, alignment);
    void* result = reinterpret_cast<void*>(chunk.base + aligned_offset);
    chunk.used = aligned_offset + bytes;
    stack_.push_back({result, chunks_.size() - 1, previous});
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
        (alignment & (alignment - 1)) != 0) return nullptr;
    for (auto block = blocks_.begin(); block != blocks_.end(); ++block) {
      if (!block->free) continue;
      const size_t aligned = align_up(block->offset, alignment);
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
        bytes > std::numeric_limits<size_t>::max() - alignment) return nullptr;
    const size_t required = bytes + alignment - 1;
    auto block = find_block(required);
    if (block == blocks_.end()) return nullptr;
    remove_bin(&*block);
    const size_t aligned_offset = align_up(block->offset, alignment);
    const size_t consumed_end = aligned_offset + bytes;
    const size_t original_end = block->offset + block->size;
    block->size = consumed_end - block->offset;
    block->free = false;
    void* pointer = static_cast<unsigned char*>(base_) + aligned_offset;
    live_[pointer] = block;
    if (consumed_end < original_end) {
      auto remainder = blocks_.insert(std::next(block),
          FreeBlock{consumed_end, original_end - consumed_end, true});
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
        block = blocks_.erase(block);
        block = previous;
      }
    }
    auto next = std::next(block);
    if (next != blocks_.end() && next->free) {
      remove_bin(&*next);
      block->size += next->size;
      blocks_.erase(next);
    }
    insert_bin(&*block);
  }

  bool reset() override {
    live_.clear();
    blocks_.clear();
    clear_bins();
    if (!base_) base_ = _aligned_malloc(capacity_, capacity_);
    if (!base_) return false;
    blocks_.push_back(FreeBlock{0, capacity_, true});
    insert_bin(&blocks_.front());
    return true;
  }

 private:
  static constexpr size_t kSecondLevels = 8;
  static constexpr size_t kFirstLevels = 64;
  struct Bin { std::list<FreeBlock*> blocks; };
  using Iterator = std::list<FreeBlock>::iterator;

  std::pair<size_t, size_t> mapping(size_t size) const {
    size_t first = 0;
    size_t value = size;
    while (value >>= 1) ++first;
    if (first >= kFirstLevels) first = kFirstLevels - 1;
    const size_t base = size_t{1} << first;
    const size_t second = first == 0 ? 0 :
        std::min(kSecondLevels - 1, ((size - base) * kSecondLevels) / base);
    return {first, second};
  }

  Iterator find_block(size_t required) {
    const auto start = mapping(required);
    for (size_t first = start.first; first < kFirstLevels; ++first) {
      const size_t first_second = first == start.first ? start.second : 0;
      for (size_t second = first_second; second < kSecondLevels; ++second) {
        auto& bin = bins_[first * kSecondLevels + second].blocks;
        for (FreeBlock* candidate : bin) {
          if (candidate->size >= required) {
            for (auto it = blocks_.begin(); it != blocks_.end(); ++it)
              if (&*it == candidate) return it;
          }
        }
      }
    }
    return blocks_.end();
  }

  void insert_bin(FreeBlock* block) {
    const auto [first, second] = mapping(block->size);
    bins_[first * kSecondLevels + second].blocks.push_back(block);
  }

  void remove_bin(FreeBlock* block) {
    const auto [first, second] = mapping(block->size);
    auto& bin = bins_[first * kSecondLevels + second].blocks;
    bin.remove(block);
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
};

std::mutex allocator_mutex;
std::unordered_map<void*, std::unique_ptr<Allocator>> allocator_handles;
std::unordered_map<Allocator*, size_t> selected_counts;
struct AllocationOwner { Allocator* allocator; size_t bytes; };
std::unordered_map<void*, AllocationOwner> live_allocations;
NrqMemoryProfile memory_profile{};
bool memory_profiler_enabled = false;
GeneralAllocator default_allocator;
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

Allocator* active_allocator() {
  return thread_selection.allocator ? thread_selection.allocator : &default_allocator;
}

std::unique_ptr<Allocator> make_allocator(NrqAllocatorKind kind, size_t capacity) {
  switch (kind) {
    case NRQ_ALLOCATOR_GENERAL: return std::make_unique<GeneralAllocator>();
    case NRQ_ALLOCATOR_POOL: return std::make_unique<FixedPoolAllocator>(capacity, false);
    case NRQ_ALLOCATOR_ARENA: return std::make_unique<ArenaAllocator>(capacity);
    case NRQ_ALLOCATOR_STACK: return std::make_unique<StackAllocator>(capacity);
    case NRQ_ALLOCATOR_SLAB: return std::make_unique<FixedPoolAllocator>(capacity, true);
    case NRQ_ALLOCATOR_BUDDY: return std::make_unique<BuddyAllocator>(capacity);
    case NRQ_ALLOCATOR_FREE_LIST: return std::make_unique<FreeListAllocator>(capacity);
    case NRQ_ALLOCATOR_TLSF: return std::make_unique<TlsfAllocator>(capacity);
  }
  return nullptr;
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

int32_t nrq_allocator_destroy(void* handle) {
  if (!handle) return 0;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  auto selected = found == allocator_handles.end()
      ? selected_counts.end() : selected_counts.find(found->second.get());
  if (found == allocator_handles.end() ||
      (selected != selected_counts.end() && selected->second != 0))
    return 0;
  for (const auto& allocation : live_allocations)
    if (allocation.second.allocator == found->second.get()) return 0;
  allocator_handles.erase(found);
  return 1;
}

int32_t nrq_allocator_reset(void* handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return 0;
  Allocator* allocator = found->second.get();
  if (!allocator->reset()) return 0;
  for (auto it = live_allocations.begin(); it != live_allocations.end();) {
    if (it->second.allocator == allocator) {
      if (memory_profiler_enabled) {
        memory_profile.current_bytes -= it->second.bytes;
        ++memory_profile.deallocations;
      }
      it = live_allocations.erase(it);
    } else ++it;
  }
  return 1;
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

void* nrq_memory_allocate(size_t bytes, size_t alignment) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  Allocator* allocator = active_allocator();
  void* pointer = nullptr;
  try {
    pointer = allocator->allocate(bytes, alignment);
  } catch (...) {
    return nullptr;
  }
  if (!pointer) return nullptr;
  try {
    live_allocations.emplace(pointer, AllocationOwner{allocator, bytes});
  } catch (...) {
    allocator->deallocate(pointer);
    return nullptr;
  }
  if (memory_profiler_enabled) {
    memory_profile.current_bytes += bytes;
    memory_profile.peak_bytes = std::max(memory_profile.peak_bytes,
                                         memory_profile.current_bytes);
    ++memory_profile.allocations;
  }
  return pointer;
}

void nrq_memory_deallocate_checked(void* pointer) {
  if (!pointer) return;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = live_allocations.find(pointer);
  if (found == live_allocations.end())
    nrq_runtime_fatal("memory.deallocate received an invalid or already freed pointer");
  if (!found->second.allocator->can_deallocate(pointer))
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
  found->second.allocator->deallocate(pointer);
  if (memory_profiler_enabled) {
    memory_profile.current_bytes -= found->second.bytes;
    ++memory_profile.deallocations;
  }
  live_allocations.erase(found);
}

void nrq_memory_deallocate(void* pointer, size_t bytes, size_t alignment) {
  (void)bytes;
  (void)alignment;
  nrq_memory_deallocate_checked(pointer);
}

void nrq_memory_profiler_start(void) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  memory_profile = {};
  for (const auto& allocation : live_allocations)
    memory_profile.current_bytes += allocation.second.bytes;
  memory_profile.peak_bytes = memory_profile.current_bytes;
  memory_profiler_enabled = true;
}

void nrq_memory_profiler_stop(void) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  memory_profiler_enabled = false;
}

NrqMemoryProfile nrq_memory_profiler_capture(void) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  return memory_profile;
}

uint64_t nrq_memory_profiler_get_current_bytes(void) {
  return nrq_memory_profiler_capture().current_bytes;
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
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = live_allocations.find(pointer);
  if (found == live_allocations.end())
    nrq_runtime_fatal("memory.secure_release received an invalid pointer");
  SecureZeroMemory(pointer, found->second.bytes);
  if (!found->second.allocator->can_deallocate(pointer))
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
  found->second.allocator->deallocate(pointer);
  if (memory_profiler_enabled) {
    memory_profile.current_bytes -= found->second.bytes;
    ++memory_profile.deallocations;
  }
  live_allocations.erase(found);
}

uint64_t nrq_memory_available_bytes(void) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return 0;
  return static_cast<uint64_t>(status.ullAvailPhys);
}

}  // extern "C"
