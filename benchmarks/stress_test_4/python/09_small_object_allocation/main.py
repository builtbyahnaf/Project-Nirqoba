import time
import sys
import gc

class Node:
    __slots__ = ('a', 'b', 'next')
    def __init__(self, a, b, next_node):
        self.a = a
        self.b = b
        self.next = next_node

def run():
    n = 10_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    # Allocation & link
    t0 = time.perf_counter()
    head = None
    curr = None
    for i in range(n):
        node = Node(i, i * 2, None)
        if head is None:
            head = node
            curr = node
        else:
            curr.next = node
            curr = node
    t1 = time.perf_counter()
    alloc_ms = (t1 - t0) * 1000.0
    
    # Touch & traversal
    t2 = time.perf_counter()
    checksum = 0
    walker = head
    while walker is not None:
        checksum += walker.a + walker.b
        walker = walker.next
    t3 = time.perf_counter()
    traverse_ms = (t3 - t2) * 1000.0
    
    # Reclamation
    t4 = time.perf_counter()
    head = None
    curr = None
    walker = None
    gc.collect()
    t5 = time.perf_counter()
    reclaim_ms = (t5 - t4) * 1000.0
    
    total_ms = alloc_ms + traverse_ms + reclaim_ms
    print(f"WORKLOAD: ST4-09")
    print(f"LANGUAGE: Python")
    print(f"OBJECTS: {n}")
    print(f"ALLOC_MS: {alloc_ms:.3f}")
    print(f"TRAVERSE_MS: {traverse_ms:.3f}")
    print(f"RECLAIM_MS: {reclaim_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
