#!/usr/bin/env python3
"""Measures cleanup-heavy compiler and runtime behavior for Pre-MMM Phase 3."""

import argparse
import json
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CASE_DIR = Path(__file__).resolve().parent
RUNTIME_SOURCE = CASE_DIR / "cleanup_runtime.nrqba"
COMPILE_SOURCE = CASE_DIR / "cleanup_compile.nrqba"


def run(command, cwd=ROOT):
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def timed(command, runs):
    samples = []
    for _ in range(runs):
        started = time.perf_counter()
        proc = run(command)
        elapsed = (time.perf_counter() - started) * 1000.0
        if proc.returncode != 0:
            raise RuntimeError(
                f"{' '.join(map(str, command))} failed with {proc.returncode}\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        samples.append(elapsed)
    return {
        "median_ms": statistics.median(samples),
        "mean_ms": statistics.mean(samples),
        "min_ms": min(samples),
        "max_ms": max(samples),
        "samples_ms": samples,
    }


def measure(binary, label, runs):
    binary = Path(binary).resolve()
    with tempfile.TemporaryDirectory(prefix=f"nirqoba_{label}_") as temp:
        out_dir = Path(temp)
        built = run([
            str(binary), "build", str(RUNTIME_SOURCE),
            "--out-dir", str(out_dir), "--ipo=minimal",
        ])
        if built.returncode != 0:
            raise RuntimeError(
                f"{label} runtime build failed\nstdout:\n{built.stdout}\n"
                f"stderr:\n{built.stderr}"
            )
        executable = out_dir / f"{RUNTIME_SOURCE.stem}.exe"
        if not executable.exists():
            raise RuntimeError(f"{label} did not produce {executable}")

        run_samples = []
        outputs = []
        for _ in range(max(3, runs // 3)):
            started = time.perf_counter()
            proc = run([str(executable)], cwd=out_dir)
            elapsed = (time.perf_counter() - started) * 1000.0
            if proc.returncode != 0:
                raise RuntimeError(
                    f"{label} runtime failed with {proc.returncode}\n"
                    f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
                )
            outputs.append(proc.stdout.strip())
            run_samples.append(elapsed)
        if len(set(outputs)) != 1:
            raise RuntimeError(f"{label} produced inconsistent output: {outputs}")

        compile_result = timed([str(binary), "check", str(COMPILE_SOURCE)], runs)
        runtime_result = {
            "median_ms": statistics.median(run_samples),
            "mean_ms": statistics.mean(run_samples),
            "min_ms": min(run_samples),
            "max_ms": max(run_samples),
            "samples_ms": run_samples,
        }
        return {
            "label": label,
            "runtime_stdout": outputs[0],
            "compile_cleanup": compile_result,
            "runtime_cleanup": runtime_result,
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--runs", type=int, default=15)
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    results = [
        measure(args.baseline, "baseline_pre_phase3", args.runs),
        measure(args.current, "current_phase3", args.runs),
    ]
    for result in results:
        compile_result = result["compile_cleanup"]
        runtime_result = result["runtime_cleanup"]
        print(
            f"{result['label']}: "
            f"compile_cleanup_median_ms={compile_result['median_ms']:.3f} "
            f"compile_cleanup_mean_ms={compile_result['mean_ms']:.3f} "
            f"runtime_cleanup_median_ms={runtime_result['median_ms']:.3f} "
            f"runtime_cleanup_mean_ms={runtime_result['mean_ms']:.3f} "
            f"stdout={result['runtime_stdout']!r}"
        )
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2) + "\n",
                                   encoding="utf-8")


if __name__ == "__main__":
    main()
