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

// Scalar printing helpers (Phase 2): format value followed by '\n' to stdout.
int32_t nrq_print_int(int64_t val);
int32_t nrq_print_uint(uint64_t val);
int32_t nrq_print_f64(double val);
int32_t nrq_print_f32(float val);
int32_t nrq_print_bool(int32_t val);
int32_t nrq_print_char(uint32_t code_point);

// Initialises the Nirqoba runtime.
void nrq_runtime_init(void);

// Shuts down the Nirqoba runtime.
void nrq_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif
