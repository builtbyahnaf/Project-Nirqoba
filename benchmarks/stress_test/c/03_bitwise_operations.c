#include <stdio.h>
#include <stdint.h>

int main() {
    int64_t state = 123456789;
    for (int64_t i = 0; i < 100000000LL; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state += i;
    }
    printf("%lld\n", state);
    return 0;
}
