"""
Nirqoba Stress Test 4 — Unified Multi-Language Benchmark Runner
Executes, measures, and validates all 16 workloads across:
C, C++, Rust, Java, JavaScript, Python (and Nirqoba when built).
"""

import os
import sys
import subprocess
import time
import json
import csv
import statistics
import shutil

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARTIFACTS_DIR = os.path.join(BASE_DIR, "artifacts")
RESULTS_RAW = os.path.join(BASE_DIR, "results", "raw")
RESULTS_PROCESSED = os.path.join(BASE_DIR, "results", "processed")

WORKLOADS = [
    "01_integer_arithmetic",
    "02_floating_point",
    "03_branch_heavy",
    "04_matrix_multiplication",
    "05_vector_reduction",
    "06_sorting",
    "07_hash_map",
    "08_binary_search",
    "09_small_object_allocation",
    "10_large_memory_throughput",
    "11_pointer_chasing",
    "12_binary_tree",
    "13_recursive_overhead",
    "14_string_processing",
    "15_csv_parsing",
    "16_cyclic_objects"
]

LANGUAGES = ["c", "cpp", "rust", "java", "javascript", "python"]

def compile_c(wl_dir):
    src = os.path.join(BASE_DIR, "c", wl_dir, "main.c")
    out = os.path.join(BASE_DIR, "c", wl_dir, "bench.exe")
    cmd = ["gcc", "-O3", "-DNDEBUG", src, "-o", out, "-lm"]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0, out

def compile_cpp(wl_dir):
    src = os.path.join(BASE_DIR, "cpp", wl_dir, "main.cpp")
    out = os.path.join(BASE_DIR, "cpp", wl_dir, "bench.exe")
    cmd = ["g++", "-O3", "-DNDEBUG", "-std=c++20", src, "-o", out]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0, out

def compile_rust(wl_dir):
    src = os.path.join(BASE_DIR, "rust", wl_dir, "main.rs")
    out = os.path.join(BASE_DIR, "rust", wl_dir, "bench.exe")
    cmd = ["rustc", "-O", src, "-o", out]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.returncode == 0, out

def compile_java(wl_dir):
    src = os.path.join(BASE_DIR, "java", wl_dir, "Main.java")
    cmd = ["javac", src]
    if shutil.which("javac"):
        res = subprocess.run(cmd, capture_output=True, text=True)
        return res.returncode == 0, os.path.dirname(src)
    return False, "javac not found"

def get_run_command(lang, wl_dir, scale_arg=None):
    if lang == "c":
        exe = os.path.join(BASE_DIR, "c", wl_dir, "bench.exe")
        return [exe] + ([str(scale_arg)] if scale_arg else [])
    elif lang == "cpp":
        exe = os.path.join(BASE_DIR, "cpp", wl_dir, "bench.exe")
        return [exe] + ([str(scale_arg)] if scale_arg else [])
    elif lang == "rust":
        exe = os.path.join(BASE_DIR, "rust", wl_dir, "bench.exe")
        return [exe] + ([str(scale_arg)] if scale_arg else [])
    elif lang == "java":
        cp = os.path.join(BASE_DIR, "java", wl_dir)
        return ["java", "-cp", cp, "Main"] + ([str(scale_arg)] if scale_arg else [])
    elif lang == "javascript":
        src = os.path.join(BASE_DIR, "javascript", wl_dir, "main.js")
        return ["node", "--expose-gc", src] + ([str(scale_arg)] if scale_arg else [])
    elif lang == "python":
        src = os.path.join(BASE_DIR, "python", wl_dir, "main.py")
        return ["python", "-O", src] + ([str(scale_arg)] if scale_arg else [])
    return None

def parse_benchmark_output(stdout):
    info = {}
    for line in stdout.splitlines():
        if ":" in line:
            parts = line.split(":", 1)
            k = parts[0].strip()
            v = parts[1].strip()
            info[k] = v
    return info

def run_single(lang, wl, scale=None, repetitions=1):
    cmd = get_run_command(lang, wl, scale)
    if not cmd:
        return None

    times = []
    last_info = {}
    for r in range(repetitions):
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if p.returncode == 0:
                info = parse_benchmark_output(p.stdout)
                last_info = info
                if "TIME_MS" in info:
                    times.append(float(info["TIME_MS"]))
            else:
                print(f"[{lang.upper()}:{wl}] Error (code {p.returncode}): {p.stderr}")
                return {"status": "FAIL", "error": p.stderr}
        except subprocess.TimeoutExpired:
            return {"status": "TIMEOUT"}

    if times:
        last_info["median_time_ms"] = round(statistics.median(times), 3)
        last_info["min_time_ms"] = round(min(times), 3)
        last_info["max_time_ms"] = round(max(times), 3)
        last_info["std_dev_ms"] = round(statistics.stdev(times), 3) if len(times) > 1 else 0.0
        last_info["runs"] = len(times)
        return last_info
    return {"status": "NO_OUTPUT"}

def main():
    import argparse
    parser = argparse.ArgumentParser(description="Run Stress Test 4 Benchmarks")
    parser.add_argument("--lang", choices=LANGUAGES + ["all"], default="all")
    parser.add_argument("--workload", default="all")
    parser.add_argument("--scale", type=int, default=None, help="Scale parameter (e.g. smaller count for dry run)")
    parser.add_argument("--runs", type=int, default=1, help="Number of repetitions")
    args = parser.parse_args()

    selected_langs = LANGUAGES if args.lang == "all" else [args.lang]
    selected_wls = WORKLOADS if args.workload == "all" else [w for w in WORKLOADS if args.workload in w]

    print(f"Selected Languages: {selected_langs}")
    print(f"Selected Workloads: {len(selected_wls)} workloads")

    for wl in selected_wls:
        print(f"\n==================== WORKLOAD: {wl} ====================")
        # Pre-compile native tracks if needed
        if "c" in selected_langs:
            ok, _ = compile_c(wl)
            if not ok: print(f"C compile failed for {wl}")
        if "cpp" in selected_langs:
            ok, _ = compile_cpp(wl)
            if not ok: print(f"C++ compile failed for {wl}")
        if "rust" in selected_langs:
            ok, _ = compile_rust(wl)
            if not ok: print(f"Rust compile failed for {wl}")
        if "java" in selected_langs:
            compile_java(wl)

        for lang in selected_langs:
            res = run_single(lang, wl, scale=args.scale, repetitions=args.runs)
            if res and "TIME_MS" in res:
                print(f"  {lang.upper():10} | Time: {res.get('median_time_ms', res.get('TIME_MS'))} ms | Checksum: {res.get('CHECKSUM', 'N/A')} | Status: {res.get('STATUS', 'PASS')}")
            else:
                print(f"  {lang.upper():10} | Result: {res}")

if __name__ == "__main__":
    main()
