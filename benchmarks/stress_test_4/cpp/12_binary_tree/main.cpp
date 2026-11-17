#include <iostream>
#include <cstdint>
#include <chrono>

struct TreeNode {
    int val;
    TreeNode* left;
    TreeNode* right;
    TreeNode(int v, TreeNode* l = nullptr, TreeNode* r = nullptr) : val(v), left(l), right(r) {}
};

static TreeNode* build_tree(int depth, int idx) {
    if (depth == 0) return nullptr;
    return new TreeNode(idx, build_tree(depth - 1, idx * 2), build_tree(depth - 1, idx * 2 + 1));
}

static uint64_t preorder(const TreeNode* node) {
    if (!node) return 0;
    return (uint64_t)node->val + preorder(node->left) + preorder(node->right);
}

static uint64_t inorder(const TreeNode* node) {
    if (!node) return 0;
    return inorder(node->left) + (uint64_t)node->val + inorder(node->right);
}

static uint64_t postorder(const TreeNode* node) {
    if (!node) return 0;
    return postorder(node->left) + postorder(node->right) + (uint64_t)node->val;
}

static void free_tree(TreeNode* node) {
    if (!node) return;
    free_tree(node->left);
    free_tree(node->right);
    delete node;
}

int main(int argc, char** argv) {
    int depth = 20; // 1,048,575 nodes
    if (argc > 1) {
        depth = std::stoi(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    TreeNode* root = build_tree(depth, 1);
    auto t1 = std::chrono::high_resolution_clock::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t sum_pre = preorder(root);
    auto t3 = std::chrono::high_resolution_clock::now();
    double pre_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    uint64_t sum_in = inorder(root);
    auto t5 = std::chrono::high_resolution_clock::now();
    double in_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    auto t6 = std::chrono::high_resolution_clock::now();
    uint64_t sum_post = postorder(root);
    auto t7 = std::chrono::high_resolution_clock::now();
    double post_ms = std::chrono::duration<double, std::milli>(t7 - t6).count();

    double total_ms = build_ms + pre_ms + in_ms + post_ms;
    std::cout << "WORKLOAD: ST4-12\n";
    std::cout << "LANGUAGE: C++\n";
    std::cout << "NODES: " << ((1 << depth) - 1) << "\n";
    std::cout << "BUILD_MS: " << build_ms << "\n";
    std::cout << "PREORDER_MS: " << pre_ms << "\n";
    std::cout << "INORDER_MS: " << in_ms << "\n";
    std::cout << "POSTORDER_MS: " << post_ms << "\n";
    std::cout << "TIME_MS: " << total_ms << "\n";
    std::cout << "CHECKSUM: " << sum_pre << "\n";
    std::cout << "STATUS: " << (sum_pre == sum_in && sum_in == sum_post ? "PASS" : "FAIL") << "\n";

    free_tree(root);
    return 0;
}
