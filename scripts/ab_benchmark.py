#!/usr/bin/env python3
"""Interleaved A/B comparison of two collector benchmark binaries.

The phase reports this project wrote before compared "all baseline runs" with "all
optimized runs", which cannot separate a change from slow drift on the host. This
runs the two binaries alternately, A, B, A, B, ... for N sessions. Each session
contributes one number per metric to each side, so the comparison is paired: the
thermal state, background load and allocator state of session k are shared by both
sides of session k instead of being spread unevenly over them.

What it reports, per metric:

  medians   the middle session value for each side
  ranges    the min..max of each side, so a difference smaller than the spread is
            visible as such instead of being read as an effect
  wins      how many sessions each side was faster in, which is the sign test
  paired    the median of (B - A) per session and its range

Usage:
  ab_benchmark.py --a build/ab/baseline.exe --b build/ab/candidate.exe \
                  [--objects 300000] [--runs 3] [--sessions 6] [--json out.json]

The binaries are the same program with identical inputs; only the code under test
differs.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
from dataclasses import dataclass, field

# A shape's median row looks like:
#   h0-chain-16B-median     300001      32.8     11064.0       36.88 ...
SHAPE_ROW = re.compile(
    r"^(?P<shape>[A-Za-z0-9\-]+)-median\s+(?P<objects>\d+)\s+"
    r"(?P<alloc>[0-9.]+)\s+(?P<mark_us>[0-9.]+)\s+(?P<mark_ns>[0-9.]+)\s+"
    r"(?P<sweep_us>[0-9.]+)\s+(?P<sweep_ns>[0-9.]+)"
)
BARRIER_ROW = re.compile(r"^barrier-old-to-young median\s+(?P<ns>[0-9.]+)")
PHASE_ROW = re.compile(
    r"^(?P<shape>[A-Za-z0-9\-]+)\s+(?P<cycle>live|dead)\s+(?P<pause>[0-9.]+)\s+"
    r"(?P<prepare>[0-9.]+)\s+(?P<mark>[0-9.]+)\s+(?P<sweep>[0-9.]+)\s+"
    r"(?P<finish>[0-9.]+)\s+(?P<mark_ns>[0-9.]+)\s+(?P<sweep_ns>[0-9.]+)"
)
FRAG_ROW = re.compile(r"^\s+(?P<key>[a-z ]+):\s+(?P<value>[0-9.]+)")
PERCENT_ROW = re.compile(r"^\s+utilization:\s+(?P<value>[0-9.]+)")


@dataclass
class Metrics:
    """Every number one invocation reports, keyed by metric name."""

    values: dict[str, float] = field(default_factory=dict)


def parse(output: str) -> Metrics:
    metrics = Metrics()
    for line in output.splitlines():
        line = line.rstrip()
        match = SHAPE_ROW.match(line)
        if match:
            shape = match.group("shape")
            metrics.values[f"{shape}.alloc_ns"] = float(match.group("alloc"))
            metrics.values[f"{shape}.mark_ns"] = float(match.group("mark_ns"))
            metrics.values[f"{shape}.mark_us"] = float(match.group("mark_us"))
            metrics.values[f"{shape}.sweep_ns"] = float(match.group("sweep_ns"))
            metrics.values[f"{shape}.sweep_us"] = float(match.group("sweep_us"))
            continue
        match = BARRIER_ROW.match(line)
        if match:
            metrics.values["barrier.ns_per_store"] = float(match.group("ns"))
            continue
        match = PHASE_ROW.match(line)
        if match:
            shape = match.group("shape")
            cycle = match.group("cycle")
            for key in ("pause", "prepare", "mark", "sweep", "finish"):
                metrics.values[f"{shape}.{cycle}.{key}_us"] = float(
                    match.group(key)
                )
            metrics.values[f"{shape}.{cycle}.mark_ns_per_object"] = float(
                match.group("mark_ns")
            )
            metrics.values[f"{shape}.{cycle}.sweep_ns_per_object"] = float(
                match.group("sweep_ns")
            )
            continue
        match = PERCENT_ROW.match(line)
        if match:
            metrics.values["fragmentation.utilization"] = float(
                match.group("value")
            )
            continue
        match = FRAG_ROW.match(line)
        if match:
            key = "_".join(match.group("key").split())
            metrics.values[f"fragmentation.{key}"] = float(match.group("value"))
    return metrics


def run_binary(binary: str, objects: int, runs: int) -> Metrics:
    # Windows resolves a relative executable only through the current directory of
    # the creating process, so the path is made absolute before it is handed over.
    command = [os.path.abspath(binary), str(objects), str(runs)]
    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise SystemExit(
            f"{shlex.join(command)} exited {completed.returncode}\n"
            f"{completed.stderr}"
        )
    return parse(completed.stdout)


def summarize(name: str, series: list[float]) -> dict[str, float]:
    return {
        "name": name,
        "median": statistics.median(series),
        "min": min(series),
        "max": max(series),
        "count": len(series),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", required=True, help="baseline binary")
    parser.add_argument("--b", required=True, help="candidate binary")
    parser.add_argument("--objects", type=int, default=300000)
    parser.add_argument("--runs", type=int, default=3,
                        help="internal runs per invocation")
    parser.add_argument("--sessions", type=int, default=6,
                        help="A/B alternations")
    parser.add_argument("--metric", action="append", default=None,
                        help="restrict the report to metrics containing this text")
    parser.add_argument("--json", default=None, help="write the full result here")
    args = parser.parse_args()

    if args.sessions < 2:
        parser.error("--sessions must be at least 2 for a paired comparison")

    a_sessions: list[Metrics] = []
    b_sessions: list[Metrics] = []
    for session in range(args.sessions):
        # Alternate the order inside a session too, so a side never inherits the
        # warm start of the other for the whole series.
        if session % 2 == 0:
            a_sessions.append(run_binary(args.a, args.objects, args.runs))
            b_sessions.append(run_binary(args.b, args.objects, args.runs))
        else:
            b_sessions.append(run_binary(args.b, args.objects, args.runs))
            a_sessions.append(run_binary(args.a, args.objects, args.runs))
        print(f"session {session + 1}/{args.sessions} complete", file=sys.stderr)

    keys = sorted(set(a_sessions[0].values) & set(b_sessions[0].values))
    if args.metric:
        needles = [needle.lower() for needle in args.metric]
        keys = [key for key in keys if any(n in key.lower() for n in needles)]

    rows = []
    for key in keys:
        a_series = [session.values[key] for session in a_sessions]
        b_series = [session.values[key] for session in b_sessions]
        a_summary = summarize("a", a_series)
        b_summary = summarize("b", b_series)
        deltas = [b - a for a, b in zip(a_series, b_series)]
        a_wins = sum(1 for delta in deltas if delta < 0)
        b_wins = sum(1 for delta in deltas if delta > 0)
        row = {
            "metric": key,
            "a": a_summary,
            "b": b_summary,
            "a_wins": a_wins,
            "b_wins": b_wins,
            "paired_delta": {
                "median": statistics.median(deltas),
                "min": min(deltas),
                "max": max(deltas),
            },
        }
        if a_summary["median"] != 0:
            row["ratio"] = b_summary["median"] / a_summary["median"]
            row["paired_ratio"] = statistics.median(
                [b / a for a, b in zip(a_series, b_series) if a != 0]
            )
        rows.append(row)

    print()
    print(
        f"{'metric':44s} {'A med':>10s} {'A range':>19s} "
        f"{'B med':>10s} {'B range':>19s} {'B/A':>7s} {'sessions':>9s}"
    )
    for row in rows:
        a = row["a"]
        b = row["b"]
        ratio = f"{row['ratio']:.3f}" if "ratio" in row else "-"
        print(
            f"{row['metric']:44s} {a['median']:10.1f} "
            f"{a['min']:9.1f}..{a['max']:<8.1f} "
            f"{b['median']:10.1f} {b['min']:9.1f}..{b['max']:<8.1f} "
            f"{ratio:>7s} {row['a_wins']:>4d}/{row['b_wins']:<4d}"
        )

    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(
                {
                    "a": args.a,
                    "b": args.b,
                    "objects": args.objects,
                    "runs": args.runs,
                    "sessions": args.sessions,
                    "results": rows,
                },
                handle,
                indent=2,
            )
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
