// runtime/include/nirqoba/runtime/ffi.h
// Release path for storage that a C function handed to Nirqoba.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Releases a buffer that foreign code allocated and transferred to Nirqoba.
//
// The runtime's own releases (nrq_file_text_release, nrq_array_free) hand
// pointers back to the deterministic allocator that issued them and reject
// anything else, so a C allocation must not travel through them. This is the one
// release the language emits for storage it did not allocate, and it is
// deliberately the C allocator's own `free`. Null is a no-op: a C function may
// report failure that way, and the release path never special-cases it.
void nrq_ffi_free(void* ptr);

#ifdef __cplusplus
}
#endif
