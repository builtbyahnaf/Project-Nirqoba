#define NOMINMAX
#include "nirqoba/runtime/gc.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

double now_ms() {
  static LARGE_INTEGER frequency{};
  static bool initialized = false;
  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = true;
  }
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

struct Node {
  void* next;
  int64_t value;
};

constexpr size_t kCount = 1'000'000;

volatile int64_t checksum;

void report(const char* name, double elapsed_ms) {
  std::printf("%-46s %10.3f ms  %8.2f ns/op\n", name, elapsed_ms,
              elapsed_ms * 1.0e6 / static_cast<double>(kCount));
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

// Populate the object index with kCount published young Node objects, exactly
// as build_chain does (rooted allocation + pin_transfer per return).
std::vector<Node*> populate(void** root_slot) {
  const size_t offset = 0;
  std::vector<Node*> objects(kCount);
  for (size_t i = 0; i < kCount; ++i) {
    void* object = nrq_gc_allocate_array_typed_rooted(
        root_slot, 1, sizeof(Node), &offset, 1);
    nrq_gc_pin_transfer(object);
    nrq_gc_unpin(object);
    objects[i] = static_cast<Node*>(object);
    objects[i]->value = static_cast<int64_t>(i);
  }
  return objects;
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "store_on";
  nrq_gc_disable();

  if (std::strcmp(mode, "alloc_raw") == 0) {
    const size_t offset = 0;
    std::vector<double> alloc_samples;
    std::vector<double> publish_samples;
    for (int sample = 0; sample < 3; ++sample) {
      // Invariant: depth 0 at the top of every sample.
      nrq_gc_collect();
      nrq_gc_mutator_enter();  // depth 1
      nrq_gc_mutator_enter();  // depth 2: leave below must not publish
      const double alloc_start = now_ms();
      for (size_t i = 0; i < kCount; ++i) {
        void* object = nrq_gc_allocate_array_typed(1, sizeof(Node), &offset, 1);
        static_cast<Node*>(object)->value = static_cast<int64_t>(i);
        if ((i & 4095) == 0) checksum += 1;
      }
      alloc_samples.push_back(now_ms() - alloc_start);
      nrq_gc_mutator_leave();  // depth 1: no publication

      const double publish_start = now_ms();
      nrq_gc_mutator_leave();  // depth 0: publish all pending
      publish_samples.push_back(now_ms() - publish_start);
    }
    report("alloc_raw: TLAB bump + header + zero", median(alloc_samples));
    report("alloc_raw: bulk publication", median(publish_samples));
    return 0;
  }

  if (std::strcmp(mode, "alloc_unrooted") == 0 ||
      std::strcmp(mode, "alloc_rooted") == 0 ||
      std::strcmp(mode, "publish_bulk") == 0 ||
      std::strcmp(mode, "publish_each") == 0) {
    const size_t offset = 0;
    void* root = nullptr;
    nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);

    std::vector<double> samples;
    for (int sample = 0; sample < 3; ++sample) {
      root = nullptr;
      nrq_gc_collect();
      nrq_gc_mutator_enter();
      const double start = now_ms();
      if (std::strcmp(mode, "alloc_unrooted") == 0) {
        for (size_t i = 0; i < kCount; ++i) {
          void* object = nrq_gc_allocate_array_typed(1, sizeof(Node), &offset, 1);
          static_cast<Node*>(object)->value = static_cast<int64_t>(i);
          if ((i & 4095) == 0) checksum += 1;
        }
      } else if (std::strcmp(mode, "alloc_rooted") == 0) {
        for (size_t i = 0; i < kCount; ++i) {
          void* object = nrq_gc_allocate_array_typed_rooted(
              &root, 1, sizeof(Node), &offset, 1);
          static_cast<Node*>(object)->value = static_cast<int64_t>(i);
        }
      } else if (std::strcmp(mode, "publish_bulk") == 0) {
        for (size_t i = 0; i < kCount; ++i) {
          void* object = nrq_gc_allocate_array_typed(1, sizeof(Node), &offset, 1);
          static_cast<Node*>(object)->value = static_cast<int64_t>(i);
          if ((i & 4095) == 0) checksum += 1;
        }
        nrq_gc_mutator_leave();
        samples.push_back(now_ms() - start);
        continue;
      } else {  // publish_each
        for (size_t i = 0; i < kCount; ++i) {
          void* object = nrq_gc_allocate_array_typed_rooted(
              &root, 1, sizeof(Node), &offset, 1);
          nrq_gc_pin_transfer(object);
          nrq_gc_unpin(object);
          static_cast<Node*>(object)->value = static_cast<int64_t>(i);
        }
      }
      nrq_gc_mutator_leave();  // depth 0: publishes anything still pending
      samples.push_back(now_ms() - start);
    }
    report(mode, median(samples));
    nrq_gc_root_unregister(&root);
    return 0;
  }

  // Everything below needs a published population.
  nrq_gc_mutator_enter();
  const size_t offset = 0;
  void* root = nullptr;
  nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
  std::vector<Node*> objects = populate(&root);

  if (std::strcmp(mode, "store_off") == 0) nrq_gc_set_generational(0);

  std::vector<double> samples;
  for (int sample = 0; sample < 5; ++sample) {
    const double start = now_ms();
    if (std::strcmp(mode, "store_on") == 0 ||
        std::strcmp(mode, "store_off") == 0) {
      for (size_t i = 0; i < kCount; ++i)
        nrq_gc_root_store(&root, objects[i]);
    } else if (std::strcmp(mode, "copy_on") == 0) {
      for (size_t i = 0; i < kCount; ++i)
        nrq_gc_root_copy(&root, &objects[i], sizeof(void*));
    } else if (std::strcmp(mode, "copy_off") == 0) {
      nrq_gc_set_generational(0);
      for (size_t i = 0; i < kCount; ++i)
        nrq_gc_root_copy(&root, &objects[i], sizeof(void*));
      nrq_gc_set_generational(1);
    } else if (std::strcmp(mode, "pin_unpin") == 0) {
      for (size_t i = 0; i < kCount; ++i) {
        nrq_gc_pin_transfer(objects[i]);
        nrq_gc_unpin(objects[i]);
      }
    } else if (std::strcmp(mode, "unpin_only") == 0) {
      for (size_t i = 0; i < kCount; ++i) nrq_gc_unpin(objects[i]);
    } else if (std::strcmp(mode, "field_store") == 0) {
      for (size_t i = 0; i < kCount; ++i)
        nrq_gc_root_store(&objects[i]->next, objects[(i + 1) % kCount]);
    }
    samples.push_back(now_ms() - start);
  }
  report(mode, median(samples));

  nrq_gc_root_unregister(&root);
  nrq_gc_mutator_leave();
  return 0;
}
