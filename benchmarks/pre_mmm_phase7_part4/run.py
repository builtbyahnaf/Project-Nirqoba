#!/usr/bin/env python3
"""Part 4 benchmark: aggregate destruction lowering cost.

The Part 4 traversal change removes a heap-allocated recursive callable from
every `aggregate.owned_drop` and `aggregate.shared_retain` site. That cost is
paid by the compiler, not by the generated program, so this benchmark builds a
synthetic module with many independent aggregate lifetimes and measures compiler
wall time and peak RSS. Both compilers are alternated to reduce ordering bias.
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

FIELDS = ("first", "second", "third", "fourth", "fifth", "sixth",
          "seventh", "eighth")


def generate_source(path, functions, scopes_per_function):
    fields = "\n".join(f"    owned_array<int> {name};" for name in FIELDS)
    lines = [
        "struct Holder {",
        fields,
        "    int tag;",
        "}",
        "",
    ]
    for function in range(functions):
        lines.append(f"int function consume_{function}(int seed) {{")
        lines.append("    var checksum = 0;")
        for scope in range(scopes_per_function):
            lines.extend([
                "    {",
                "        var holder: Holder = uninit;",
                f"        holder.tag = seed + {scope};",
                "        checksum += holder.tag;",
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
    parser.add_argument("--functions", type=int, default=20)
    parser.add_argument("--scopes", type=int, default=250)
    parser.add_argument("--repeats", type=int, default=7)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    baseline = Path(args.baseline).resolve()
    current = Path(args.current).resolve()
    samples = {"baseline": [], "current": []}

    with tempfile.TemporaryDirectory(prefix="nrq-phase7-p4-") as temp:
        root = Path(temp)
        source = root / "aggregate_drop_many.nrqba"
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
                    "object_bytes": (out_dir / "aggregate_drop_many.obj").stat().st_size,
                    "executable_bytes": (out_dir / "aggregate_drop_many.exe").stat().st_size,
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
        "mode": "aggregate-drop-compile",
        "functions": args.functions,
        "scopes_per_function": args.scopes,
        "drop_sites": args.functions * args.scopes,
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