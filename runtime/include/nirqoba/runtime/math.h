// runtime/include/nirqoba/runtime/math.h
// C ABI math functions exposed by the Nirqoba runtime.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t nrq_pow_i64(int64_t base, int64_t exp);
double  nrq_pow_f64(double base, double exp);

#ifdef __cplusplus
}
#endif
