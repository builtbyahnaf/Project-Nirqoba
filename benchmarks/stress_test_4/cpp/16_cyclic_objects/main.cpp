#include <iostream>
#include <vector>
#include <memory>
#include <chrono>

struct CycleNode {
    std::shared_ptr<CycleNode> peer;
    int value;
    CycleNode(int val) : value(val), peer(nullptr) {}
};

int main(int argc, char** argv) {
    size_t pairs_count = 500000; // 1M nodes total
    if (argc > 1) {
        pairs_count = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::shared_ptr<CycleNode>> a_nodes(pairs_count);
    std::vector<std::shared_ptr<CycleNode>> b_nodes(pairs_count);

    for (size_t i = 0; i < pairs_count; i++) {
        auto a = std::make_shared<CycleNode>((int)i);
        auto b = std::make_shared<CycleNode>((int)i);
        a->peer = b;
        b->peer = a;
        a_nodes[i] = a;
        b_nodes[i] = b;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Reclamation phase: break cycles explicitly for smart pointer reclamation
    auto t2 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < pairs_count; i++) {
        a_nodes[i]->peer.reset();
        b_nodes[i]->peer.reset();
        a_nodes[i].reset();
        b_nodes[i].reset();
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double reclaim_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    double total_ms = build_ms + reclaim_ms;
    std::cout << "WORKLOAD: ST4-16\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "CYCLES: " << pairs_count << "\n";
    std::cout << "BUILD_MS: " << build_ms << "\n";
    std::cout << "RECLAIM_MS: " << reclaim_ms << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "STATUS: PASS\n";
    return 0;
}
