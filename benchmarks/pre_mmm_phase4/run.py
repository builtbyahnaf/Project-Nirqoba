#!/usr/bin/env python3
"""Compares Pre-MMM Phase 4 owned-allocation workloads across two builds.

Each program times only its workload with `time.cycles()` and prints a checksum
followed by the cycle count. The runner builds with both compilers, verifies that
every run produces the same checksum, and alternates baseline/current samples so
thermal drift does not consistently favour one side.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASE_DIR = Path(__file__).resolve().parent
PROGRAMS = [
    ("allocation_heavy", "workload_alloc.nrqba", 1000000),
    ("destruction_heavy", "workload_destroy.nrqba", 300000),
    ("mixed", "workload_mixed.nrqba", 200000),
]


def run(command, cwd=ROOT):
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def compiler_version(binary):
    result = run([str(binary), "--version"])
    if result.returncode != 0:
        raise RuntimeError(result.stderr or result.stdout)
    return (result.stdout or result.stderr).strip().splitlines()[0]


def build(binary, source, out_dir):
    result = run([str(binary), "build", str(CASE_DIR / source),
                  "--out-dir", str(out_dir)])
    if result.returncode != 0:
        raise RuntimeError(
            f"build failed for {source}\nstdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )
    executable = out_dir / f"{Path(source).stem}.exe"
    if not executable.exists():
        raise RuntimeError(f"{source} did not produce {executable}")
    return executable


def execute(executable):
    result = run([str(executable)], cwd=executable.parent)
    if result.returncode != 0:
        raise RuntimeError(
            f"{executable} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    values = [int(value) for value in re.findall(r"-?\d+", result.stdout)]
    if len(values) != 2:
        raise RuntimeError(f"expected checksum and cycles, got {result.stdout!r}")
    return values[0], values[1]


def execute_with_memory(executable):
    """Runs once and reads the Windows lifetime peak working set."""
    try:
        import psutil
    except ImportError:
        return None
    process = subprocess.Popen(
        [str(executable)], cwd=executable.parent,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    observed = psutil.Process(process.pid)
    peak = 0
    try:
        while process.poll() is None:
            try:
                info = observed.memory_info()
                peak = max(peak, int(getattr(info, "peak_wset", info.rss)))
            except Exception:
                pass
            time.sleep(0.001)
        process.communicate(timeout=120)
    finally:
        if process.poll() is None:
            process.kill()
    if process.returncode != 0:
        raise RuntimeError(f"memory probe failed for {executable}")
    return {"peak_wset_bytes": peak}


def summarize(samples):
    return {
        "median": statistics.median(samples),
        "mean": statistics.mean(samples),
        "min": min(samples),
        "max": max(samples),
        "samples": samples,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    baseline_compiler = Path(args.baseline).resolve()
    current_compiler = Path(args.current).resolve()
    records = []
    with tempfile.TemporaryDirectory(prefix="nirqoba_phase4_") as temp:
        temp_dir = Path(temp)
        for name, source, iterations in PROGRAMS:
            base_dir = temp_dir / f"baseline_{name}"
            current_dir = temp_dir / f"current_{name}"
            base_dir.mkdir()
            current_dir.mkdir()
            base_exe = build(baseline_compiler, source, base_dir)
            current_exe = build(current_compiler, source, current_dir)

            base_memory = execute_with_memory(base_exe)
            current_memory = execute_with_memory(current_exe)
            base_checksums = []
            current_checksums = []
            base_cycles = []
            current_cycles = []
            for _ in range(max(3, args.runs)):
                base_checksum, base_cycle = execute(base_exe)
                current_checksum, current_cycle = execute(current_exe)
                base_checksums.append(base_checksum)
                current_checksums.append(current_checksum)
                base_cycles.append(base_cycle)
                current_cycles.append(current_cycle)
            if len(set(base_checksums + current_checksums)) != 1:
                raise RuntimeError(
                    f"checksum drift for {name}: baseline={set(base_checksums)} "
                    f"current={set(current_checksums)}"
                )

            base_summary = summarize(base_cycles)
            current_summary = summarize(current_cycles)
            records.append({
                "workload": name,
                "source": source,
                "iterations": iterations,
                "checksum": base_checksums[0],
                "baseline": base_summary,
                "current": current_summary,
                "baseline_cycles_per_iteration": base_summary["median"] / iterations,
                "current_cycles_per_iteration": current_summary["median"] / iterations,
                "baseline_memory": base_memory,
                "current_memory": current_memory,
            })
            print(
                f"{name}: baseline_median={base_summary['median']:.0f} cyc "
                f"current_median={current_summary['median']:.0f} cyc "
                f"baseline_per_iteration={base_summary['median'] / iterations:.2f} "
                f"current_per_iteration={current_summary['median'] / iterations:.2f} "
                f"checksum={base_checksums[0]}"
            )

    payload = {
        "environment": {
            "baseline_compiler": compiler_version(baseline_compiler),
            "current_compiler": compiler_version(current_compiler),
            "runs": max(3, args.runs),
            "timer": "time.cycles() around the workload only",
            "os": os.environ.get("OS", "Windows"),
        },
        "results": records,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(payload, indent=2) + "\n",
                                   encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
