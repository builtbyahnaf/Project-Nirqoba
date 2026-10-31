// Deterministic Windows file mapping handles for the optional memory API.
#ifndef _WIN32
#  error "mapping_windows.cpp is the Windows-only memory mapping implementation."
#endif

#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/fatal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mutex>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace std;

namespace {
struct Mapping {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE section = nullptr;
  size_t bytes = 0;
  bool writable = false;
};
std::mutex mapping_mutex;
std::unordered_map<void*, Mapping> mappings;

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
      std::lock_guard<std::mutex> lock(mapping_mutex);
      mappings.emplace(address, Mapping{file, section,
          static_cast<size_t>(file_size.QuadPart), writable != 0});
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
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  if (found == mappings.end()) return 0;
  const Mapping mapping = found->second;
  if (!UnmapViewOfFile(address)) return 0;
  mappings.erase(found);
  CloseHandle(mapping.section);
  CloseHandle(mapping.file);
  return 1;
}

uint64_t nrq_memory_mapping_size(void* address) {
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  return found == mappings.end() ? 0 : static_cast<uint64_t>(found->second.bytes);
}

int32_t nrq_memory_mapping_read(void* address, size_t offset, size_t bytes,
                                void* output) {
  if (!output) return 0;
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  if (found == mappings.end() || offset > found->second.bytes ||
      bytes > found->second.bytes - offset) {
    nrq_runtime_fatal("mapped file read is outside a live mapping");
    return 0;
  }
  std::memcpy(output, static_cast<const unsigned char*>(address) + offset, bytes);
  return 1;
}

int32_t nrq_memory_mapping_write(void* address, size_t offset, size_t bytes,
                                 const void* input) {
  if (!input) return 0;
  std::lock_guard<std::mutex> lock(mapping_mutex);
  auto found = mappings.find(address);
  if (found == mappings.end() || !found->second.writable ||
      offset > found->second.bytes || bytes > found->second.bytes - offset) {
    nrq_runtime_fatal("mapped file write is invalid or outside a writable mapping");
    return 0;
  }
  std::memcpy(static_cast<unsigned char*>(address) + offset, input, bytes);
  return 1;
}

}  // extern "C"
