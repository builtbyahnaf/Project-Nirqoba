// runtime/include/nirqoba/runtime/system.h
// C ABI System and Time functions for Nirqoba Phase 5.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns milliseconds since Unix epoch (January 1, 1970).
int64_t nrq_time_now_ms(void);

// Exits the process with the given exit code.
void nrq_system_exit(int32_t exit_code);

#ifdef __cplusplus
}
#endif
