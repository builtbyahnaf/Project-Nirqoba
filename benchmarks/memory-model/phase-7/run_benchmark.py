#!/usr/bin/env python3
"""Build and run the Phase 7 GC benchmark harness.

Each mode runs in a fresh process. The report records every sample, median,
minimum, maximum and sample standard deviation; it does not claim a universal
speedup or compare different workloads. Build artifacts and logs stay under the
ignored build/ directory.
"""
import argparse
import json
import pathlib
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent.parent
OUTPUT = ROOT / "build"
MODES = ["managed", "roots", "safepoint", "barrier", "collect", "cycles", "mt", "isolation"]


def build():
    source = ROOT / "main.cpp"
    executable = OUTPUT / "phase7_benchmark.exe"
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
        f'"{source}" /Fe:"{executable}" /link "{runtime_lib}" dxgi.lib psapi.lib\r\n'
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
        if not line:
            continue
        records.append(json.loads(line))
    if not records:
        raise RuntimeError(f"{mode}: no benchmark records")
    return records


def summarize(values):
    if not values:
        return None
    median = statistics.median(values)
    mean = statistics.fmean(values)
    deviation = statistics.pstdev(values) if len(values) > 1 else 0.0
    return {"median": round(median, 3), "mean": round(mean, 3),
            "min": round(min(values), 3), "max": round(max(values), 3),
            "stddev": round(deviation, 3), "samples": values}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--out", default=str(ROOT / "results.json"))
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    executable = build()

    report = {
        "configuration": {
            "compiler_version": "0.575-beta",
            "build": "RelWithDebInfo",
            "optimization": "-O2",
            "timer": "steady_clock around the measured loop",
            "samples": args.runs,
            "note": "fresh process per mode; no cross-workload comparison",
        },
        "modes": {},
        "limitations": [
            "Collection latency is a single full collection after a fixed garbage load; "
            "it is not a worst-case pause distribution.",
            "Peak resident memory is process-wide and includes runtime metadata.",
            "The C++ harness measures ABI entry points; language-level optimization "
            "effects are covered by the integration fixtures.",
        ],
    }

    for mode in MODES:
        records = [run_mode(executable, mode) for _ in range(args.runs)]
        groups = {}
        for sample in records:
            for record in sample:
                groups.setdefault(record["mode"], []).append(record)
        mode_report = {}
        for name, group in groups.items():
            checksums = {record["checksum"] for record in group}
            if len(checksums) != 1:
                raise RuntimeError(f"{name}: nondeterministic checksum {checksums}")
            values = [record["ns_per_op"] for record in group]
            peak = max(record.get("peak_rss_bytes", 0) for record in group)
            entry = {
                "checksum": group[0]["checksum"],
                "ns_per_op": summarize(values),
                "peak_rss_bytes": peak,
            }
            for key in ("operations", "gc_allocated_bytes", "gc_collections",
                        "gc_live_objects", "gc_pages", "gc_layouts",
                        "live_before", "live_after", "threads", "collections",
                        "allocated", "gc_clean", "layouts_before", "layouts_after",
                        "gc_allocated_before", "gc_allocated_after",
                        "gc_collections_before", "gc_collections_after"):
                if key in group[0]:
                    entry[key] = group[0][key]
            if "gc_clean" in entry:
                entry["gc_clean"] = all(record.get("gc_clean") for record in group)
            mode_report[name] = entry
        report["modes"][mode] = mode_report

    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
