// Process-lifetime string arena for runtime buffers that escape into the
// language as borrowed `string` values.
//
// A block is a fixed-size bump region; a large string gets its own block. The
// language represents `string` as a raw borrowed char pointer and has no
// per-string destruction contract, so the only sound lifetime is the process.
// Copies, returns, aggregate fields and sharing all copy the pointer; none of
// them is observable as a release point. The arena is deliberately process-owned
// and is not released by entry shutdown: strings stay valid through deferred
// cleanup and static destructors, and the OS reclaims the blocks at termination.

#ifndef _WIN32
#  error "runtime_buffers.cpp is the Windows-only runtime buffer arena."
#endif

#include "runtime_buffers.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

namespace nirq::runtime {
namespace {

constexpr std::size_t kBlockBytes = 1024 * 1024;
constexpr std::size_t kAlignment = alignof(std::max_align_t);

struct alignas(std::max_align_t) Block {
  Block* next = nullptr;
  std::size_t capacity = 0;
  std::size_t used = 0;
};

class BufferArena {
 public:
  BufferArena() = default;
  BufferArena(const BufferArena&) = delete;
  BufferArena& operator=(const BufferArena&) = delete;
  ~BufferArena() { release_all(); }

  char* allocate(std::size_t bytes) noexcept {
    if (bytes == 0) bytes = 1;
    if (bytes > std::numeric_limits<std::size_t>::max() - (kAlignment - 1)) {
      return nullptr;
    }
    const std::size_t need = (bytes + kAlignment - 1) & ~(kAlignment - 1);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!head_ || head_->capacity - head_->used < need) {
      if (!commit_block(std::max(kBlockBytes, need))) return nullptr;
    }
    char* result = reinterpret_cast<char*>(head_ + 1) + head_->used;
    head_->used += need;
    return result;
  }

  void release_all() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    Block* block = head_;
    while (block) {
      Block* next = block->next;
      HeapFree(GetProcessHeap(), 0, block);
      block = next;
    }
    head_ = nullptr;
    committed_bytes_ = 0;
  }

  std::size_t committed_bytes() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_bytes_;
  }

 private:
  bool commit_block(std::size_t capacity) noexcept {
    if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(Block)) {
      return false;
    }
    auto* block = static_cast<Block*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Block) + capacity));
    if (!block) return false;
    block->next = head_;
    block->capacity = capacity;
    block->used = 0;
    head_ = block;
    committed_bytes_ += capacity;
    return true;
  }

  std::mutex mutex_;
  Block* head_ = nullptr;
  std::size_t committed_bytes_ = 0;
};

BufferArena& arena() {
  // Intentionally never destroyed: a static destructor would run before some
  // later-constructed statics and could invalidate a string still reachable
  // from their destructors. Process termination is the release point.
  static BufferArena* instance = new BufferArena();
  return *instance;
}

}  // namespace

char* allocate_process_string(std::size_t bytes) noexcept {
  return arena().allocate(bytes);
}

void release_process_strings() noexcept {
  arena().release_all();
}

std::size_t process_string_bytes() noexcept {
  return arena().committed_bytes();
}

}  // namespace nirq::runtime
