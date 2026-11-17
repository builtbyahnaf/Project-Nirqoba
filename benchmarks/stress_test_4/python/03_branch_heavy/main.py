import time
import sys

def run():
    n = 100_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    t0 = time.perf_counter()
    state = 123456789
    checksum = 0
    MASK32 = 0xFFFFFFFF
    MASK64 = 0xFFFFFFFFFFFFFFFF
    
    step = 5_000_000
    curr = 0
    while curr < n:
        limit = min(curr + step, n)
        for i in range(curr, limit):
            state = (state * 1664525 + 1013904223) & MASK32
            if i % 2 == 0:
                if state & 1:
                    checksum = (checksum + (state ^ 0x55555555)) & MASK64
                else:
                    if (state >> 16) & 1:
                        checksum = (checksum + (state ^ 0xAAAAAAAA)) & MASK64
                    else:
                        checksum = (checksum + (state * 3)) & MASK64
            else:
                if state < 0x80000000:
                    checksum = (checksum ^ (state >> 3)) & MASK64
                else:
                    checksum = (checksum - (state & 0xFFFF)) & MASK64
        curr = limit

    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    print(f"WORKLOAD: ST4-03")
    print(f"LANGUAGE: Python")
    print(f"INPUTS: {n}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
