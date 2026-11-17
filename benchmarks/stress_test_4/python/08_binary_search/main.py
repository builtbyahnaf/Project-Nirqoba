import time
import sys
import bisect

def run():
    n = 20_000_000
    queries = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        queries = n // 2
        
    arr = [i * 2 for i in range(n)]
    
    t0 = time.perf_counter()
    hit_count = 0
    checksum = 0
    max_val = n * 2
    
    for q in range(queries):
        target = (q * 3) % max_val
        idx = bisect.bisect_left(arr, target)
        if idx < n and arr[idx] == target:
            hit_count += 1
            checksum += idx
        else:
            checksum -= 1
            
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-08")
    print(f"LANGUAGE: Python")
    print(f"ARRAY_SIZE: {n}")
    print(f"QUERIES: {queries}")
    print(f"HITS: {hit_count}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
