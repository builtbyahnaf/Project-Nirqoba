#!/usr/bin/env python3
"""Recompute the ColO regression matrix and stage breakdown from raw runs.

Reads:
  results/raw/<sha>_<mode>_runN.txt        (unmodified harness output)
  results/stage_raw/<sha>_<mode>_runN.txt  (patched-gc diagnostic build)

Writes:
  results/matrix.md                        (median / min / max per revision)
  results/matrix.json

Only the fields the harness prints are used; nothing is estimated here.
"""

from __future__ import annotations

import json
import re
import statistics
from pathlib import Path

RESULTS = Path(__file__).resolve().parent / "results"

ORDER = [
    ("98a1099", "0.257-beta release"),
    ("f1d2b42", "free-span ordering fix"),
    ("1ca3c37", "page index for reusable runs"),
    ("e830b65", "page-ordered sweep  <-- regression"),
    ("1646b93", "minor skips page with no young object"),
    ("0708f75", "byte-sized mark/sweep slices (0.260-beta code)"),
    ("5d62abf", "narrow-layout marking"),
    ("904aee0", "HEAD (0.266-beta working tree)"),
]

FIELDS = ("COLLECT_MEDIAN", "COLLECT_MIN", "COLLECT_MAX", "BUILD_MS",
          "BUILD_MEDIAN", "TRAVERSE_MS", "ALLOC_MS", "PAGES")


def parse(path: Path) -> dict:
    text = path.read_text(encoding="utf-8", errors="replace")
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] in FIELDS:
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                pass
        # ROUND lines carry per-round timings; keep the medians the harness
        # already computed elsewhere, but capture PAGES from the first round.
        if parts and parts[0] == "PAGES" and len(parts) >= 2:
            out["PAGES"] = float(parts[1])
        if line.startswith("ROUND") and "PAGES" in line:
            m = re.search(r"COLLECT_MS ([\d.]+).*PAGES (\d+)", line)
            if m and "PAGES" not in out:
                out["PAGES"] = float(m.group(2))
    return out


def collect(sha: str, mode: str) -> list[dict]:
    runs = []
    for path in sorted(RESULTS.joinpath("raw").glob(f"{sha}_{mode}_run*.txt")):
        runs.append(parse(path))
    return runs


def summarize(values: list[float]) -> str:
    if not values:
        return "n/a"
    med = statistics.median(values)
    if len(values) == 1:
        return f"{med:.1f}"
    return f"{med:.1f} ({min(values):.1f}..{max(values):.1f})"


def main() -> None:
    rows = []
    for sha, label in ORDER:
        row = {"sha": sha, "label": label}
        for mode in ("02", "03"):
            runs = collect(sha, mode)
            row[f"{mode}_n"] = len(runs)
            for field in ("COLLECT_MEDIAN", "BUILD_MS", "BUILD_MEDIAN",
                          "TRAVERSE_MS", "PAGES"):
                values = [r[field] for r in runs if field in r]
                row[f"{mode}_{field}"] = summarize(values)
            # 02 prints a single BUILD_MS; expose it as the build column.
            if mode == "02":
                row["02_BUILD"] = row.get("02_BUILD_MS", "n/a")
            else:
                row["03_BUILD"] = row.get("03_BUILD_MEDIAN", "n/a")
        runs = collect(sha, "alloc")
        values = [r["ALLOC_MS"] for r in runs if "ALLOC_MS" in r]
        row["alloc_ALLOC_MS"] = summarize(values)
        rows.append(row)

    lines = ["| revision | COLLECT 02 (ms) | COLLECT 03 (ms) | BUILD 02 (ms) | BUILD 03 (ms) | ALLOC 01 (ms) |",
             "|---|---|---|---|---|---|"]
    for r in rows:
        lines.append(
            f"| `{r['sha']}` {r['label']} | {r.get('02_COLLECT_MEDIAN','n/a')} "
            f"| {r.get('03_COLLECT_MEDIAN','n/a')} | {r.get('02_BUILD','n/a')} "
            f"| {r.get('03_BUILD','n/a')} | {r['alloc_ALLOC_MS']} |")
    matrix = "\n".join(lines) + "\n"

    # Stage probe: totals over every collection in the run, then per collection.
    stage_lines = ["| revision | mode | collections | begin ms/col | mark ms/col | sweep ms/col | zero-bit iters/col | objects/col |",
                   "|---|---|---|---|---|---|---|---|"]
    stage = {}
    for sha in ("1ca3c37", "e830b65", "904aee0"):
        for mode in ("02", "03"):
            per_run = []
            for path in sorted(RESULTS.joinpath("stage_raw").glob(f"{sha}_{mode}_run*.txt")):
                text = path.read_text(encoding="utf-8", errors="replace")
                probe = {}
                for line in text.splitlines():
                    if line.startswith("PROBE "):
                        _, key, value = line.split(maxsplit=2)
                        probe[key] = float(value)
                if "collections" in probe and probe["collections"] > 0:
                    n = probe["collections"]
                    per_run.append({
                        "begin": probe["begin_ms"] / n,
                        "mark": probe["mark_ms"] / n,
                        "sweep": probe["sweep_ms"] / n,
                        "zeros": probe["zero_bits"] / n,
                        "objects": probe["set_bits"] / n,
                    })
            if not per_run:
                continue
            def med(key):
                return statistics.median([r[key] for r in per_run])
            stage[f"{sha}_{mode}"] = {k: med(k) for k in ("begin", "mark", "sweep", "zeros", "objects")}
            stage_lines.append(
                f"| `{sha}` | {mode} | {len(per_run)} runs | {med('begin'):.2f} "
                f"| {med('mark'):.2f} | {med('sweep'):.2f} | {med('zeros'):,.0f} "
                f"| {med('objects'):,.0f} |")

    out = RESULTS / "matrix.md"
    out.write_text("# Reconstructed regression matrix\n\n" + matrix + "\n" +
                   "\n".join(stage_lines) + "\n", encoding="utf-8")
    (RESULTS / "matrix.json").write_text(
        json.dumps({"rows": rows, "stage": stage}, indent=2), encoding="utf-8")
    print(matrix)
    print("\n".join(stage_lines))
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
