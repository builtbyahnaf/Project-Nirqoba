#!/usr/bin/env python3
"""Build and run the Phase 8 manual-memory workloads.

Each workload runs in a fresh process. Production (`-O2`) and debug-runtime
(`-O2 --debug-enabled`) builds are measured separately so the report can show
the cost of the existing debug ownership registry instead of asserting that
manual memory is universally faster. No FFI benchmark exists because the
language has no FFI surface.
"""
import argparse
import json
import pathlib
import platform
import statistics
import subprocess
import sys
import time

try:
    import psutil
except ImportError:  # pragma: no cover - the host normally has psutil
    psutil = None

ROOT = pathlib.Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent.parent
OUTPUT = ROOT / "build"
WORKLOADS = {
    "ordinary_alloc": 1_000_000,
    "manual_alloc": 1_000_000,
    "manual_alloc_from": 1_000_000,
    "manual_fallback": 1_000_000,
    "manual_borrow": 10_000_000,
    "manual_managed_root": 100_000,
}


def compile_workload(compiler, name, executable, debug):
    command = [compiler, "build", str(ROOT / f"{name}.nrqba"), "-O2",
               "-o", str(executable)]
    if debug:
        command.append("--debug-enabled")
    subprocess.run(command, cwd=OUTPUT, check=True, capture_output=True,
                   text=True)


def run_workload(executable, runs):
    checksum = None
    cycles = []
    peak_rss = []
    elapsed = []
    for _ in range(runs):
        process = subprocess.Popen([str(executable)], cwd=PROJECT,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        started = time.perf_counter()
        peak = 0
        while process.poll() is None:
            if psutil is not None:
                try:
                    peak = max(peak, psutil.Process(process.pid).memory_info().peak_wset)
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass
            time.sleep(0.005)
        stdout, stderr = process.communicate()
        if process.returncode != 0:
            raise RuntimeError(f"{executable}: exit {process.returncode}: {stderr}")
        elapsed.append((time.perf_counter() - started) * 1e9)
        lines = [line.strip() for line in stdout.splitlines() if line.strip()]
        if len(lines) < 2:
            raise RuntimeError(f"{executable}: expected checksum and cycles")
        current = int(lines[0])
        if checksum is None:
            checksum = current
        elif checksum != current:
            raise RuntimeError(f"{executable}: nondeterministic checksum")
        cycles.append(int(lines[1]))
        if peak:
            peak_rss.append(peak)
    return checksum, cycles, peak_rss, elapsed


def summarize(values, divisor=1.0):
    normalized = [value / divisor for value in values]
    return {
        "median": round(statistics.median(normalized), 3),
        "mean": round(statistics.fmean(normalized), 3),
        "min": round(min(normalized), 3),
        "max": round(max(normalized), 3),
        "stddev": round(statistics.pstdev(normalized), 3) if len(normalized) > 1 else 0.0,
        "samples": [round(value, 3) for value in normalized],
    }


def compiler_version(compiler):
    result = subprocess.run([compiler, "--version"], capture_output=True,
                            text=True, check=False)
    return result.stdout.strip().splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=str(
        PROJECT / "build/msvc-dev/bin/nirqoba.exe"))
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--out", default=str(ROOT / "results.json"))
    args = parser.parse_args()
    OUTPUT.mkdir(exist_ok=True)

    report = {
        "configuration": {
            "compiler_version": compiler_version(args.compiler),
            "build": "RelWithDebInfo compiler, -O2 workloads",
            "optimization": "-O2",
            "debug_runtime": "-O2 --debug-enabled",
            "timer": "time.cycles() around the measured language loop",
            "samples": args.runs,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpus": psutil.cpu_count(logical=True) if psutil else None,
            "physical_cpus": psutil.cpu_count(logical=False) if psutil else None,
            "total_memory_bytes": psutil.virtual_memory().total if psutil else None,
            "note": "fresh process per sample; checksum equality is checked per workload",
        },
        "workloads": {},
        "limitations": [
            "Manual and ordinary explicit allocation use the same runtime allocator and ownership registry; the measured difference is language/lowering overhead, not a different heap.",
            "The timer is the process steady counter exposed by time.cycles(); it is not converted to nanoseconds because its frequency is platform-defined.",
            "Peak RSS is sampled while the process is alive and can miss a shorter peak.",
            "Manual reallocation is rejected by the supported contract; no reallocation benchmark is reported.",
            "Aligned manual allocation is not a separate API; the payload type's natural alignment is used.",
            "Native file/socket/resource timing is excluded because it measures the host filesystem and network, not ownership dispatch.",
            "There is no FFI surface, so no FFI ownership-transfer benchmark is possible.",
        ],
    }

    for name, iterations in WORKLOADS.items():
        entry = {"iterations": iterations, "production": {}, "debug": {}}
        checksums = {}
        for label, debug in (("production", False), ("debug", True)):
            executable = OUTPUT / f"{name}_{label}.exe"
            compile_workload(args.compiler, name, executable, debug)
            checksum, cycles, peak, elapsed = run_workload(executable, args.runs)
            checksums[label] = checksum
            entry[label] = {
                "checksum": checksum,
                "cycles_per_operation": summarize(cycles, iterations),
                "total_cycles": summarize(cycles),
                "process_elapsed_ns": summarize(elapsed),
                "peak_rss_bytes": max(peak) if peak else None,
            }
        if len(set(checksums.values())) != 1:
            raise RuntimeError(f"{name}: production/debug checksum mismatch: {checksums}")
        entry["checksum"] = checksums["production"]
        report["workloads"][name] = entry

    ordinary = report["workloads"]["ordinary_alloc"]["production"][
        "cycles_per_operation"]["median"]
    for name in ("manual_alloc", "manual_alloc_from", "manual_fallback"):
        value = report["workloads"][name]["production"][
            "cycles_per_operation"]["median"]
        report["workloads"][name]["ratio_vs_ordinary"] = round(value / ordinary, 4)

    for name in ("manual_alloc", "manual_alloc_from", "manual_fallback",
                 "manual_borrow", "manual_managed_root"):
        production = report["workloads"][name]["production"][
            "cycles_per_operation"]["median"]
        debug = report["workloads"][name]["debug"][
            "cycles_per_operation"]["median"]
        report["workloads"][name]["debug_overhead_ratio"] = round(
            debug / production, 4) if production else None

    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
