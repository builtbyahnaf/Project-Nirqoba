# Stress Test 4 — C Track

- **Language:** c
- **Build Command:** gcc -O3 -DNDEBUG -std=c17
- **Memory Model:** malloc/free or stack allocation
- **Source Extension:** .c

## Workload Directory Mapping

| Directory | Workload & Objective | Target Implementation |
|---|---|---|
| 01_integer_arithmetic/ | ST4-01: Integer arithmetic throughput (500M ops) | main.c |
| 02_floating_point/ | ST4-02: Floating-point throughput (100M ops) | main.c |
| 03_branch_heavy/ | ST4-03: Branch-heavy computation (100M decisions) | main.c |
| 04_matrix_multiplication/ | ST4-04: Matrix multiplication (1024x1024 dense) | main.c |
| 05_vector_reduction/ | ST4-05: Vector construction & reduction (50M elements) | main.c |
| 06_sorting/ | ST4-06: Sorting (5M 32-bit integers) | main.c |
| 07_hash_map/ | ST4-07: Hash map operations (5M insert/lookup, 1M remove) | main.c |
| 08_binary_search/ | ST4-08: Binary search (20M sorted, 10M queries) | main.c |
| 09_small_object_allocation/ | ST4-09: Small-object allocation (10M objects) | main.c |
| 10_large_memory_throughput/ | ST4-10: Large memory throughput (512 MiB buffer) | main.c |
| 11_pointer_chasing/ | ST4-11: Randomized pointer chasing (10M nodes) | main.c |
| 12_binary_tree/ | ST4-12: Binary tree construction & traversal (depth 20) | main.c |
| 13_recursive_overhead/ | ST4-13: Recursive function overhead (4M merge sort) | main.c |
| 14_string_processing/ | ST4-14: String processing (256 MiB text operations) | main.c |
| 15_csv_parsing/ | ST4-15: CSV parsing (5M rows x 8 cols in-memory) | main.c |
| 16_cyclic_objects/ | ST4-16: Cyclic object allocation & reclamation (500k cycles) | main.c |
