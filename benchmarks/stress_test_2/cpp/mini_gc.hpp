// C++20 counterpart to c/mini_gc.h.
//
// The algorithmic contract is identical: precise trace callbacks, a worklist
// mark phase, a linear sweep over an all-objects list, and a size-uniform free
// list fed by bump-allocated blocks. C++ uses std::vector for the mark worklist
// and new[]/delete[] for the blocks. Every GC workload in the C++ track uses
// this collector so the comparison is against the same algorithm, not against
// native new/delete.
#ifndef NIRQOBA_BENCH_STRESS_TEST_2_CPP_MINI_GC_HPP
#define NIRQOBA_BENCH_STRESS_TEST_2_CPP_MINI_GC_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

struct MiniGcObject;

using MiniGcVisitFn = void (*)(MiniGcObject* referenced_object, void* context);
using MiniGcTraceFn = void (*)(MiniGcObject* object, MiniGcVisitFn visit,
                               void* context);

struct MiniGcObject {
  MiniGcObject* next_all;
  MiniGcObject* next_free;
  MiniGcTraceFn trace;
  std::uint32_t object_bytes;
  std::uint32_t marked;
};

struct MiniGcBlock {
  MiniGcBlock* next;
  std::size_t used;
  std::size_t capacity;
  unsigned char* data;
};

class MiniGc {
 public:
  explicit MiniGc(std::size_t block_bytes)
      : block_bytes_(block_bytes < 4096 ? 4096 : block_bytes) {}

  ~MiniGc() {
    MiniGcBlock* block = blocks_;
    while (block != nullptr) {
      MiniGcBlock* next = block->next;
      delete[] block->data;
      delete block;
      block = next;
    }
  }

  MiniGc(const MiniGc&) = delete;
  MiniGc& operator=(const MiniGc&) = delete;

  MiniGcObject* alloc(std::size_t bytes, MiniGcTraceFn trace) {
    MiniGcObject* object;
    if (free_list_ != nullptr) {
      object = free_list_;
      free_list_ = object->next_free;
    } else {
      MiniGcBlock* block = blocks_;
      if (block == nullptr || block->used + bytes > block->capacity) {
        const std::size_t capacity =
            block_bytes_ < bytes ? bytes : block_bytes_;
        block = new MiniGcBlock{};
        block->data = new unsigned char[capacity];
        block->used = 0;
        block->capacity = capacity;
        block->next = blocks_;
        blocks_ = block;
      }
      object = reinterpret_cast<MiniGcObject*>(block->data + block->used);
      block->used += bytes;
    }

    std::memset(object, 0, bytes);
    object->next_all = all_objects_;
    all_objects_ = object;
    object->next_free = nullptr;
    object->trace = trace;
    object->object_bytes = static_cast<std::uint32_t>(bytes);
    object->marked = 0;
    ++live_objects_;
    ++total_allocations_;
    return object;
  }

  void set_root(MiniGcObject* root) { root_ = root; }

  std::size_t collect() {
    mark_stack_.clear();
    if (root_ != nullptr) {
      mark_stack_.push_back(root_);
    }
    while (!mark_stack_.empty()) {
      MiniGcObject* object = mark_stack_.back();
      mark_stack_.pop_back();
      if (object->marked != 0) {
        continue;
      }
      object->marked = 1;
      if (object->trace != nullptr) {
        object->trace(object, &MiniGc::visit, this);
      }
    }

    MiniGcObject** link = &all_objects_;
    while (*link != nullptr) {
      MiniGcObject* object = *link;
      if (object->marked != 0) {
        object->marked = 0;
        link = &object->next_all;
      } else {
        *link = object->next_all;
        object->next_free = free_list_;
        free_list_ = object;
        --live_objects_;
      }
    }

    ++collections_;
    return live_objects_;
  }

  [[nodiscard]] std::size_t live_objects() const { return live_objects_; }
  [[nodiscard]] std::size_t collections() const { return collections_; }

 private:
  static void visit(MiniGcObject* object, void* context) {
    static_cast<MiniGc*>(context)->mark_stack_.push_back(object);
  }

  MiniGcBlock* blocks_ = nullptr;
  MiniGcObject* all_objects_ = nullptr;
  MiniGcObject* free_list_ = nullptr;
  MiniGcObject* root_ = nullptr;
  std::vector<MiniGcObject*> mark_stack_;
  std::size_t live_objects_ = 0;
  std::size_t collections_ = 0;
  std::size_t total_allocations_ = 0;
  std::size_t block_bytes_;
};

#endif  // NIRQOBA_BENCH_STRESS_TEST_2_CPP_MINI_GC_HPP
