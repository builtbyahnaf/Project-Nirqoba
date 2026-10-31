// runtime/include/nirqoba/runtime/io.h
// C ABI I/O functions exposed by the Nirqoba runtime.
//
// These declarations are the ABI boundary established in Phase 1 & 2.
// The compiler emits calls to these symbols; the runtime library provides
// the definitions. Nothing above this boundary may assume how the
// implementations work (WriteFile, stdio, etc.).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes `byte_len` bytes from `utf8` followed by a newline ('\n') to stdout.
int32_t nrq_print_string(const char* utf8, size_t byte_len);

// Writes a NUL-terminated UTF-8 string followed by a newline to stdout.
// Used when printing a `string` value (for example one returned by
// `file.read_text`), where the length is not known statically.
int32_t nrq_print_cstr(const char* utf8);

// Scalar printing helpers (Phase 2): format value followed by '\n' to stdout.
int32_t nrq_print_int(int64_t val);
int32_t nrq_print_uint(uint64_t val);
int32_t nrq_print_f64(double val);
int32_t nrq_print_f32(float val);
int32_t nrq_print_bool(int32_t val);
int32_t nrq_print_char(uint32_t code_point);

// Console input (Phase 4): reads up to max_bytes-1 bytes from stdin into out_buf,
// strips trailing '\r' and '\n', and writes the byte count to *out_bytes_read.
// Returns 0 on success or a Win32 error code on failure.
int32_t nrq_read_line(char* out_buf, size_t max_bytes, size_t* out_bytes_read);

// Initialises the Nirqoba runtime.
void nrq_runtime_init(void);

// Shuts down the Nirqoba runtime.
void nrq_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif
