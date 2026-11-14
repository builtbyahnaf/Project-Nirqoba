#!/usr/bin/env python3
"""Pre-MMM Phase 7 artifact-resource benchmark.

Compares the 0.473-beta compiler with the 0.477-beta compiler on the compiler
temporary-resource paths that changed: ordinary build, ephemeral run artifacts,
and project scaffolding. Timing is wall time around the compiler process; the
peak RSS is sampled while that process is alive because a Windows process object
does not retain its peak after exit.

Usage:
  python benchmarks/pre_mmm_phase7/run.py \
      --baseline .phase7-baseline/build/msvc-dev/bin/nirqoba.exe \
      --current build/msvc-dev/bin/nirqoba.exe
"""

import argparse
import json
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

import psutil


def run_timed(command):
    start = time.perf_counter()
    process = psutil.Popen(command, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
    peak = 0
    while process.poll() is None:
        try:
            peak = max(peak, process.memory_info().rss)
        except psutil.Error:
            pass
        time.sleep(0.001)
    out, err = process.communicate()
    elapsed = time.perf_counter() - start
    return elapsed, peak, process.returncode, out, err


def files_in(directory):
    return [p for p in Path(directory).rglob("*") if p.is_file()]


def measure_build(compiler, source, root, repeats):
    samples = []
    rss = []
    artifacts = []
    for index in range(repeats):
        out_dir = root / f"build-{index}"
        out_dir.mkdir(parents=True, exist_ok=True)
        elapsed, peak, code, _, err = run_timed(
            [str(compiler), "build", str(source), "--out-dir", str(out_dir)])
        if code != 0:
            raise RuntimeError(f"build failed: {err}")
        samples.append(elapsed * 1000)
        rss.append(peak)
        artifacts.append(sum(p.stat().st_size for p in files_in(out_dir)))
    return samples, rss, artifacts


def measure_run(compiler, source, root, repeats):
    samples = []
    artifact_counts = []
    artifact_bytes = []
    for index in range(repeats):
        out_dir = root / f"run-{index}"
        out_dir.mkdir(parents=True, exist_ok=True)
        elapsed, _, code, out, err = run_timed(
            [str(compiler), "run", str(source), "--out-dir", str(out_dir)])
        if code != 0:
            raise RuntimeError(f"run failed: {err}")
        samples.append(elapsed * 1000)
        artifacts = files_in(out_dir)
        artifact_counts.append(len(artifacts))
        artifact_bytes.append(sum(p.stat().st_size for p in artifacts))
    return samples, artifact_counts, artifact_bytes


def measure_new(compiler, root, repeats):
    samples = []
    files = []
    for index in range(repeats):
        target = root / f"project-{index}"
        elapsed, _, code, _, err = run_timed(
            [str(compiler), "new", str(target)])
        if code != 0:
            raise RuntimeError(f"new failed: {err}")
        samples.append(elapsed * 1000)
        files.append(len(files_in(target)))
    return samples, files


def median(values):
    return round(statistics.median(values), 3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--output", default=str(Path(__file__).parent /
                                                "results.json"))
    args = parser.parse_args()

    baseline = Path(args.baseline).resolve()
    current = Path(args.current).resolve()
    source = (Path(__file__).parent / "hello.nrqba").resolve()
    results = {
        "baseline_compiler": str(baseline),
        "current_compiler": str(current),
        "repeats": args.repeats,
        "source": str(source),
        "environment": {
            "platform": os.name,
            "python": os.sys.version.split()[0],
        },
    }

    with tempfile.TemporaryDirectory(prefix="nrq-phase7-bench-") as temp:
        root = Path(temp)
        data = {
            name: {"build_ms": [], "build_rss": [], "build_bytes": [],
                   "run_ms": [], "run_counts": [], "run_bytes": [],
                   "new_ms": [], "new_files": []}
            for name in ("baseline", "current")
        }
        compilers = {"baseline": baseline, "current": current}
        # Interleave baseline and current so a slow period (defender scan, CPU
        # frequency change) cannot land entirely in one column.
        for repeat in range(args.repeats):
            order = (("baseline", "current") if repeat % 2 == 0
                     else ("current", "baseline"))
            for name in order:
                compiler = compilers[name]
                build = measure_build(compiler, source,
                                      root / f"{name}-build-{repeat}", 1)
                run = measure_run(compiler, source,
                                  root / f"{name}-run-{repeat}", 1)
                new = measure_new(compiler,
                                  root / f"{name}-new-{repeat}", 1)
                data[name]["build_ms"].append(build[0][0])
                data[name]["build_rss"].append(build[1][0])
                data[name]["build_bytes"].append(build[2][0])
                data[name]["run_ms"].append(run[0][0])
                data[name]["run_counts"].append(run[1][0])
                data[name]["run_bytes"].append(run[2][0])
                data[name]["new_ms"].append(new[0][0])
                data[name]["new_files"].append(new[1][0])

        for name, samples in data.items():
            results[name] = {
                "build_ms": samples["build_ms"],
                "build_median_ms": median(samples["build_ms"]),
                "build_peak_rss_bytes": samples["build_rss"],
                "build_peak_rss_median_bytes": int(
                    statistics.median(samples["build_rss"])),
                "build_artifact_bytes": samples["build_bytes"],
                "run_ms": samples["run_ms"],
                "run_median_ms": median(samples["run_ms"]),
                "run_artifact_count": samples["run_counts"],
                "run_artifact_bytes": samples["run_bytes"],
                "new_ms": samples["new_ms"],
                "new_median_ms": median(samples["new_ms"]),
                "new_file_count": samples["new_files"],
            }

    b = results["baseline"]
    c = results["current"]
    results["comparison"] = {
        "build_median_change_percent": round(
            (c["build_median_ms"] - b["build_median_ms"]) /
            b["build_median_ms"] * 100, 2),
        "run_median_change_percent": round(
            (c["run_median_ms"] - b["run_median_ms"]) /
            b["run_median_ms"] * 100, 2),
        "new_median_change_percent": round(
            (c["new_median_ms"] - b["new_median_ms"]) /
            b["new_median_ms"] * 100, 2),
        "run_artifact_count_before": b["run_artifact_count"],
        "run_artifact_count_after": c["run_artifact_count"],
        "run_artifact_bytes_before": b["run_artifact_bytes"],
        "run_artifact_bytes_after": c["run_artifact_bytes"],
    }

    output = Path(args.output)
    output.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results["comparison"], indent=2))
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
