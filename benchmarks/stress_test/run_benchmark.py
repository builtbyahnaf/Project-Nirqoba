#!/usr/bin/env python3
"""
Nirqoba vs C vs C++ vs Rust Stress Benchmark Runner
=====================================================
Compiles all benchmark source files and runs timed measurements.
Outputs: benchmark_results.json + printed summary table.

Usage:
    python run_benchmark.py [--compile-only] [--run-only] [--test ID]
"""

import os
import sys
import json
import time
import subprocess
import statistics
import platform
import argparse
import shutil
from pathlib import Path

# -- Paths --------------------------------------------------------------------
PROJECT_ROOT = Path(r"D:\Actual Project Files\Project Nirqoba")
BENCH_DIR    = PROJECT_ROOT / "benchmarks" / "stress_test"
BIN_DIR      = BENCH_DIR / "bin"

NRQ_EXE     = PROJECT_ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"
RUSTC_EXE   = Path(r"C:\Users\USER\.cargo\bin\rustc.exe")
VCVARS_BAT  = Path(r"C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat")

WARMUP_RUNS   = 3
MEASURE_RUNS  = 10

# -- Test registry ------------------------------------------------------------
TESTS = [
    {
        "id": "01",
        "name": "Integer Arithmetic",
        "expected_lines": ["1096522383423917612"],
        "timeout": 120,
    },
    {
        "id": "02",
        "name": "Float Arithmetic (Leibniz pi)",
        "expected_lines": None,   # floating point - just check runs
        "timeout": 120,
    },
    {
        "id": "03",
        "name": "Bitwise Operations (XorShift)",
        "expected_lines": ["-6808847343156257348"],
        "timeout": 60,
    },
    {
        "id": "04",
        "name": "Fibonacci Recursion fib(40)",
        "expected_lines": ["102334155"],
        "timeout": 180,
    },
    {
        "id": "05",
        "name": "Array Traversal (10M ints, 5 passes)",
        "expected_lines": ["1019846433803136448"],
        "timeout": 120,
    },
    {
        "id": "06",
        "name": "Dynamic Allocation (1M alloc/free)",
        "expected_lines": ["1000000"],
        "timeout": 60,
    },
    {
        "id": "07",
        "name": "Ownership Transfer (1M objects)",
        "expected_lines": None,   # sum varies by language semantics
        "timeout": 120,
    },
    {
        "id": "08",
        "name": "Sorting (1M ints, quicksort)",
        "expected_lines": ["955", "1074530052", "2147482721"],
        "timeout": 30,
    },
    {
        "id": "09",
        "name": "Matrix Multiplication (512-512 f64)",
        "expected_lines": None,   # floating point
        "timeout": 120,
    },
    {
        "id": "10",
        "name": "String/Byte Processing (10M chars)",
        "expected_lines": ["250000", "2000000"],
        "timeout": 60,
    },
]

# -- Compilation helpers -------------------------------------------------------

def run_cmd(cmd, cwd=None, timeout=120):
    """Run a command, return (returncode, stdout, stderr)."""
    result = subprocess.run(
        cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout
    )
    return result.returncode, result.stdout, result.stderr


def run_cmd_via_cmd(cmd_str, cwd=None, timeout=120):
    """Run a command string via cmd.exe (needed for vcvars)."""
    full = f'cmd /c "{cmd_str}"'
    result = subprocess.run(
        full, shell=True, cwd=cwd, capture_output=True, text=True, timeout=timeout
    )
    return result.returncode, result.stdout, result.stderr


def msvc_compile(src: Path, out: Path, flags="/O2 /nologo", std=""):
    """Compile a C or C++ file with MSVC via vcvars64."""
    std_flag = f"/std:{std}" if std else ""
    cmd = (
        f'call "{VCVARS_BAT}" >nul 2>&1 && '
        f'cl {flags} {std_flag} "{src}" /Fe:"{out}" /link /SUBSYSTEM:CONSOLE'
    )
    rc, out_txt, err = run_cmd_via_cmd(cmd, timeout=60)
    return rc, out_txt + err


def nrq_compile(src: Path, out: Path):
    """Compile a Nirqoba file with -O3."""
    cmd = [str(NRQ_EXE), "build", str(src), "-o", str(out), "-O3"]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


def rust_compile(src: Path, out: Path):
    """Compile a Rust file with rustc -C opt-level=3."""
    cmd = [str(RUSTC_EXE), "-C", "opt-level=3", str(src), "-o", str(out)]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return result.returncode, result.stdout + result.stderr


# -- Run and time --------------------------------------------------------------

def run_exe(exe: Path, timeout=120):
    """Run executable, return (output_lines, elapsed_ms)."""
    t0 = time.perf_counter_ns()
    result = subprocess.run(
        [str(exe)], capture_output=True, text=True, timeout=timeout
    )
    t1 = time.perf_counter_ns()
    elapsed_ms = (t1 - t0) / 1_000_000
    lines = result.stdout.strip().splitlines()
    return lines, elapsed_ms, result.returncode


def measure(exe: Path, warmup=WARMUP_RUNS, runs=MEASURE_RUNS, timeout=120):
    """Warmup then measure; return (samples_ms, last_output_lines)."""
    for _ in range(warmup):
        run_exe(exe, timeout=timeout)
    samples = []
    last_lines = []
    for _ in range(runs):
        lines, ms, _ = run_exe(exe, timeout=timeout)
        samples.append(ms)
        last_lines = lines
    return samples, last_lines


# -- Verification --------------------------------------------------------------

def verify(output_lines, expected_lines, test_id, lang):
    if expected_lines is None:
        return True, "no-check"
    ok = True
    for i, exp in enumerate(expected_lines):
        if i >= len(output_lines):
            print(f"  [WARN] {lang} test {test_id}: expected line {i+1}='{exp}' but output has only {len(output_lines)} lines")
            ok = False
        elif output_lines[i].strip() != exp:
            print(f"  [WARN] {lang} test {test_id}: line {i+1} expected '{exp}' got '{output_lines[i].strip()}'")
            ok = False
    return ok, "ok" if ok else "mismatch"


# -- Main ----------------------------------------------------------------------

def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Nirqoba stress benchmark runner")
    parser.add_argument("--compile-only", action="store_true")
    parser.add_argument("--run-only", action="store_true")
    parser.add_argument("--test", metavar="ID", help="Run only test with this id (e.g. 01)")
    args = parser.parse_args()

    BIN_DIR.mkdir(parents=True, exist_ok=True)

    selected_tests = TESTS
    if args.test:
        selected_tests = [t for t in TESTS if t["id"] == args.test]
        if not selected_tests:
            print(f"No test with id '{args.test}'")
            sys.exit(1)

    langs = [
        ("nirqoba", "nrqba"),
        ("c",       "c"),
        ("cpp",     "cpp"),
        ("rust",    "rs"),
    ]

    results = {}

    # -- Compilation pass -----------------------------------------------------
    if not args.run_only:
        print("=" * 70)
        print("COMPILATION")
        print("=" * 70)
        for test in selected_tests:
            tid = test["id"]
            for lang, ext in langs:
                src_dir = BENCH_DIR / lang
                # find file matching id prefix
                candidates = list(src_dir.glob(f"{tid}_*.{ext}"))
                if not candidates:
                    print(f"  [SKIP] {lang}/{tid}: source not found in {src_dir}")
                    continue
                src = candidates[0]
                out = BIN_DIR / f"{tid}_{lang}.exe"

                print(f"  Compiling {lang}/{src.name} -> {out.name} ...", end=" ", flush=True)
                try:
                    if lang == "nirqoba":
                        rc, log = nrq_compile(src, out)
                    elif lang == "c":
                        rc, log = msvc_compile(src, out, flags="/O2 /nologo")
                    elif lang == "cpp":
                        rc, log = msvc_compile(src, out, flags="/O2 /nologo", std="c++20")
                    elif lang == "rust":
                        rc, log = rust_compile(src, out)
                    if rc == 0:
                        print("OK")
                    else:
                        print(f"FAILED (rc={rc})")
                        print(f"    {log[:500]}")
                except Exception as e:
                    print(f"EXCEPTION: {e}")

    # -- Measurement pass -----------------------------------------------------
    if not args.compile_only:
        print()
        print("=" * 70)
        print(f"BENCHMARKING  ({WARMUP_RUNS} warmups, {MEASURE_RUNS} measured runs each)")
        print("=" * 70)

        for test in selected_tests:
            tid = test["id"]
            name = test["name"]
            timeout = test.get("timeout", 120)
            print(f"\n[{tid}] {name}")
            results[tid] = {"name": name, "langs": {}}

            for lang, _ in langs:
                exe = BIN_DIR / f"{tid}_{lang}.exe"
                if not exe.exists():
                    print(f"  {lang:10s}: SKIP (binary not found)")
                    continue
                try:
                    samples, output_lines = measure(exe, timeout=timeout)
                    ok, status = verify(output_lines, test["expected_lines"], tid, lang)
                    med = statistics.median(samples)
                    mn  = min(samples)
                    mx  = max(samples)
                    stdev = statistics.stdev(samples) if len(samples) > 1 else 0
                    tag = "[OK]" if ok else "[MISMATCH]"
                    print(f"  {lang:10s}: med={med:9.2f}ms  min={mn:9.2f}  max={mx:9.2f}  stdev={stdev:7.2f}  {tag}  output={output_lines[:2]}")
                    results[tid]["langs"][lang] = {
                        "median_ms": med,
                        "min_ms": mn,
                        "max_ms": mx,
                        "stdev_ms": stdev,
                        "samples_ms": samples,
                        "output": output_lines,
                        "output_ok": ok,
                        "status": status,
                    }
                except subprocess.TimeoutExpired:
                    print(f"  {lang:10s}: TIMEOUT (>{timeout}s)")
                except Exception as e:
                    print(f"  {lang:10s}: ERROR: {e}")

    # -- Save JSON -------------------------------------------------------------
    out_json = BENCH_DIR / "benchmark_results.json"
    with open(out_json, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to: {out_json}")

    # -- Summary table ---------------------------------------------------------
    if results:
        print()
        print("=" * 100)
        print("SUMMARY TABLE  (median ms, lower is better)")
        print("=" * 100)
        header = f"{'Test':<45} {'Nirqoba':>12} {'C':>12} {'C++':>12} {'Rust':>12}"
        print(header)
        print("-" * 100)
        for tid, data in results.items():
            row = f"{data['name']:<45}"
            for lang in ("nirqoba", "c", "cpp", "rust"):
                ld = data["langs"].get(lang)
                if ld:
                    row += f" {ld['median_ms']:>11.1f}ms"
                else:
                    row += f" {'N/A':>12}"
            print(row)
        print("-" * 100)
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
