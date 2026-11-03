#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main() {
    int size = 10000000;
    unsigned char *buf = (unsigned char*)malloc(size);

    for (int i = 0; i < size; i++) {
        int mod = i % 40;
        if (mod == 39) buf[i] = 32;
        else buf[i] = (unsigned char)((mod % 26) + 97);
    }

    int64_t words = 0, vowels = 0;
    for (int j = 0; j < size; j++) {
        unsigned char b = buf[j];
        if (b == 32) { words++; }
        else if (b=='a'||b=='e'||b=='i'||b=='o'||b=='u') { vowels++; }
    }

    for (int k = 0; k < size; k++) {
        unsigned char ch = buf[k];
        if (ch >= 97 && ch <= 122) buf[k] = (unsigned char)(((ch - 97 + 13) % 26) + 97);
    }

    uint64_t hash = 14695981039346656037ULL;
    for (int m = 0; m < size; m++) {
        hash = (hash ^ buf[m]) * 1099511628211ULL;
    }

    printf("%lld\n", words);
    printf("%lld\n", vowels);
    printf("%llu\n", hash);
    free(buf);
    return 0;
}
