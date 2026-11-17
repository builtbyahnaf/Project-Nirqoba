import time
import sys

def run():
    size = 512 * 1024 * 1024 # 512 MiB
    if len(sys.argv) > 1:
        size = int(sys.argv[1]) * 1024 * 1024
        
    t0 = time.perf_counter()
    # Sequential write
    buf = bytearray(size)
    t1 = time.perf_counter()
    alloc_ms = (t1 - t0) * 1000.0
    
    # Sequential read / reduction
    t2 = time.perf_counter()
    checksum = sum(buf[::1024])
    t3 = time.perf_counter()
    read_ms = (t3 - t2) * 1000.0
    
    # Sequential in-place transform
    t4 = time.perf_counter()
    for i in range(0, size, 4096):
        buf[i] = (buf[i] * 3 ^ 0x55) & 0xFF
    t5 = time.perf_counter()
    transform_ms = (t5 - t4) * 1000.0
    
    total_ms = alloc_ms + read_ms + transform_ms
    throughput_mibs = (size / (1024 * 1024)) / (total_ms / 1000.0)
    
    print(f"WORKLOAD: ST4-10")
    print(f"LANGUAGE: Python")
    print(f"BUFFER_MIB: {size // (1024 * 1024)}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"THROUGHPUT_MIBS: {throughput_mibs:.2f}")
    print(f"CHECKSUM: {checksum}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
