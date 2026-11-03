#define NOMINMAX
#include "nirqoba/runtime/gc.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <windows.h>

namespace {

double now_ms() {
  static LARGE_INTEGER frequency{};
  static bool initialized = false;
  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = true;
  }
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

struct TreeNode {
  void* left;
  void* right;
  int64_t value;
};

constexpr int kDepth = 20;
constexpr size_t kNodeCount = (size_t{1} << (kDepth + 1)) - 1;

volatile int64_t checksum;

void report(const char* name, double elapsed_ms, size_t operations) {
  const double per_op_ns =
      elapsed_ms * 1.0e6 / static_cast<double>(operations);
  std::printf("%-44s %10.3f ms  %8.2f ns/op\n", name, elapsed_ms, per_op_ns);
}

double median3(double a, double b, double c) {
  return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

__declspec(noinline) int64_t tree_raw(const TreeNode* node, int64_t depth) {
  if (depth == 0) return node->value;
  const auto* left = static_cast<const TreeNode*>(node->left);
  const auto* right = static_cast<const TreeNode*>(node->right);
  return node->value + tree_raw(left, depth - 1) +
         tree_raw(right, depth - 1);
}

// One root: exactly probe2's replica.
__declspec(noinline) int64_t tree_one_root(const TreeNode* param, int64_t depth) {
  nrq_gc_mutator_enter();
  const uint64_t marker = nrq_gc_root_frame_begin();
  void* root = nullptr;
  const size_t offset = 0;
  nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);
  const void* parameter = param;
  nrq_gc_root_copy(&root, &parameter, sizeof(void*));

  int64_t result = 0;
  const auto* current = static_cast<const TreeNode*>(root);
  if (depth == 0) {
    result = current->value;
  } else {
    const auto* left = static_cast<const TreeNode*>(current->left);
    const auto* right = static_cast<const TreeNode*>(current->right);
    result = current->value + tree_one_root(left, depth - 1) +
             tree_one_root(right, depth - 1);
  }

  nrq_gc_root_frame_end(marker);
  nrq_gc_mutator_leave();
  return result;
}

// Exact replica of the optimized IR for sum_tree: two registered roots, a
// root_copy of the parameter, and a root_store before every dereference.
__declspec(noinline) int64_t tree_exact(const TreeNode* param, int64_t depth) {
  nrq_gc_mutator_enter();
  const uint64_t marker = nrq_gc_root_frame_begin();

  void* parameter_root = nullptr;
  const size_t offset = 0;
  nrq_gc_root_register_typed(&parameter_root, sizeof(void*), &offset, 1);
  const void* parameter = param;
  nrq_gc_root_copy(&parameter_root, &parameter, sizeof(void*));

  void* node_root = nullptr;
  nrq_gc_root_register_typed(&node_root, sizeof(void*), &offset, 1);
  nrq_gc_root_store(&node_root, const_cast<TreeNode*>(param));

  int64_t result = 0;
  const auto* current = static_cast<const TreeNode*>(node_root);
  if (depth == 0) {
    result = current->value;
  } else {
    const auto* left = static_cast<const TreeNode*>(current->left);
    result = current->value + tree_exact(left, depth - 1);
    // The IR reloads the root slot after the first recursive call.
    const auto* reloaded = static_cast<const TreeNode*>(node_root);
    const auto* right = static_cast<const TreeNode*>(reloaded->right);
    result += tree_exact(right, depth - 1);
  }

  nrq_gc_root_frame_end(marker);
  nrq_gc_mutator_leave();
  return result;
}

// Exact minus the redundant second root registration and its root_store.
__declspec(noinline) int64_t tree_exact_no_second_root(const TreeNode* param,
                                                       int64_t depth) {
  nrq_gc_mutator_enter();
  const uint64_t marker = nrq_gc_root_frame_begin();

  void* node_root = nullptr;
  const size_t offset = 0;
  nrq_gc_root_register_typed(&node_root, sizeof(void*), &offset, 1);
  const void* parameter = param;
  nrq_gc_root_copy(&node_root, &parameter, sizeof(void*));

  int64_t result = 0;
  const auto* current = static_cast<const TreeNode*>(node_root);
  if (depth == 0) {
    result = current->value;
  } else {
    const auto* left = static_cast<const TreeNode*>(current->left);
    result = current->value + tree_exact_no_second_root(left, depth - 1);
    const auto* reloaded = static_cast<const TreeNode*>(node_root);
    const auto* right = static_cast<const TreeNode*>(reloaded->right);
    result += tree_exact_no_second_root(right, depth - 1);
  }

  nrq_gc_root_frame_end(marker);
  nrq_gc_mutator_leave();
  return result;
}

// Callee-side allocations that match the benchmark's preorder physical layout.
__declspec(noinline) TreeNode* build_like_benchmark(int64_t depth,
                                                    void** root_slot) {
  const size_t offset = 0;
  void* object = nrq_gc_allocate_array_typed_rooted(
      root_slot, 1, sizeof(TreeNode), &offset, 1);
  nrq_gc_pin_transfer(object);
  nrq_gc_unpin(object);
  auto* node = static_cast<TreeNode*>(object);
  node->value = depth;  // replaced below with an index-based value
  node->left = nullptr;
  node->right = nullptr;
  if (depth > 0) {
    node->left = build_like_benchmark(depth - 1, root_slot);
    node->right = build_like_benchmark(depth - 1, root_slot);
  }
  return node;
}

size_t fill_values(TreeNode* node, int64_t depth, size_t next) {
  if (!node) return next;
  node->value = static_cast<int64_t>(++next);
  const int64_t child_depth = depth - 1;
  next = fill_values(static_cast<TreeNode*>(node->left), child_depth, next);
  next = fill_values(static_cast<TreeNode*>(node->right), child_depth, next);
  return next;
}

template <typename Function>
void run_variant(const char* name, Function function, const TreeNode* tree,
                 double (&samples)[3]) {
  for (double& sample : samples) {
    const double start = now_ms();
    const int64_t total = function(tree, int64_t{kDepth});
    sample = now_ms() - start;
    checksum = checksum + total;
  }
  report(name, median3(samples[0], samples[1], samples[2]), kNodeCount);
}

}  // namespace

int main() {
  nrq_gc_disable();
  nrq_gc_mutator_enter();

  const size_t offset = 0;
  void* root = nullptr;
  nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);

  TreeNode* tree = build_like_benchmark(kDepth, &root);
  fill_values(tree, kDepth, 0);
  nrq_gc_root_store(&root, tree);

  double samples[3];

  for (double& sample : samples) {
    const double start = now_ms();
    const int64_t total = tree_raw(tree, int64_t{kDepth});
    sample = now_ms() - start;
    checksum = checksum + total;
  }
  report("tree: raw recursive checksum", median3(samples[0], samples[1], samples[2]), kNodeCount);

  run_variant("tree: 1 root, copy (probe2 replica)", tree_one_root, tree, samples);
  run_variant("tree: exact sum_tree IR, generational ON", tree_exact, tree, samples);
  run_variant("tree: exact minus 2nd root/store", tree_exact_no_second_root, tree, samples);

  nrq_gc_set_generational(0);
  run_variant("tree: exact sum_tree IR, generational OFF", tree_exact, tree, samples);
  run_variant("tree: exact minus 2nd root/store, gen OFF", tree_exact_no_second_root, tree, samples);
  nrq_gc_set_generational(1);

  std::printf("checksum=%lld\n", static_cast<long long>(checksum));
  nrq_gc_root_unregister(&root);
  nrq_gc_mutator_leave();
  return 0;
}
