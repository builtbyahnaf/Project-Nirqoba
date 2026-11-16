#!/usr/bin/env python3
"""Build and run the Phase 9 memory-strategy harness.

Each mode runs in a fresh process. The runner preserves every sample and
reports median, mean, minimum, maximum and population standard deviation. It
never compares semantically different strategies as if one were universally
faster; ratios are diagnostic.
"""
import argparse
import json
import pathlib
import platform
import statistics
import subprocess
import sys

try:
    import psutil
except ImportError:
    psutil = None

ROOT = pathlib.Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent.parent
OUTPUT = ROOT / "build"
MODES = ["owned", "arena", "pool", "shared", "gc", "mixed"]


def build():
    source = ROOT / "main.cpp"
    executable = OUTPUT / "phase9_benchmark.exe"
    candidates = sorted(pathlib.Path(
        r"C:\Program Files\Microsoft Visual Studio").glob(
            "*/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    if not candidates:
        raise RuntimeError("Visual Studio x64 developer environment not found")
    runtime_include = PROJECT / "runtime" / "include"
    runtime_lib = PROJECT / "build" / "msvc-dev" / "lib" / "nirqoba_runtime.lib"
    batch = OUTPUT / "build_benchmark.bat"
    batch.write_text(
        "@echo off\r\n"
        f'call "{candidates[-1]}" >nul\r\n'
        "if errorlevel 1 exit /b 1\r\n"
        f'cl /nologo /std:c++20 /O2 /MD /EHsc /I"{runtime_include}" '
        f'"{source}" /Fe:"{executable}" /link "{runtime_lib}" psapi.lib\r\n'
        "exit /b %errorlevel%\r\n")
    subprocess.run(["cmd", "/c", str(batch)], cwd=OUTPUT, check=True,
                   capture_output=True, text=True)
    return executable


def run_mode(executable, mode):
    result = subprocess.run([str(executable), mode], cwd=PROJECT,
                            check=True, capture_output=True, text=True)
    records = []
    for line in result.stdout.splitlines():
        line = line.strip()
        if line:
            records.append(json.loads(line))
    if not records:
        raise RuntimeError(f"{mode}: no benchmark records")
    return records


def summarize(values):
    return {
        "median": round(statistics.median(values), 3),
        "mean": round(statistics.fmean(values), 3),
        "min": round(min(values), 3),
        "max": round(max(values), 3),
        "stddev": round(statistics.pstdev(values), 3) if len(values) > 1 else 0.0,
        "samples": values,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--out", default=str(ROOT / "results.json"))
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    executable = build()

    version = subprocess.run(
        [str(PROJECT / "build/msvc-dev/bin/nirqoba.exe"), "--version"],
        capture_output=True, text=True, check=False).stdout.strip().splitlines()
    report = {
        "configuration": {
            "compiler_version": version,
            "build": "RelWithDebInfo runtime, MSVC /O2 harness",
            "timer": "steady_clock around the measured loop",
            "samples": args.runs,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpus": psutil.cpu_count(logical=True) if psutil else None,
            "physical_cpus": psutil.cpu_count(logical=False) if psutil else None,
            "total_memory_bytes": psutil.virtual_memory().total if psutil else None,
            "note": "fresh process per mode; checksum equality is checked per record",
        },
        "modes": {},
        "limitations": [
            "The harness measures ABI entry points, not language syntax; compiler cleanup lowering is covered by TIR/LLVM fixtures.",
            "The manual mode uses the same checked allocation entry point because manual storage has no separate heap; it measures runtime path overhead, not a distinct allocator.",
            "Arena reset is measured separately from arena allocation and retains only the initial chunk.",
            "Shared contention depends on the host scheduler and is not a real-time guarantee.",
            "Peak RSS is process-wide and includes runtime metadata and the benchmark executable.",
            "Collection latency is one full collection after a fixed garbage load, not a pause distribution.",
            "No FFI mode exists because the language has no FFI surface.",
        ],
    }

    for mode in MODES:
        samples = [run_mode(executable, mode) for _ in range(args.runs)]
        groups = {}
        for sample in samples:
            for record in sample:
                groups.setdefault(record["mode"], []).append(record)
        mode_report = {}
        for name, group in groups.items():
            checksums = {record["checksum"] for record in group}
            if len(checksums) != 1:
                raise RuntimeError(f"{name}: nondeterministic checksum {checksums}")
            entry = {
                "checksum": group[0]["checksum"],
                "ns_per_op": summarize([record["ns_per_op"] for record in group]),
                "peak_rss_bytes": max(record.get("peak_rss_bytes", 0) for record in group),
                "current_rss_bytes": max(record.get("current_rss_bytes", 0) for record in group),
            }
            for key in ("operations", "live_before", "live_after"):
                if key in group[0]:
                    entry[key] = group[0][key]
            mode_report[name] = entry
        report["modes"][mode] = mode_report

    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
