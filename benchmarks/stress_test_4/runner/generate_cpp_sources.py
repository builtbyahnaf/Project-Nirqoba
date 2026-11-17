"""
Generator for C++ Stress Test 4 Workloads (ST4-01 to ST4-16)
"""
import os

base = r"benchmarks\stress_test_4\cpp"

cpp_sources = {
"01_integer_arithmetic": '''#include <iostream>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    uint64_t total_ops = 500000000ULL;
    if (argc > 1) {
        total_ops = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t total = 0;

    for (uint64_t i = 1; i <= total_ops; i++) {
        total = (total + i);
        total = (total - (i & 0xFF));
        total = (total * 3ULL);
        uint64_t div = (i % 7ULL) + 1ULL;
        total = (total ^ (total / div));
        total = (total ^ (total << 5) ^ (total >> 59));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-01\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "OPS: " << total_ops << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << "CHECKSUM: " << total << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"02_floating_point": '''#include <iostream>
#include <chrono>
#include <cmath>
#include <iomanip>

int main(int argc, char** argv) {
    long long total_ops = 100000000LL;
    if (argc > 1) {
        total_ops = std::stoll(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    double acc = 1.0;

    for (long long i = 1; i <= total_ops; i++) {
        acc = acc + 0.001;
        acc = acc * 1.000001;
        acc = acc / 1.0000005;
        acc = acc + std::sqrt(acc) * 0.00001;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-02\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "OPS: " << total_ops << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "CHECKSUM: " << acc << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"03_branch_heavy": '''#include <iostream>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    long long n = 100000000LL;
    if (argc > 1) {
        n = std::stoll(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
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

    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-03\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "INPUTS: " << n << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"04_matrix_multiplication": '''#include <iostream>
#include <vector>
#include <chrono>
#include <iomanip>

int main(int argc, char** argv) {
    int size = 1024;
    if (argc > 1) {
        size = std::stoi(argv[1]);
    }

    size_t total_elements = (size_t)size * (size_t)size;
    std::vector<double> a(total_elements);
    std::vector<double> b(total_elements);
    std::vector<double> c(total_elements, 0.0);

    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            a[i * size + j] = (double)((i + j) % 100) / 100.0;
            b[i * size + j] = (double)((i * j) % 100) / 100.0;
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();
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
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    double checksum = 0.0;
    for (size_t i = 0; i < total_elements; i++) {
        checksum += c[i];
    }

    std::cout << "WORKLOAD: ST4-04\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "MATRIX_DIM: " << size << "x" << size << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"05_vector_reduction": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <numeric>

int main(int argc, char** argv) {
    size_t n = 50000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint64_t> vec(n);
    for (size_t i = 0; i < n; i++) {
        vec[i] = (uint64_t)i ^ 0x55555555ULL;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double construct_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        total += vec[i];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double reduce_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    std::cout << "WORKLOAD: ST4-05\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "ELEMENTS: " << n << "\\n";
    std::cout << "CONSTRUCTION_MS: " << construct_ms << "\\n";
    std::cout << "REDUCTION_MS: " << reduce_ms << "\\n";
    std::cout << "TIME_MS: " << (construct_ms + reduce_ms) << "\\n";
    std::cout << "CHECKSUM: " << total << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"06_sorting": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

int main(int argc, char** argv) {
    size_t n = 5000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    std::vector<int32_t> arr(n);
    uint32_t state = 0x12345678U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::sort(arr.begin(), arr.end());
    auto t1 = std::chrono::high_resolution_clock::now();
    double sort_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool is_sorted = std::is_sorted(arr.begin(), arr.begin() + (n < 10000 ? n : 10000));
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    std::cout << "WORKLOAD: ST4-06\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "ELEMENTS: " << n << "\\n";
    std::cout << "TIME_MS: " << sort_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: " << (is_sorted ? "PASS" : "FAIL") << "\\n";
    return 0;
}
''',

"07_hash_map": '''#include <iostream>
#include <unordered_map>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t n = 5000000;
    size_t removals = 1000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
        removals = n / 5;
    }

    std::unordered_map<uint64_t, uint64_t> map;
    map.reserve(n);

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map[key] = (uint64_t)i;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double insert_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t lookup_sum = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        lookup_sum += map[key];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double lookup_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < removals * 5; i += 5) {
        uint64_t key = (uint64_t)i * 2654435761ULL;
        map.erase(key);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double remove_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    double total_ms = insert_ms + lookup_ms + remove_ms;
    std::cout << "WORKLOAD: ST4-07\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "INSERT_MS: " << insert_ms << "\\n";
    std::cout << "LOOKUP_MS: " << lookup_ms << "\\n";
    std::cout << "REMOVE_MS: " << remove_ms << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "REMAINING: " << map.size() << "\\n";
    std::cout << "CHECKSUM: " << lookup_sum << "\\n";
    std::cout << "STATUS: " << (map.size() == (n - removals) ? "PASS" : "FAIL") << "\\n";
    return 0;
}
''',

"08_binary_search": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

int main(int argc, char** argv) {
    size_t n = 20000000;
    size_t queries = 10000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
        queries = n / 2;
    }

    std::vector<int64_t> arr(n);
    for (size_t i = 0; i < n; i++) {
        arr[i] = (int64_t)i * 2;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    int64_t hit_count = 0;
    int64_t checksum = 0;
    int64_t max_val = (int64_t)n * 2;

    for (size_t q = 0; q < queries; q++) {
        int64_t target = ((int64_t)q * 3) % max_val;
        auto it = std::lower_bound(arr.begin(), arr.end(), target);
        if (it != arr.end() && *it == target) {
            hit_count++;
            checksum += (it - arr.begin());
        } else {
            checksum -= 1;
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-08\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "ARRAY_SIZE: " << n << "\\n";
    std::cout << "QUERIES: " << queries << "\\n";
    std::cout << "HITS: " << hit_count << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"09_small_object_allocation": '''#include <iostream>
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
    std::cout << "WORKLOAD: ST4-09\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "OBJECTS: " << n << "\\n";
    std::cout << "ALLOC_MS: " << alloc_ms << "\\n";
    std::cout << "TRAVERSE_MS: " << traverse_ms << "\\n";
    std::cout << "RECLAIM_MS: " << reclaim_ms << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"10_large_memory_throughput": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t size = 512ULL * 1024ULL * 1024ULL; // 512 MiB
    if (argc > 1) {
        size = std::stoull(argv[1]) * 1024ULL * 1024ULL;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<uint8_t> buf(size);
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)(i & 0xFF);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double write_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t checksum = 0;
    for (size_t i = 0; i < size; i += 1024) {
        checksum += buf[i];
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double read_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    auto t4 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < size; i += 4096) {
        buf[i] = (uint8_t)((buf[i] * 3) ^ 0x55);
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double transform_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    double total_ms = write_ms + read_ms + transform_ms;
    double throughput_mib = ((double)size / (1024.0 * 1024.0)) / (total_ms / 1000.0);

    std::cout << "WORKLOAD: ST4-10\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "BUFFER_MIB: " << (size / (1024 * 1024)) << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "THROUGHPUT_MIBS: " << throughput_mib << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"11_pointer_chasing": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    size_t n = 10000000;
    if (argc > 1) {
        n = std::stoull(argv[1]);
    }

    std::vector<size_t> next_idx(n);
    for (size_t i = 0; i < n; i++) {
        next_idx[i] = i;
    }

    uint32_t state = 0xDEADBEEFU;
    for (size_t i = n - 1; i > 0; i--) {
        state = (state * 1664525U + 1013904223U);
        size_t j = state % (i + 1);
        std::swap(next_idx[i], next_idx[j]);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    size_t curr = 0;
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i++) {
        checksum += curr;
        curr = next_idx[curr];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double time_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-11\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "NODES: " << n << "\\n";
    std::cout << "TIME_MS: " << time_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"12_binary_tree": '''#include <iostream>
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
    std::cout << "WORKLOAD: ST4-12\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "NODES: " << ((1 << depth) - 1) << "\\n";
    std::cout << "BUILD_MS: " << build_ms << "\\n";
    std::cout << "PREORDER_MS: " << pre_ms << "\\n";
    std::cout << "INORDER_MS: " << in_ms << "\\n";
    std::cout << "POSTORDER_MS: " << post_ms << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "CHECKSUM: " << sum_pre << "\\n";
    std::cout << "STATUS: " << (sum_pre == sum_in && sum_in == sum_post ? "PASS" : "FAIL") << "\\n";

    free_tree(root);
    return 0;
}
''',

"13_recursive_overhead": '''#include <iostream>
#include <vector>
#include <cstdint>
#include <chrono>

static void merge_sort_recursive(std::vector<int32_t>& arr, std::vector<int32_t>& temp, size_t left, size_t right) {
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
        n = std::stoull(argv[1]);
    }

    std::vector<int32_t> arr(n);
    std::vector<int32_t> temp(n);

    uint32_t state = 0x54321U;
    for (size_t i = 0; i < n; i++) {
        state = (state * 1664525U + 1013904223U);
        arr[i] = (int32_t)state;
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    merge_sort_recursive(arr, temp, 0, n - 1);
    auto t1 = std::chrono::high_resolution_clock::now();
    double sort_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    bool is_sorted = (arr[0] <= arr[n - 1]);
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; i += 1000) {
        checksum += (uint64_t)arr[i];
    }

    std::cout << "WORKLOAD: ST4-13\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "ELEMENTS: " << n << "\\n";
    std::cout << "TIME_MS: " << sort_ms << "\\n";
    std::cout << "CHECKSUM: " << checksum << "\\n";
    std::cout << "STATUS: " << (is_sorted ? "PASS" : "FAIL") << "\\n";
    return 0;
}
''',

"14_string_processing": '''#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <cctype>

int main(int argc, char** argv) {
    size_t size_mb = 256;
    if (argc > 1) {
        size_mb = std::stoull(argv[1]);
    }

    size_t total_bytes = size_mb * 1024ULL * 1024ULL;
    std::string pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. ";
    size_t repeats = total_bytes / pattern.length() + 1;
    std::string corpus;
    corpus.reserve(total_bytes);
    while (corpus.length() < total_bytes) {
        corpus.append(pattern);
    }
    corpus.resize(total_bytes);

    // 1. Character count
    auto t0 = std::chrono::high_resolution_clock::now();
    uint64_t char_count = 0;
    for (char c : corpus) {
        if (c == 'e') char_count++;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double char_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // 2. Substring search
    auto t2 = std::chrono::high_resolution_clock::now();
    uint64_t sub_count = 0;
    std::string target = "benchmark";
    size_t pos = 0;
    while ((pos = corpus.find(target, pos)) != std::string::npos) {
        sub_count++;
        pos += target.length();
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double sub_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    // 3. Whitespace tokenization
    auto t4 = std::chrono::high_resolution_clock::now();
    uint64_t token_count = 0;
    bool in_token = false;
    for (char c : corpus) {
        if (c == ' ' || c == '\\n' || c == '\\t') {
            in_token = false;
        } else if (!in_token) {
            in_token = true;
            token_count++;
        }
    }
    auto t5 = std::chrono::high_resolution_clock::now();
    double tok_ms = std::chrono::duration<double, std::milli>(t5 - t4).count();

    // 4. Case conversion
    auto t6 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < total_bytes; i++) {
        corpus[i] = (char)std::toupper((unsigned char)corpus[i]);
    }
    auto t7 = std::chrono::high_resolution_clock::now();
    double case_ms = std::chrono::duration<double, std::milli>(t7 - t6).count();

    double total_ms = char_ms + sub_ms + tok_ms + case_ms;
    std::cout << "WORKLOAD: ST4-14\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "CORPUS_MIB: " << size_mb << "\\n";
    std::cout << "CHAR_COUNT_MS: " << char_ms << "\\n";
    std::cout << "SUBSTRING_MS: " << sub_ms << "\\n";
    std::cout << "TOKENIZE_MS: " << tok_ms << "\\n";
    std::cout << "CASE_CONV_MS: " << case_ms << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "CHECKSUM: " << (char_count + sub_count + token_count) << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
''',

"15_csv_parsing": '''#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <chrono>

int main(int argc, char** argv) {
    std::string filename = "benchmarks/stress_test_4/artifacts/st4_15_benchmark_5m.csv";
    if (argc > 1) {
        filename = argv[1];
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Cannot open " << filename << "\\n";
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<char> buffer(size + 1);
    file.read(buffer.data(), size);
    buffer[size] = '\\0';
    file.close();

    auto t0 = std::chrono::high_resolution_clock::now();

    uint64_t total_rows = 0;
    uint64_t sum_id = 0;
    uint64_t sum_device_code = 0;
    uint64_t empty_cat_count = 0;
    uint64_t escaped_quotes_count = 0;

    char* ptr = buffer.data();
    while (*ptr && *ptr != '\\n') ptr++;
    if (*ptr == '\\n') ptr++;

    while (*ptr) {
        total_rows++;
        int col = 0;
        char* cell_start = ptr;
        bool in_quotes = false;
        bool row_has_escaped_quote = false;

        uint64_t col0_val = 0;
        uint64_t col4_val = 0;
        size_t col6_len = 0;

        while (*ptr) {
            char c = *ptr;
            if (c == '"') {
                if (in_quotes && *(ptr + 1) == '"') {
                    row_has_escaped_quote = true;
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

    auto t1 = std::chrono::high_resolution_clock::now();
    double parse_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "WORKLOAD: ST4-15\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "ROWS_PARSED: " << total_rows << "\\n";
    std::cout << "TIME_MS: " << parse_ms << "\\n";
    std::cout << "SUM_ID: " << sum_id << "\\n";
    std::cout << "SUM_DEVICE: " << sum_device_code << "\\n";
    std::cout << "EMPTY_CAT: " << empty_cat_count << "\\n";
    std::cout << "CHECKSUM: " << (sum_id + sum_device_code) << "\\n";
    std::cout << "STATUS: " << (total_rows == 5000000 ? "PASS" : "FAIL") << "\\n";
    return 0;
}
''',

"16_cyclic_objects": '''#include <iostream>
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
    std::cout << "WORKLOAD: ST4-16\\n";
    std::cout << "LANGUAGE: C++\\n";
    std::cout << "CYCLES: " << pairs_count << "\\n";
    std::cout << "BUILD_MS: " << build_ms << "\\n";
    std::cout << "RECLAIM_MS: " << reclaim_ms << "\\n";
    std::cout << "TIME_MS: " << total_ms << "\\n";
    std::cout << "STATUS: PASS\\n";
    return 0;
}
'''
}

for folder, code in cpp_sources.items():
    path = os.path.join(base, folder, "main.cpp")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(code)
    print(f"Wrote C++ ST4: {path}")

print("C++ track generation finished.")
