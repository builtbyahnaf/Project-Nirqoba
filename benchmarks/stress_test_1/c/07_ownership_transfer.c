#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Simulate ownership transfer: caller allocates, passes to function, function sums and frees */
int64_t consume_array(int64_t *arr, int sz) {
    int64_t sum = 0;
    for (int i = 0; i < sz; i++) sum += arr[i];
    free(arr);
    return sum;
}

int main() {
    int64_t total = 0;
    for (int i = 0; i < 1000000; i++) {
        int64_t *arr = (int64_t*)malloc(8 * sizeof(int64_t));
        for (int j = 0; j < 8; j++) arr[j] = i + j;
        total += consume_array(arr, 8);
    }
    printf("%lld\n", total);
    return 0;
}
