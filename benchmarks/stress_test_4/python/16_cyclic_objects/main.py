import time
import gc
import sys

class CycleNode:
    __slots__ = ('peer', 'value')
    def __init__(self, value):
        self.value = value
        self.peer = None

def run():
    pairs_count = 500_000 # 1,000,000 nodes total
    if len(sys.argv) > 1:
        pairs_count = int(sys.argv[1])
        
    # Allocation & cycle establishment
    t0 = time.perf_counter()
    nodes = []
    for i in range(pairs_count):
        a = CycleNode(i)
        b = CycleNode(i)
        a.peer = b
        b.peer = a
        nodes.append((a, b))
    t1 = time.perf_counter()
    build_ms = (t1 - t0) * 1000.0
    
    # Drop external references
    del nodes
    
    # Reclamation phase
    t2 = time.perf_counter()
    reclaimed = gc.collect()
    t3 = time.perf_counter()
    reclaim_ms = (t3 - t2) * 1000.0
    
    total_ms = build_ms + reclaim_ms
    print(f"WORKLOAD: ST4-16")
    print(f"LANGUAGE: Python")
    print(f"CYCLES: {pairs_count}")
    print(f"BUILD_MS: {build_ms:.3f}")
    print(f"RECLAIM_MS: {reclaim_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"RECLAIMED_OBJECTS: {reclaimed}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
