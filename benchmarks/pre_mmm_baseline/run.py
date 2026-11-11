"""Phase 0 baseline runner for the pre-migration roadmap.

Builds every program in this directory with the working compiler, runs one warmup
and several measured repetitions, checks that every repetition produced the same
checksum, and writes BASELINE.md / BASELINE.json.

Each program times only its own workload with `time.cycles()` and prints
`checksum` then `cycles`, so compiler startup, runtime initialisation and process
launch stay outside the measured region. The runner also records whole-process
wall time, which does include start-up; the difference between the two is why the
roadmap asks for both.

Usage:
    python benchmarks/pre_mmm_baseline/run.py [--reps 7] [--warmups 1]
"""

import argparse
import datetime
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COMPILER = os.path.join(ROOT, "build", "msvc-dev", "bin", "nirqoba.exe")
BUILD_DIR = os.path.join(ROOT, "build", "msvc-dev")
OUT_DIR = os.path.join(ROOT, "build", "ao", "pre_mmm")
HERE = os.path.dirname(os.path.abspath(__file__))

# name, source, operation unit, operations per measured run
PROGRAMS = [
    ("startup", "00_startup.nrqba", "program", 1),
    ("small_alloc", "01_small_alloc.nrqba", "64 B alloc+free", 200000),
    ("large_alloc", "02_large_alloc.nrqba", "1 MiB alloc+free", 500),
    ("alloc_loop", "03_alloc_loop.nrqba", "owned array", 20000),
    ("array_traversal", "04_array_traversal.nrqba", "element visit", 7000000),
    ("aggregate_construct", "05_aggregate_construct.nrqba", "aggregate", 500000),
    ("nested_owned", "06_nested_owned.nrqba", "iteration", 10000),
]

# The roadmap asks for a multi-threaded allocation workload. The compiler has no
# thread-spawn builtin (`thread.spawn` and `nrq_thread_create` do not exist), so
# that workload cannot be written in Nirqoba yet. It is recorded as unavailable
# rather than approximated with a single-threaded loop.
UNAVAILABLE = [
    ("multi_threaded_allocation", "no thread creation API in the compiler or runtime"),
]


def compiler_version():
    result = subprocess.run([COMPILER, "--version"], capture_output=True, text=True)
    return (result.stdout or result.stderr).strip().splitlines()[0]


def build_mode():
    cache = os.path.join(BUILD_DIR, "CMakeCache.txt")
    if not os.path.exists(cache):
        return "unknown"
    with open(cache, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("CMAKE_BUILD_TYPE:"):
                return line.split("=", 1)[1].strip()
    return "unknown"


def build(source):
    exe = os.path.join(OUT_DIR, os.path.splitext(source)[0] + ".exe")
    result = subprocess.run(
        [COMPILER, "build", os.path.join(HERE, source), "-o", exe],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("build failed for %s:\n%s%s" % (source, result.stdout, result.stderr))
    return exe


def run_once(exe):
    started = time.perf_counter()
    result = subprocess.run([exe], capture_output=True, text=True)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if result.returncode != 0:
        sys.exit("execution failed for %s:\n%s" % (exe, result.stderr))
    values = [int(v) for v in re.findall(r"-?\d+", result.stdout)]
    if len(values) != 2:
        sys.exit("expected checksum and cycles from %s, got %r" % (exe, result.stdout))
    return values[0], values[1], elapsed_ms


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reps", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=1)
    args = parser.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    records = []
    for name, source, unit, operations in PROGRAMS:
        exe = build(source)
        for _ in range(args.warmups):
            run_once(exe)
        samples = [run_once(exe) for _ in range(args.reps)]
        checksums = {s[0] for s in samples}
        if len(checksums) != 1:
            sys.exit("checksum drifted across runs for %s: %s" % (name, sorted(checksums)))
        cycles = [s[1] for s in samples]
        wall = [s[2] for s in samples]
        # A workload that the optimizer removed still prints a checksum, because the
        # checksum is folded to a constant. Guard against reporting that as a result:
        # no real workload in this suite finishes in under 0.05 cycles per unit.
        if min(cycles) / operations < 0.05:
            sys.exit("%s measured %.6f cycles per %s; the workload was almost certainly "
                     "optimized away. Give it a runtime-derived input or an observable "
                     "side effect." % (name, min(cycles) / operations, unit))
        records.append({
            "workload": name,
            "source": source,
            "unit": unit,
            "operations": operations,
            "checksum": samples[0][0],
            "cycles": {"median": statistics.median(cycles), "best": min(cycles),
                       "samples": cycles},
            "wall_ms": {"median": statistics.median(wall), "best": min(wall),
                        "samples": wall},
            "cycles_per_unit": {"median": statistics.median(cycles) / operations,
                                "best": min(cycles) / operations},
        })
        record = records[-1]
        print("%-20s checksum=%-16d median=%12d cyc  best=%12d cyc  median=%7.2f ms  %.3f cyc/%s"
              % (name, record["checksum"], record["cycles"]["median"],
                 record["cycles"]["best"], record["wall_ms"]["median"],
                 record["cycles_per_unit"]["median"], unit))

    environment = {
        "date": datetime.datetime.now().isoformat(timespec="seconds"),
        "compiler": compiler_version(),
        "build_mode": build_mode(),
        "flags": "default (-O2), RelWithDebInfo build",
        "cpu": platform.processor() or platform.machine(),
        "machine": platform.machine(),
        "os": platform.platform(),
        "python": platform.python_version(),
        "warmups": args.warmups,
        "reps": args.reps,
        "timer": "time.cycles() (__rdtsc) around the workload only",
    }

    payload = {"environment": environment, "results": records, "unavailable": UNAVAILABLE}
    with open(os.path.join(HERE, "BASELINE.json"), "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")

    lines = [
        "# Pre-MMM Phase 0 baseline",
        "",
        "Generated by `python benchmarks/pre_mmm_baseline/run.py` on %s." % environment["date"],
        "",
        "| Property | Value |",
        "|---|---|",
        "| Compiler | %s |" % environment["compiler"],
        "| Build mode | %s |" % environment["build_mode"],
        "| Flags | %s |" % environment["flags"],
        "| CPU | %s |" % environment["cpu"],
        "| OS | %s |" % environment["os"],
        "| Warmups / reps | %d / %d |" % (args.warmups, args.reps),
        "| Timer | %s |" % environment["timer"],
        "",
        "Every repetition produced the recorded checksum; a drifting checksum aborts the run.",
        "",
        "| Workload | Checksum | Median cycles | Best cycles | Median wall ms | Best wall ms | Median cyc/unit |",
        "|---|---|---|---|---|---|---|",
    ]
    for record in records:
        lines.append("| %s | %d | %d | %d | %.2f | %.2f | %.3f %s |" % (
            record["workload"], record["checksum"],
            record["cycles"]["median"], record["cycles"]["best"],
            record["wall_ms"]["median"], record["wall_ms"]["best"],
            record["cycles_per_unit"]["median"], record["unit"]))
    lines += [
        "",
        "## Not measured",
        "",
        "| Workload | Reason |",
        "|---|---|",
    ]
    for name, reason in UNAVAILABLE:
        lines.append("| %s | %s |" % (name, reason))
    lines += [
        "",
        "Cycles come from `__rdtsc` inside the program, so compiler startup, runtime",
        "initialisation and process launch are excluded. Wall time is measured by the",
        "runner around the whole process and therefore includes them; the startup row",
        "shows how much of a short run is start-up rather than work.",
        "",
    ]
    with open(os.path.join(HERE, "BASELINE.md"), "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    print("\nwrote BASELINE.md and BASELINE.json")


if __name__ == "__main__":
    main()
