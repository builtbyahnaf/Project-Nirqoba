"""
Deterministic 5,000,000-row CSV Generator for Nirqoba Stress Test 4 (ST4-15)
Adheres strictly to ST4-15 specification in INSTRUCTIONS.md:
- Deterministic CSV file with exactly 5,000,000 rows and 8 columns
- Contains:
  1. integers (id, timestamp, device_code)
  2. decimal numbers (reading_val, variance)
  3. quoted strings with internal commas (label)
  4. empty fields (category, note)
  5. escaped quotes within quoted strings (notes)
- Computes exact mathematical aggregates to enable multi-language correctness validation.
"""

import time
import os
import json

def generate_csv(out_path):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    t0 = time.time()
    total_rows = 5_000_000
    batch_size = 50_000
    
    header = "id,timestamp,reading_val,label,device_code,variance,category,notes\n"
    
    labels = [
        '"Sensor Alpha, Primary"',
        '"Batch reading, OK"',
        '"System telemetry, nominal"',
        '"Sector 7G, active"',
        '"Calibration, pass"'
    ]

    categories = [
        "",              # Empty field
        "category_A",
        "category_B",
        "",              # Empty field
        "category_C"
    ]

    notes_samples = [
        "Normal observation",
        '"Note with ""important"" flag"',                   # Escaped quotes
        "",                                                  # Empty field
        '"Remarks with, comma and ""special"" keyword"',     # Comma and escaped quotes
        "Routine sample check"
    ]

    # Deterministic LCG parameters for reproducibility:
    # X_{n+1} = (a * X_n + c) mod 2^32
    state = 42
    a = 1664525
    c = 1013904223
    
    # Aggregates for validation
    sum_id = 0
    sum_timestamp = 0
    sum_device_code = 0
    sum_reading_val = 0.0
    sum_variance = 0.0
    count_empty_category = 0
    count_empty_notes = 0
    count_escaped_quotes = 0

    print(f"Generating {total_rows:,} rows to: {out_path} ...")

    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(header)
        buffer = []
        
        for i in range(1, total_rows + 1):
            state = (state * a + c) & 0xFFFFFFFF
            
            ts = 1700000000 + (i % 86400)
            val = ((state % 100000) - 50000) / 1000.0
            label = labels[i % 5]
            dev_code = 100 + (state % 900)
            var_val = ((state >> 8) % 1000) / 10000.0
            cat = categories[i % 5]
            note = notes_samples[i % 5]
            
            # Aggregates
            sum_id += i
            sum_timestamp += ts
            sum_device_code += dev_code
            sum_reading_val += val
            sum_variance += var_val
            if not cat:
                count_empty_category += 1
            if not note:
                count_empty_notes += 1
            if '""' in note:
                count_escaped_quotes += 1

            buffer.append(f"{i},{ts},{val:.3f},{label},{dev_code},{var_val:.4f},{cat},{note}\n")
            
            if i % batch_size == 0:
                f.writelines(buffer)
                buffer = []
                if i % 1_000_000 == 0:
                    print(f"  Progress: {i:,} / {total_rows:,} rows ({time.time() - t0:.2f}s elapsed)")

        if buffer:
            f.writelines(buffer)

    elapsed = time.time() - t0
    file_size_bytes = os.path.getsize(out_path)
    file_size_mib = file_size_bytes / (1024 * 1024)

    aggregates = {
        "file_name": os.path.basename(out_path),
        "total_rows": total_rows,
        "total_columns": 8,
        "file_size_bytes": file_size_bytes,
        "file_size_mib": round(file_size_mib, 2),
        "generation_time_seconds": round(elapsed, 2),
        "validation_aggregates": {
            "sum_id": sum_id,
            "sum_timestamp": sum_timestamp,
            "sum_device_code": sum_device_code,
            "sum_int_all": sum_id + sum_timestamp + sum_device_code,
            "sum_reading_val": round(sum_reading_val, 3),
            "sum_variance": round(sum_variance, 4),
            "sum_float_all": round(sum_reading_val + sum_variance, 4),
            "count_empty_category": count_empty_category,
            "count_empty_notes": count_empty_notes,
            "count_rows_with_escaped_quotes": count_escaped_quotes
        }
    }

    # Save aggregates alongside dataset
    summary_path = os.path.splitext(out_path)[0] + "_summary.json"
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(aggregates, f, indent=2)

    print("\nGeneration Complete!")
    print(f"  Target File: {out_path}")
    print(f"  File Size: {file_size_mib:.2f} MiB ({file_size_bytes:,} bytes)")
    print(f"  Total Duration: {elapsed:.2f} seconds")
    print(f"  Aggregates Summary: {summary_path}")
    print(json.dumps(aggregates["validation_aggregates"], indent=4))
    return aggregates

if __name__ == "__main__":
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else r"benchmarks\stress_test_4\artifacts\st4_15_benchmark_5m.csv"
    generate_csv(out)
