#ifndef _WIN32
#  error "sync_windows.cpp is the Windows-only synchronization runtime."
#endif

#include "nirqoba/runtime/sync.h"
#include "nirqoba/runtime/fatal.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace {

struct MutexResource {
  std::mutex state_mutex;
  std::condition_variable changed;
  std::thread::id owner;
  bool locked = false;
  bool closing = false;
};

std::mutex registry_mutex;
std::unordered_map<void*, std::shared_ptr<MutexResource>> resources;
uintptr_t next_handle = 1;

std::shared_ptr<MutexResource> find_mutex(void* handle) {
  std::lock_guard<std::mutex> lock(registry_mutex);
  auto found = resources.find(handle);
  return found == resources.end() ? nullptr : found->second;
}

}  // namespace

extern "C" {

void* nrq_mutex_create(void) {
  try {
    auto resource = std::make_shared<MutexResource>();
    std::lock_guard<std::mutex> lock(registry_mutex);
    if (next_handle == 0 || next_handle > UINTPTR_MAX - 1) return nullptr;
    void* handle = reinterpret_cast<void*>(next_handle++);
    resources.emplace(handle, std::move(resource));
    return handle;
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
  std::shared_ptr<MutexResource> resource;
  {
    std::lock_guard<std::mutex> registry_lock(registry_mutex);
    auto found = resources.find(handle);
    if (found == resources.end())
      nrq_runtime_fatal("mutex.destroy received an invalid or destroyed mutex");
    resource = found->second;
    std::lock_guard<std::mutex> state_lock(resource->state_mutex);
    if (resource->locked)
      nrq_runtime_fatal("mutex.destroy cannot destroy a locked mutex");
    resource->closing = true;
    resources.erase(found);
  }
  resource->changed.notify_all();
}

}  // extern "C"
