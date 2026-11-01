#!/usr/bin/env python3
"""Readiness audit runner.

Compiles, links and executes every program under tests/readiness/programs and
tests/readiness/benchmarks through the real `nirqoba run` pipeline, compares the
programs against their recorded expected output, and measures wall-clock time
for the benchmarks.

This is deliberately *not* part of the integration suite: its purpose is to
report what a person can actually build with the language today, so a failure
here is a finding to document, not necessarily a regression.

Usage:
    python tests/readiness/run_readiness.py [--compiler PATH] [--runs N] [--json PATH]
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
READINESS = ROOT / "tests" / "readiness"
DEFAULT_COMPILER = ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"
TIMEOUT_SECONDS = 120


def default_compiler() -> Path:
    if os.name != "nt":
        candidate = ROOT / "build" / "posix-dev" / "bin" / "nirqoba"
        if candidate.exists():
            return candidate
    return DEFAULT_COMPILER


def run_program(compiler: Path, source: Path) -> dict:
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            [str(compiler), "run", str(source)],
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        return {
            "exit": None,
            "stdout": "",
            "stderr": f"timed out after {TIMEOUT_SECONDS}s",
            "seconds": float(TIMEOUT_SECONDS),
        }
    elapsed = time.perf_counter() - started
    return {
        "exit": completed.returncode,
        "stdout": completed.stdout.replace("\r\n", "\n"),
        "stderr": completed.stderr.replace("\r\n", "\n"),
        "seconds": elapsed,
    }


def first_error_line(stderr: str) -> str:
    for line in stderr.splitlines():
        stripped = line.strip()
        if stripped:
            return stripped
    return ""


def check_program(compiler: Path, source: Path) -> dict:
    result = run_program(compiler, source)
    expect_path = source.with_suffix(".expect.txt")
    expected = expect_path.read_text(encoding="utf-8").replace("\r\n", "\n") if expect_path.exists() else None
    status = "pass"
    detail = ""
    if result["exit"] is None:
        status, detail = "timeout", first_error_line(result["stderr"])
    elif result["exit"] != 0:
        status, detail = "failed", first_error_line(result["stderr"]) or f"exit {result['exit']}"
    elif expected is not None and result["stdout"] != expected:
        status = "wrong-output"
        detail = "stdout differs from expect.txt"
    return {
        "name": source.stem,
        "path": str(source.relative_to(ROOT)).replace("\\", "/"),
        "status": status,
        "detail": detail,
        "seconds": round(result["seconds"], 3),
        "stdout": result["stdout"] if status != "pass" else "",
        "stderr": result["stderr"] if status != "pass" else "",
    }


def gap_probe(compiler: Path, source: Path) -> dict:
    """Build probe: does the whole pipeline accept this construct?"""
    completed = subprocess.run(
        [str(compiler), "build", str(source)],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )
    stderr = completed.stderr.replace("\r\n", "\n")
    available = completed.returncode == 0 and not [
        line for line in stderr.splitlines() if "error" in line
    ]
    return {
        "name": source.stem,
        "path": str(source.relative_to(ROOT)).replace("\\", "/"),
        "status": "available" if available else "unavailable",
        "detail": "" if available else first_error_line(stderr),
    }


def build_to(compiler: Path, source: Path, directory: Path) -> tuple[Path | None, str]:
    """Compiles `source` into `directory`; returns the executable or a diagnostic."""
    completed = subprocess.run(
        [str(compiler), "build", str(source)],
        cwd=str(directory),
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )
    if completed.returncode != 0:
        return None, first_error_line(completed.stderr.replace("\r\n", "\n")) or f"exit {completed.returncode}"
    for candidate in (directory / f"{source.stem}.exe", directory / source.stem):
        if candidate.exists():
            return candidate, ""
    return None, "build produced no executable"


def time_executable(executable: Path, runs: int) -> tuple[list[float], str, int]:
    samples: list[float] = []
    stdout = ""
    code = 0
    for _ in range(runs):
        started = time.perf_counter()
        completed = subprocess.run(
            [str(executable)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
        )
        samples.append(time.perf_counter() - started)
        code = completed.returncode
        stdout = completed.stdout
        if code != 0:
            break
    return samples, stdout, code


def benchmark(compiler: Path, source: Path, runs: int, workdir: Path) -> dict:
    executable, detail = build_to(compiler, source, workdir)
    if executable is None:
        return {
            "name": source.stem,
            "path": str(source.relative_to(ROOT)).replace("\\", "/"),
            "status": "failed",
            "detail": detail,
            "runs": 0,
            "median_seconds": None,
            "min_seconds": None,
            "max_seconds": None,
            "stdout": "",
        }
    samples, stdout, code = time_executable(executable, runs)
    status = "pass" if code == 0 and samples else "failed"
    return {
        "name": source.stem,
        "path": str(source.relative_to(ROOT)).replace("\\", "/"),
        "status": status,
        "detail": "" if status == "pass" else f"exit {code}",
        "runs": len(samples),
        "median_seconds": round(statistics.median(samples), 4) if samples else None,
        "min_seconds": round(min(samples), 4) if samples else None,
        "max_seconds": round(max(samples), 4) if samples else None,
        "stdout": stdout.strip(),
    }


def startup_baseline(compiler: Path, runs: int, workdir: Path) -> dict:
    """Times an empty program so process startup can be separated from work."""
    source = workdir / "__baseline.nrqba"
    source.write_text("int function main() {\n    return 0;\n}\n", encoding="utf-8")
    executable, detail = build_to(compiler, source, workdir)
    if executable is None:
        return {"median_seconds": None, "detail": detail}
    samples, _, _ = time_executable(executable, runs)
    return {
        "median_seconds": round(statistics.median(samples), 4) if samples else None,
        "detail": "",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Nirqoba readiness audit")
    parser.add_argument("--compiler", default=str(default_compiler()))
    parser.add_argument("--runs", type=int, default=5, help="benchmark repetitions")
    parser.add_argument("--json", help="write the full report to this path")
    args = parser.parse_args()

    compiler = Path(args.compiler)
    if not compiler.exists():
        print(f"compiler not found: {compiler}", file=sys.stderr)
        return 2

    programs = sorted((READINESS / "programs").glob("*.nrqba"))
    benchmarks = sorted((READINESS / "benchmarks").glob("*.nrqba"))
    gaps = sorted((READINESS / "gaps").glob("*.nrqba"))
    if not programs and not benchmarks:
        print("no readiness programs found", file=sys.stderr)
        return 2

    program_results = [check_program(compiler, source) for source in programs]
    gap_results = [gap_probe(compiler, source) for source in gaps]
    workdir = Path(tempfile.mkdtemp(prefix="nrq-readiness-"))
    baseline = startup_baseline(compiler, args.runs, workdir)
    benchmark_results = [benchmark(compiler, source, args.runs, workdir) for source in benchmarks]

    passed = sum(1 for r in program_results if r["status"] == "pass")
    print(f"Readiness programs: {passed}/{len(program_results)} pass")
    for result in program_results:
        suffix = f" — {result['detail']}" if result["detail"] else ""
        print(f"  [{result['status']:>11}] {result['name']:<24} {result['seconds']:>6.2f}s{suffix}")

    print("\nGap probes (language features the corpus had to work around):")
    for result in gap_results:
        suffix = f" — {result['detail']}" if result["detail"] else ""
        print(f"  [{result['status']:>11}] {result['name']:<24}{suffix}")

    print("\nBenchmarks:")
    if baseline["median_seconds"] is not None:
        print(f"  process startup baseline: {baseline['median_seconds']:.4f}s (n={args.runs})")
    for result in benchmark_results:
        timing = (
            f"median {result['median_seconds']:.3f}s "
            f"(min {result['min_seconds']:.3f}s, max {result['max_seconds']:.3f}s, "
            f"n={result['runs']})"
            if result["median_seconds"] is not None
            else f"not measured — {result['detail']}"
        )
        print(f"  [{result['status']:>11}] {result['name']:<24} {timing}")

    report = {
        "compiler": str(compiler),
        "startup_baseline_seconds": baseline["median_seconds"],
        "programs": program_results,
        "gaps": gap_results,
        "benchmarks": benchmark_results,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"\nreport written to {args.json}")

    failed = [r for r in program_results + benchmark_results if r["status"] != "pass"]
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
