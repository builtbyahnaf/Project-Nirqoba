#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    const char* filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
    if (argc > 1) {
        filename = argv[1];
    }

    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", filename);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return 1;
    }
    fread(buf, 1, fsize, f);
    fclose(f);
    buf[fsize] = '\0';

    double t0 = bench_now_ms();

    uint64_t total_rows = 0;
    uint64_t sum_id = 0;
    uint64_t sum_device_code = 0;
    uint64_t empty_cat_count = 0;
    uint64_t escaped_quotes_count = 0;

    char* ptr = buf;
    // Skip header line
    while (*ptr && *ptr != '\n') ptr++;
    if (*ptr == '\n') ptr++;

    while (*ptr) {
        total_rows++;
        int col = 0;
        char* cell_start = ptr;
        int in_quotes = 0;
        int row_has_escaped_quote = 0;

        uint64_t col0_val = 0;
        uint64_t col4_val = 0;
        size_t col6_len = 0;

        while (*ptr) {
            char c = *ptr;
            if (c == '"') {
                if (in_quotes && *(ptr + 1) == '"') {
                    row_has_escaped_quote = 1;
                    ptr += 2;
                    continue;
                }
                in_quotes = !in_quotes;
                ptr++;
                continue;
            }

            if (!in_quotes && (c == ',' || c == '\n' || c == '\r')) {
                size_t cell_len = ptr - cell_start;
                if (col == 0) {
                    uint64_t num = 0;
                    for (char* k = cell_start; k < ptr; k++) {
                        num = num * 10 + (*k - '0');
                    }
                    col0_val = num;
                } else if (col == 4) {
                    uint64_t num = 0;
                    for (char* k = cell_start; k < ptr; k++) {
                        num = num * 10 + (*k - '0');
                    }
                    col4_val = num;
                } else if (col == 6) {
                    col6_len = cell_len;
                }

                col++;
                ptr++;
                if (c == '\r' && *ptr == '\n') ptr++;
                if (c == '\n' || c == '\r') break;
                cell_start = ptr;
            } else {
                ptr++;
            }
        }

        sum_id += col0_val;
        sum_device_code += col4_val;
        if (col6_len == 0) empty_cat_count++;
        if (row_has_escaped_quote) escaped_quotes_count++;
    }

    double t1 = bench_now_ms();
    double parse_ms = t1 - t0;

    printf("WORKLOAD: ST4-15\n");
    printf("LANGUAGE: C\n");
    printf("ROWS_PARSED: %" PRIu64 "\n", total_rows);
    printf("TIME_MS: %.3f\n", parse_ms);
    printf("SUM_ID: %" PRIu64 "\n", sum_id);
    printf("SUM_DEVICE: %" PRIu64 "\n", sum_device_code);
    printf("EMPTY_CAT: %" PRIu64 "\n", empty_cat_count);
    printf("CHECKSUM: %" PRIu64 "\n", sum_id + sum_device_code);
    printf("STATUS: %s\n", (total_rows == 5000000) ? "PASS" : "FAIL");

    free(buf);
    return 0;
}
