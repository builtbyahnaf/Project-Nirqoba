import time
import sys

def run():
    size = 1024
    if len(sys.argv) > 1:
        size = int(sys.argv[1])
        
    # Flat 1D row-major matrices
    a = [(float((i + j) % 100) / 100.0) for i in range(size) for j in range(size)]
    b = [(float((i * j) % 100) / 100.0) for i in range(size) for j in range(size)]
    c = [0.0] * (size * size)
    
    t0 = time.perf_counter()
    # Cache-friendly i-k-j loop
    for i in range(size):
        i_off = i * size
        for k in range(size):
            a_ik = a[i_off + k]
            k_off = k * size
            for j in range(size):
                c[i_off + j] += a_ik * b[k_off + j]
                
    t1 = time.perf_counter()
    time_ms = (t1 - t0) * 1000.0
    
    checksum = sum(c)
    print(f"WORKLOAD: ST4-04")
    print(f"LANGUAGE: Python")
    print(f"MATRIX_DIM: {size}x{size}")
    print(f"TIME_MS: {time_ms:.3f}")
    print(f"CHECKSUM: {checksum:.6f}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
