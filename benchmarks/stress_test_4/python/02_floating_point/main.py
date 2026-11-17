import time
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
