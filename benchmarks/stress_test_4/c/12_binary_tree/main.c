#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

typedef struct TreeNode {
    int val;
    struct TreeNode* left;
    struct TreeNode* right;
} TreeNode;

static TreeNode* build_tree(int depth, int idx) {
    if (depth == 0) return NULL;
    TreeNode* node = (TreeNode*)malloc(sizeof(TreeNode));
    node->val = idx;
    node->left = build_tree(depth - 1, idx * 2);
    node->right = build_tree(depth - 1, idx * 2 + 1);
    return node;
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
    free(node);
}

int main(int argc, char** argv) {
    int depth = 20; // 1,048,575 nodes
    if (argc > 1) {
        depth = atoi(argv[1]);
    }

    double t0 = bench_now_ms();
    TreeNode* root = build_tree(depth, 1);
    double t1 = bench_now_ms();
    double build_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t sum_pre = preorder(root);
    double t3 = bench_now_ms();
    double pre_ms = t3 - t2;

    double t4 = bench_now_ms();
    uint64_t sum_in = inorder(root);
    double t5 = bench_now_ms();
    double in_ms = t5 - t4;

    double t6 = bench_now_ms();
    uint64_t sum_post = postorder(root);
    double t7 = bench_now_ms();
    double post_ms = t7 - t6;

    double total_ms = build_ms + pre_ms + in_ms + post_ms;
    printf("WORKLOAD: ST4-12\n");
    printf("LANGUAGE: C\n");
    printf("NODES: %d\n", (1 << depth) - 1);
    printf("BUILD_MS: %.3f\n", build_ms);
    printf("PREORDER_MS: %.3f\n", pre_ms);
    printf("INORDER_MS: %.3f\n", in_ms);
    printf("POSTORDER_MS: %.3f\n", post_ms);
    printf("TIME_MS: %.3f\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\n", sum_pre);
    printf("STATUS: %s\n", (sum_pre == sum_in && sum_in == sum_post) ? "PASS" : "FAIL");

    free_tree(root);
    return 0;
}
