#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main() {
    int size = 10000000;
    int64_t *arr = (int64_t*)malloc(size * sizeof(int64_t));
    for (int i = 0; i < size; i++) arr[i] = i;
    for (int p = 0; p < 5; p++) {
        for (int j = 0; j < size; j++) {
            arr[j] = (arr[j] * 3 + 7) ^ 1431655765LL;
        }
    }
    int64_t sum = 0;
    for (int k = 0; k < size; k++) sum += arr[k];
    printf("%lld\n", sum);
    free(arr);
    return 0;
}
