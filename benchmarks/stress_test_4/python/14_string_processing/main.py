import time
import sys

def run():
    size_mb = 256
    if len(sys.argv) > 1:
        size_mb = int(sys.argv[1])
        
    # Generate deterministic text
    pattern = "Nirqoba high performance compiler benchmark suite text processing tokenization. "
    repeats = (size_mb * 1024 * 1024) // len(pattern) + 1
    corpus = (pattern * repeats)[:size_mb * 1024 * 1024]
    
    # 1. Character frequency counting
    t0 = time.perf_counter()
    char_count = corpus.count('e')
    t1 = time.perf_counter()
    char_ms = (t1 - t0) * 1000.0
    
    # 2. Substring search
    t2 = time.perf_counter()
    sub_count = corpus.count("benchmark")
    t3 = time.perf_counter()
    sub_ms = (t3 - t2) * 1000.0
    
    # 3. Whitespace tokenization
    t4 = time.perf_counter()
    tokens = corpus.split()
    token_count = len(tokens)
    t5 = time.perf_counter()
    tok_ms = (t5 - t4) * 1000.0
    
    # 4. Case conversion
    t6 = time.perf_counter()
    upper_corpus = corpus.upper()
    t7 = time.perf_counter()
    case_ms = (t7 - t6) * 1000.0
    
    total_ms = char_ms + sub_ms + tok_ms + case_ms
    print(f"WORKLOAD: ST4-14")
    print(f"LANGUAGE: Python")
    print(f"CORPUS_MIB: {size_mb}")
    print(f"CHAR_COUNT_MS: {char_ms:.3f}")
    print(f"SUBSTRING_MS: {sub_ms:.3f}")
    print(f"TOKENIZE_MS: {tok_ms:.3f}")
    print(f"CASE_CONV_MS: {case_ms:.3f}")
    print(f"TIME_MS: {total_ms:.3f}")
    print(f"CHECKSUM: {token_count + sub_count + char_count}")
    print(f"STATUS: PASS")

if __name__ == "__main__":
    run()
