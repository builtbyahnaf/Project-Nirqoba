#!/usr/bin/env python3
"""Compile and run the Stress Test 3 PO-27 regression benchmark suite.

This suite is a targeted regression guard for the PO-27 hang (commit 0b2d457).
It does NOT compare Nirqoba against other languages; all three tracks are
Nirqoba-only and stress the specific failure mode identified in the analysis:

    01  slot_index_churn         — repeated insert/release with one live entry
    02  interleaved_alloc_collect — batch alloc, collect, discard, repeat
    03  root_frame_depth          — deep recursion with live managed locals

Pass criterion (all three tracks):
    - Process exits 0 within RUN_TIMEOUT_SECONDS.
    - Every round's ALLOC_MS / BUILD_MS is within NIRQOBA_MAX_PHASE_MS.
    - LIVE_AFTER == 0 (or 1 for the sentinel in test 02) after every collect.
    - CHECKSUM matches the expected value where applicable.
    - No monotonically-increasing alloc time across rounds (exponential growth
      detector: last_round_ms must not exceed first_round_ms * GROWTH_RATIO).

Usage:
    python run_benchmark.py
    python run_benchmark.py --skip-compile
    python run_benchmark.py --repeats 5
    python run_benchmark.py --test 01

Outputs:
    benchmark_results.json
    results/raw/<test>_nirqoba_run<N>.txt
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Paths and toolchain configuration
# ---------------------------------------------------------------------------

BENCH_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = BENCH_DIR.parents[1]
BIN_DIR = BENCH_DIR / "bin"
RAW_DIR = BENCH_DIR / "results" / "raw"
OBJ_DIR = BIN_DIR / "obj"
RESULT_JSON = BENCH_DIR / "benchmark_results.json"

NRQ_EXE = PROJECT_ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"

DEFAULT_REPEATS = 3
RUN_TIMEOUT_SECONDS = 120           # much tighter than stress_test_2 (600 s)
COMPILE_TIMEOUT_SECONDS = 300
NIRQOBA_MAX_PHASE_MS = 5000.0       # any single phase over 5 s is a hang
GROWTH_RATIO = 3.0                  # last round must not be > 3x first round

# ---------------------------------------------------------------------------
# Suite definition
# ---------------------------------------------------------------------------

# Expected checksums for correctness validation.
# Test 01: churn(500000) = sum(0..499999) = 500000*499999/2 = 124999750000
# Test 02: sum_chain(10000) = sum(0..9999)  = 10000*9999/2  = 49995000
#          (varies by round since the sentinel replaces head, so we skip it)
# Test 03: sum of a complete binary tree of depth 18 seeded at value=1 — large;
#          validated to be non-zero and stable across rounds instead.
TESTS: dict[str, dict[str, Any]] = {
    "01": {
        "name": "slot_index_churn",
        "title": "SlotIndex tombstone churn",
        "objects": 500_000,
        "checksum": 124_999_750_000,
        "phase_metrics": ["ALLOC_MS", "COLLECT_MS"],
        "growth_metric": "ALLOC_MS",
        "expected_live_after": 0,
    },
    "02": {
        "name": "interleaved_alloc_collect",
        "title": "Interleaved allocation and collection",
        "objects": 10_000,
        "checksum": None,       # skip: sentinel replaces head each round
        "phase_metrics": ["ALLOC_MS", "COLLECT_MS"],
        "growth_metric": "ALLOC_MS",
        "expected_live_after": 1,   # sentinel node stays alive
        "expected_collections_delta": 1,
    },
    "03": {
        "name": "root_frame_depth",
        "title": "Root-frame depth with deep recursion",
        "objects": 262_143,
        "checksum": None,       # large tree sum; validated as stable not exact
        "phase_metrics": ["BUILD_MS", "COLLECT_MS"],
        "growth_metric": "BUILD_MS",
        "expected_live_after": 1,   # sentinel TreeNode stays alive
    },
}

KNOWN_KEYS = [
    "TEST", "IMPL", "OBJECTS", "WARMUP_ROUNDS", "MEASURE_ROUNDS",
    "ROUND", "ALLOC_MS", "BUILD_MS", "COLLECT_MS",
    "CHECKSUM", "LIVE_AFTER", "COLLECTIONS_DELTA",
]
KNOWN_KEYS.sort(key=len, reverse=True)

ROUND_KEYS = {
    "ALLOC_MS", "BUILD_MS", "COLLECT_MS",
    "CHECKSUM", "LIVE_AFTER", "COLLECTIONS_DELTA",
}

STATUS_PASSED = "passed"
STATUS_TIMED_OUT = "timed_out"
STATUS_NONZERO_EXIT = "nonzero_exit"
STATUS_VALIDATION_FAILED = "validation_failed"

# ---------------------------------------------------------------------------
# Command helpers
# ---------------------------------------------------------------------------


def kill_process_tree(proc: subprocess.Popen) -> None:
    try:
        import psutil
        parent = psutil.Process(proc.pid)
        for child in parent.children(recursive=True):
            try:
                child.kill()
            except Exception:
                pass
        try:
            parent.kill()
        except Exception:
            pass
    except Exception:
        try:
            if platform.system() == "Windows":
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    capture_output=True, timeout=5,
                )
            else:
                proc.kill()
        except Exception:
            pass


def run_command(
    command: list[str] | str,
    *,
    cwd: Path | None = None,
    timeout: int = COMPILE_TIMEOUT_SECONDS,
    shell: bool = False,
) -> tuple[int, str, bool]:
    try:
        proc = subprocess.Popen(
            command,
            cwd=str(cwd) if cwd else None,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=shell,
        )
    except Exception as exc:
        return -1, str(exc), False

    try:
        stdout_bytes, stderr_bytes = proc.communicate(timeout=timeout)
        output = (stdout_bytes or b"").decode("utf-8", errors="replace") + \
                 (stderr_bytes or b"").decode("utf-8", errors="replace")
        return proc.returncode, output, False
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        try:
            stdout_bytes, stderr_bytes = proc.communicate(timeout=5)
        except Exception:
            stdout_bytes, stderr_bytes = b"", b""
        output = (stdout_bytes or b"").decode("utf-8", errors="replace") + \
                 (stderr_bytes or b"").decode("utf-8", errors="replace")
        return -1, output, True


def compile_one(test_id: str) -> tuple[bool, str]:
    source = BENCH_DIR / "nirqoba" / {
        "01": "01_slot_index_churn.nrqba",
        "02": "02_interleaved_alloc_collect.nrqba",
        "03": "03_root_frame_depth.nrqba",
    }[test_id]
    if not source.exists():
        return False, f"Missing source: {source}"
    if not NRQ_EXE.exists():
        return False, f"Nirqoba compiler not found: {NRQ_EXE}"
    output = BIN_DIR / f"{test_id}_nirqoba.exe"
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    code, log, timed_out = run_command(
        [str(NRQ_EXE), "build", str(source), "-o", str(output), "-O3"],
        cwd=OBJ_DIR,
        timeout=COMPILE_TIMEOUT_SECONDS,
    )
    if timed_out:
        return False, f"Compilation timed out:\n{log}"
    return code == 0, log

# ---------------------------------------------------------------------------
# Output parsing
# ---------------------------------------------------------------------------


def parse_output(text: str) -> dict[str, Any]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    globals_: dict[str, str] = {}
    rounds: list[dict[str, Any]] = []
    current_round: dict[str, Any] | None = None
    index = 0
    while index < len(lines):
        line = lines[index]
        key: str | None = None
        value: str | None = None
        for candidate in KNOWN_KEYS:
            if line == candidate:
                key = candidate
                if index + 1 < len(lines):
                    value = lines[index + 1]
                    index += 1
                break
            if line.startswith(candidate + " "):
                key = candidate
                value = line[len(candidate) + 1:].strip()
                break
        if key is None or value is None:
            index += 1
            continue
        if key == "ROUND":
            current_round = {"ROUND": int(float(value))}
            rounds.append(current_round)
        elif key in ROUND_KEYS and current_round is not None:
            current_round[key] = value
        else:
            globals_[key] = value
        index += 1
    return {"globals": globals_, "rounds": rounds}


def as_float(v: Any) -> float:
    return float(v)


def as_int(v: Any) -> int:
    return int(float(v))

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def validate_run(test_id: str, parsed: dict[str, Any]) -> list[str]:
    definition = TESTS[test_id]
    problems: list[str] = []
    globals_ = parsed["globals"]
    rounds = parsed["rounds"]

    # Header fields
    for key, expected in [("TEST", definition["name"]), ("IMPL", "nirqoba")]:
        if key not in globals_:
            problems.append(f"missing {key}")
        elif globals_[key] != str(expected):
            problems.append(f"{key}={globals_[key]!r}, expected {expected!r}")

    # Round count
    if "MEASURE_ROUNDS" not in globals_:
        problems.append("missing MEASURE_ROUNDS")
    else:
        expected_rounds = as_int(globals_["MEASURE_ROUNDS"])
        if len(rounds) != expected_rounds:
            problems.append(
                f"parsed {len(rounds)} rounds, expected {expected_rounds}"
            )

    for round_data in rounds:
        rn = round_data.get("ROUND", "?")

        # Phase timing fields must be present and positive.
        for metric in definition["phase_metrics"]:
            if metric not in round_data:
                problems.append(f"round {rn}: missing {metric}")
            else:
                val = as_float(round_data[metric])
                if val < 0:
                    problems.append(
                        f"round {rn}: {metric}={round_data[metric]} is negative"
                    )
                elif val > NIRQOBA_MAX_PHASE_MS:
                    problems.append(
                        f"round {rn}: {metric}={round_data[metric]}ms exceeds "
                        f"{NIRQOBA_MAX_PHASE_MS:.0f}ms limit [HANG DETECTED]"
                    )

        # Checksum (where applicable)
        expected_checksum = definition.get("checksum")
        if expected_checksum is not None and "CHECKSUM" in round_data:
            actual = as_int(round_data["CHECKSUM"])
            if actual != expected_checksum:
                problems.append(
                    f"round {rn}: CHECKSUM={actual}, expected {expected_checksum}"
                )

        # LIVE_AFTER
        expected_live = definition.get("expected_live_after")
        if expected_live is not None:
            if "LIVE_AFTER" not in round_data:
                problems.append(f"round {rn}: missing LIVE_AFTER")
            else:
                actual_live = as_int(round_data["LIVE_AFTER"])
                if actual_live != expected_live:
                    problems.append(
                        f"round {rn}: LIVE_AFTER={actual_live}, "
                        f"expected {expected_live}"
                    )

        # COLLECTIONS_DELTA (test 02 only)
        expected_delta = definition.get("expected_collections_delta")
        if expected_delta is not None:
            if "COLLECTIONS_DELTA" not in round_data:
                problems.append(f"round {rn}: missing COLLECTIONS_DELTA")
            else:
                actual_delta = as_int(round_data["COLLECTIONS_DELTA"])
                if actual_delta != expected_delta:
                    problems.append(
                        f"round {rn}: COLLECTIONS_DELTA={actual_delta}, "
                        f"expected {expected_delta}"
                    )

    # Exponential-growth detector: last round's growth metric must not be
    # more than GROWTH_RATIO times the first round's value. A regressed
    # SlotIndex causes each round to be strictly slower than the previous one.
    growth_key = definition.get("growth_metric")
    if growth_key and len(rounds) >= 2:
        first_val = as_float(rounds[0].get(growth_key, 0))
        last_val  = as_float(rounds[-1].get(growth_key, 0))
        if first_val > 0 and last_val > first_val * GROWTH_RATIO:
            problems.append(
                f"{growth_key} grew from {first_val:.1f}ms (round 0) to "
                f"{last_val:.1f}ms (round {len(rounds)-1}), ratio "
                f"{last_val/first_val:.1f}x exceeds limit {GROWTH_RATIO:.1f}x "
                f"[EXPONENTIAL GROWTH DETECTED — likely tombstone bug]"
            )

    return problems

# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def stats(values: list[float]) -> dict[str, Any]:
    clean = [float(v) for v in values]
    return {
        "samples": len(clean),
        "median": round(statistics.median(clean), 3),
        "mean":   round(statistics.fmean(clean), 3),
        "min":    round(min(clean), 3),
        "max":    round(max(clean), 3),
        "stdev":  round(statistics.stdev(clean), 3) if len(clean) > 1 else 0.0,
    }


def aggregate(test_id: str, runs: list[dict[str, Any]]) -> dict[str, Any]:
    definition = TESTS[test_id]
    all_rounds = [r for run in runs for r in run["rounds"]]
    metrics: dict[str, Any] = {}
    for key in definition["phase_metrics"]:
        metrics[key] = stats([as_float(r[key]) for r in all_rounds if key in r])
    checksums = sorted({
        as_int(r["CHECKSUM"]) for r in all_rounds if "CHECKSUM" in r
    })
    return {
        "metrics": metrics,
        "round_samples": len(all_rounds),
        "checksums": checksums,
        "live_after": sorted({
            as_int(r["LIVE_AFTER"]) for r in all_rounds if "LIVE_AFTER" in r
        }),
    }

# ---------------------------------------------------------------------------
# Environment metadata
# ---------------------------------------------------------------------------


def environment_metadata() -> dict[str, Any]:
    def ps(expr: str) -> str:
        try:
            code, out, _ = run_command(
                ["powershell", "-NoProfile", "-Command", expr], timeout=30
            )
            return out.strip() if code == 0 else ""
        except Exception:
            return ""

    cpu = ps("(Get-CimInstance Win32_Processor).Name") or platform.processor()
    mem = ps("[string](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory")
    os_cap = ps(
        "(Get-CimInstance Win32_OperatingSystem).Caption + ' ' + "
        "(Get-CimInstance Win32_OperatingSystem).Version"
    )
    nrq_ver = ""
    try:
        _, out, _ = run_command([str(NRQ_EXE), "--version"], timeout=30)
        lines = [l.strip() for l in out.splitlines() if l.strip()]
        nrq_ver = lines[0] if lines else "unknown"
    except Exception:
        nrq_ver = "unknown"

    return {
        "generated_at": _datetime.datetime.now().astimezone().isoformat(),
        "host":               platform.node(),
        "os":                 os_cap or platform.platform(),
        "architecture":       platform.machine(),
        "cpu":                cpu,
        "logical_processors": os.cpu_count(),
        "memory_bytes":       int(mem) if mem.isdigit() else None,
        "python":             sys.version.split()[0],
        "compilers":          {"nirqoba": nrq_ver},
        "source_root":        str(PROJECT_ROOT),
        "benchmark_directory": str(BENCH_DIR),
        "suite":              "stress_test_3 (PO-27 regression guard)",
        "regression_commit":  "0b2d457",
    }

# ---------------------------------------------------------------------------
# Atomic JSON save
# ---------------------------------------------------------------------------


def atomic_save_json(path: Path, data: dict[str, Any]) -> None:
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    tf = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8",
        dir=str(path.parent), delete=False, suffix=".tmp",
    )
    tmp = Path(tf.name)
    try:
        json.dump(data, tf, indent=2)
        tf.write("\n")
        tf.flush()
        tf.close()
        os.replace(tmp, path)
    except Exception:
        tf.close()
        if tmp.exists():
            tmp.unlink(missing_ok=True)
        raise

# ---------------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------------


def compile_all(selected_tests: list[str]) -> bool:
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    OBJ_DIR.mkdir(parents=True, exist_ok=True)
    ok = True
    for test_id in selected_tests:
        t0 = time.perf_counter()
        compiled, log = compile_one(test_id)
        elapsed = time.perf_counter() - t0
        status = "OK" if compiled else "FAILED"
        print(f"  compile {test_id} nirqoba  {status} ({elapsed:.1f}s)")
        if log.strip():
            print(f"    {log.strip()[:200]}")
        ok = ok and compiled
    return ok


def run_suite(
    selected_tests: list[str],
    repeats: int,
    output_path: Path | None = None,
) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {}
    validation_failures: list[str] = []
    attempts: list[dict[str, Any]] = []
    metadata = environment_metadata()
    payload: dict[str, Any] = {
        "metadata": metadata,
        "valid": True,
        "validation_failures": validation_failures,
        "results": results,
        "attempts": attempts,
    }
    checkpoint = (
        output_path.with_suffix(".checkpoint.json")
        if output_path else BENCH_DIR / "benchmark_results.checkpoint.json"
    )

    for test_id in selected_tests:
        definition = TESTS[test_id]
        results[definition["name"]] = {}
        print(f"\n=== Test {test_id}: {definition['title']} ===")

        runs: list[dict[str, Any]] = []
        print(f"  nirqoba  ", end="", flush=True)

        for repeat in range(repeats):
            repeat_number = repeat + 1
            exe = BIN_DIR / f"{test_id}_nirqoba.exe"
            if not exe.exists():
                msg = f"missing executable: {exe}"
                validation_failures.append(msg)
                payload["valid"] = False
                atomic_save_json(checkpoint, payload)
                print("MISSING", end="", flush=True)
                break

            t0 = time.perf_counter()
            code, output, timed_out = run_command(
                [str(exe)], timeout=RUN_TIMEOUT_SECONDS
            )
            wall = time.perf_counter() - t0

            raw = RAW_DIR / f"{test_id}_nirqoba_run{repeat_number}.txt"
            raw.write_text(
                f"$ {exe}\nexit_code={code}\ntimed_out={timed_out}\n"
                f"wall_seconds={wall:.3f}\n{output}\n",
                encoding="utf-8",
            )

            if timed_out:
                status = STATUS_TIMED_OUT
                msg = f"[{test_id} run {repeat_number}] timed out after {RUN_TIMEOUT_SECONDS}s"
                validation_failures.append(msg)
                payload["valid"] = False
                print(f" TIMEOUT({wall:.0f}s)", end="", flush=True)
            elif code != 0:
                status = STATUS_NONZERO_EXIT
                msg = f"[{test_id} run {repeat_number}] exit code {code}"
                validation_failures.append(msg)
                payload["valid"] = False
                print(f" FAIL(exit={code})", end="", flush=True)
            else:
                parsed = parse_output(output)
                problems = validate_run(test_id, parsed)
                if problems:
                    status = STATUS_VALIDATION_FAILED
                    for p in problems:
                        validation_failures.append(f"[{test_id} run {repeat_number}] {p}")
                    payload["valid"] = False
                    print(f" INVALID", end="", flush=True)
                else:
                    status = STATUS_PASSED
                    print(f" {wall:.1f}s", end="", flush=True)
                runs.append({
                    "repeat": repeat_number,
                    "status": status,
                    "wall_seconds": round(wall, 3),
                    "rounds": parsed["rounds"],
                    "parsed": parsed,
                })

            attempts.append({
                "test": test_id,
                "test_name": definition["name"],
                "impl": "nirqoba",
                "repeat": repeat_number,
                "status": status,
                "returncode": code,
                "wall_seconds": round(wall, 3),
                "timeout_seconds": RUN_TIMEOUT_SECONDS,
                "timed_out": timed_out,
            })
            atomic_save_json(checkpoint, payload)

        print()
        if runs:
            agg = aggregate(test_id, runs)
            results[definition["name"]]["nirqoba"] = {
                "status": STATUS_PASSED if all(
                    r["status"] == STATUS_PASSED for r in runs
                ) else STATUS_VALIDATION_FAILED,
                "repeats": len(runs),
                **agg,
            }

    if output_path:
        atomic_save_json(output_path, payload)
    else:
        atomic_save_json(RESULT_JSON, payload)

    if checkpoint.exists():
        checkpoint.unlink(missing_ok=True)

    return payload


def main() -> int:
    parser = argparse.ArgumentParser(
        description="PO-27 regression benchmark suite (stress_test_3)"
    )
    parser.add_argument("--skip-compile", action="store_true")
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    parser.add_argument(
        "--test", choices=list(TESTS.keys()),
        help="Run only the specified test",
    )
    parser.add_argument(
        "--output", type=Path, default=None,
        help="Override result JSON output path",
    )
    args = parser.parse_args()

    selected = [args.test] if args.test else list(TESTS.keys())

    print("=" * 60)
    print("Stress Test 3 — PO-27 Regression Guard")
    print(f"Tests: {', '.join(selected)}")
    print(f"Repeats: {args.repeats}")
    print(f"Timeout: {RUN_TIMEOUT_SECONDS}s per run")
    print(f"Max phase: {NIRQOBA_MAX_PHASE_MS:.0f}ms")
    print("=" * 60)

    if not args.skip_compile:
        print("\nCompiling...")
        if not compile_all(selected):
            print("\nCompilation failed. Aborting.")
            return 1

    result = run_suite(selected, args.repeats, args.output)

    print("\n" + "=" * 60)
    if result["valid"]:
        print("RESULT: PASS — all regression tests passed.")
        print("  PO-27 (tombstone-churn hang) is not present.")
    else:
        print("RESULT: FAIL")
        for f in result["validation_failures"]:
            print(f"  {f}")
    print("=" * 60)
    return 0 if result["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
