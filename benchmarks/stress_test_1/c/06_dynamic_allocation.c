#include <stdio.h>
#include <stdlib.h>

int main() {
    int count = 0;
    for (int i = 0; i < 1000000; i++) {
        char *ptr = (char*)malloc(64);
        free(ptr);
        count++;
    }
    printf("%d\n", count);
    return 0;
}
