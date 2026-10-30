// runtime/src/file_windows.cpp
// Windows x64 implementation of File I/O for Nirqoba Phase 5 using Win32 API directly.

#ifndef _WIN32
#  error "file_windows.cpp is the Windows-only runtime file implementation."
#endif

#include "nirqoba/runtime/file.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>

static const char kEmptyString[] = "";

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

}  // extern "C"
