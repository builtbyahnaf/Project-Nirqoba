#!/usr/bin/env python3
"""Build and run the Phase 5 arena/pool comparison workloads.

Each workload prints a checksum followed by elapsed TSC cycles. The checksum is
verified against the ordinary baseline so a failed optimization cannot hide a
wrong workload. Reports median cycles over repeated runs; no claim is made that
arenas are universally faster.
"""
import argparse
import json
import pathlib
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
PROJECT = ROOT.parent.parent


def compile_workload(compiler, source, output):
    subprocess.run(
        [compiler, "build", str(source), "-O2", "-o", str(output)],
        cwd=output.parent, check=True, capture_output=True, text=True)


def run_workload(executable):
    result = subprocess.run([str(executable)], cwd=PROJECT, check=True,
                            capture_output=True, text=True)
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(lines) < 2:
        raise RuntimeError(f"{executable}: expected checksum and cycles")
    return int(lines[0]), int(lines[1])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=str(PROJECT / "build/msvc-dev/bin/nirqoba.exe"))
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--out", default=str(ROOT / "results.json"))
    args = parser.parse_args()

    output_dir = ROOT / "build"
    output_dir.mkdir(exist_ok=True)
    groups = {
        "small": ["small_ordinary", "small_pool", "small_arena"],
        "mixed": ["mixed_ordinary", "mixed_arena"],
    }
    report = {
        "compiler": args.compiler,
        "optimization": "-O2",
        "timer": "time.cycles() around the workload only",
        "runs": args.runs,
        "workloads": {},
    }
    for group, names in groups.items():
        checksums = {}
        for name in names:
            source = ROOT / f"{name}.nrqba"
            executable = output_dir / f"{name}.exe"
            compile_workload(args.compiler, source, executable)
            samples = []
            for _ in range(args.runs):
                checksum, cycles = run_workload(executable)
                samples.append(cycles)
                checksums.setdefault(name, checksum)
                if checksums[name] != checksum:
                    raise RuntimeError(f"{name}: nondeterministic checksum")
            report["workloads"][name] = {
                "checksum": checksums[name],
                "median_cycles": int(statistics.median(samples)),
                "min_cycles": min(samples),
                "max_cycles": max(samples),
                "samples": samples,
            }
        ordinary = report["workloads"][f"{group}_ordinary"]["median_cycles"]
        for name in names:
            entry = report["workloads"][name]
            entry["ratio_vs_ordinary"] = round(entry["median_cycles"] / ordinary, 4)
    pathlib.Path(args.out).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
