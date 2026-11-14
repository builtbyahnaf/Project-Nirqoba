#!/usr/bin/env python3
"""Phase 8 benchmark: allocation-boundary policy checks.

The Phase 8 rules inspect the payload type at every explicit, shared, owned-array
and collector allocation site. This workload generates many such sites with a
managed-bearing struct payload, measures compiler wall time and peak RSS, and
alternates the baseline and current compilers to reduce ordering bias. The
generated program is compiled but not run.
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


def generate_source(path, functions, scopes_per_function):
    lines = [
        "struct Node {",
        "    int value;",
        "}",
        "",
        "struct Payload {",
        "    int a;",
        "    managed<Node> node;",
        "}",
        "",
    ]
    for function in range(functions):
        lines.append(f"int function consume_{function}(int seed) {{")
        lines.append("    var checksum = 0;")
        for scope in range(scopes_per_function):
            lines.extend([
                "    {",
                "        var raw = memory.allocate<Payload>(64);",
                f"        (*raw).a = seed + {scope};",
                "        checksum += (*raw).a;",
                "    }",
                "    {",
                "        var bulk = memory.owned_array<Payload>(2);",
                f"        bulk[0].a = seed + {scope};",
                "        checksum += bulk[0].a;",
                "    }",
                "    {",
                "        var shared = memory.shared_allocate<Payload>(64);",
                f"        (*shared).a = seed + {scope};",
                "        checksum += (*shared).a;",
                "    }",
                "    {",
                "        var managed = array.allocate<Payload>(1);",
                f"        managed[0].a = seed + {scope};",
                "        checksum += managed[0].a;",
                "    }",
            ])
        lines.append("    return checksum;")
        lines.append("}")
        lines.append("")
    lines.append("int function main() {")
    lines.append("    var checksum = 0;")
    for function in range(functions):
        lines.append(f"    checksum += consume_{function}({function});")
    lines.append("    print(checksum);")
    lines.append("    return 0;")
    lines.append("}")
    path.write_text("\n".join(lines), encoding="utf-8")


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


def build(compiler, source, out_dir):
    out_dir.mkdir(parents=True, exist_ok=True)
    wall, peak, code, out, err = run_timed(
        [str(compiler), "build", str(source), "--out-dir", str(out_dir)])
    if code != 0:
        raise RuntimeError(f"build failed with {compiler}: {err}")
    return wall, peak, out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--current", required=True)
    parser.add_argument("--functions", type=int, default=50)
    parser.add_argument("--scopes", type=int, default=20)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    baseline = Path(args.baseline).resolve()
    current = Path(args.current).resolve()
    samples = {"baseline": [], "current": []}

    with tempfile.TemporaryDirectory(prefix="nrq-phase8-") as temp:
        root = Path(temp)
        source = root / "policy_allocations.nrqba"
        generate_source(source, args.functions, args.scopes)
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
                    "object_bytes": (out_dir / "policy_allocations.obj").stat().st_size,
                    "executable_bytes": (out_dir / "policy_allocations.exe").stat().st_size,
                    "stdout": stdout.strip(),
                })

    summary = {}
    for name, values in samples.items():
        summary[name] = {
            "median_wall_ms": round(statistics.median(
                [v["wall_ms"] for v in values]), 3),
            "median_peak_rss_bytes": int(statistics.median(
                [v["peak_rss_bytes"] for v in values])),
            "object_bytes": values[0]["object_bytes"],
            "executable_bytes": values[0]["executable_bytes"],
            "outputs": sorted({v["stdout"] for v in values}),
        }
    b, c = summary["baseline"], summary["current"]
    result = {
        "mode": "allocation-policy-compile",
        "functions": args.functions,
        "scopes_per_function": args.scopes,
        "allocation_sites": args.functions * args.scopes * 4,
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
