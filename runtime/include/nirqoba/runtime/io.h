// runtime/include/nirqoba/runtime/io.h
// C ABI I/O functions exposed by the Nirqoba runtime.
//
// These declarations are the ABI boundary established in Phase 1.
// The compiler emits calls to these symbols; the runtime library provides
// the definitions. Nothing above this boundary may assume how the
// implementations work (WriteFile, stdio, etc.).
//
// Thread-safety: nrq_print_string is not thread-safe in Phase 1.
//               Thread-aware I/O is scheduled for Phase 10.
//
// Ownership: the caller retains ownership of all pointer arguments.
//
// Error handling: both functions return 0 on success and a non-zero
// Windows error code on failure. The caller decides whether to propagate
// or ignore the error; the runtime never calls ExitProcess on I/O failure.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes `byte_len` bytes from `utf8` followed by a newline ('\n') to the
// standard output handle. Returns 0 on success, a non-zero Windows error
// code on failure. An empty string (byte_len == 0) writes only the newline.
//
// This function is the sole output primitive in Phase 1. The runtime ABI
// note in docs/architecture/06-runtime.md reserves the flush/buffer/GC-root
// hooks for Phase 6.5 without implementing them here.
int32_t nrq_print_string(const char* utf8, size_t byte_len);

// Initialises the Nirqoba runtime. Called once at the start of main() by
// the generated entry-point stub. Phase 1 has no state to initialise; the
// function exists so the ABI is stable when Phase 6.5 adds the GC.
void nrq_runtime_init(void);

// Shuts down the Nirqoba runtime. Called once at the end of main() before
// returning to the OS. Phase 1 flushes stdout; Phase 6.5 will drain the GC.
void nrq_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif
