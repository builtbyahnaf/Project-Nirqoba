// Precise non-moving mark-and-sweep collector for explicitly described objects.
// The runtime owns a linked allocation registry, while roots are addresses of
// pointer slots registered by generated code or native callers. Marking follows
// only trace callbacks, so integer data that happens to resemble an address is
// never treated as a reference.

#include "nirqoba/runtime/gc.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

struct ObjectHeader {
  ObjectHeader* next = nullptr;
  ObjectHeader* mark_next = nullptr;
  void* object = nullptr;
  size_t size = 0;
  size_t element_size = 0;
  nrq_gc_trace_fn trace = nullptr;
  bool marked = false;
  std::vector<size_t> reference_offsets;
};

std::mutex gc_mutex;
ObjectHeader* objects = nullptr;
std::unordered_map<void*, ObjectHeader*> object_index;
struct RootInfo {
  size_t references = 0;
  size_t object_size = 0;
  std::vector<size_t> reference_offsets;
};
std::unordered_map<void*, RootInfo> roots;
std::unordered_map<void*, size_t> pinned_objects;
bool reference_diagnostics_enabled = false;
thread_local std::vector<void*> thread_roots;
bool automatic_collection_enabled = true;
size_t collection_threshold = 1024 * 1024;
size_t bytes_since_collection = 0;
nrq_gc_stats statistics{};

struct MarkContext {
  ObjectHeader* pending = nullptr;
};

void enqueue_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* context = static_cast<MarkContext*>(opaque_context);
  auto found = object_index.find(object);
  // Roots may refer to static/native objects. Only collector-owned objects
  // enter the intrusive mark stack, so marking never allocates auxiliary heap.
  if (found == object_index.end() || found->second->marked) return;
  ObjectHeader* header = found->second;
  header->marked = true;
  header->mark_next = context->pending;
  context->pending = header;
}

void count_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* counts = static_cast<std::unordered_map<void*, uint64_t>*>(opaque_context);
  if (object_index.find(object) != object_index.end()) ++(*counts)[object];
}

void collect_locked() {
  ++statistics.collections;
  MarkContext context;
  for (const auto& root : roots) {
    auto* bytes = static_cast<const unsigned char*>(root.first);
    if (root.second.reference_offsets.empty()) continue;
    for (size_t offset : root.second.reference_offsets) {
      if (offset > root.second.object_size ||
          sizeof(void*) > root.second.object_size - offset) continue;
      void* reference = nullptr;
      std::memcpy(&reference, bytes + offset, sizeof(reference));
      enqueue_reference(reference, &context);
    }
  }
  for (const auto& pinned : pinned_objects) {
    if (pinned.second != 0) enqueue_reference(pinned.first, &context);
  }

  while (context.pending) {
    ObjectHeader* header = context.pending;
    context.pending = header->mark_next;
    header->mark_next = nullptr;
    void* object = header->object;
    if (header->trace) {
      header->trace(object, header->size, enqueue_reference, &context);
    }
    if (!header->reference_offsets.empty() && header->element_size != 0) {
      const auto* bytes = static_cast<const unsigned char*>(object);
      for (size_t base = 0; base + header->element_size <= header->size;
           base += header->element_size) {
        for (size_t offset : header->reference_offsets) {
          if (offset > header->element_size ||
              sizeof(void*) > header->element_size - offset) continue;
          void* child = nullptr;
          std::memcpy(&child, bytes + base + offset, sizeof(child));
          enqueue_reference(child, &context);
        }
      }
    }
  }

  ObjectHeader** link = &objects;
  while (*link) {
    ObjectHeader* header = *link;
    if (!header->marked) {
      *link = header->next;
      object_index.erase(header->object);
      statistics.collected_bytes += header->size;
      --statistics.live_objects;
      std::free(header->object);
      delete header;
    } else {
      header->marked = false;
      link = &header->next;
    }
  }
  statistics.allocated_bytes = 0;
  for (ObjectHeader* header = objects; header; header = header->next) {
    statistics.allocated_bytes += header->size;
  }
  bytes_since_collection = 0;
}

void* allocate_locked(size_t bytes, size_t element_size, nrq_gc_trace_fn trace,
                      const size_t* reference_offsets, size_t reference_count) {
  if (bytes == 0 || bytes > SIZE_MAX - sizeof(ObjectHeader) || element_size == 0)
    return nullptr;
  if (automatic_collection_enabled && bytes_since_collection >= collection_threshold)
    collect_locked();

  auto* header = new (std::nothrow) ObjectHeader();
  if (!header) return nullptr;
  void* object = std::calloc(1, bytes);
  if (!object) {
    delete header;
    return nullptr;
  }
  try {
    if (reference_count != 0) {
      if (!reference_offsets || reference_count > SIZE_MAX / sizeof(size_t)) {
        std::free(object);
        delete header;
        return nullptr;
      }
      header->reference_offsets.assign(reference_offsets,
                                       reference_offsets + reference_count);
      for (size_t offset : header->reference_offsets) {
        if (offset > element_size || sizeof(void*) > element_size - offset) {
          std::free(object);
          delete header;
          return nullptr;
        }
      }
    }
    header->object = object;
    header->size = bytes;
    header->element_size = element_size;
    header->trace = trace;
    header->next = objects;
    object_index.emplace(object, header);
  } catch (...) {
    std::free(object);
    delete header;
    return nullptr;
  }
  objects = header;
  statistics.allocated_bytes += bytes;
  ++statistics.live_objects;
  bytes_since_collection += bytes;
  return object;
}

}  // namespace

extern "C" {

void* nrq_gc_allocate(size_t bytes, nrq_gc_trace_fn trace) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(bytes, bytes, trace, nullptr, 0);
}

void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(count * element_size, element_size, trace, nullptr, 0);
}

void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count) {
  if (count == 0 || element_size == 0 || count > SIZE_MAX / element_size)
    return nullptr;
  std::lock_guard<std::mutex> lock(gc_mutex);
  return allocate_locked(count * element_size, element_size, nullptr,
                         reference_offsets, reference_count);
}

void nrq_gc_root_register(void** slot) {
  static constexpr size_t pointer_offset = 0;
  nrq_gc_root_register_typed(slot, sizeof(void*), &pointer_offset, 1);
}

void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count) {
  if (!slot || object_size == 0 || reference_count == 0 || !reference_offsets)
    return;
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
      return;
    }
  } else if (found->second.object_size != object_size ||
             found->second.reference_offsets.size() != reference_count ||
             !std::equal(found->second.reference_offsets.begin(),
                         found->second.reference_offsets.end(), reference_offsets)) {
    return;
  }
  try {
    thread_roots.push_back(slot);
  } catch (...) {
    if (inserted) roots.erase(found);
    return;
  }
  ++found->second.references;
}

void nrq_gc_root_unregister(void** slot) {
  if (!slot) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = roots.find(slot);
  if (found != roots.end() && --found->second.references == 0) roots.erase(found);
  for (auto item = thread_roots.rbegin(); item != thread_roots.rend(); ++item) {
    if (*item == slot) {
      thread_roots.erase(std::next(item).base());
      break;
    }
  }
}

void nrq_gc_root_store(void** slot, void* value) {
  if (!slot) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  *slot = value;
}

void nrq_gc_root_copy(void* destination, const void* source, size_t bytes) {
  if (!destination || !source || bytes == 0) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  std::memcpy(destination, source, bytes);
}

uint64_t nrq_gc_root_frame_begin(void) {
  return static_cast<uint64_t>(thread_roots.size());
}

void nrq_gc_root_frame_end(uint64_t marker) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  const size_t target = marker > thread_roots.size()
      ? 0
      : static_cast<size_t>(marker);
  while (thread_roots.size() > target) {
    void* slot = thread_roots.back();
    thread_roots.pop_back();
    auto found = roots.find(slot);
    if (found != roots.end() && --found->second.references == 0) roots.erase(found);
  }
}

void nrq_gc_enable(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  automatic_collection_enabled = true;
}

void nrq_gc_disable(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  automatic_collection_enabled = false;
}

void nrq_gc_collect(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  collect_locked();
}

void nrq_gc_set_threshold(size_t bytes) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  collection_threshold = bytes == 0 ? 1 : bytes;
}

nrq_gc_stats nrq_gc_get_stats(void) {
  std::lock_guard<std::mutex> lock(gc_mutex);
  return statistics;
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
  if (object_index.find(object) == object_index.end()) return nullptr;
  try {
    ++pinned_objects[object];
    return object;
  } catch (...) {
    return nullptr;
  }
}

void nrq_gc_unpin(void* object) {
  if (!object) return;
  std::lock_guard<std::mutex> lock(gc_mutex);
  auto found = pinned_objects.find(object);
  if (found == pinned_objects.end()) return;
  if (--found->second == 0) pinned_objects.erase(found);
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
  if (!reference_diagnostics_enabled) return -1;
  if (!object || object_index.find(object) == object_index.end()) return 0;
  try {
    std::unordered_map<void*, uint64_t> counts;
    for (ObjectHeader* header = objects; header; header = header->next)
      if (header->trace)
        header->trace(header->object, header->size, count_reference, &counts);
    for (const auto& root : roots) {
      auto* bytes = static_cast<const unsigned char*>(root.first);
      for (size_t offset : root.second.reference_offsets) {
        if (offset > root.second.object_size ||
            sizeof(void*) > root.second.object_size - offset) continue;
        void* reference = nullptr;
        std::memcpy(&reference, bytes + offset, sizeof(reference));
        count_reference(reference, &counts);
      }
    }
    auto found = counts.find(object);
    return found == counts.end() ? 0 : static_cast<int64_t>(found->second);
  } catch (...) {
    return -1;
  }
}

}  // extern "C"
