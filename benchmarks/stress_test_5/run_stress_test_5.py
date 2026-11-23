#!/usr/bin/env python3
"""Compile and run Stress Test 5, the cross-language CDMM gauntlet.

Seven implementations of one four-phase workload:

    nirqoba   CDMM: stack, pool, arena, collector - the compiler decides
    c         manual malloc/free
    cpp       unique_ptr / shared_ptr
    rust      Box / Rc<RefCell<..>>
    java      G1 collector
    js        Node.js, V8
    py        CPython, reference counting + cycle collector

Each process is timed end to end (wall clock) and its peak working set is read
from the OS while it runs. Both numbers include process start-up, which is the
honest cost of running the workload in that language: the harness measures what
a user waits for. The four checksums each track prints are validated against the
values the workload defines, so a fast wrong answer fails the run.

Outputs:
    benchmark_results.json
    results/raw/<impl>_run<N>.txt

Usage:
    python run_stress_test_5.py
    python run_stress_test_5.py --skip-compile
    python run_stress_test_5.py --repeats 5
    python run_stress_test_5.py --impl nirqoba --impl c
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wintypes
import datetime as _datetime
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Paths and toolchain configuration
# ---------------------------------------------------------------------------

BENCH_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = BENCH_DIR.parents[1]
BIN_DIR = BENCH_DIR / "bin"
RAW_DIR = BENCH_DIR / "results" / "raw"
RESULT_JSON = BENCH_DIR / "benchmark_results.json"

NRQ_EXE = PROJECT_ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"
RUSTC_EXE = Path(r"C:\Users\USER\.cargo\bin\rustc.exe")
JDK_BIN = Path(
    r"D:\Victim of space issue\.gradle\jdks\eclipse_adoptium-17-amd64-windows.2\bin"
)
JAVAC_EXE = JDK_BIN / "javac.exe"
JAVA_EXE = JDK_BIN / "java.exe"
VCVARS_BAT = Path(
    r"C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)
NODE_EXE = Path(shutil.which("node") or "node")
PYTHON_EXE = Path(shutil.which("python") or sys.executable)

DEFAULT_REPEATS = 3
RUN_TIMEOUT_SECONDS = 600
COMPILE_TIMEOUT_SECONDS = 300

# ---------------------------------------------------------------------------
# Workload definition
# ---------------------------------------------------------------------------

STACK_ITERATIONS = 200_000
POOL_ITERATIONS = 2_000
POOL_SLOTS = 8_192
TREE_NODES = 8_191
GC_NODES = 40_000

# The four checksums every track must print, in order.
EXPECTED_CHECKSUMS = [
    STACK_ITERATIONS * (STACK_ITERATIONS - 1) // 2,  # stack: sum of ids
    sum(3 * i for i in range(POOL_ITERATIONS)),  # pool: kind + payload
    5 + 1 + 2 + 4,  # arena: node 0 plus the promoted leaf
    GC_NODES * (GC_NODES - 1) // 2,  # gc: sum of node values
]

LANGUAGES = ["nirqoba", "c", "cpp", "rust", "java", "js", "py"]

SOURCE_FILES = {
    "nirqoba": BENCH_DIR / "nirqoba_impl.nrqba",
    "c": BENCH_DIR / "c_impl.c",
    "cpp": BENCH_DIR / "cpp_impl.cpp",
    "rust": BENCH_DIR / "rust_impl.rs",
    "java": BENCH_DIR / "java_impl.java",
    "js": BENCH_DIR / "js_impl.js",
    "py": BENCH_DIR / "py_impl.py",
}

# ---------------------------------------------------------------------------
# Process measurement
# ---------------------------------------------------------------------------


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)


def _peak_working_set_bytes(handle: int) -> int | None:
    counters = PROCESS_MEMORY_COUNTERS()
    counters.cb = ctypes.sizeof(counters)
    ok = _psapi.GetProcessMemoryInfo(
        handle, ctypes.byref(counters), ctypes.sizeof(counters)
    )
    if not ok:
        return None
    return int(counters.PeakWorkingSetSize)


def run_command(command: list[str] | str, *, timeout: int, shell: bool = False) -> tuple[int, str]:
    """Run a command and return (returncode, combined output)."""
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            shell=shell,
        )
    except subprocess.TimeoutExpired as exc:
        return -1, f"timed out after {timeout}s: {exc}"
    except Exception as exc:  # noqa: BLE001 - reported, never swallowed
        return -1, str(exc)
    return completed.returncode, (completed.stdout or b"").decode("utf-8", "replace")


def measure(command: list[str]) -> dict[str, Any]:
    """Run one process, returning wall-clock milliseconds and peak RSS in MiB."""
    started = time.perf_counter()
    proc = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    peak = 0
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, proc.pid)
    try:
        while proc.poll() is None:
            if handle:
                value = _peak_working_set_bytes(handle)
                if value:
                    peak = max(peak, value)
            time.sleep(0.005)
        if handle:
            value = _peak_working_set_bytes(handle)
            if value:
                peak = max(peak, value)
        stdout = (proc.communicate()[0] or b"").decode("utf-8", "replace")
    finally:
        if handle:
            _kernel32.CloseHandle(handle)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    return {
        "wall_ms": elapsed_ms,
        "peak_rss_mib": peak / (1024.0 * 1024.0),
        "returncode": proc.returncode,
        "stdout": stdout,
    }

# ---------------------------------------------------------------------------
# Compilation
# ---------------------------------------------------------------------------


def msvc_command(source: Path, output: Path, *, cpp: bool) -> list[str]:
    std = "/std:c++20 /EHsc" if cpp else ""
    obj = BIN_DIR / (output.stem + ".obj")
    log = Path(os.environ["TEMP"]) / "nrq_vc.log"
    line = (
        f'call "{VCVARS_BAT}" > "{log}" 2>&1 && '
        f'cl /O2 /nologo {std} "{source}" /Fo:"{obj}" /Fe:"{output}" '
        f"/link /SUBSYSTEM:CONSOLE"
    )
    return ["cmd", "/c", line]


def compile_language(language: str) -> tuple[bool, str]:
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    source = SOURCE_FILES[language]
    if not source.exists():
        return False, f"missing source: {source}"

    if language == "nirqoba":
        if not NRQ_EXE.exists():
            return False, f"nirqoba compiler not found: {NRQ_EXE}"
        output = BIN_DIR / "nirqoba_impl.exe"
        code, log = run_command(
            [str(NRQ_EXE), "build", str(source), "-o", str(output), "-O3"],
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        return code == 0, log

    if language == "c":
        output = BIN_DIR / "c_impl.exe"
        gcc = shutil.which("gcc")
        if gcc:
            code, log = run_command(
                [gcc, "-O3", "-o", str(output), str(source)],
                timeout=COMPILE_TIMEOUT_SECONDS,
            )
        else:
            code, log = run_command(
                msvc_command(source, output, cpp=False), timeout=COMPILE_TIMEOUT_SECONDS
            )
        return code == 0, log

    if language == "cpp":
        output = BIN_DIR / "cpp_impl.exe"
        gpp = shutil.which("g++")
        if gpp:
            code, log = run_command(
                [gpp, "-O3", "-std=c++20", "-o", str(output), str(source)],
                timeout=COMPILE_TIMEOUT_SECONDS,
            )
        else:
            code, log = run_command(
                msvc_command(source, output, cpp=True), timeout=COMPILE_TIMEOUT_SECONDS
            )
        return code == 0, log

    if language == "rust":
        if not RUSTC_EXE.exists():
            return False, f"rustc not found: {RUSTC_EXE}"
        output = BIN_DIR / "rust_impl.exe"
        code, log = run_command(
            [str(RUSTC_EXE), "-O", "-C", "target-cpu=native", "-o", str(output), str(source)],
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        return code == 0, log

    if language == "java":
        if not JAVAC_EXE.exists():
            return False, f"javac not found: {JAVAC_EXE}"
        code, log = run_command(
            [str(JAVAC_EXE), "-d", str(BIN_DIR), str(source)],
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        return code == 0, log

    if language == "js":
        if not NODE_EXE.exists():
            return False, f"node not found: {NODE_EXE}"
        return True, "interpreted"

    if language == "py":
        if not PYTHON_EXE.exists():
            return False, f"python not found: {PYTHON_EXE}"
        return True, "interpreted"

    return False, f"unknown language {language}"


def command_for(language: str) -> list[str]:
    if language in ("nirqoba", "c", "cpp", "rust"):
        return [str(BIN_DIR / f"{language}_impl.exe")]
    if language == "java":
        return [str(JAVA_EXE), "-XX:+UseG1GC", "-cp", str(BIN_DIR), "java_impl"]
    if language == "js":
        return [str(NODE_EXE), "--expose-gc", str(SOURCE_FILES["js"])]
    if language == "py":
        return [str(PYTHON_EXE), str(SOURCE_FILES["py"])]
    raise ValueError(language)


def parse_checksums(text: str) -> list[int]:
    values: list[int] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            values.append(int(line))
        except ValueError:
            continue
    return values

# ---------------------------------------------------------------------------
# Suite
# ---------------------------------------------------------------------------


def git_revision() -> str | None:
    code, output = run_command(["git", "rev-parse", "HEAD"], timeout=30)
    return output.strip() if code == 0 else None


def stats(values: list[float]) -> dict[str, float]:
    if not values:
        return {"median": 0.0, "min": 0.0, "max": 0.0, "stdev": 0.0, "samples": 0}
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "samples": len(values),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="untimed warm-up runs per language, excluded from the statistics",
    )
    parser.add_argument("--impl", action="append", choices=LANGUAGES, dest="impls")
    args = parser.parse_args()

    languages = args.impls or LANGUAGES
    RAW_DIR.mkdir(parents=True, exist_ok=True)

    print("Stress Test 5 - the CDMM gauntlet")
    print(f"  languages: {', '.join(languages)}")
    print(f"  repeats:   {args.repeats}")
    print(f"  expected checksums: {EXPECTED_CHECKSUMS}")

    if not args.skip_compile:
        print("\nCompiling:")
        for language in languages:
            ok, log = compile_language(language)
            status = "OK" if ok else "FAILED"
            print(f"  {language:8s} {status}")
            if not ok:
                print(log)
                return 1

    results: dict[str, Any] = {}
    failures: list[str] = []

    print("\nRunning:")
    for language in languages:
        command = command_for(language)
        for _ in range(args.warmup):
            warm = measure(command)
            if warm["returncode"] != 0:
                failures.append(f"{language} warm-up exited {warm['returncode']}")
                break
        runs: list[dict[str, Any]] = []
        checksums: list[int] = []
        for run_index in range(1, args.repeats + 1):
            measured = measure(command)
            raw_path = RAW_DIR / f"{language}_run{run_index}.txt"
            raw_path.write_text(measured["stdout"], encoding="utf-8")
            if measured["returncode"] != 0:
                failures.append(f"{language} run {run_index} exited {measured['returncode']}")
                break
            parsed = parse_checksums(measured["stdout"])
            if parsed != EXPECTED_CHECKSUMS:
                failures.append(
                    f"{language} run {run_index} checksums {parsed} != {EXPECTED_CHECKSUMS}"
                )
                break
            checksums = parsed
            runs.append(measured)
            print(
                f"  {language:8s} run {run_index}: "
                f"{measured['wall_ms']:8.1f} ms  {measured['peak_rss_mib']:7.1f} MiB"
            )
        if not runs:
            continue
        results[language] = {
            "wall_ms": stats([r["wall_ms"] for r in runs]),
            "peak_rss_mib": stats([r["peak_rss_mib"] for r in runs]),
            "checksums": checksums,
            "runs": [
                {
                    "wall_ms": round(r["wall_ms"], 3),
                    "peak_rss_mib": round(r["peak_rss_mib"], 3),
                }
                for r in runs
            ],
        }

    payload = {
        "suite": "stress_test_5",
        "generated_at": _datetime.datetime.now().isoformat(timespec="seconds"),
        "git_revision": git_revision(),
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python": platform.python_version(),
        },
        "workload": {
            "stack_iterations": STACK_ITERATIONS,
            "pool_iterations": POOL_ITERATIONS,
            "pool_slots": POOL_SLOTS,
            "tree_nodes": TREE_NODES,
            "gc_nodes": GC_NODES,
            "expected_checksums": EXPECTED_CHECKSUMS,
        },
        "repeats": args.repeats,
        "results": results,
        "failures": failures,
    }
    RESULT_JSON.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    print("\n| Language | Wall clock (ms) | Peak RSS (MiB) | vs C |")
    print("|---|---|---|---|")
    baseline = results.get("c", {}).get("wall_ms", {}).get("median")
    for language in languages:
        if language not in results:
            print(f"| {language} | n/a | n/a | n/a |")
            continue
        wall = results[language]["wall_ms"]["median"]
        rss = results[language]["peak_rss_mib"]["median"]
        ratio = f"{wall / baseline:.2f}x" if baseline else "n/a"
        print(f"| {language} | {wall:.1f} | {rss:.1f} | {ratio} |")

    print(f"\nResults saved to: {RESULT_JSON}")
    if failures:
        print("\nFailures:")
        for failure in failures:
            print(f"  {failure}")
        return 1
    print("\nAll validation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
