#!/usr/bin/env python3
"""Build and run the Phase 6 shared-ownership benchmarks.

Language workloads measure ordinary versus shared allocation, explicit retain
versus move, aggregate copy and arena reset reuse. The C++ harness measures
uncontended, independent-object and contended retain/release directly because
the language has no thread-spawn surface. Medians and full ranges are retained;
no universal speedup is claimed.
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


def compile_workload(compiler, source, executable):
    subprocess.run(
        [compiler, "build", str(source), "-O2", "-o", str(executable)],
        cwd=OUTPUT, check=True, capture_output=True, text=True)


def run_workload(executable, runs):
    samples = []
    checksum = None
    for _ in range(runs):
        result = subprocess.run([str(executable)], cwd=PROJECT, check=True,
                                capture_output=True, text=True)
        lines = [line.strip() for line in result.stdout.splitlines()
                 if line.strip()]
        if len(lines) < 2:
            raise RuntimeError(f"{executable}: expected checksum and cycles")
        current = int(lines[0])
        if checksum is None:
            checksum = current
        elif checksum != current:
            raise RuntimeError(f"{executable}: nondeterministic checksum")
        samples.append(int(lines[1]))
    return checksum, samples


def summarize(samples):
    return {
        "median_cycles": int(statistics.median(samples)),
        "min_cycles": min(samples),
        "max_cycles": max(samples),
        "samples": samples,
    }


def build_contention():
    source = ROOT / "contention.cpp"
    executable = OUTPUT / "contention.exe"
    candidates = sorted(pathlib.Path(
        r"C:\Program Files\Microsoft Visual Studio").glob(
            "*/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    if not candidates:
        raise RuntimeError("Visual Studio x64 developer environment not found")
    runtime_include = PROJECT / "runtime" / "include"
    runtime_lib = PROJECT / "build" / "msvc-dev" / "lib" / "nirqoba_runtime.lib"
    batch = OUTPUT / "build_contention.bat"
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=str(
        PROJECT / "build/msvc-dev/bin/nirqoba.exe"))
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--out", default=str(ROOT / "results.json"))
    args = parser.parse_args()
    OUTPUT.mkdir(exist_ok=True)

    workloads = [
        "ordinary_alloc", "shared_alloc", "shared_retain", "shared_move",
        "shared_aggregate", "arena_reset",
    ]
    report = {
        "compiler": args.compiler,
        "optimization": "-O2",
        "timer": "time.cycles() around the language workload only",
        "runs": args.runs,
        "workloads": {},
        "contention": {},
    }
    for name in workloads:
        executable = OUTPUT / f"{name}.exe"
        compile_workload(args.compiler, ROOT / f"{name}.nrqba", executable)
        checksum, samples = run_workload(executable, args.runs)
        entry = {"checksum": checksum, **summarize(samples)}
        report["workloads"][name] = entry

    ordinary = report["workloads"]["ordinary_alloc"]["median_cycles"]
    shared = report["workloads"]["shared_alloc"]["median_cycles"]
    report["workloads"]["shared_alloc"]["ratio_vs_ordinary"] = round(
        shared / ordinary, 4)
    move = report["workloads"]["shared_move"]["median_cycles"]
    retain = report["workloads"]["shared_retain"]["median_cycles"]
    report["workloads"]["shared_retain"]["ratio_vs_move"] = round(
        retain / move, 4)

    executable = build_contention()
    contention_samples = []
    for _ in range(5):
        result = subprocess.run([str(executable)], cwd=PROJECT, check=True,
                                capture_output=True, text=True)
        contention_samples.append(json.loads(result.stdout))
    for key in ("uncontended_ns", "independent_ns", "contended_ns",
                "final_release_ns"):
        values = [sample[key] for sample in contention_samples]
        report["contention"][key] = {
            "median_ns": int(statistics.median(values)),
            "min_ns": min(values),
            "max_ns": max(values),
            "samples": values,
        }
    report["contention"]["threads"] = contention_samples[0]["threads"]
    report["contention"]["ops_per_thread"] = contention_samples[0]["ops_per_thread"]
    report["contention"]["final_release_objects"] = contention_samples[0][
        "final_release_objects"]
    report["contention"]["memory_objects"] = contention_samples[0]["memory_objects"]
    report["contention"]["memory_bytes_per_object"] = contention_samples[0][
        "memory_bytes_per_object"]
    report["contention"]["correct"] = all(
        sample["ok"] for sample in contention_samples)

    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
