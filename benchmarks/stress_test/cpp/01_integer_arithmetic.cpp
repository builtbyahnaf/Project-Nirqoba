#include <cstdio>
#include <cstdint>

int main() {
    int64_t total = 0;
    for (int64_t i = 0; i < 500000000LL; i++) {
        total = ((total + i) * 3) ^ (total / 7);
    }
    printf("%lld\n", total);
    return 0;
}
