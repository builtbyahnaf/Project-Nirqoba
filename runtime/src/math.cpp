// runtime/src/math.cpp
// Math helper implementations for Nirqoba Phase 2 & 4.
#include "nirqoba/runtime/math.h"

#include <cmath>

using namespace std;

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

// Phase 4 math primitives.
double nrq_sqrt(double val)  { return std::sqrt(val); }
double nrq_sin(double val)   { return std::sin(val); }
double nrq_cos(double val)   { return std::cos(val); }
double nrq_abs(double val)   { return std::fabs(val); }
double nrq_floor(double val) { return std::floor(val); }
double nrq_ceil(double val)  { return std::ceil(val); }

}  // extern "C"
