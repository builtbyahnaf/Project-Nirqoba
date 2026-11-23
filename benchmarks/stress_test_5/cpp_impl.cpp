// Stress Test 5 - the CDMM gauntlet, C++ baseline.
//
// The owning-handle baseline: unique_ptr for the short-lived values, a vector of
// unique_ptr for the dropped graph, and a single block for the tree. Sizes match
// the tiers the Nirqoba compiler chose for the same phases.

#include <cstdio>
#include <memory>
#include <vector>

namespace {

struct Vec1 {
    int x;
};

struct Event {
    int kind;
    int payload;
};

struct CycleNode {
    int value;
    std::shared_ptr<CycleNode> next;
};

constexpr int kStackIterations = 200000;
constexpr int kPoolIterations = 2000;
constexpr int kPoolSlots = 8192;
constexpr int kTreeNodes = 8191;
constexpr int kGcNodes = 40000;

long long stack_phase() {
    long long checksum = 0;
    for (int i = 0; i < kStackIterations; ++i) {
        auto v = std::make_unique<Vec1>();
        v->x = i;
        checksum += v->x;
    }
    return checksum;
}

long long pool_phase() {
    long long events = 0;
    for (int i = 0; i < kPoolIterations; ++i) {
        auto batch = std::make_unique<Event[]>(kPoolSlots);
        batch[0].kind = i;
        batch[0].payload = i * 2;
        events += batch[0].kind + batch[0].payload;
    }
    return events;
}

long long arena_phase() {
    auto tree = std::make_unique<int[]>(kTreeNodes * 3);
    tree[0] = 5;
    tree[1] = 1;
    tree[2] = 2;
    long long tree_sum = tree[0] + tree[1] + tree[2];

    auto leaf = std::make_unique<Vec1>();
    leaf->x = 4;
    tree_sum += leaf->x;
    return tree_sum;
}

long long gc_phase() {
    std::vector<std::unique_ptr<CycleNode>> kept;
    kept.reserve(kGcNodes);
    long long cycles = 0;
    for (int i = 0; i < kGcNodes; ++i) {
        auto node = std::make_unique<CycleNode>();
        node->value = i;
        kept.push_back(std::move(node));
        cycles += kept.back()->value;
    }
    kept.clear();
    return cycles;
}

}  // namespace

int main() {
    std::printf("%lld\n", stack_phase());
    std::printf("%lld\n", pool_phase());
    std::printf("%lld\n", arena_phase());
    std::printf("%lld\n", gc_phase());
    return 0;
}
