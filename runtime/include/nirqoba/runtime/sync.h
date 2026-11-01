// Opaque native synchronization resources.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* nrq_mutex_create(void);
void nrq_mutex_lock(void* mutex);
void nrq_mutex_unlock(void* mutex);
void nrq_mutex_destroy(void* mutex);

#ifdef __cplusplus
}
#endif
