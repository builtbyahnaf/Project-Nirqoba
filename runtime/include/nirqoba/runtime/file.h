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

// Reads the whole file into a buffer this call owns. The buffer comes from the
// deterministic allocator and must be handed back with nrq_file_text_release;
// the language surface (`file.read_text_owned`) binds that release as the
// value's destructor, so a lexical scope releases it without the programmer
// naming it. An unreadable file yields an owned empty string rather than null,
// so the release path never has to special-case failure.
char* nrq_file_read_text_owned(const char* path);
// Releases a buffer from nrq_file_read_text_owned. The ownership registry
// rejects a pointer it did not hand out, so a double release is a diagnostic
// rather than a quiet corruption.
void nrq_file_text_release(char* text);
// Buffers currently owned by the language. Exists so a leak can be asserted in a
// test instead of inferred from a working-set number.
int32_t nrq_file_text_live_count(void);
// Allocates `bytes` bytes of owned text from the deterministic allocator and
// counts it as live owned text, so the count above covers every buffer the
// language can be given, not only the ones a file read produced. The string
// operations build their results here and hand them out as `string_owned`; the
// matching release is nrq_file_text_release, exactly like a read buffer.
char* nrq_owned_text_allocate(int64_t bytes);

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
