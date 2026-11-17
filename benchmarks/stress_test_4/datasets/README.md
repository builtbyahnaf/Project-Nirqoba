# Stress Test 4 — Datasets Specification

This directory manages the deterministic data generation and reference datasets for Stress Test 4.

## Datasets Overview

1. **ST4-06 (Sorting Input):**
   - 5,000,000 deterministic 32-bit signed integers generated with PRNG seed `0x12345678`.
2. **ST4-08 (Binary Search Query Set):**
   - 20,000,000 sorted integers, 10,000,000 query values with 50% hit probability.
3. **ST4-14 (String Processing Corpus):**
   - 256 MiB deterministic ASCII text containing structured patterns, repetitive keywords, variable whitespace, and mixed case.
4. **ST4-15 (CSV Parsing Dataset):**
   - **File:** [`../artifacts/st4_15_benchmark_5m.csv`](../artifacts/st4_15_benchmark_5m.csv)
   - **Generated Size:** 429.52 MiB (450,387,624 bytes), exactly 5,000,000 data rows × 8 columns.
   - **Validation Summary:** [`../artifacts/st4_15_benchmark_5m_summary.json`](../artifacts/st4_15_benchmark_5m_summary.json)
   - **Columns:**
     1. `id` (int64, sequential $1 \dots 5,000,000$)
     2. `timestamp` (int64, unix timestamp)
     3. `reading_val` (float64, 3 decimal places)
     4. `label` (quoted string containing internal commas)
     5. `device_code` (int32)
     6. `variance` (float64, 4 decimal places)
     7. `category` (string, includes empty fields)
     8. `notes` (string, includes empty fields and escaped quotes `""`)
   - **Exact Mathematical Invariants for Parser Validation:**
     - `sum_id`: `12,500,002,500,000`
     - `sum_timestamp`: `8,500,215,576,455,200`
     - `sum_device_code`: `2,747,402,264`
     - `sum_reading_val`: `-17,612.736`
     - `sum_variance`: `249,789.3917`
     - `count_empty_category`: `2,000,000`
     - `count_empty_notes`: `1,000,000`
     - `count_rows_with_escaped_quotes`: `2,000,000`
