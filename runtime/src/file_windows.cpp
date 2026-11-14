// runtime/src/file_windows.cpp
// Windows x64 implementation of File I/O for Nirqoba Phase 5 using Win32 API directly.

#ifndef _WIN32
#  error "file_windows.cpp is the Windows-only runtime file implementation."
#endif

#include "nirqoba/runtime/file.h"

#include "runtime_buffers.h"
#include "runtime_shutdown.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

static const char kEmptyString[] = "";
namespace {
struct FileResource {
    explicit FileResource(HANDLE value) : handle(value) {}
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::mutex operation_mutex;
};

// A token carries a slot index in its low half and a generation in its high
// half. Closing a handle clears the slot; reusing that slot increments the
// generation, so a stale token from before the close can never name the new
// resource. A node-based map allocated and freed once per open/close just to
// hold this pair; the slot table stores it inline and keeps lookup O(1).
struct FileSlot {
    std::shared_ptr<FileResource> resource;
    uint32_t generation = 0;
};

std::mutex file_resource_mutex;
std::vector<FileSlot> file_slots;
uint32_t next_file_generation = 1;

bool decode_file_token(void* token, uint32_t& index, uint32_t& generation) {
    const uintptr_t raw = reinterpret_cast<uintptr_t>(token);
    const uint32_t encoded_index = static_cast<uint32_t>(raw & 0xffffffffu);
    generation = static_cast<uint32_t>(raw >> 32);
    if (encoded_index == 0 || generation == 0) return false;
    index = encoded_index - 1;
    return true;
}

std::shared_ptr<FileResource> find_file_resource(void* token) {
    uint32_t index = 0;
    uint32_t generation = 0;
    if (!decode_file_token(token, index, generation)) return nullptr;
    std::lock_guard<std::mutex> lock(file_resource_mutex);
    if (index >= file_slots.size()) return nullptr;
    FileSlot& slot = file_slots[index];
    if (slot.generation != generation) return nullptr;
    return slot.resource;
}
}

namespace nirq::runtime {

void release_file_resources() noexcept {
    std::vector<std::shared_ptr<FileResource>> doomed;
    {
        std::lock_guard<std::mutex> lock(file_resource_mutex);
        doomed.reserve(file_slots.size());
        for (auto& slot : file_slots)
            if (slot.resource) doomed.push_back(std::move(slot.resource));
        file_slots.clear();
    }
    for (auto& resource : doomed) {
        std::lock_guard<std::mutex> operation_lock(resource->operation_mutex);
        if (resource->handle != INVALID_HANDLE_VALUE) {
            CloseHandle(resource->handle);
            resource->handle = INVALID_HANDLE_VALUE;
        }
    }
}

}  // namespace nirq::runtime

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

    // The returned string is borrowed by the language, so its lifetime is the
    // process. Allocate it from the runtime-owned arena; shutdown releases it.
    size_t bytes_to_read = static_cast<size_t>(file_size.QuadPart);
    char* buffer = nirq::runtime::allocate_process_string(bytes_to_read + 1);
    if (!buffer) {
        CloseHandle(h);
        return kEmptyString;
    }
    buffer[bytes_to_read] = '\0';

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
    try {
        auto resource = std::make_shared<FileResource>(handle);
        std::lock_guard<std::mutex> lock(file_resource_mutex);
        size_t index = 0;
        while (index < file_slots.size() && file_slots[index].resource) ++index;
        if (index == file_slots.size()) file_slots.emplace_back();
        if (next_file_generation == 0) next_file_generation = 1;
        FileSlot& slot = file_slots[index];
        slot.generation = next_file_generation++;
        slot.resource = std::move(resource);
        const uint64_t raw = (static_cast<uint64_t>(slot.generation) << 32) |
                             (static_cast<uint64_t>(index) + 1);
        return reinterpret_cast<void*>(static_cast<uintptr_t>(raw));
    } catch (...) {
        CloseHandle(handle);
        return nullptr;
    }
}

int32_t nrq_file_close_handle(void* opaque) {
    if (!opaque) return 0;
    uint32_t index = 0;
    uint32_t generation = 0;
    if (!decode_file_token(opaque, index, generation)) return ERROR_INVALID_HANDLE;
    std::shared_ptr<FileResource> resource;
    {
        std::lock_guard<std::mutex> lock(file_resource_mutex);
        if (index >= file_slots.size()) return ERROR_INVALID_HANDLE;
        FileSlot& slot = file_slots[index];
        if (slot.generation != generation || !slot.resource) return ERROR_INVALID_HANDLE;
        resource = std::move(slot.resource);
    }
    std::lock_guard<std::mutex> operation_lock(resource->operation_mutex);
    if (resource->handle == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    const BOOL closed = CloseHandle(resource->handle);
    const DWORD error = closed ? ERROR_SUCCESS : GetLastError();
    resource->handle = INVALID_HANDLE_VALUE;
    return static_cast<int32_t>(error);
}

int32_t nrq_file_write_handle(void* opaque, const char* text) {
    if (!opaque) return ERROR_INVALID_HANDLE;
    std::shared_ptr<FileResource> resource = find_file_resource(opaque);
    if (!resource) return ERROR_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock(resource->operation_mutex);
    if (resource->handle == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
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
    std::shared_ptr<FileResource> resource = find_file_resource(opaque);
    if (!resource) return -ERROR_INVALID_HANDLE;
    std::lock_guard<std::mutex> operation_lock(resource->operation_mutex);
    if (resource->handle == INVALID_HANDLE_VALUE) return -ERROR_INVALID_HANDLE;
    const DWORD request = static_cast<DWORD>(std::min<size_t>(capacity, MAXDWORD));
    DWORD count = 0;
    if (!ReadFile(resource->handle, destination, request, &count, nullptr))
        return -static_cast<int64_t>(GetLastError());
    return static_cast<int64_t>(count);
}

}  // extern "C"
