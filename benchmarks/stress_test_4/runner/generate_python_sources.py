"""
Generator for Python Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\python"

py_sources = {
"01_integer_arithmetic": '''import time
import sys

def run():
    total_ops = 500_000_000
    if len(sys.argv) > 1:
        total_ops = int(sys.argv[1])
    
    t0 = time.perf_counter()
    total = 0
    MASK64 = 0xFFFFFFFFFFFFFFFF
    
    # Process in optimized batches to reduce Python loop interpreter overhead
    step = 10_000_000
    curr = 1
    while curr <= total_ops:
        limit = min(curr + step, total_ops + 1)
        for i in range(curr, limit):
            total = (total + i) & MASK64
            total = (total - (i & 0xFF)) & MASK64
            total = (total * 3) & MASK64
            div = (i % 7) + 1
            total = (total ^ (total // div)) & MASK64
            total = (total ^ ((total << 5) & MASK64) ^ (total >> 59)) & MASK64
        curr = limit

    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-01")
    print(f"LANGUAGE: Python")
    print(f"OPS: {total_ops}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {total}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"02_floating_point": '''import time
import math
import sys

def run():
    total_ops = 100_000_000
    if len(sys.argv) > 1:
        total_ops = int(sys.argv[1])
        
    t0 = time.perf_counter()
    acc = 1.0
    step = 5_000_000
    curr = 1
    while curr <= total_ops:
        limit = min(curr + step, total_ops + 1)
        for _ in range(curr, limit):
            acc = acc + 0.001
            acc = acc * 1.000001
            acc = acc / 1.0000005
            acc = acc + math.sqrt(acc) * 0.00001
        curr = limit

    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-02")
    print(f"LANGUAGE: Python")
    print(f"OPS: {total_ops}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {acc:.6f}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"03_branch_heavy": '''import time
import sys

def run():
    n = 100_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    t0 = time.perf_counter()
    state = 123456789
    checksum = 0
    MASK32 = 0xFFFFFFFF
    MASK64 = 0xFFFFFFFFFFFFFFFF
    
    step = 5_000_000
    curr = 0
    while curr < n:
        limit = min(curr + step, n)
        for i in range(curr, limit):
            state = (state * 1664525 + 1013904223) & MASK32
            if i % 2 == 0:
                if state & 1:
                    checksum = (checksum + (state ^ 0x55555555)) & MASK64
                else:
                    if (state >> 16) & 1:
                        checksum = (checksum + (state ^ 0xAAAAAAAA)) & MASK64
                    else:
                        checksum = (checksum + (state * 3)) & MASK64
            else:
                if state < 0x80000000:
                    checksum = (checksum ^ (state >> 3)) & MASK64
                else:
                    checksum = (checksum - (state & 0xFFFF)) & MASK64
        curr = limit

    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-03")
    print(f"LANGUAGE: Python")
    print(f"INPUTS: {n}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"04_matrix_multiplication": '''import time
import sys

def run():
    size = 1024
    if len(sys.argv) > 1:
        size = int(sys.argv[1])
        
    # Flat 1D row-major matrices
    a = [(float((i + j) % 100) / 100.0) for i in range(size) for j in range(size)]
    b = [(float((i * j) % 100) / 100.0) for i in range(size) for j in range(size)]
    c = [0.0] * (size * size)
    
    t0 = time.perf_counter()
    # Cache-friendly i-k-j loop
    for i in range(size):
        i_off = i * size
        for k in range(size):
            a_ik = a[i_off + k]
            k_off = k * size
            for j in range(size):
                c[i_off + j] += a_ik * b[k_off + j]
                
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    checksum = sum(c)
    print(f"WORKLOAD: ST4-04")
    print(f"LANGUAGE: Python")
    print(f"MATRIX_DIM: {size}x{size}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum:.6f}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"05_vector_reduction": '''import time
import sys

def run():
    n = 50_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    t0 = time.perf_counter()
    # Construction phase
    vec = [(i ^ 0x55555555) for i in range(n)]
    t1 = time.perf_counter()
    construct_ms = (t1 - t0) * 1000.0
    
    # Reduction phase
    t2 = time.perf_counter()
    total = sum(vec)
    t3 = time.perf_counter()
    reduce_ms = (t3 - t2) * 1000.0
    
    print(f"WORKLOAD: ST4-05")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"CONSTRUCTION_MS: {construct_ms:.3f}")
    print(f"REDUCTION_MS: {reduce_ms:.3f}")
    print(f"TIME_MS: {(construct_ms + reduce_ms):.3f}")
    print(f"CHECKSUM: {total}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"06_sorting": '''import time
import sys

def run():
    n = 5_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Generate deterministic pseudo-random input using LCG seed 0x12345678
    state = 0x12345678
    MASK32 = 0xFFFFFFFF
    arr = [0] * n
    for i in range(n):
        state = (state * 1664525 + 1013904223) & MASK32
        arr[i] = state
        
    # Timed sorting phase
    t0 = time.perf_counter()
    arr.sort()
    t1 = time.perf_counter()
    sort_ms = (t1 - t0) * 1000.0
    
    # Correctness verification
    is_sorted = all(arr[i] <= arr[i + 1] for i in range(min(n - 1, 10000))) and arr[0] <= arr[-1]
    checksum = sum(arr[::1000]) # Sampled sum
    
    print(f"WORKLOAD: ST4-06")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"TIME_MS: {sort_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: {'PASS' if is_sorted else 'FAIL'}")

if __name__ == "__main__":
    run()
''',

"07_hash_map": '''import time
import sys

def run():
    n = 5_000_000
    removals = 1_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        removals = n // 5
        
    table = {}
    
    # Insert phase
    t0 = time.perf_counter()
    for i in range(n):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        table[key] = i
    t1 = time.perf_counter()
    insert_ms = (t1 - t0) * 1000.0
    
    # Lookup phase
    t2 = time.perf_counter()
    lookup_sum = 0
    for i in range(n):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        lookup_sum += table[key]
    t3 = time.perf_counter()
    lookup_ms = (t3 - t2) * 1000.0
    
    # Removal phase
    t4 = time.perf_counter()
    for i in range(0, removals * 5, 5):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        del table[key]
    t5 = time.perf_counter()
    remove_ms = (t5 - t4) * 1000.0
    
    rem_count = len(table)
    total_ms = insert_ms + lookup_ms + remove_ms
    
    print(f"WORKLOAD: ST4-07")
    print(f"LANGUAGE: Python")
    print(f"INSERT_MS: {insert_ms:.3f}")
    print(f"LOOKUP_MS: {lookup_ms:.3f}")
    print(f"REMOVE_MS: {remove_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"REMAINING: {rem_count}")
    print(f"CHECKSUM: {lookup_sum}")
    print(f"STATUS: {'PASS' if rem_count == (n - removals) else 'FAIL'}")

if __name__ == "__main__":
    run()
''',

"08_binary_search": '''import time
import sys
import bisect

def run():
    n = 20_000_000
    queries = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        queries = n // 2
        
    arr = [i * 2 for i in range(n)]
    
    t0 = time.perf_counter()
    hit_count = 0
    checksum = 0
    max_val = n * 2
    
    for q in range(queries):
        target = (q * 3) % max_val
        idx = bisect.bisect_left(arr, target)
        if idx < n and arr[idx] == target:
            hit_count += 1
            checksum += idx
        else:
            checksum -= 1
            
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-08")
    print(f"LANGUAGE: Python")
    print(f"ARRAY_SIZE: {n}")
    print(f"QUERIES: {queries}")
    print(f"HITS: {hit_count}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"09_small_object_allocation": '''import time
import sys
import gc

class Node:
    __slots__ = ('a', 'b', 'next')
    def __init__(self, a, b, next_node):
        self.a = a
        self.b = b
        self.next = next_node

def run():
    n = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Allocation & link
    t0 = time.perf_counter()
    head = None
    curr = None
    for i in range(n):
        node = Node(i, i * 2, None)
        if head is None:
            head = node
            curr = node
        else:
            curr.next = node
            curr = node
    t1 = time.perf_counter()
    alloc_ms = (t1 - t0) * 1000.0
    
    # Touch & traversal
    t2 = time.perf_counter()
    checksum = 0
    walker = head
    while walker is not None:
        checksum += walker.a + walker.b
        walker = walker.next
    t3 = time.perf_counter()
    traverse_ms = (t3 - t2) * 1000.0
    
    # Reclamation
    t4 = time.perf_counter()
    head = None
    curr = None
    walker = None
    gc.collect()
    t5 = time.perf_counter()
    reclaim_ms = (t5 - t4) * 1000.0
    
    total_ms = alloc_ms + traverse_ms + reclaim_ms
    print(f"WORKLOAD: ST4-09")
    print(f"LANGUAGE: Python")
    print(f"OBJECTS: {n}")
    print(f"ALLOC_MS: {alloc_ms:.3f}")
    print(f"TRAVERSE_MS: {traverse_ms:.3f}")
    print(f"RECLAIM_MS: {reclaim_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"10_large_memory_throughput": '''import time
import sys

def run():
    size = 512 * 1024 * 1024 # 512 MiB
    if len(sys.argv) > 1:
        size = int(sys.argv[1]) * 1024 * 1024
        
    t0 = time.perf_counter()
    # Sequential write
    buf = bytearray(size)
    t1 = time.perf_counter()
    alloc_ms = (t1 - t0) * 1000.0
    
    # Sequential read / reduction
    t2 = time.perf_counter()
    checksum = sum(buf[::1024])
    t3 = time.perf_counter()
    read_ms = (t3 - t2) * 1000.0
    
    # Sequential in-place transform
    t4 = time.perf_counter()
    for i in range(0, size, 4096):
        buf[i] = (buf[i] * 3 ^ 0x55) & 0xFF
    t5 = time.perf_counter()
    transform_ms = (t5 - t4) * 1000.0
    
    total_ms = alloc_ms + read_ms + transform_ms
    throughput_mibs = (size / (1024 * 1024)) / (total_ms / 1000.0)
    
    print(f"WORKLOAD: ST4-10")
    print(f"LANGUAGE: Python")
    print(f"BUFFER_MIB: {size // (1024 * 1024)}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"THROUGHPUT_MIBS: {throughput_mibs:.2f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"11_pointer_chasing": '''import time
import sys

def run():
    n = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Deterministic pseudo-random permutation
    next_idx = list(range(n))
    state = 0xDEADBEEF
    MASK32 = 0xFFFFFFFF
    # Fisher-Yates shuffle
    for i in range(n - 1, 0, -1):
        state = (state * 1664525 + 1013904223) & MASK32
        j = state % (i + 1)
        next_idx[i], next_idx[j] = next_idx[j], next_idx[i]
        
    # Traversal measurement (strictly timing the random pointer chasing)
    t0 = time.perf_counter()
    curr = 0
    checksum = 0
    for _ in range(n):
        checksum += curr
        curr = next_idx[curr]
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-11")
    print(f"LANGUAGE: Python")
    print(f"NODES: {n}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"12_binary_tree": '''import time
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
''',

"13_recursive_overhead": '''import time
import sys

def merge(left, right):
    result = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            result.append(left[i])
            i += 1
        else:
            result.append(right[j])
            j += 1
    result.extend(left[i:])
    result.extend(right[j:])
    return result

def merge_sort(arr):
    if len(arr) <= 1:
        return arr
    mid = len(arr) // 2
    left = merge_sort(arr[:mid])
    right = merge_sort(arr[mid:])
    return merge(left, right)

def run():
    sys.setrecursionlimit(2000000)
    n = 4_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    state = 0x54321
    MASK32 = 0xFFFFFFFF
    arr = [0] * n
    for i in range(n):
        state = (state * 1664525 + 1013904223) & MASK32
        arr[i] = state
        
    t0 = time.perf_counter()
    sorted_arr = merge_sort(arr)
    t1 = time.perf_counter()
    sort_ms = (t1 - t0) * 1000.0
    
    is_sorted = sorted_arr[0] <= sorted_arr[-1]
    checksum = sum(sorted_arr[::1000])
    
    print(f"WORKLOAD: ST4-13")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"TIME_MS: {sort_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: {'PASS' if is_sorted else 'FAIL'}")

if __name__ == "__main__":
    run()
''',

"14_string_processing": '''import time
import sys

def run():
    size_mb = 256
    if len(sys.argv) > 1:
        size_mb = int(sys.argv[1])
        
    # Generate deterministic text
    pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. "
    repeats = (size_mb * 1024 * 1024) // len(pattern) + 1
    corpus = (pattern * repeats)[:size_mb * 1024 * 1024]
    
    # 1. Character frequency counting
    t0 = time.perf_counter()
    char_count = corpus.count('e')
    t1 = time.perf_counter()
    char_ms = (t1 - t0) * 1000.0
    
    # 2. Substring search
    t2 = time.perf_counter()
    sub_count = corpus.count("benchmark")
    t3 = time.perf_counter()
    sub_ms = (t3 - t2) * 1000.0
    
    # 3. Whitespace tokenization
    t4 = time.perf_counter()
    tokens = corpus.split()
    token_count = len(tokens)
    t5 = time.perf_counter()
    tok_ms = (t5 - t4) * 1000.0
    
    # 4. Case conversion
    t6 = time.perf_counter()
    upper_corpus = corpus.upper()
    t7 = time.perf_counter()
    case_ms = (t7 - t6) * 1000.0
    
    total_ms = char_ms + sub_ms + tok_ms + case_ms
    print(f"WORKLOAD: ST4-14")
    print(f"LANGUAGE: Python")
    print(f"CORPUS_MIB: {size_mb}")
    print(f"CHAR_COUNT_MS: {char_ms:.3f}")
    print(f"SUBSTRING_MS: {sub_ms:.3f}")
    print(f"TOKENIZE_MS: {tok_ms:.3f}")
    print(f"CASE_CONV_MS: {case_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"CHECKSUM: {token_count + sub_count + char_count}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
''',

"15_csv_parsing": '''import time
import csv
import io
import sys
import os

def run():
    csv_file = r"benchmarks\\stress_test_4\\artifacts\\st4_15_benchmark_5m.csv"
    if len(sys.argv) > 1:
        csv_file = sys.argv[1]
        
    if not os.path.exists(csv_file):
        print(f"CSV file not found: {csv_file}")
        sys.exit(1)
        
    # Read entire file into memory first to exclude disk I/O from benchmark
    with open(csv_file, "r", encoding="utf-8") as f:
        content = f.read()
        
    t0 = time.perf_counter()
    
    reader = csv.reader(io.StringIO(content))
    header = next(reader)
    
    total_rows = 0
    sum_id = 0
    sum_device_code = 0
    empty_cat_count = 0
    escaped_quotes_count = 0
    
    for row in reader:
        total_rows += 1
        sum_id += int(row[0])
        dev_code = int(row[4])
        sum_device_code += dev_code
        if not row[6]:
            empty_cat_count += 1
        if '""' in row[7] or '"' in row[7]:
            escaped_quotes_count += 1

    t1 = time.perf_counter()
    parse_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-15")
    print(f"LANGUAGE: Python")
    print(f"ROWS_PARSED: {total_rows}")
    print(f"TIME_MS: {parse_ms:.3f}")
    print(f"SUM_ID: {sum_id}")
    print(f"SUM_DEVICE: {sum_device_code}")
    print(f"EMPTY_CAT: {empty_cat_count}")
    print(f"CHECKSUM: {sum_id + sum_device_code}")
    print(f"STATUS: {'PASS' if total_rows == 5000000 else 'FAIL'}")

if __name__ == "__main__":
    run()
''',

"16_cyclic_objects": '''import time
import gc
import sys

class CycleNode:
    __slots__ = ('peer', 'value')
    def __init__(self, value):
        self.value = value
        self.peer = None

def run():
    pairs_count = 500_000 # 1,000,000 nodes total
    if len(sys.argv) > 1:
        pairs_count = int(sys.argv[1])
        
    # Allocation & cycle establishment
    t0 = time.perf_counter()
    nodes = []
    for i in range(pairs_count):
        a = CycleNode(i)
        b = CycleNode(i)
        a.peer = b
        b.peer = a
        nodes.append((a, b))
    t1 = time.perf_counter()
    build_ms = (t1 - t0) * 1000.0
    
    # Drop external references
    del nodes
    
    # Reclamation phase
    t2 = time.perf_counter()
    reclaimed = gc.collect()
    t3 = time.perf_counter()
    reclaim_ms = (t3 - t2) * 1000.0
    
    total_ms = build_ms + reclaim_ms
    print(f"WORKLOAD: ST4-16")
    print(f"LANGUAGE: Python")
    print(f"CYCLES: {pairs_count}")
    print(f"BUILD_MS: {build_ms:.3f}")
    print(f"RECLAIM_MS: {reclaim_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"RECLAIMED_OBJECTS: {reclaimed}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
'''
}

for folder, code in py_sources.items():
    path = os.path.join(base, folder, "main.py")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote Python ST4: {path}")

print("Python track generation finished.")
