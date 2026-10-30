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

#ifdef __cplusplus
}
#endif
