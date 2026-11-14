#!/usr/bin/env python3
"""Pre-MMM Phase 7 runtime-buffer benchmark.

Builds the same repeated-read program with the baseline and current compiler,
runs the executables alternately, and records wall time, peak working set, and
the program's checksum. The workload deliberately keeps every returned string
alive in the runtime arena, so the memory comparison reflects the ownership
change rather than a compiler optimisation.
"""

import argparse
import json
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

import psutil


REPO = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).parent / "read_text.nrqba"


def run_timed(command, cwd=REPO):
    start = time.perf_counter()
    process = psutil.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
    peak = 0
    while process.poll() is None:
        try:
            peak = max(peak, process.memory_info().rss)
        except psutil.Error:
            pass
        time.sleep(0.001)
    out, err = process.communicate()
    return time.perf_counter() - start, peak, process.returncode, out, err


def build(compiler, output):
    elapsed, peak, code, _, err = run_timed(
        [str(compiler), "build", str(SOURCE), "-o", str(output)])
    if code != 0:
        raise RuntimeError(f"build failed with {compiler}: {err}")
    return elapsed * 1000, peak


def median(values):
    return round(statistics.median(values), 3)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--output", default=str(Path(__file__).parent /
                                                "results.json"))
    args = parser.parse_args()

    compilers = {
        "baseline": Path(args.baseline).resolve(),
        "current": Path(args.current).resolve(),
    }

    with tempfile.TemporaryDirectory(prefix="nrq-phase7-runtime-") as temp:
        root = Path(temp)
        binaries = {}
        builds = {}
        for name, compiler in compilers.items():
            output = root / f"{name}.exe"
            builds[name] = build(compiler, output)
            binaries[name] = output

        samples = {name: [] for name in compilers}
        # Alternate order so a frequency or antivirus event cannot sit entirely
        # in one column.
        for repeat in range(args.repeats):
            order = (("baseline", "current") if repeat % 2 == 0
                     else ("current", "baseline"))
            for name in order:
                elapsed, peak, code, out, err = run_timed([str(binaries[name])])
                if code != 0:
                    raise RuntimeError(
                        f"{name} failed ({code}) at {binaries[name]}: {err}")
                samples[name].append({
                    "wall_ms": elapsed * 1000,
                    "peak_rss_bytes": peak,
                    "stdout": out.strip(),
                })

        results = {
            "source": str(SOURCE),
            "repeats": args.repeats,
            "baseline_compiler": str(compilers["baseline"]),
            "current_compiler": str(compilers["current"]),
            "build": {
                name: {"wall_ms": builds[name][0],
                       "peak_rss_bytes": builds[name][1]}
                for name in builds
            },
            "runs": samples,
            "summary": {},
        }
        for name, values in samples.items():
            walls = [value["wall_ms"] for value in values]
            peaks = [value["peak_rss_bytes"] for value in values]
            results["summary"][name] = {
                "median_wall_ms": median(walls),
                "median_peak_rss_bytes": int(statistics.median(peaks)),
                "stdout": sorted({value["stdout"] for value in values}),
            }
        b = results["summary"]["baseline"]
        c = results["summary"]["current"]
        results["comparison"] = {
            "wall_change_percent": round(
                (c["median_wall_ms"] - b["median_wall_ms"]) /
                b["median_wall_ms"] * 100, 2),
            "peak_rss_change_bytes": (c["median_peak_rss_bytes"] -
                                      b["median_peak_rss_bytes"]),
            "peak_rss_change_percent": round(
                (c["median_peak_rss_bytes"] - b["median_peak_rss_bytes"]) /
                b["median_peak_rss_bytes"] * 100, 2),
            "same_stdout": b["stdout"] == c["stdout"],
        }

    output = Path(args.output)
    output.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(json.dumps(results["summary"], indent=2))
    print(json.dumps(results["comparison"], indent=2))
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
