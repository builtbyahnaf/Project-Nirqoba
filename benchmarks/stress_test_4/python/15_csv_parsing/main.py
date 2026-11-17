import time
import csv
import io
import sys
import os

def run():
    csv_file = r"benchmarks\stress_test_4\artifacts\st4_15_benchmark_5m.csv"
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
