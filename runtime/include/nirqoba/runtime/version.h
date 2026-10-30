// runtime/include/nirqoba/runtime/version.h
// Runtime version query.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Returns a NUL-terminated version string, e.g. "0.1.0".
// The returned pointer is valid for the lifetime of the process; the caller
// must not free it.
const char* nrq_runtime_version(void);

#ifdef __cplusplus
}
#endif
