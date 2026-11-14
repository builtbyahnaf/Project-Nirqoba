#ifndef _WIN32
#  error "sync_windows.cpp is the Windows-only synchronization runtime."
#endif

#include "nirqoba/runtime/sync.h"
#include "nirqoba/runtime/fatal.h"

#include "runtime_shutdown.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct MutexResource {
  std::mutex state_mutex;
  std::condition_variable changed;
  std::thread::id owner;
  bool locked = false;
  bool closing = false;
};

struct MutexSlot {
  std::shared_ptr<MutexResource> resource;
  uint32_t generation = 0;
};

std::mutex registry_mutex;
std::vector<MutexSlot> mutex_slots;
uint32_t next_mutex_generation = 1;

bool decode_mutex_token(void* handle, uint32_t& index, uint32_t& generation) {
  const uintptr_t raw = reinterpret_cast<uintptr_t>(handle);
  const uint32_t encoded_index = static_cast<uint32_t>(raw & 0xffffffffu);
  generation = static_cast<uint32_t>(raw >> 32);
  if (encoded_index == 0 || generation == 0) return false;
  index = encoded_index - 1;
  return true;
}

std::shared_ptr<MutexResource> find_mutex(void* handle) {
  uint32_t index = 0;
  uint32_t generation = 0;
  if (!decode_mutex_token(handle, index, generation)) return nullptr;
  std::lock_guard<std::mutex> lock(registry_mutex);
  if (index >= mutex_slots.size()) return nullptr;
  MutexSlot& slot = mutex_slots[index];
  if (slot.generation != generation) return nullptr;
  return slot.resource;
}

}  // namespace

namespace nirq::runtime {

void release_mutex_resources() noexcept {
  std::vector<std::shared_ptr<MutexResource>> doomed;
  {
    std::lock_guard<std::mutex> lock(registry_mutex);
    doomed.reserve(mutex_slots.size());
    for (MutexSlot& slot : mutex_slots)
      if (slot.resource) doomed.push_back(std::move(slot.resource));
    mutex_slots.clear();
  }
  for (auto& resource : doomed) {
    std::lock_guard<std::mutex> state_lock(resource->state_mutex);
    resource->closing = true;
  }
  for (auto& resource : doomed) resource->changed.notify_all();
}

}  // namespace nirq::runtime

extern "C" {

void* nrq_mutex_create(void) {
  try {
    auto resource = std::make_shared<MutexResource>();
    std::lock_guard<std::mutex> lock(registry_mutex);
    size_t index = 0;
    while (index < mutex_slots.size() && mutex_slots[index].resource) ++index;
    if (index == mutex_slots.size()) mutex_slots.emplace_back();
    if (next_mutex_generation == 0) next_mutex_generation = 1;
    MutexSlot& slot = mutex_slots[index];
    slot.generation = next_mutex_generation++;
    slot.resource = std::move(resource);
    const uint64_t raw = (static_cast<uint64_t>(slot.generation) << 32) |
                         (static_cast<uint64_t>(index) + 1);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
  } catch (...) {
    return nullptr;
  }
}

void nrq_mutex_lock(void* handle) {
  std::shared_ptr<MutexResource> resource = find_mutex(handle);
  if (!resource) nrq_runtime_fatal("mutex.lock received an invalid mutex");
  std::unique_lock<std::mutex> lock(resource->state_mutex);
  if (resource->locked && resource->owner == std::this_thread::get_id())
    nrq_runtime_fatal("mutex.lock is not recursive");
  resource->changed.wait(lock, [&] { return !resource->locked || resource->closing; });
  if (resource->closing) nrq_runtime_fatal("mutex.lock received a destroyed mutex");
  resource->locked = true;
  resource->owner = std::this_thread::get_id();
}

void nrq_mutex_unlock(void* handle) {
  std::shared_ptr<MutexResource> resource = find_mutex(handle);
  if (!resource) nrq_runtime_fatal("mutex.unlock received an invalid mutex");
  {
    std::lock_guard<std::mutex> lock(resource->state_mutex);
    if (!resource->locked || resource->owner != std::this_thread::get_id())
      nrq_runtime_fatal("mutex.unlock must be called by the owning thread");
    resource->locked = false;
    resource->owner = {};
  }
  resource->changed.notify_one();
}

void nrq_mutex_destroy(void* handle) {
  // Compiler-generated cleanup runs after moves and explicit destruction with
  // the owning slot cleared. Treat that null sentinel as an already-released
  // owner while continuing to diagnose stale non-null tokens.
  if (!handle) return;
  uint32_t index = 0;
  uint32_t generation = 0;
  if (!decode_mutex_token(handle, index, generation))
    nrq_runtime_fatal("mutex.destroy received an invalid or destroyed mutex");
  std::shared_ptr<MutexResource> resource;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex);
    if (index >= mutex_slots.size() ||
        mutex_slots[index].generation != generation ||
        !mutex_slots[index].resource)
      nrq_runtime_fatal("mutex.destroy received an invalid or destroyed mutex");
    resource = std::move(mutex_slots[index].resource);
    std::lock_guard<std::mutex> state_lock(resource->state_mutex);
    if (resource->locked)
      nrq_runtime_fatal("mutex.destroy cannot destroy a locked mutex");
    resource->closing = true;
  }
  resource->changed.notify_all();
}

}  // extern "C"
