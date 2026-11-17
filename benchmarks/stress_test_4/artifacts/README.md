# Stress Test 4 — Benchmark Artifacts

This directory contains the benchmark suite artifacts and dataset files:

| Artifact | Size | Description |
|---|---|---|
| [`st4_15_benchmark_5m.csv`](st4_15_benchmark_5m.csv) | 429.52 MiB | **ST4-15 Dataset:** Exactly 5,000,000 rows × 8 columns containing deterministic integers, decimal values, quoted strings with commas, empty fields, and escaped quotes (`""`). |
| [`st4_15_benchmark_5m_summary.json`](st4_15_benchmark_5m_summary.json) | 598 B | Precalculated mathematical aggregates and invariants for ST4-15 verification across all 7 languages. |
| [`stress_test_4_metrics_matrix.csv`](stress_test_4_metrics_matrix.csv) | 35.7 KiB | 154-row results schema matrix covering all 16 workloads + concurrency across Nirqoba, C, C++, Rust, Java, JavaScript, and Python. |

## ST4-15 Validation Aggregates Invariant

All language implementations parsing `st4_15_benchmark_5m.csv` must produce these exact values:

```json
{
  "total_rows": 5000000,
  "sum_id": 12500002500000,
  "sum_timestamp": 8500215576455200,
  "sum_device_code": 2747402264,
  "sum_reading_val": -17612.736,
  "sum_variance": 249789.3917,
  "count_empty_category": 2000000,
  "count_empty_notes": 1000000,
  "count_rows_with_escaped_quotes": 2000000
}
```
