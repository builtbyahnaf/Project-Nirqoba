import time
import sys

def run():
    n = 5_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Generate deterministic pseudo-random input using LCG seed 0x12345678
    state = 0x12345678
    MASK32 = 0xFFFFFFFF
    arr = [0] * n
    for i in range(n):
        state = (state * 1664525 + 1013904223) & MASK32
        arr[i] = state
        
    # Timed sorting phase
    t0 = time.perf_counter()
    arr.sort()
    t1 = time.perf_counter()
    sort_ms = (t1 - t0) * 1000.0
    
    # Correctness verification
    is_sorted = all(arr[i] <= arr[i + 1] for i in range(min(n - 1, 10000))) and arr[0] <= arr[-1]
    checksum = sum(arr[::1000]) # Sampled sum
    
    print(f"WORKLOAD: ST4-06")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"TIME_MS: {sort_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: {'PASS' if is_sorted else 'FAIL'}")

if __name__ == "__main__":
    run()
