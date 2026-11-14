#!/usr/bin/env python3
"""Part 3 benchmarks: artifact publication and mutex registry ownership.

`build` measures an end-to-end build that publishes an object and executable.
`mutex` builds one executable per compiler and measures repeated
create/lock/unlock/destroy round trips. Both alternate compiler order and verify
output, exit status, and peak working set.
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


def median(values):
    return round(statistics.median(values), 3)


def build(compiler, source, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    wall, peak, code, out, err = run_timed(
        [str(compiler), "build", str(source), "--out-dir", str(out_dir)])
    if code != 0:
        raise RuntimeError(f"build failed with {compiler}: {err}")
    return wall, peak, out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("build", "mutex"), required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--repeats", type=int, default=9)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    baseline = Path(args.baseline).resolve()
    current = Path(args.current).resolve()
    samples = {"baseline": [], "current": []}

    with tempfile.TemporaryDirectory(prefix="nrq-phase7-p3-") as temp:
        root = Path(temp)
        if args.mode == "build":
            source = Path(__file__).parent / "build_hello.nrqba"
            for repeat in range(args.repeats):
                order = (("baseline", "current") if repeat % 2 == 0
                         else ("current", "baseline"))
                for name in order:
                    compiler = baseline if name == "baseline" else current
                    out_dir = root / f"{name}-{repeat}"
                    wall, peak, stdout = build(compiler, source, out_dir)
                    samples[name].append({
                        "wall_ms": wall * 1000,
                        "peak_rss_bytes": peak,
                        "exe_bytes": (out_dir / "build_hello.exe").stat().st_size,
                        "obj_bytes": (out_dir / "build_hello.obj").stat().st_size,
                        "stdout": stdout.strip(),
                    })
        else:
            source = Path(__file__).parent / "mutex_roundtrip.nrqba"
            binaries = {}
            for name, compiler in (("baseline", baseline), ("current", current)):
                out_dir = root / f"exe-{name}"
                build(compiler, source, out_dir)
                binaries[name] = out_dir / "mutex_roundtrip.exe"
            for repeat in range(args.repeats):
                order = (("baseline", "current") if repeat % 2 == 0
                         else ("current", "baseline"))
                for name in order:
                    wall, peak, code, stdout, err = run_timed([str(binaries[name])])
                    if code != 0:
                        raise RuntimeError(f"{name} run failed: {err}")
                    samples[name].append({
                        "wall_ms": wall * 1000,
                        "peak_rss_bytes": peak,
                        "stdout": stdout.strip(),
                    })

    summary = {}
    for name, values in samples.items():
        summary[name] = {
            "median_wall_ms": median([v["wall_ms"] for v in values]),
            "median_peak_rss_bytes": int(statistics.median(
                [v["peak_rss_bytes"] for v in values])),
            "outputs": sorted({v["stdout"] for v in values}),
        }
    b, c = summary["baseline"], summary["current"]
    result = {
        "mode": args.mode,
        "repeats": args.repeats,
        "baseline": summary["baseline"],
        "current": summary["current"],
        "comparison": {
            "wall_change_percent": round(
                (c["median_wall_ms"] - b["median_wall_ms"]) /
                b["median_wall_ms"] * 100, 2),
            "peak_rss_change_percent": round(
                (c["median_peak_rss_bytes"] - b["median_peak_rss_bytes"]) /
                b["median_peak_rss_bytes"] * 100, 2),
        },
        "samples": samples,
    }
    Path(args.output).write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result["baseline"], indent=2))
    print(json.dumps(result["current"], indent=2))
    print(json.dumps(result["comparison"], indent=2))


if __name__ == "__main__":
    main()
