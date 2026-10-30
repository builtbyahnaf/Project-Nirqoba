// runtime/include/nirqoba/runtime/math.h
// C ABI math functions exposed by the Nirqoba runtime.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Integer/float exponentiation (Phase 2).
int64_t nrq_pow_i64(int64_t base, int64_t exp);
double  nrq_pow_f64(double base, double exp);

// Standard math functions (Phase 4).
double nrq_sqrt(double val);
double nrq_sin(double val);
double nrq_cos(double val);
double nrq_abs(double val);
double nrq_floor(double val);
double nrq_ceil(double val);

#ifdef __cplusplus
}
#endif
