#include "nirqoba/runtime/gc.h"

#include <cstdio>
#include <cstdint>

struct Node {
  void* next;
  int64_t value;
};

struct TreeNode {
  void* left;
  void* right;
  int64_t value;
};

int main() {
  nrq_gc_disable();
  nrq_gc_mutator_enter();
  const size_t offset = 0;
  void* root = nullptr;
  nrq_gc_root_register_typed(&root, sizeof(void*), &offset, 1);

  std::printf("sizeof(Node)=%zu sizeof(TreeNode)=%zu\n", sizeof(Node),
              sizeof(TreeNode));

  void* nodes[5];
  for (int i = 0; i < 5; ++i) {
    nodes[i] = nrq_gc_allocate_array_typed_rooted(&root, 1, sizeof(Node),
                                                  &offset, 1);
    nrq_gc_pin_transfer(nodes[i]);
    nrq_gc_unpin(nodes[i]);
  }
  std::printf("Node strides: ");
  for (int i = 1; i < 5; ++i)
    std::printf("%lld ", static_cast<long long>(
        reinterpret_cast<unsigned char*>(nodes[i]) -
        reinterpret_cast<unsigned char*>(nodes[i - 1])));
  std::printf("\n");

  void* trees[5];
  for (int i = 0; i < 5; ++i) {
    trees[i] = nrq_gc_allocate_array_typed_rooted(&root, 1, sizeof(TreeNode),
                                                  &offset, 1);
    nrq_gc_pin_transfer(trees[i]);
    nrq_gc_unpin(trees[i]);
  }
  std::printf("TreeNode strides: ");
  for (int i = 1; i < 5; ++i)
    std::printf("%lld ", static_cast<long long>(
        reinterpret_cast<unsigned char*>(trees[i]) -
        reinterpret_cast<unsigned char*>(trees[i - 1])));
  std::printf("\n");

  nrq_gc_root_unregister(&root);
  nrq_gc_mutator_leave();
  return 0;
}
