// Deterministic Windows file mapping handles for the optional memory API.
#ifndef _WIN32
#  error "mapping_windows.cpp is the Windows-only memory mapping implementation."
#endif

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/fatal.h"

#include "runtime_shutdown.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace {
struct Mapping {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE section = nullptr;
  void* view = nullptr;
  size_t bytes = 0;
  bool writable = false;
  bool closed = false;
  std::mutex operation_mutex;
};
std::mutex mapping_mutex;
std::unordered_map<void*, std::shared_ptr<Mapping>> mappings;

std::shared_ptr<Mapping> find_mapping(void* address) {
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  return found == mappings.end() ? nullptr : found->second;
}

std::wstring utf8_to_wide(const char* path) {
  if (!path || !*path) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        path, -1, nullptr, 0);
  if (needed <= 1) return {};
  std::wstring result(static_cast<size_t>(needed), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                           result.data(), needed)) return {};
  result.pop_back();
  return result;
}
}  // namespace

namespace nirq::runtime {

void release_mapping_resources(bool wait) noexcept {
  std::vector<std::shared_ptr<Mapping>> doomed;
  {
    std::unique_lock<std::mutex> lock(mapping_mutex, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return;
    doomed.reserve(mappings.size());
    for (auto& entry : mappings) doomed.push_back(std::move(entry.second));
    mappings.clear();
  }
  for (auto& mapping : doomed) {
    std::lock_guard<std::mutex> operation_lock(mapping->operation_mutex);
    if (mapping->closed) continue;
    if (mapping->view) UnmapViewOfFile(mapping->view);
    if (mapping->section) CloseHandle(mapping->section);
    if (mapping->file != INVALID_HANDLE_VALUE) CloseHandle(mapping->file);
    mapping->view = nullptr;
    mapping->section = nullptr;
    mapping->file = INVALID_HANDLE_VALUE;
    mapping->closed = true;
  }
}

}  // namespace nirq::runtime

extern "C" {

void* nrq_memory_map_file(const char* path, int32_t writable) {
  try {
    std::wstring wide_path = utf8_to_wide(path);
    if (wide_path.empty()) return nullptr;
    HANDLE file = CreateFileW(wide_path.c_str(),
        writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
        static_cast<unsigned long long>(file_size.QuadPart) > SIZE_MAX) {
      CloseHandle(file);
      return nullptr;
    }
    HANDLE section = CreateFileMappingW(file, nullptr,
        writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (!section) {
      CloseHandle(file);
      return nullptr;
    }
    void* address = MapViewOfFile(section,
        writable ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
    if (!address) {
      CloseHandle(section);
      CloseHandle(file);
      return nullptr;
    }
    try {
      auto mapping = std::make_shared<Mapping>();
      mapping->file = file;
      mapping->section = section;
      mapping->view = address;
      mapping->bytes = static_cast<size_t>(file_size.QuadPart);
      mapping->writable = writable != 0;
      std::lock_guard<std::mutex> lock(mapping_mutex);
      if (!mappings.emplace(address, std::move(mapping)).second) {
        UnmapViewOfFile(address);
        CloseHandle(section);
        CloseHandle(file);
        return nullptr;
      }
    } catch (...) {
      UnmapViewOfFile(address);
      CloseHandle(section);
      CloseHandle(file);
      return nullptr;
    }
    return address;
  } catch (...) {
    return nullptr;
  }
}

int32_t nrq_memory_unmap_file(void* address) {
  if (!address) return 0;
  std::shared_ptr<Mapping> mapping = find_mapping(address);
  if (!mapping) return 0;
  {
    std::lock_guard<std::mutex> operation_lock(mapping->operation_mutex);
    if (mapping->closed || !UnmapViewOfFile(mapping->view)) return 0;
    mapping->view = nullptr;
    mapping->closed = true;
    CloseHandle(mapping->section);
    CloseHandle(mapping->file);
    mapping->section = nullptr;
    mapping->file = INVALID_HANDLE_VALUE;
  }
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  if (found != mappings.end() && found->second == mapping) mappings.erase(found);
  return 1;
}

uint64_t nrq_memory_mapping_size(void* address) {
  std::shared_ptr<Mapping> mapping = find_mapping(address);
  if (!mapping) return 0;
  std::lock_guard<std::mutex> operation_lock(mapping->operation_mutex);
  return mapping->closed ? 0 : static_cast<uint64_t>(mapping->bytes);
}

int32_t nrq_memory_mapping_read(void* address, size_t offset, size_t bytes,
                                void* output) {
  if (!output) return 0;
  std::shared_ptr<Mapping> mapping = find_mapping(address);
  if (!mapping) {
    nrq_runtime_fatal("mapped file read is outside a live mapping");
    return 0;
  }
  std::lock_guard<std::mutex> operation_lock(mapping->operation_mutex);
  if (mapping->closed || offset > mapping->bytes ||
      bytes > mapping->bytes - offset) {
    nrq_runtime_fatal("mapped file read is outside a live mapping");
    return 0;
  }
  std::memcpy(output, static_cast<const unsigned char*>(mapping->view) + offset,
              bytes);
  return 1;
}

int32_t nrq_memory_mapping_write(void* address, size_t offset, size_t bytes,
                                 const void* input) {
  if (!input) return 0;
  std::shared_ptr<Mapping> mapping = find_mapping(address);
  if (!mapping) {
    nrq_runtime_fatal("mapped file write is invalid or outside a writable mapping");
    return 0;
  }
  std::lock_guard<std::mutex> operation_lock(mapping->operation_mutex);
  if (mapping->closed || !mapping->writable || offset > mapping->bytes ||
      bytes > mapping->bytes - offset) {
    nrq_runtime_fatal("mapped file write is invalid or outside a writable mapping");
    return 0;
  }
  std::memcpy(static_cast<unsigned char*>(mapping->view) + offset, input, bytes);
  return 1;
}

}  // extern "C"
