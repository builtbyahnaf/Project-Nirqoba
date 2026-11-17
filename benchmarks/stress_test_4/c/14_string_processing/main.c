#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t size_mb = 256;
    if (argc > 1) {
        size_mb = (size_t)atoll(argv[1]);
    }

    size_t total_bytes = size_mb * 1024ULL * 1024ULL;
    char* corpus = (char*)malloc(total_bytes + 1);
    if (!corpus) return 1;

    const char* pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    size_t pat_len = strlen(pattern);
    for (size_t i = 0; i < total_bytes; i += pat_len) {
        size_t chunk = (total_bytes - i < pat_len) ? (total_bytes - i) : pat_len;
        memcpy(corpus + i, pattern, chunk);
    }
    corpus[total_bytes] = '\0';

    // 1. Character count
    double t0 = bench_now_ms();
    uint64_t char_count = 0;
    for (size_t i = 0; i < total_bytes; i++) {
        if (corpus[i] == 'e') char_count++;
    }
    double t1 = bench_now_ms();
    double char_ms = t1 - t0;

    // 2. Substring search
    double t2 = bench_now_ms();
    uint64_t sub_count = 0;
    const char* target = "benchmark";
    size_t target_len = strlen(target);
    const char* p = corpus;
    while ((p = strstr(p, target)) != NULL) {
        sub_count++;
        p += target_len;
    }
    double t3 = bench_now_ms();
    double sub_ms = t3 - t2;

    // 3. Whitespace tokenization
    double t4 = bench_now_ms();
    uint64_t token_count = 0;
    int in_token = 0;
    for (size_t i = 0; i < total_bytes; i++) {
        unsigned char c = (unsigned char)corpus[i];
        if (c == ' ' || c == '\n' || c == '\t') {
            in_token = 0;
        } else if (!in_token) {
            in_token = 1;
            token_count++;
        }
    }
    double t5 = bench_now_ms();
    double tok_ms = t5 - t4;

    // 4. Case conversion
    double t6 = bench_now_ms();
    for (size_t i = 0; i < total_bytes; i++) {
        corpus[i] = (char)toupper((unsigned char)corpus[i]);
    }
    double t7 = bench_now_ms();
    double case_ms = t7 - t6;

    double total_ms = char_ms + sub_ms + tok_ms + case_ms;
    printf("WORKLOAD: ST4-14\n");
    printf("LANGUAGE: C\n");
    printf("CORPUS_MIB: %zu\n", size_mb);
    printf("CHAR_COUNT_MS: %.3f\n", char_ms);
    printf("SUBSTRING_MS: %.3f\n", sub_ms);
    printf("TOKENIZE_MS: %.3f\n", tok_ms);
    printf("CASE_CONV_MS: %.3f\n", case_ms);
    printf("TIME_MS: %.3f\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\n", char_count + sub_count + token_count);
    printf("STATUS: PASS\n");

    free(corpus);
    return 0;
}
