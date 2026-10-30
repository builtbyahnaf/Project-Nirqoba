// runtime/src/math.cpp
// Math helper implementations for Nirqoba Phase 2.
#include "nirqoba/runtime/math.h"

#include <cmath>

extern "C" {

int64_t nrq_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t result = 1;
    int64_t current = base;
    uint64_t power = static_cast<uint64_t>(exp);
    while (power > 0) {
        if (power & 1) {
            result *= current;
        }
        current *= current;
        power >>= 1;
    }
    return result;
}

double nrq_pow_f64(double base, double exp) {
    return std::pow(base, exp);
}

}  // extern "C"
