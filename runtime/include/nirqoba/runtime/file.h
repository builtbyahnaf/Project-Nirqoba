// runtime/include/nirqoba/runtime/file.h
// C ABI File I/O functions for Nirqoba Phase 5.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the entire content of a UTF-8 text file into a null-terminated buffer.
// Returns a pointer to the buffer, or an empty string on error.
const char* nrq_file_read_text(const char* path);

// Writes text to path, creating or overwriting it. Returns 0 on success, or Win32 error code.
int32_t nrq_file_write_text(const char* path, const char* text);

// Appends text to path, creating it if it doesn't exist. Returns 0 on success, or Win32 error code.
int32_t nrq_file_append_text(const char* path, const char* text);

// Checks if file exists and is a regular file. Returns 1 if exists, 0 otherwise.
int32_t nrq_file_exists(const char* path);

// Opaque deterministic file resource. Close with nrq_file_close_handle; the
// Nirqoba surface pairs this with `defer file.close(handle)`.
void* nrq_file_open_handle(const char* path, int32_t writable);
int32_t nrq_file_close_handle(void* resource);
int32_t nrq_file_write_handle(void* resource, const char* text);
// Reads at most capacity bytes. Returns bytes read, or a negative Win32 error.
int64_t nrq_file_read_handle(void* resource, void* destination, size_t capacity);

#ifdef __cplusplus
}
#endif
