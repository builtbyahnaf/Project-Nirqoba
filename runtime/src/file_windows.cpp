// runtime/src/file_windows.cpp
// Windows x64 implementation of File I/O for Nirqoba Phase 5 using Win32 API directly.

#ifndef _WIN32
#  error "file_windows.cpp is the Windows-only runtime file implementation."
#endif

#include "nirqoba/runtime/file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_set>

static const char kEmptyString[] = "";
namespace {
struct FileResource { HANDLE handle; };
std::mutex file_resource_mutex;
std::unordered_set<FileResource*> file_resources;
}

extern "C" {

const char* nrq_file_read_text(const char* path) {
    if (!path || path[0] == '\0') return kEmptyString;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return kEmptyString;
    }

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(h, &file_size) || file_size.QuadPart < 0) {
        CloseHandle(h);
        return kEmptyString;
    }

    // Allocate buffer on process heap (+1 for null terminator).
    size_t bytes_to_read = static_cast<size_t>(file_size.QuadPart);
    char* buffer = static_cast<char*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes_to_read + 1));
    if (!buffer) {
        CloseHandle(h);
        return kEmptyString;
    }

    DWORD total_read = 0;
    while (total_read < bytes_to_read) {
        DWORD chunk = 0;
        DWORD to_read = static_cast<DWORD>(bytes_to_read - total_read);
        if (!ReadFile(h, buffer + total_read, to_read, &chunk, nullptr) || chunk == 0) {
            break;
        }
        total_read += chunk;
    }

    buffer[total_read] = '\0';
    CloseHandle(h);
    return buffer;
}

int32_t nrq_file_write_text(const char* path, const char* text) {
    if (!path) return ERROR_INVALID_PARAMETER;
    if (!text) text = kEmptyString;

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(GetLastError());
    }

    DWORD len = static_cast<DWORD>(strlen(text));
    DWORD written = 0;
    while (written < len) {
        DWORD chunk = 0;
        if (!WriteFile(h, text + written, len - written, &chunk, nullptr)) {
            int32_t err = static_cast<int32_t>(GetLastError());
            CloseHandle(h);
            return err;
        }
        written += chunk;
    }

    CloseHandle(h);
    return 0;
}

int32_t nrq_file_append_text(const char* path, const char* text) {
    if (!path) return ERROR_INVALID_PARAMETER;
    if (!text) text = kEmptyString;

    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return static_cast<int32_t>(GetLastError());
    }

    LARGE_INTEGER zero = {0};
    SetFilePointerEx(h, zero, nullptr, FILE_END);

    DWORD len = static_cast<DWORD>(strlen(text));
    DWORD written = 0;
    while (written < len) {
        DWORD chunk = 0;
        if (!WriteFile(h, text + written, len - written, &chunk, nullptr)) {
            int32_t err = static_cast<int32_t>(GetLastError());
            CloseHandle(h);
            return err;
        }
        written += chunk;
    }

    CloseHandle(h);
    return 0;
}

int32_t nrq_file_exists(const char* path) {
    if (!path || path[0] == '\0') return 0;
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return 0;
    return 1;
}

void* nrq_file_open_handle(const char* path, int32_t writable) {
    if (!path || !*path) return nullptr;
    const DWORD access = writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    const DWORD creation = writable ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE handle = CreateFileA(path, access, FILE_SHARE_READ, nullptr, creation,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    auto* resource = static_cast<FileResource*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FileResource)));
    if (!resource) { CloseHandle(handle); return nullptr; }
    resource->handle = handle;
    try {
        std::lock_guard<std::mutex> lock(file_resource_mutex);
        file_resources.insert(resource);
    } catch (...) {
        CloseHandle(handle); HeapFree(GetProcessHeap(), 0, resource); return nullptr;
    }
    return resource;
}

int32_t nrq_file_close_handle(void* opaque) {
    if (!opaque) return 0;
    auto* resource = static_cast<FileResource*>(opaque);
    std::lock_guard<std::mutex> lock(file_resource_mutex);
    auto found = file_resources.find(resource);
    if (found == file_resources.end()) return ERROR_INVALID_HANDLE;
    file_resources.erase(found);
    const BOOL closed = CloseHandle(resource->handle);
    HeapFree(GetProcessHeap(), 0, resource);
    return closed ? 0 : static_cast<int32_t>(GetLastError());
}

int32_t nrq_file_write_handle(void* opaque, const char* text) {
    if (!opaque) return ERROR_INVALID_HANDLE;
    auto* resource = static_cast<FileResource*>(opaque);
    std::lock_guard<std::mutex> lock(file_resource_mutex);
    if (file_resources.find(resource) == file_resources.end()) return ERROR_INVALID_HANDLE;
    if (!text) text = kEmptyString;
    const size_t length = std::strlen(text);
    size_t offset = 0;
    while (offset < length) {
        const DWORD request = static_cast<DWORD>(std::min<size_t>(length - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(resource->handle, text + offset, request, &written, nullptr))
            return static_cast<int32_t>(GetLastError());
        if (written == 0) return ERROR_WRITE_FAULT;
        offset += written;
    }
    return 0;
}

int64_t nrq_file_read_handle(void* opaque, void* destination, size_t capacity) {
    if (!opaque || (!destination && capacity != 0)) return -ERROR_INVALID_PARAMETER;
    if (capacity == 0) return 0;
    auto* resource = static_cast<FileResource*>(opaque);
    std::lock_guard<std::mutex> lock(file_resource_mutex);
    if (file_resources.find(resource) == file_resources.end()) return -ERROR_INVALID_HANDLE;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(capacity, MAXDWORD));
    DWORD count = 0;
    if (!ReadFile(resource->handle, destination, request, &count, nullptr))
        return -static_cast<int64_t>(GetLastError());
    return static_cast<int64_t>(count);
}

}  // extern "C"
