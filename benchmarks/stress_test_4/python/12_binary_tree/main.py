import time
import sys

class TreeNode:
    __slots__ = ('val', 'left', 'right')
    def __init__(self, val, left=None, right=None):
        self.val = val
        self.left = left
        self.right = right

def build_tree(depth, idx=1):
    if depth == 0:
        return None
    return TreeNode(idx, build_tree(depth - 1, idx * 2), build_tree(depth - 1, idx * 2 + 1))

def preorder(node):
    if not node:
        return 0
    return node.val + preorder(node.left) + preorder(node.right)

def inorder(node):
    if not node:
        return 0
    return inorder(node.left) + node.val + inorder(node.right)

def postorder(node):
    if not node:
        return 0
    return postorder(node.left) + postorder(node.right) + node.val

def run():
    sys.setrecursionlimit(2000000)
    depth = 20 # 1,048,575 nodes
    if len(sys.argv) > 1:
        depth = int(sys.argv[1])
        
    t0 = time.perf_counter()
    root = build_tree(depth)
    t1 = time.perf_counter()
    build_ms = (t1 - t0) * 1000.0
    
    t2 = time.perf_counter()
    sum_pre = preorder(root)
    t3 = time.perf_counter()
    pre_ms = (t3 - t2) * 1000.0
    
    t4 = time.perf_counter()
    sum_in = inorder(root)
    t5 = time.perf_counter()
    in_ms = (t5 - t4) * 1000.0
    
    t6 = time.perf_counter()
    sum_post = postorder(root)
    t7 = time.perf_counter()
    post_ms = (t7 - t6) * 1000.0
    
    total_ms = build_ms + pre_ms + in_ms + post_ms
    print(f"WORKLOAD: ST4-12")
    print(f"LANGUAGE: Python")
    print(f"NODES: {(1 << depth) - 1}")
    print(f"BUILD_MS: {build_ms:.3f}")
    print(f"PREORDER_MS: {pre_ms:.3f}")
    print(f"INORDER_MS: {in_ms:.3f}")
    print(f"POSTORDER_MS: {post_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"CHECKSUM: {sum_pre}")
    print(f"STATUS: {'PASS' if sum_pre == sum_in == sum_post else 'FAIL'}")

if __name__ == "__main__":
    run()
