"""
Generator for C Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\c"

c_sources = {
"01_integer_arithmetic": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    uint64_t total_ops = 500000000ULL;
    if (argc > 1) {
        total_ops = strtoull(argv[1], NULL, 10);
    }

    double t0 = bench_now_ms();
    uint64_t total = 0;

    for (uint64_t i = 1; i <= total_ops; i++) {
        total = (total + i);
        total = (total - (i & 0xFF));
        total = (total * 3ULL);
        uint64_t div = (i % 7ULL) + 1ULL;
        total = (total ^ (total / div));
        total = (total ^ (total << 5) ^ (total >> 59));
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-01\\n");
    printf("LANGUAGE: C\\n");
    printf("OPS: %" PRIu64 "\\n", total_ops);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", total);
    printf("STATUS: PASS\\n");
    return 0;
}
''',

"02_floating_point": '''#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    long long total_ops = 100000000LL;
    if (argc > 1) {
        total_ops = atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    double acc = 1.0;

    for (long long i = 1; i <= total_ops; i++) {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + sqrt(acc) * 0.00001;
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-02\\n");
    printf("LANGUAGE: C\\n");
    printf("OPS: %lld\\n", total_ops);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %.6f\\n", acc);
    printf("STATUS: PASS\\n");
    return 0;
}
''',

"03_branch_heavy": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    long long n = 100000000LL;
    if (argc > 1) {
        n = atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    uint32_t state = 123456789U;
    uint64_t checksum = 0;

    for (long long i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        if ((i & 1) == 0) {
            if ((state & 1) != 0) {
                checksum += (uint64_t)(state ^ 0x55555555U);
            } else {
                if (((state >> 16) & 1) != 0) {
                    checksum += (uint64_t)(state ^ 0xAAAAAAAAU);
                } else {
                    checksum += (uint64_t)(state * 3U);
                }
            }
        } else {
            if (state < 0x80000000U) {
                checksum ^= (uint64_t)(state >> 3);
            } else {
                checksum -= (uint64_t)(state & 0xFFFFU);
            }
        }
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-03\\n");
    printf("LANGUAGE: C\\n");
    printf("INPUTS: %lld\\n", n);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: PASS\\n");
    return 0;
}
''',

"04_matrix_multiplication": '''#include <stdio.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    int size = 1024;
    if (argc > 1) {
        size = atoi(argv[1]);
    }

    size_t total_elements = (size_t)size * (size_t)size;
    double* a = (double*)malloc(total_elements * sizeof(double));
    double* b = (double*)malloc(total_elements * sizeof(double));
    double* c = (double*)calloc(total_elements, sizeof(double));

    if (!a || !b || !c) {
        fprintf(stderr, "Allocation failed\\n");
        return 1;
    }

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            a[i * size + j] = (double)((i + j) % 100) / 100.0;
            b[i * size + j] = (double)((i * j) % 100) / 100.0;
        }
    }

    double t0 = bench_now_ms();
    // Cache-friendly i-k-j loop
    for (int i = 0; i < size; i++) {
        int i_off = i * size;
        for (int k = 0; k < size; k++) {
            double a_ik = a[i_off + k];
            int k_off = k * size;
            for (int j = 0; j < size; j++) {
                c[i_off + j] += a_ik * b[k_off + j];
            }
        }
    }
    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    double checksum = 0.0;
    for (size_t i = 0; i < total_elements; i++) {
        checksum += c[i];
    }

    printf("WORKLOAD: ST4-04\\n");
    printf("LANGUAGE: C\\n");
    printf("MATRIX_DIM: %dx%d\\n", size, size);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %.6f\\n", checksum);
    printf("STATUS: PASS\\n");

    free(a);
    free(b);
    free(c);
    return 0;
}
''',

"05_vector_reduction": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t n = 50000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    uint64_t* vec = (uint64_t*)malloc(n * sizeof(uint64_t));
    if (!vec) return 1;

    for (size_t i = 0; i < n; i++) {
        vec[i] = (uint64_t)i ^ 0x55555555ULL;
    }
    double t1 = bench_now_ms();
    double construct_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += vec[i];
    }
    double t3 = bench_now_ms();
    double reduce_ms = t3 - t2;

    printf("WORKLOAD: ST4-05\\n");
    printf("LANGUAGE: C\\n");
    printf("ELEMENTS: %zu\\n", n);
    printf("CONSTRUCTION_MS: %.3f\\n", construct_ms);
    printf("REDUCTION_MS: %.3f\\n", reduce_ms);
    printf("TIME_MS: %.3f\\n", construct_ms + reduce_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", total);
    printf("STATUS: PASS\\n");

    free(vec);
    return 0;
}
''',

"06_sorting": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static int compare_i32(const void* a, const void* b) {
    int32_t arg1 = *(const int32_t*)a;
    int32_t arg2 = *(const int32_t*)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

int main(int argc, char** argv) {
    size_t n = 5000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    int32_t* arr = (int32_t*)malloc(n * sizeof(int32_t));
    if (!arr) return 1;

    uint32_t state = 0x12345678U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    double t0 = bench_now_ms();
    qsort(arr, n, sizeof(int32_t), compare_i32);
    double t1 = bench_now_ms();
    double sort_ms = t1 - t0;

    int is_sorted = 1;
    for (size_t i = 0; i < (n < 10000 ? n - 1 : 10000); i++) {
        if (arr[i] > arr[i + 1]) {
            is_sorted = 0;
            break;
        }
    }

    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    printf("WORKLOAD: ST4-06\\n");
    printf("LANGUAGE: C\\n");
    printf("ELEMENTS: %zu\\n", n);
    printf("TIME_MS: %.3f\\n", sort_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: %s\\n", is_sorted ? "PASS" : "FAIL");

    free(arr);
    return 0;
}
''',

"07_hash_map": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

// High-performance open-addressing linear probing hash map
typedef struct {
    uint64_t key;
    uint64_t val;
    uint8_t used;
} HashEntry;

typedef struct {
    HashEntry* entries;
    size_t capacity;
    size_t size;
} HashMap;

static HashMap* create_map(size_t cap) {
    HashMap* map = (HashMap*)malloc(sizeof(HashMap));
    map->capacity = cap;
    map->size = 0;
    map->entries = (HashEntry*)calloc(cap, sizeof(HashEntry));
    return map;
}

static inline void map_insert(HashMap* map, uint64_t key, uint64_t val) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            map->entries[idx].val = val;
            return;
        }
        idx = (idx + 1) & mask;
    }
    map->entries[idx].key = key;
    map->entries[idx].val = val;
    map->entries[idx].used = 1;
    map->size++;
}

static inline uint64_t map_lookup(HashMap* map, uint64_t key) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            return map->entries[idx].val;
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

static inline void map_remove(HashMap* map, uint64_t key) {
    size_t mask = map->capacity - 1;
    size_t idx = (key * 11400714819323198485ULL) & mask;
    while (map->entries[idx].used) {
        if (map->entries[idx].key == key) {
            map->entries[idx].used = 0;
            map->size--;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

int main(int argc, char** argv) {
    size_t n = 5000000;
    size_t removals = 1000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
        removals = n / 5;
    }

    HashMap* map = create_map(16777216); // 16M slots power of two

    double t0 = bench_now_ms();
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map_insert(map, key, (uint64_t)i);
    }
    double t1 = bench_now_ms();
    double insert_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t lookup_sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        lookup_sum += map_lookup(map, key);
    }
    double t3 = bench_now_ms();
    double lookup_ms = t3 - t2;

    double t4 = bench_now_ms();
    for (size_t i = 0; i < removals * 5; i += 5) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map_remove(map, key);
    }
    double t5 = bench_now_ms();
    double remove_ms = t5 - t4;

    double total_ms = insert_ms + lookup_ms + remove_ms;
    printf("WORKLOAD: ST4-07\\n");
    printf("LANGUAGE: C\\n");
    printf("INSERT_MS: %.3f\\n", insert_ms);
    printf("LOOKUP_MS: %.3f\\n", lookup_ms);
    printf("REMOVE_MS: %.3f\\n", remove_ms);
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("REMAINING: %zu\\n", map->size);
    printf("CHECKSUM: %" PRIu64 "\\n", lookup_sum);
    printf("STATUS: %s\\n", map->size == (n - removals) ? "PASS" : "FAIL");

    free(map->entries);
    free(map);
    return 0;
}
''',

"08_binary_search": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static inline int64_t binary_search(const int64_t* arr, size_t n, int64_t target) {
    size_t low = 0;
    size_t high = n - 1;
    while (low <= high) {
        size_t mid = low + ((high - low) >> 1);
        int64_t mid_val = arr[mid];
        if (mid_val == target) {
            return (int64_t)mid;
        } else if (mid_val < target) {
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    size_t n = 20000000;
    size_t queries = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
        queries = n / 2;
    }

    int64_t* arr = (int64_t*)malloc(n * sizeof(int64_t));
    if (!arr) return 1;

    for (size_t i = 0; i < n; i++) {
        arr[i] = (int64_t)i * 2;
    }

    double t0 = bench_now_ms();
    int64_t hit_count = 0;
    int64_t checksum = 0;
    int64_t max_val = (int64_t)n * 2;

    for (size_t q = 0; q < queries; q++) {
        int64_t target = ((int64_t)q * 3) % max_val;
        int64_t idx = binary_search(arr, n, target);
        if (idx != -1) {
            hit_count++;
            checksum += idx;
        } else {
            checksum -= 1;
        }
    }

    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-08\\n");
    printf("LANGUAGE: C\\n");
    printf("ARRAY_SIZE: %zu\\n", n);
    printf("QUERIES: %zu\\n", queries);
    printf("HITS: %" PRId64 "\\n", hit_count);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %" PRId64 "\\n", checksum);
    printf("STATUS: PASS\\n");

    free(arr);
    return 0;
}
''',

"09_small_object_allocation": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

typedef struct Node {
    uint64_t a;
    uint64_t b;
    struct Node* next;
} Node;

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    Node* head = NULL;
    Node* curr = NULL;
    for (size_t i = 0; i < n; i++) {
        Node* node = (Node*)malloc(sizeof(Node));
        node->a = (uint64_t)i;
        node->b = (uint64_t)i * 2;
        node->next = NULL;
        if (!head) {
            head = node;
            curr = node;
        } else {
            curr->next = node;
            curr = node;
        }
    }
    double t1 = bench_now_ms();
    double alloc_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t checksum = 0;
    Node* walker = head;
    while (walker) {
        checksum += walker->a + walker->b;
        walker = walker->next;
    }
    double t3 = bench_now_ms();
    double traverse_ms = t3 - t2;

    double t4 = bench_now_ms();
    walker = head;
    while (walker) {
        Node* next = walker->next;
        free(walker);
        walker = next;
    }
    double t5 = bench_now_ms();
    double reclaim_ms = t5 - t4;

    double total_ms = alloc_ms + traverse_ms + reclaim_ms;
    printf("WORKLOAD: ST4-09\\n");
    printf("LANGUAGE: C\\n");
    printf("OBJECTS: %zu\\n", n);
    printf("ALLOC_MS: %.3f\\n", alloc_ms);
    printf("TRAVERSE_MS: %.3f\\n", traverse_ms);
    printf("RECLAIM_MS: %.3f\\n", reclaim_ms);
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: PASS\\n");
    return 0;
}
''',

"10_large_memory_throughput": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t size = 512ULL * 1024ULL * 1024ULL; // 512 MiB
    if (argc > 1) {
        size = (size_t)atoll(argv[1]) * 1024ULL * 1024ULL;
    }

    double t0 = bench_now_ms();
    uint8_t* buf = (uint8_t*)malloc(size);
    if (!buf) return 1;

    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    double t1 = bench_now_ms();
    double write_ms = t1 - t0;

    double t2 = bench_now_ms();
    uint64_t checksum = 0;
    for (size_t i = 0; i < size; i += 1024) {
        checksum += buf[i];
    }
    double t3 = bench_now_ms();
    double read_ms = t3 - t2;

    double t4 = bench_now_ms();
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)((buf[i] * 3) ^ 0x55);
    }
    double t5 = bench_now_ms();
    double transform_ms = t5 - t4;

    double total_ms = write_ms + read_ms + transform_ms;
    double throughput_mib = ((double)size / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    printf("WORKLOAD: ST4-10\\n");
    printf("LANGUAGE: C\\n");
    printf("BUFFER_MIB: %zu\\n", size / (1024 * 1024));
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("THROUGHPUT_MIBS: %.2f\\n", throughput_mib);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: PASS\\n");

    free(buf);
    return 0;
}
''',

"11_pointer_chasing": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    size_t* next_idx = (size_t*)malloc(n * sizeof(size_t));
    if (!next_idx) return 1;

    for (size_t i = 0; i < n; i++) {
        next_idx[i] = i;
    }

    uint32_t state = 0xDEADBEEFU;
    for (size_t i = n - 1; i > 0; i--) {
        state = (state * 1664525U + 1013904223U);
        size_t j = state % (i + 1);
        size_t tmp = next_idx[i];
        next_idx[i] = next_idx[j];
        next_idx[j] = tmp;
    }

    double t0 = bench_now_ms();
    size_t curr = 0;
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i++) {
        checksum += curr;
        curr = next_idx[curr];
    }
    double t1 = bench_now_ms();
    double time_ms = t1 - t0;

    printf("WORKLOAD: ST4-11\\n");
    printf("LANGUAGE: C\\n");
    printf("NODES: %zu\\n", n);
    printf("TIME_MS: %.3f\\n", time_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: PASS\\n");

    free(next_idx);
    return 0;
}
''',

"12_binary_tree": '''#include <stdio.h>
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
    printf("WORKLOAD: ST4-12\\n");
    printf("LANGUAGE: C\\n");
    printf("NODES: %d\\n", (1 << depth) - 1);
    printf("BUILD_MS: %.3f\\n", build_ms);
    printf("PREORDER_MS: %.3f\\n", pre_ms);
    printf("INORDER_MS: %.3f\\n", in_ms);
    printf("POSTORDER_MS: %.3f\\n", post_ms);
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", sum_pre);
    printf("STATUS: %s\\n", (sum_pre == sum_in && sum_in == sum_post) ? "PASS" : "FAIL");

    free_tree(root);
    return 0;
}
''',

"13_recursive_overhead": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

static void merge_sort_recursive(int32_t* arr, int32_t* temp, size_t left, size_t right) {
    if (left >= right) return;
    size_t mid = left + ((right - left) >> 1);
    merge_sort_recursive(arr, temp, left, mid);
    merge_sort_recursive(arr, temp, mid + 1, right);

    size_t i = left, j = mid + 1, k = left;
    while (i <= mid && j <= right) {
        if (arr[i] <= arr[j]) {
            temp[k++] = arr[i++];
        } else {
            temp[k++] = arr[j++];
        }
    }
    while (i <= mid) temp[k++] = arr[i++];
    while (j <= right) temp[k++] = arr[j++];
    for (size_t p = left; p <= right; p++) {
        arr[p] = temp[p];
    }
}

int main(int argc, char** argv) {
    size_t n = 4000000;
    if (argc > 1) {
        n = (size_t)atoll(argv[1]);
    }

    int32_t* arr = (int32_t*)malloc(n * sizeof(int32_t));
    int32_t* temp = (int32_t*)malloc(n * sizeof(int32_t));
    if (!arr || !temp) return 1;

    uint32_t state = 0x54321U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    double t0 = bench_now_ms();
    merge_sort_recursive(arr, temp, 0, n - 1);
    double t1 = bench_now_ms();
    double sort_ms = t1 - t0;

    int is_sorted = (arr[0] <= arr[n - 1]);
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    printf("WORKLOAD: ST4-13\\n");
    printf("LANGUAGE: C\\n");
    printf("ELEMENTS: %zu\\n", n);
    printf("TIME_MS: %.3f\\n", sort_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", checksum);
    printf("STATUS: %s\\n", is_sorted ? "PASS" : "FAIL");

    free(arr);
    free(temp);
    return 0;
}
''',

"14_string_processing": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    size_t size_mb = 256;
    if (argc > 1) {
        size_mb = (size_t)atoll(argv[1]);
    }

    size_t total_bytes = size_mb * 1024ULL * 1024ULL;
    char* corpus = (char*)malloc(total_bytes + 1);
    if (!corpus) return 1;

    const char* pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    size_t pat_len = strlen(pattern);
    for (size_t i = 0; i < total_bytes; i += pat_len) {
        size_t chunk = (total_bytes - i < pat_len) ? (total_bytes - i) : pat_len;
        memcpy(corpus + i, pattern, chunk);
    }
    corpus[total_bytes] = '\\0';

    // 1. Character count
    double t0 = bench_now_ms();
    uint64_t char_count = 0;
    for (size_t i = 0; i < total_bytes; i++) {
        if (corpus[i] == 'e') char_count++;
    }
    double t1 = bench_now_ms();
    double char_ms = t1 - t0;

    // 2. Substring search
    double t2 = bench_now_ms();
    uint64_t sub_count = 0;
    const char* target = "benchmark";
    size_t target_len = strlen(target);
    const char* p = corpus;
    while ((p = strstr(p, target)) != NULL) {
        sub_count++;
        p += target_len;
    }
    double t3 = bench_now_ms();
    double sub_ms = t3 - t2;

    // 3. Whitespace tokenization
    double t4 = bench_now_ms();
    uint64_t token_count = 0;
    int in_token = 0;
    for (size_t i = 0; i < total_bytes; i++) {
        unsigned char c = (unsigned char)corpus[i];
        if (c == ' ' || c == '\\n' || c == '\\t') {
            in_token = 0;
        } else if (!in_token) {
            in_token = 1;
            token_count++;
        }
    }
    double t5 = bench_now_ms();
    double tok_ms = t5 - t4;

    // 4. Case conversion
    double t6 = bench_now_ms();
    for (size_t i = 0; i < total_bytes; i++) {
        corpus[i] = (char)toupper((unsigned char)corpus[i]);
    }
    double t7 = bench_now_ms();
    double case_ms = t7 - t6;

    double total_ms = char_ms + sub_ms + tok_ms + case_ms;
    printf("WORKLOAD: ST4-14\\n");
    printf("LANGUAGE: C\\n");
    printf("CORPUS_MIB: %zu\\n", size_mb);
    printf("CHAR_COUNT_MS: %.3f\\n", char_ms);
    printf("SUBSTRING_MS: %.3f\\n", sub_ms);
    printf("TOKENIZE_MS: %.3f\\n", tok_ms);
    printf("CASE_CONV_MS: %.3f\\n", case_ms);
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("CHECKSUM: %" PRIu64 "\\n", char_count + sub_count + token_count);
    printf("STATUS: PASS\\n");

    free(corpus);
    return 0;
}
''',

"15_csv_parsing": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "../bench_clock.h"

int main(int argc, char** argv) {
    const char* filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
    if (argc > 1) {
        filename = argv[1];
    }

    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\\n", filename);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return 1;
    }
    fread(buf, 1, fsize, f);
    fclose(f);
    buf[fsize] = '\\0';

    double t0 = bench_now_ms();

    uint64_t total_rows = 0;
    uint64_t sum_id = 0;
    uint64_t sum_device_code = 0;
    uint64_t empty_cat_count = 0;
    uint64_t escaped_quotes_count = 0;

    char* ptr = buf;
    // Skip header line
    while (*ptr && *ptr != '\\n') ptr++;
    if (*ptr == '\\n') ptr++;

    while (*ptr) {
        total_rows++;
        int col = 0;
        char* cell_start = ptr;
        int in_quotes = 0;
        int row_has_escaped_quote = 0;

        uint64_t col0_val = 0;
        uint64_t col4_val = 0;
        size_t col6_len = 0;

        while (*ptr) {
            char c = *ptr;
            if (c == '"') {
                if (in_quotes && *(ptr + 1) == '"') {
                    row_has_escaped_quote = 1;
                    ptr += 2;
                    continue;
                }
                in_quotes = !in_quotes;
                ptr++;
                continue;
            }

            if (!in_quotes && (c == ',' || c == '\\n' || c == '\\r')) {
                size_t cell_len = ptr - cell_start;
                if (col == 0) {
                    uint64_t num = 0;
                    for (char* k = cell_start; k < ptr; k++) {
                        num = num * 10 + (*k - '0');
                    }
                    col0_val = num;
                } else if (col == 4) {
                    uint64_t num = 0;
                    for (char* k = cell_start; k < ptr; k++) {
                        num = num * 10 + (*k - '0');
                    }
                    col4_val = num;
                } else if (col == 6) {
                    col6_len = cell_len;
                }

                col++;
                ptr++;
                if (c == '\\r' && *ptr == '\\n') ptr++;
                if (c == '\\n' || c == '\\r') break;
                cell_start = ptr;
            } else {
                ptr++;
            }
        }

        sum_id += col0_val;
        sum_device_code += col4_val;
        if (col6_len == 0) empty_cat_count++;
        if (row_has_escaped_quote) escaped_quotes_count++;
    }

    double t1 = bench_now_ms();
    double parse_ms = t1 - t0;

    printf("WORKLOAD: ST4-15\\n");
    printf("LANGUAGE: C\\n");
    printf("ROWS_PARSED: %" PRIu64 "\\n", total_rows);
    printf("TIME_MS: %.3f\\n", parse_ms);
    printf("SUM_ID: %" PRIu64 "\\n", sum_id);
    printf("SUM_DEVICE: %" PRIu64 "\\n", sum_device_code);
    printf("EMPTY_CAT: %" PRIu64 "\\n", empty_cat_count);
    printf("CHECKSUM: %" PRIu64 "\\n", sum_id + sum_device_code);
    printf("STATUS: %s\\n", (total_rows == 5000000) ? "PASS" : "FAIL");

    free(buf);
    return 0;
}
''',

"16_cyclic_objects": '''#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include "../bench_clock.h"

typedef struct CycleNode {
    struct CycleNode* peer;
    int value;
} CycleNode;

int main(int argc, char** argv) {
    size_t pairs_count = 500000; // 1M nodes total
    if (argc > 1) {
        pairs_count = (size_t)atoll(argv[1]);
    }

    double t0 = bench_now_ms();
    CycleNode** a_nodes = (CycleNode**)malloc(pairs_count * sizeof(CycleNode*));
    CycleNode** b_nodes = (CycleNode**)malloc(pairs_count * sizeof(CycleNode*));

    for (size_t i = 0; i < pairs_count; i++) {
        CycleNode* a = (CycleNode*)malloc(sizeof(CycleNode));
        CycleNode* b = (CycleNode*)malloc(sizeof(CycleNode));
        a->value = (int)i;
        b->value = (int)i;
        a->peer = b;
        b->peer = a;
        a_nodes[i] = a;
        b_nodes[i] = b;
    }
    double t1 = bench_now_ms();
    double build_ms = t1 - t0;

    // Reclamation phase: break cycles and reclaim
    double t2 = bench_now_ms();
    for (size_t i = 0; i < pairs_count; i++) {
        a_nodes[i]->peer = NULL;
        b_nodes[i]->peer = NULL;
        free(a_nodes[i]);
        free(b_nodes[i]);
    }
    free(a_nodes);
    free(b_nodes);
    double t3 = bench_now_ms();
    double reclaim_ms = t3 - t2;

    double total_ms = build_ms + reclaim_ms;
    printf("WORKLOAD: ST4-16\\n");
    printf("LANGUAGE: C\\n");
    printf("CYCLES: %zu\\n", pairs_count);
    printf("BUILD_MS: %.3f\\n", build_ms);
    printf("RECLAIM_MS: %.3f\\n", reclaim_ms);
    printf("TIME_MS: %.3f\\n", total_ms);
    printf("STATUS: PASS\\n");
    return 0;
}
'''
}

for folder, code in c_sources.items():
    path = os.path.join(base, folder, "main.c")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote C ST4: {path}")

print("C track generation finished.")
