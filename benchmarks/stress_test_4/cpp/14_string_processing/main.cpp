#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cctype>

int main(int argc, char** argv) {
    size_t size_mb = 256;
    if (argc > 1) {
        size_mb = std::stoull(argv[1]);
    }

    size_t total_bytes = size_mb * 1024ULL * 1024ULL;
    std::string pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    size_t repeats = total_bytes / pattern.length() + 1;
    std::string corpus;
    corpus.reserve(total_bytes);
    while (corpus.length() < total_bytes) {
        corpus.append(pattern);
    }
    corpus.resize(total_bytes);

    // 1. Character count
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t char_count = 0;
    for (char c : corpus) {
        if (c == 'e') char_count++;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double char_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 2. Substring search
    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t sub_count = 0;
    std::string target = "benchmark";
    size_t pos = 0;
    while ((pos = corpus.find(target, pos)) != std::string::npos) {
        sub_count++;
        pos += target.length();
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double sub_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    // 3. Whitespace tokenization
    auto t4 = std::chrono::high_resolution_clock::now();
    uint64_t token_count = 0;
    bool in_token = false;
    for (char c : corpus) {
        if (c == ' ' || c == '\n' || c == '\t') {
            in_token = false;
        } else if (!in_token) {
            in_token = true;
            token_count++;
        }
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double tok_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    // 4. Case conversion
    auto t6 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < total_bytes; i++) {
        corpus[i] = (char)std::toupper((unsigned char)corpus[i]);
    }
    auto t7 = std::chrono::high_resolution_clock::now();
    double case_ms = std::chrono::duration<double, std::milli>(t7 - t6).count();

    double total_ms = char_ms + sub_ms + tok_ms + case_ms;
    std::cout << "WORKLOAD: ST4-14\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "CORPUS_MIB: " << size_mb << "\n";
    std::cout << "CHAR_COUNT_MS: " << char_ms << "\n";
    std::cout << "SUBSTRING_MS: " << sub_ms << "\n";
    std::cout << "TOKENIZE_MS: " << tok_ms << "\n";
    std::cout << "CASE_CONV_MS: " << case_ms << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "CHECKSUM: " << (char_count + sub_count + token_count) << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
