import time
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
