import time
import sys

def run():
    n = 5_000_000
    removals = 1_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        removals = n // 5
        
    table = {}
    
    # Insert phase
    t0 = time.perf_counter()
    for i in range(n):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        table[key] = i
    t1 = time.perf_counter()
    insert_ms = (t1 - t0) * 1000.0
    
    # Lookup phase
    t2 = time.perf_counter()
    lookup_sum = 0
    for i in range(n):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        lookup_sum += table[key]
    t3 = time.perf_counter()
    lookup_ms = (t3 - t2) * 1000.0
    
    # Removal phase
    t4 = time.perf_counter()
    for i in range(0, removals * 5, 5):
        key = (i * 2654435761) & 0xFFFFFFFFFFFFFFFF
        del table[key]
    t5 = time.perf_counter()
    remove_ms = (t5 - t4) * 1000.0
    
    rem_count = len(table)
    total_ms = insert_ms + lookup_ms + remove_ms
    
    print(f"WORKLOAD: ST4-07")
    print(f"LANGUAGE: Python")
    print(f"INSERT_MS: {insert_ms:.3f}")
    print(f"LOOKUP_MS: {lookup_ms:.3f}")
    print(f"REMOVE_MS: {remove_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"REMAINING: {rem_count}")
    print(f"CHECKSUM: {lookup_sum}")
    print(f"STATUS: {'PASS' if rem_count == (n - removals) else 'FAIL'}")

if __name__ == "__main__":
    run()
