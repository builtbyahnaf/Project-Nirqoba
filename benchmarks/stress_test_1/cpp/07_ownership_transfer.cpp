#include <cstdio>
#include <cstdint>
#include <memory>
#include <vector>

int64_t consume_array(std::unique_ptr<std::vector<int64_t>> arr) {
    int64_t sum = 0;
    for (auto v : *arr) sum += v;
    return sum;  // arr freed here
}

int main() {
    int64_t total = 0;
    for (int i = 0; i < 1000000; i++) {
        auto arr = std::make_unique<std::vector<int64_t>>(8);
        for (int j = 0; j < 8; j++) (*arr)[j] = i + j;
        total += consume_array(std::move(arr));
    }
    printf("%lld\n", total);
    return 0;
}
