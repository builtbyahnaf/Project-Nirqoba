#include <iostream>
#include <cstdint>
#include <chrono>
#include <memory>

struct Node {
    uint64_t a;
    uint64_t b;
    std::unique_ptr<Node> next;
    Node(uint64_t val_a, uint64_t val_b) : a(val_a), b(val_b), next(nullptr) {}
};

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::unique_ptr<Node> head = nullptr;
    Node* tail = nullptr;
    for (size_t i = 0; i < n; i++) {
        auto node = std::make_unique<Node>((uint64_t)i, (uint64_t)i * 2);
        if (!head) {
            head = std::move(node);
            tail = head.get();
        } else {
            tail->next = std::move(node);
            tail = tail->next.get();
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double alloc_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t checksum = 0;
    Node* walker = head.get();
    while (walker) {
        checksum += walker->a + walker->b;
        walker = walker->next.get();
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double traverse_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    // Non-recursive linear deallocation to prevent stack overflow on deep unique_ptr chain
    while (head) {
        head = std::move(head->next);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double reclaim_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    double total_ms = alloc_ms + traverse_ms + reclaim_ms;
    std::cout << "WORKLOAD: ST4-09\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "OBJECTS: " << n << "\n";
    std::cout << "ALLOC_MS: " << alloc_ms << "\n";
    std::cout << "TRAVERSE_MS: " << traverse_ms << "\n";
    std::cout << "RECLAIM_MS: " << reclaim_ms << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "CHECKSUM: " << checksum << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
