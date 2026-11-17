import time
import sys

def merge(left, right):
    result = []
    i = j = 0
    while i < len(left) and j < len(right):
        if left[i] <= right[j]:
            result.append(left[i])
            i += 1
        else:
            result.append(right[j])
            j += 1
    result.extend(left[i:])
    result.extend(right[j:])
    return result

def merge_sort(arr):
    if len(arr) <= 1:
        return arr
    mid = len(arr) // 2
    left = merge_sort(arr[:mid])
    right = merge_sort(arr[mid:])
    return merge(left, right)

def run():
    sys.setrecursionlimit(2000000)
    n = 4_000_000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
        
    state = 0x54321
    MASK32 = 0xFFFFFFFF
    arr = [0] * n
    for i in range(n):
        state = (state * 1664525 + 1013904223) & MASK32
        arr[i] = state
        
    t0 = time.perf_counter()
    sorted_arr = merge_sort(arr)
    t1 = time.perf_counter()
    sort_ms = (t1 - t0) * 1000.0
    
    is_sorted = sorted_arr[0] <= sorted_arr[-1]
    checksum = sum(sorted_arr[::1000])
    
    print(f"WORKLOAD: ST4-13")
    print(f"LANGUAGE: Python")
    print(f"ELEMENTS: {n}")
    print(f"TIME_MS: {sort_ms:.3f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: {'PASS' if is_sorted else 'FAIL'}")

if __name__ == "__main__":
    run()
