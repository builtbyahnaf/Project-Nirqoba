#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    std::string filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
    if (argc > 1) {
        filename = argv[1];
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Cannot open " << filename << "\n";
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size + 1);
    file.read(buffer.data(), size);
    buffer[size] = '\0';
    file.close();

    auto t0 = std::chrono::high_resolution_clock::now();

    uint64_t total_rows = 0;
    uint64_t sum_id = 0;
    uint64_t sum_device_code = 0;
    uint64_t empty_cat_count = 0;
    uint64_t escaped_quotes_count = 0;

    char* ptr = buffer.data();
    while (*ptr && *ptr != '\n') ptr++;
    if (*ptr == '\n') ptr++;

    while (*ptr) {
        total_rows++;
        int col = 0;
        char* cell_start = ptr;
        bool in_quotes = false;
        bool row_has_escaped_quote = false;

        uint64_t col0_val = 0;
        uint64_t col4_val = 0;
        size_t col6_len = 0;

        while (*ptr) {
            char c = *ptr;
            if (c == '"') {
                if (in_quotes && *(ptr + 1) == '"') {
                    row_has_escaped_quote = true;
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

    auto t1 = std::chrono::high_resolution_clock::now();
    double parse_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-15\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "ROWS_PARSED: " << total_rows << "\n";
    std::cout << "TIME_MS: " << parse_ms << "\n";
    std::cout << "SUM_ID: " << sum_id << "\n";
    std::cout << "SUM_DEVICE: " << sum_device_code << "\n";
    std::cout << "EMPTY_CAT: " << empty_cat_count << "\n";
    std::cout << "CHECKSUM: " << (sum_id + sum_device_code) << "\n";
    std::cout << "STATUS: " << (total_rows == 5000000 ? "PASS" : "FAIL") << "\n";
    return 0;
}
