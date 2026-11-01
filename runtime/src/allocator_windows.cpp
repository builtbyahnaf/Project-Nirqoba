// Windows explicit allocator implementations. Every strategy is owned by an
// opaque runtime handle; the common allocation registry records which handle
// owns each returned pointer so frees cannot cross allocator domains.

#ifndef _WIN32
#  error "allocator_windows.cpp is the Windows-only allocator implementation."
#endif

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/fatal.h"
#include "nirqoba/runtime/system.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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

using namespace std;

namespace {

constexpr size_t kDefaultRegionBytes = 1024 * 1024;
constexpr size_t kMinimumBlockBytes = 16;
// Alignment used when `memory.reallocate` is asked to grow a null pointer, i.e.
// when the call is really a fresh allocation with no prior alignment to reuse.
constexpr size_t kDefaultAlignmentBytes = 16;

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
  virtual NrqAllocatorKind kind() const { return NRQ_ALLOCATOR_GENERAL; }
  size_t capacity() const { return capacity_; }

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
    deallocate(pointer);
    return replacement;
  }

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
    for (const ArenaChunk& chunk : chunks_)
      HeapFree(GetProcessHeap(), 0, chunk.raw);
    chunks_.clear();
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
    return true;
  }

  std::vector<ArenaChunk> chunks_;
};

class StackAllocator final : public ArenaAllocator {
 public:
  explicit StackAllocator(size_t capacity) : ArenaAllocator(capacity) {}
  NrqAllocatorKind kind() const override { return NRQ_ALLOCATOR_STACK; }
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

std::mutex allocator_mutex;
std::unordered_map<void*, std::unique_ptr<Allocator>> allocator_handles;
std::unordered_map<Allocator*, size_t> selected_counts;
struct AllocationOwner { Allocator* allocator; size_t bytes; size_t alignment; };
std::unordered_map<void*, AllocationOwner> live_allocations;
std::unordered_map<void*, uint64_t> shared_reference_counts;
NrqMemoryProfile memory_profile{};
bool memory_profiler_enabled = false;
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
  if (!memory_profiler_enabled) return;
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
  if (!memory_profiler_enabled) return;
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
  auto selected = selected_counts.find(allocator);
  if (selected != selected_counts.end() && selected->second != 0) return 0;
  for (const auto& allocation : live_allocations)
    if (allocation.second.allocator == allocator) return 0;
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
      shared_reference_counts.erase(it->first);
      record_deallocation_profile(it->second.bytes);
      it = live_allocations.erase(it);
    } else ++it;
  }
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

static void* allocate_with_allocator(Allocator* allocator, size_t bytes,
                                     size_t alignment) {
  void* pointer = nullptr;
  try {
    pointer = allocator->allocate(bytes, alignment);
  } catch (...) {
    return nullptr;
  }
  if (!pointer) return nullptr;
  try {
    live_allocations.emplace(pointer, AllocationOwner{allocator, bytes, alignment});
  } catch (...) {
    allocator->deallocate(pointer);
    return nullptr;
  }
  record_allocation_profile(bytes);
  return pointer;
}

void* nrq_memory_allocate(size_t bytes, size_t alignment) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  return allocate_with_allocator(active_allocator(), bytes, alignment);
}

void* nrq_memory_allocate_array(size_t count, size_t element_size,
                                size_t alignment) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  return nrq_memory_allocate(count * element_size, alignment);
}

void nrq_memory_array_bounds_check(uint64_t index, uint64_t length) {
  if (index >= length)
    nrq_runtime_fatal("owned array index is outside the array bounds");
}

void* nrq_memory_shared_allocate(size_t bytes, size_t alignment) {
  void* pointer = nrq_memory_allocate(bytes, alignment);
  if (!pointer) return nullptr;
  try {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    shared_reference_counts.emplace(pointer, 1);
  } catch (...) {
    nrq_memory_deallocate_checked(pointer);
    return nullptr;
  }
  return pointer;
}

void* nrq_memory_shared_retain(void* pointer) {
  if (!pointer) return nullptr;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = shared_reference_counts.find(pointer);
  if (found == shared_reference_counts.end() || found->second == UINT64_MAX) return nullptr;
  ++found->second;
  return pointer;
}

int32_t nrq_memory_shared_release(void* pointer) {
  if (!pointer) return 0;
  bool release_allocation = false;
  {
    std::lock_guard<std::mutex> lock(allocator_mutex);
    auto found = shared_reference_counts.find(pointer);
    if (found == shared_reference_counts.end()) return 0;
    if (--found->second == 0) {
      shared_reference_counts.erase(found);
      release_allocation = true;
    }
  }
  if (release_allocation) nrq_memory_deallocate_checked(pointer);
  return 1;
}

uint64_t nrq_memory_shared_count(void* pointer) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  auto found = shared_reference_counts.find(pointer);
  return found == shared_reference_counts.end() ? 0 : found->second;
}

void* nrq_memory_allocate_from(void* handle, size_t bytes, size_t alignment) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (!handle) {
    Allocator* target = policy_default_allocator ? policy_default_allocator
                                                 : &default_allocator;
    return allocate_with_allocator(target, bytes, alignment);
  }
  auto found = allocator_handles.find(handle);
  if (found == allocator_handles.end()) return nullptr;
  return allocate_with_allocator(found->second.get(), bytes, alignment);
}

void nrq_memory_deallocate_checked(void* pointer) {
  if (!pointer) return;
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (shared_reference_counts.find(pointer) != shared_reference_counts.end())
    nrq_runtime_fatal("shared allocations must be released with memory.shared_release");
  auto found = live_allocations.find(pointer);
  if (found == live_allocations.end())
    nrq_runtime_fatal("memory.deallocate received an invalid or already freed pointer");
  if (!found->second.allocator->can_deallocate(pointer))
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
  found->second.allocator->deallocate(pointer);
  record_deallocation_profile(found->second.bytes);
  live_allocations.erase(found);
}

void nrq_memory_deallocate(void* pointer, size_t bytes, size_t alignment) {
  (void)bytes;
  (void)alignment;
  nrq_memory_deallocate_checked(pointer);
}

void* nrq_memory_reallocate(void* pointer, size_t bytes) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (!pointer) {
    if (bytes == 0) return nullptr;
    return allocate_with_allocator(active_allocator(), bytes, kDefaultAlignmentBytes);
  }
  if (shared_reference_counts.find(pointer) != shared_reference_counts.end())
    nrq_runtime_fatal("shared allocations cannot be resized");
  auto found = live_allocations.find(pointer);
  if (found == live_allocations.end())
    nrq_runtime_fatal(
        "memory.reallocate received an invalid or already freed pointer");
  Allocator* allocator = found->second.allocator;
  const size_t old_bytes = found->second.bytes;
  const size_t alignment = found->second.alignment;
  if (bytes == 0) {
    // Zero bytes releases the block, matching the C realloc convention; the
    // caller cannot distinguish this from failure, which is why the language
    // documentation states that a zero-byte reallocation frees the block.
    if (!allocator->can_deallocate(pointer))
      nrq_runtime_fatal(
          "stack allocator allocations must be deallocated in LIFO order");
    allocator->deallocate(pointer);
    record_deallocation_profile(old_bytes);
    live_allocations.erase(found);
    return nullptr;
  }
  void* replacement = allocator->reallocate(pointer, old_bytes, bytes, alignment);
  if (!replacement) return nullptr;  // The original allocation is untouched.
  // The registry entry is keyed by the old address; `reallocate` may hand back
  // the same address when a strategy resized in place.
  live_allocations.erase(pointer);
  live_allocations.emplace(replacement, AllocationOwner{allocator, bytes, alignment});
  if (memory_profiler_enabled) {
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
  return replacement;
}

uint64_t nrq_memory_allocator_free_bytes(uint64_t allocator_handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  Allocator* allocator = resolve_allocator(allocator_handle);
  if (!allocator) return 0;
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  allocator->free_statistics(free_bytes, largest_free_block);
  return free_bytes;
}

uint64_t nrq_memory_allocator_fragmentation_bytes(uint64_t allocator_handle) {
  std::lock_guard<std::mutex> lock(allocator_mutex);
  Allocator* allocator = resolve_allocator(allocator_handle);
  if (!allocator) return 0;
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  allocator->free_statistics(free_bytes, largest_free_block);
  return free_bytes > largest_free_block ? free_bytes - largest_free_block : 0;
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
  NrqMemoryProfile profile = memory_profile;
  profile.gc_allocated_bytes = nrq_gc_get_allocated_bytes();
  profile.gc_collected_bytes = nrq_gc_get_collected_bytes();
  profile.gc_collections = nrq_gc_get_collection_count();
  profile.gc_live_objects = nrq_gc_get_live_objects();

  // Occupancy is sampled rather than accumulated: allocators extend and reset
  // their regions behind the profiler's back, so a running counter would drift.
  size_t free_bytes = 0;
  size_t largest_free_block = 0;
  auto sample = [&](const Allocator* allocator) {
    size_t allocator_free = 0;
    size_t allocator_largest = 0;
    allocator->free_statistics(allocator_free, allocator_largest);
    free_bytes += allocator_free;
    largest_free_block = std::max(largest_free_block, allocator_largest);
  };
  sample(&default_allocator);
  for (const auto& handle : allocator_handles) sample(handle.second.get());
  profile.free_bytes = free_bytes;
  profile.largest_free_block = largest_free_block;
  profile.fragmentation_bytes = free_bytes > largest_free_block
      ? free_bytes - largest_free_block
      : 0;

  // Live bytes per allocator kind, so a report can attribute memory to the
  // strategy that owns it. This is sampled from the registry and therefore
  // describes the current state, not an accumulated total.
  for (const auto& allocation : live_allocations) {
    const uint64_t bytes = allocation.second.bytes;
    switch (allocation.second.allocator->kind()) {
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
  std::lock_guard<std::mutex> lock(allocator_mutex);
  if (shared_reference_counts.find(pointer) != shared_reference_counts.end())
    nrq_runtime_fatal("shared allocations must be released with memory.shared_release");
  auto found = live_allocations.find(pointer);
  if (found == live_allocations.end())
    nrq_runtime_fatal("memory.secure_release received an invalid pointer");
  SecureZeroMemory(pointer, found->second.bytes);
  if (!found->second.allocator->can_deallocate(pointer))
    nrq_runtime_fatal("stack allocator allocations must be deallocated in LIFO order");
  found->second.allocator->deallocate(pointer);
  record_deallocation_profile(found->second.bytes);
  live_allocations.erase(found);
}

uint64_t nrq_memory_available_bytes(void) {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return 0;
  return static_cast<uint64_t>(status.ullAvailPhys);
}

}  // extern "C"
