import time
import sys

def run():
    n = 50_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    t0 = time.perf_counter()
    # Construction phase
    vec = [(i ^ 0x55555555) for i in range(n)]
    t1 = time.perf_counter()
    construct_ms = (t1 - t0) * 1000.0
    
    # Reduction phase
    t2 = time.perf_counter()
    total = sum(vec)
    t3 = time.perf_counter()
    reduce_ms = (t3 - t2) * 1000.0
    
    print(f"WORKLOAD: ST4-05")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"CONSTRUCTION_MS: {construct_ms:.3f}")
    print(f"REDUCTION_MS: {reduce_ms:.3f}")
    print(f"TIME_MS: {(construct_ms + reduce_ms):.3f}")
    print(f"CHECKSUM: {total}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
