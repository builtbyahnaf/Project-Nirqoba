# Stress Test 4 — Rust Track

- **Language:** rust
- **Build Command:** cargo build --release
- **Memory Model:** Ownership, borrowing, Box, std containers
- **Source Extension:** .rs

## Workload Directory Mapping

| Directory | Workload & Objective | Target Implementation |
|---|---|---|
| 01_integer_arithmetic/ | ST4-01: Integer arithmetic throughput (500M ops) | main.rs |
| 02_floating_point/ | ST4-02: Floating-point throughput (100M ops) | main.rs |
| 03_branch_heavy/ | ST4-03: Branch-heavy computation (100M decisions) | main.rs |
| 04_matrix_multiplication/ | ST4-04: Matrix multiplication (1024x1024 dense) | main.rs |
| 05_vector_reduction/ | ST4-05: Vector construction & reduction (50M elements) | main.rs |
| 06_sorting/ | ST4-06: Sorting (5M 32-bit integers) | main.rs |
| 07_hash_map/ | ST4-07: Hash map operations (5M insert/lookup, 1M remove) | main.rs |
| 08_binary_search/ | ST4-08: Binary search (20M sorted, 10M queries) | main.rs |
| 09_small_object_allocation/ | ST4-09: Small-object allocation (10M objects) | main.rs |
| 10_large_memory_throughput/ | ST4-10: Large memory throughput (512 MiB buffer) | main.rs |
| 11_pointer_chasing/ | ST4-11: Randomized pointer chasing (10M nodes) | main.rs |
| 12_binary_tree/ | ST4-12: Binary tree construction & traversal (depth 20) | main.rs |
| 13_recursive_overhead/ | ST4-13: Recursive function overhead (4M merge sort) | main.rs |
| 14_string_processing/ | ST4-14: String processing (256 MiB text operations) | main.rs |
| 15_csv_parsing/ | ST4-15: CSV parsing (5M rows x 8 cols in-memory) | main.rs |
| 16_cyclic_objects/ | ST4-16: Cyclic object allocation & reclamation (500k cycles) | main.rs |
