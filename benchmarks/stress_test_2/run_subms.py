#!/usr/bin/env python3
"""Sub-millisecond collection timing for Stress Test 2 (Nirqoba only).

Compiles and runs Tests 01 and 03 with their existing COLLECT_CYCLES
instrumentation, calibrates CPU cycles/ms, and reports per-collection time
in microseconds. Does not run the full multi-language benchmark suite.

Usage:
    python run_subms.py
    python run_subms.py --repeats 5
    python run_subms.py --skip-compile
    python run_subms.py --test 01
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

BENCH_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = BENCH_DIR.parents[1]
BIN_DIR = BENCH_DIR / "bin"
OBJ_DIR = BIN_DIR / "obj"
NRQ_EXE = PROJECT_ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"

COMPILE_TIMEOUT = 300
RUN_TIMEOUT = 600
DEFAULT_REPEATS = 3  # process repeats (each gives 5 measured rounds)

TESTS = {
    "01": {
        "name": "managed_object_allocation",
        "source": BENCH_DIR / "nirqoba" / "01_managed_object_allocation.nrqba",
        "exe": "01_nirqoba.exe",
        "expected_checksum": 499_999_500_000,
        "expected_live_after": 0,
        "expected_objects": 1_000_000,
    },
    "03": {
        "name": "cyclic_graph_reclamation",
        "source": BENCH_DIR / "nirqoba" / "03_cyclic_graph_reclamation.nrqba",
        "exe": "03_nirqoba.exe",
        "expected_checksum": 499_999_500_000,
        "expected_live_after": 0,
        "expected_objects": 1_000_000,
    },
}

CALIBRATION_SOURCE = r"""
int function main() {
    // Warm up timing path
    let warm_c = time.cycles();
    let warm_m = time.now();

    let target_ms = 200;
    let start_ms = time.now();
    let start_cycles = time.cycles();
    var spin = 0;
    while (time.now() - start_ms < target_ms) {
        spin = spin + 1;
    }
    let end_cycles = time.cycles();
    let end_ms = time.now();

    print("CYCLES");
    print(end_cycles - start_cycles);
    print("MS");
    print(end_ms - start_ms);
    // prevent dead-code elimination
    if (spin == -1) {
        print(warm_c);
        print(warm_m);
    }
    return 0;
}
"""


def run_cmd(cmd, cwd=None, timeout=RUN_TIMEOUT):
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def compile_nirqoba(source: Path, output: Path) -> tuple[bool, str]:
    if not NRQ_EXE.exists():
        return False, f"Nirqoba compiler not found: {NRQ_EXE}"
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    result = run_cmd(
        [str(NRQ_EXE), "build", str(source), "-o", str(output), "-O3"],
        cwd=OBJ_DIR,
        timeout=COMPILE_TIMEOUT,
    )
    log = (result.stdout or "") + (result.stderr or "")
    return result.returncode == 0, log


def parse_kv_output(text: str) -> dict[str, Any]:
    """Parse alternating KEY/value lines (and inline 'KEY value')."""
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    globals_: dict[str, Any] = {}
    rounds: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    known = {
        "TEST", "IMPL", "OBJECTS", "WARMUP_ROUNDS", "MEASURE_ROUNDS",
        "ROUND", "ALLOC_MS", "TRAVERSE_MS", "COLLECT_MS", "COLLECT_CYCLES",
        "BUILD_MS", "CHECKSUM", "LIVE_AFTER", "CYCLES_CLEARED",
        "CYCLES", "MS",
    }
    i = 0
    while i < len(lines):
        line = lines[i]
        key = None
        val = None
        if line in known:
            key = line
            if i + 1 < len(lines):
                val = lines[i + 1]
                i += 1
        else:
            for k in known:
                if line.startswith(k + " "):
                    key = k
                    val = line[len(k) + 1:].strip()
                    break
        if key is not None and val is not None:
            if key == "ROUND":
                current = {"ROUND": int(float(val))}
                rounds.append(current)
            elif current is not None and key in {
                "ALLOC_MS", "TRAVERSE_MS", "COLLECT_MS", "COLLECT_CYCLES",
                "BUILD_MS", "CHECKSUM", "LIVE_AFTER", "CYCLES_CLEARED",
            }:
                try:
                    current[key] = int(val) if "." not in val and "e" not in val.lower() else float(val)
                except ValueError:
                    current[key] = val
            else:
                globals_[key] = val
        i += 1
    return {"globals": globals_, "rounds": rounds}


def calibrate_cycles_per_ms() -> tuple[float, dict[str, Any]]:
    """Compile and run a short calibration to get cycles/ms."""
    with tempfile.TemporaryDirectory(prefix="nrq_cal_") as td:
        src = Path(td) / "calibrate.nrqba"
        exe = Path(td) / "calibrate.exe"
        src.write_text(CALIBRATION_SOURCE, encoding="utf-8")
        ok, log = compile_nirqoba(src, exe)
        if not ok:
            raise RuntimeError(f"Calibration compile failed:\n{log}")
        result = run_cmd([str(exe)], timeout=60)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            raise RuntimeError(f"Calibration run failed (rc={result.returncode}):\n{output}")
        parsed = parse_kv_output(output)
        g = parsed["globals"]
        cycles = int(float(g["CYCLES"]))
        ms = int(float(g["MS"]))
        if ms <= 0 or cycles <= 0:
            raise RuntimeError(f"Invalid calibration result: cycles={cycles}, ms={ms}\n{output}")
        cycles_per_ms = cycles / ms
        return cycles_per_ms, {"cycles": cycles, "ms": ms, "cycles_per_ms": cycles_per_ms, "raw": output}


def fmt_us(us: float) -> str:
    if us >= 1000:
        return f"{us/1000:.3f} ms ({us:.1f} us)"
    return f"{us:.1f} us ({us/1000:.4f} ms)"


def stats(values: list[float]) -> dict[str, float]:
    return {
        "n": len(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "min": min(values),
        "max": max(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def validate_round(test: dict, rd: dict, run_idx: int) -> list[str]:
    problems = []
    prefix = f"run {run_idx+1} round {rd.get('ROUND','?')}"
    if int(rd.get("CHECKSUM", -1)) != test["expected_checksum"]:
        problems.append(f"{prefix}: CHECKSUM={rd.get('CHECKSUM')} expected {test['expected_checksum']}")
    if int(rd.get("LIVE_AFTER", -1)) != test["expected_live_after"]:
        problems.append(f"{prefix}: LIVE_AFTER={rd.get('LIVE_AFTER')} expected {test['expected_live_after']}")
    if "COLLECT_CYCLES" not in rd:
        problems.append(f"{prefix}: missing COLLECT_CYCLES")
    elif int(rd["COLLECT_CYCLES"]) < 0:
        problems.append(f"{prefix}: negative COLLECT_CYCLES")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--test", choices=sorted(TESTS), action="append",
                    help="Test id to run (01, 03). Repeatable. Default: both.")
    ap.add_argument("--repeats", type=int, default=DEFAULT_REPEATS,
                    help=f"Process repeats (each has 5 measured rounds). Default: {DEFAULT_REPEATS}")
    ap.add_argument("--skip-compile", action="store_true")
    ap.add_argument("--json", metavar="PATH", help="Write machine-readable results JSON here")
    args = ap.parse_args()

    if args.repeats < 1:
        ap.error("--repeats must be >= 1")

    selected = args.test or sorted(TESTS.keys())

    print("Sub-millisecond GC timing (Nirqoba, Tests 01 & 03)")
    print(f"  compiler:  {NRQ_EXE}")
    try:
        ver = run_cmd([str(NRQ_EXE), "--version"], timeout=30)
        ver_line = (ver.stdout + ver.stderr).strip().splitlines()
        if ver_line:
            print(f"  version:   {ver_line[0].strip()}")
    except Exception:
        pass
    print(f"  tests:     {', '.join(selected)}")
    print(f"  repeats:   {args.repeats} process(es) x 5 rounds = {args.repeats * 5} samples/test")
    print()

    BIN_DIR.mkdir(parents=True, exist_ok=True)

    if not args.skip_compile:
        print("Compiling:")
        for tid in selected:
            t = TESTS[tid]
            if not t["source"].exists():
                print(f"  {tid} MISSING SOURCE: {t['source']}")
                return 2
            exe = BIN_DIR / t["exe"]
            ok, log = compile_nirqoba(t["source"], exe)
            status = "OK" if ok else "FAILED"
            print(f"  {tid} {t['name']}: {status}")
            if not ok:
                print(log)
                return 2
        print()

    print("Calibrating cycles/ms...")
    cycles_per_ms, cal = calibrate_cycles_per_ms()
    cycles_per_us = cycles_per_ms / 1000.0
    print(f"  {cal['cycles']:,} cycles / {cal['ms']} ms = {cycles_per_ms:,.2f} cycles/ms ({cycles_per_us:,.2f} cycles/us)")
    print()

    all_results: dict[str, Any] = {
        "compiler": str(NRQ_EXE),
        "calibration": cal,
        "tests": {},
    }
    any_problem = False

    for tid in selected:
        t = TESTS[tid]
        exe = BIN_DIR / t["exe"]
        if not exe.exists():
            print(f"[{tid}] SKIP: binary not found: {exe}")
            any_problem = True
            continue

        print(f"=== Test {tid}: {t['name']} ===")
        print(f"    objects={t['expected_objects']:,}, expected checksum={t['expected_checksum']}, expected live_after={t['expected_live_after']}")
        all_rounds = []
        problems = []
        for r in range(args.repeats):
            result = run_cmd([str(exe)], timeout=RUN_TIMEOUT)
            output = (result.stdout or "") + (result.stderr or "")
            if result.returncode != 0:
                problems.append(f"run {r+1}: exit code {result.returncode}")
                print(f"  run {r+1}: EXIT {result.returncode}")
                print(output)
                continue
            parsed = parse_kv_output(output)
            rounds = parsed["rounds"]
            if len(rounds) != 5:
                problems.append(f"run {r+1}: expected 5 rounds, got {len(rounds)}")
            for rd in rounds:
                problems.extend(validate_round(t, rd, r))
                cycles = int(rd.get("COLLECT_CYCLES", 0))
                ms_reported = int(rd.get("COLLECT_MS", 0))
                us = cycles / cycles_per_us
                ms_calc = cycles / cycles_per_ms
                all_rounds.append({
                    "run": r + 1,
                    "round": int(rd.get("ROUND", -1)),
                    "collect_cycles": cycles,
                    "collect_ms_reported": ms_reported,
                    "collect_us": us,
                    "collect_ms_calculated": ms_calc,
                    "checksum": int(rd.get("CHECKSUM", -1)),
                    "live_after": int(rd.get("LIVE_AFTER", -1)),
                })

        if not all_rounds:
            print("  No successful rounds.\n")
            any_problem = True
            all_results["tests"][tid] = {"problems": problems}
            continue

        cycles_vals = [x["collect_cycles"] for x in all_rounds]
        us_vals = [x["collect_us"] for x in all_rounds]
        ms_rep_vals = [x["collect_ms_reported"] for x in all_rounds]

        print(f"\n  Per-round results ({len(all_rounds)} samples):")
        print(f"  {'run':>3} {'rd':>3}  {'cycles':>12}  {'reported':>9}  {'calculated (sub-ms)':>22}")
        print(f"  {'-'*3} {'-'*3}  {'-'*12}  {'-'*9}  {'-'*22}")
        for x in all_rounds:
            print(f"  {x['run']:>3} {x['round']:>3}  {x['collect_cycles']:>12,}  {x['collect_ms_reported']:>6} ms  {fmt_us(x['collect_us']):>22}")

        cst = stats([float(v) for v in cycles_vals])
        ust = stats(us_vals)
        print(f"\n  Summary (COLLECT_CYCLES):")
        print(f"    median: {cst['median']:,.0f}  mean: {cst['mean']:,.1f}  min: {cst['min']:,.0f}  max: {cst['max']:,.0f}  stdev: {cst['stdev']:,.1f}")
        print(f"  Summary (calculated from cycles):")
        print(f"    median: {fmt_us(ust['median'])}")
        print(f"    mean:   {fmt_us(ust['mean'])}")
        print(f"    min:    {fmt_us(ust['min'])}")
        print(f"    max:    {fmt_us(ust['max'])}")
        print(f"    stdev:  {ust['stdev']:.1f} us")
        print(f"  Harness-reported COLLECT_MS: median={statistics.median(ms_rep_vals):g} ms, values={sorted(set(ms_rep_vals))}")

        if problems:
            any_problem = True
            print(f"\n  Validation problems ({len(problems)}):")
            for p in problems:
                print(f"    - {p}")
        else:
            print(f"\n  Validation: OK (all checksums={t['expected_checksum']}, all live_after={t['expected_live_after']})")

        all_results["tests"][tid] = {
            "name": t["name"],
            "samples": len(all_rounds),
            "rounds": all_rounds,
            "cycles": cst,
            "microseconds": ust,
            "reported_ms_median": statistics.median(ms_rep_vals),
            "problems": problems,
            "valid": len(problems) == 0,
        }
        print()

    if args.json:
        out = Path(args.json)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(all_results, indent=2), encoding="utf-8")
        print(f"JSON written to: {out}")

    return 1 if any_problem else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
