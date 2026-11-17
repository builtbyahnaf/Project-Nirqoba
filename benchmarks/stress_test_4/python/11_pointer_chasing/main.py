import time
import sys

def run():
    n = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Deterministic pseudo-random permutation
    next_idx = list(range(n))
    state = 0xDEADBEEF
    MASK32 = 0xFFFFFFFF
    # Fisher-Yates shuffle
    for i in range(n - 1, 0, -1):
        state = (state * 1664525 + 1013904223) & MASK32
        j = state % (i + 1)
        next_idx[i], next_idx[j] = next_idx[j], next_idx[i]
        
    # Traversal measurement (strictly timing the random pointer chasing)
    t0 = time.perf_counter()
    curr = 0
    checksum = 0
    for _ in range(n):
        checksum += curr
        curr = next_idx[curr]
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-11")
    print(f"LANGUAGE: Python")
    print(f"NODES: {n}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
