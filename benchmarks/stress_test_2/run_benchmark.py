#!/usr/bin/env python3
"""Compile and run the Stress Test 2 GC benchmark suite.

The suite compares Nirqoba with C, C++, Rust, and Java on three themes:

    01  managed-object allocation
    02  mark and sweep
    03  cyclic graph reclamation

Every track measures the same phases inside the program under test, so process
startup, JVM start-up, and shell overhead are not part of any reported number.
Each process is repeated (default: three times). The runner collects every
measured round from every repeat and reports median, min, max, standard
deviation, and sample count.

Outputs:
    benchmark_results.json
    results/raw/<test>_<impl>_run<N>.txt

Usage:
    python run_benchmark.py
    python run_benchmark.py --skip-compile
    python run_benchmark.py --repeats 5
    python run_benchmark.py --test 02 --impl nirqoba
"""

from __future__ import annotations

import argparse
import datetime as _datetime
import json
import os
import platform
import shutil
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
JAVA_CLASSES_DIR = BIN_DIR / "java_classes"
RESULT_JSON = BENCH_DIR / "benchmark_results.json"

NRQ_EXE = PROJECT_ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"
RUSTC_EXE = Path(r"C:\Users\USER\.cargo\bin\rustc.exe")
JDK_BIN = Path(r"C:\Users\USER\.gradle\jdks\eclipse_adoptium-25-amd64-windows.2\bin")
JAVAC_EXE = JDK_BIN / "javac.exe"
JAVA_EXE = JDK_BIN / "java.exe"
VCVARS_BAT = Path(
    r"C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

DEFAULT_REPEATS = 3
RUN_TIMEOUT_SECONDS = 600
COMPILE_TIMEOUT_SECONDS = 300
NIRQOBA_MAX_PHASE_MS = 7000.0

# ---------------------------------------------------------------------------
# Suite definition
# ---------------------------------------------------------------------------

TESTS: dict[str, dict[str, Any]] = {
    "01": {
        "name": "managed_object_allocation",
        "title": "Managed-object allocation",
        "objects": 1_000_000,
        "checksum": 499_999_500_000,
        "sources": {
            "nirqoba": BENCH_DIR / "nirqoba" / "01_managed_object_allocation.nrqba",
            "c": BENCH_DIR / "c" / "01_managed_object_allocation.c",
            "cpp": BENCH_DIR / "cpp" / "01_managed_object_allocation.cpp",
            "rust": BENCH_DIR / "rust" / "01_managed_object_allocation.rs",
            "java": BENCH_DIR / "java" / "Benchmark01Allocation.java",
        },
        "java_class": "Benchmark01Allocation",
        "phase_metrics": ["ALLOC_MS", "TRAVERSE_MS", "COLLECT_MS"],
    },
    "02": {
        "name": "mark_and_sweep",
        "title": "Mark and sweep",
        "objects": 2_097_151,
        "checksum": 2_199_022_206_976,
        "sources": {
            "nirqoba": BENCH_DIR / "nirqoba" / "02_mark_and_sweep.nrqba",
            "c": BENCH_DIR / "c" / "02_mark_and_sweep.c",
            "cpp": BENCH_DIR / "cpp" / "02_mark_and_sweep.cpp",
            "rust": BENCH_DIR / "rust" / "02_mark_and_sweep.rs",
            "java": BENCH_DIR / "java" / "Benchmark02MarkSweep.java",
        },
        "java_class": "Benchmark02MarkSweep",
        "global_metrics": ["BUILD_MS", "TRAVERSE_MS"],
        "phase_metrics": ["COLLECT_MS"],
    },
    "03": {
        "name": "cyclic_graph_reclamation",
        "title": "Cyclic graph reclamation",
        "objects": 1_000_000,
        "checksum": 499_999_500_000,
        "sources": {
            "nirqoba": BENCH_DIR / "nirqoba" / "03_cyclic_graph_reclamation.nrqba",
            "c": BENCH_DIR / "c" / "03_cyclic_graph_reclamation.c",
            "cpp": BENCH_DIR / "cpp" / "03_cyclic_graph_reclamation.cpp",
            "rust": BENCH_DIR / "rust" / "03_cyclic_graph_reclamation.rs",
            "java": BENCH_DIR / "java" / "Benchmark03Cycles.java",
        },
        "java_class": "Benchmark03Cycles",
        "phase_metrics": ["BUILD_MS", "COLLECT_MS"],
    },
}

IMPLS = ["nirqoba", "c", "cpp", "rust", "java"]

# Labels emitted by the programs. The runner understands both "KEY value" on a
# single line (C, C++, Rust, Java) and "KEY" followed by the value on the next
# line (Nirqoba, whose print statement takes one value at a time).
KNOWN_KEYS = [
    "TEST",
    "IMPL",
    "OBJECTS",
    "WARMUP_ROUNDS",
    "WARMUP_BATCHES",
    "COLLECT_BATCH",
    "MEASURE_ROUNDS",
    "ROUND",
    "ALLOC_MS",
    "TRAVERSE_MS",
    "COLLECT_MS",
    "BUILD_MS",
    "CHECKSUM",
    "FINAL_CHECKSUM",
    "LIVE_AFTER",
    "COLLECTIONS_DELTA",
    "CYCLES_CLEARED",
    "ROOT_VALUE",
]
KNOWN_KEYS.sort(key=len, reverse=True)

ROUND_KEYS = {
    "ALLOC_MS",
    "TRAVERSE_MS",
    "COLLECT_MS",
    "BUILD_MS",
    "CHECKSUM",
    "LIVE_AFTER",
    "COLLECTIONS_DELTA",
    "CYCLES_CLEARED",
}
GLOBAL_KEYS = {
    "TEST",
    "IMPL",
    "OBJECTS",
    "WARMUP_ROUNDS",
    "WARMUP_BATCHES",
    "COLLECT_BATCH",
    "MEASURE_ROUNDS",
    "BUILD_MS",
    "TRAVERSE_MS",
    "CHECKSUM",
    "FINAL_CHECKSUM",
    "ROOT_VALUE",
}

STATUS_PASSED = "passed"
STATUS_TIMED_OUT = "timed_out"
STATUS_NONZERO_EXIT = "nonzero_exit"
STATUS_VALIDATION_FAILED = "validation_failed"


# ---------------------------------------------------------------------------
# Command helpers
# ---------------------------------------------------------------------------


def kill_process_tree(proc: subprocess.Popen) -> None:
    """Safely terminate a process and all its children across platforms."""
    try:
        import psutil

        parent = psutil.Process(proc.pid)
        children = parent.children(recursive=True)
        for child in children:
            try:
                child.kill()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        try:
            parent.kill()
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    except Exception:
        # Fallback if psutil fails or on Windows taskkill fallback
        try:
            if platform.system() == "Windows":
                subprocess.run(
                    ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                    capture_output=True,
                    timeout=5,
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
    """Run a command and return (returncode, combined output, timed_out)."""
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
        output = (stdout_bytes or b"").decode("utf-8", errors="replace") + (
            stderr_bytes or b""
        ).decode("utf-8", errors="replace")
        return proc.returncode, output, False
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        try:
            stdout_bytes, stderr_bytes = proc.communicate(timeout=5)
        except Exception:
            stdout_bytes, stderr_bytes = b"", b""
        output = (stdout_bytes or b"").decode("utf-8", errors="replace") + (
            stderr_bytes or b""
        ).decode("utf-8", errors="replace")
        return -1, output, True


def msvc_command(source: Path, output: Path, object_file: Path, cpp: bool) -> str:
    """Build a cl.exe command line that runs inside a vcvars64 environment."""
    std = "/std:c++20 /EHsc" if cpp else ""
    return (
        f'call "{VCVARS_BAT}" >nul 2>&1 && '
        f'cl /O2 /nologo {std} "{source}" '
        f'/Fo:"{object_file}" /Fe:"{output}" /link /SUBSYSTEM:CONSOLE'
    )


def compile_one(test_id: str, impl: str) -> tuple[bool, str]:
    """Compile one test for one implementation. Returns (ok, log)."""
    definition = TESTS[test_id]
    source = definition["sources"][impl]
    if not source.exists():
        return False, f"Missing source: {source}"

    if impl == "nirqoba":
        if not NRQ_EXE.exists():
            return False, f"Nirqoba compiler not found: {NRQ_EXE}"
        output = BIN_DIR / f"{test_id}_nirqoba.exe"
        # The Nirqoba compiler emits its intermediate .obj in the working
        # directory, so build from bin/obj rather than the suite root.
        code, log, timed_out = run_command(
            [str(NRQ_EXE), "build", str(source), "-o", str(output), "-O3"],
            cwd=OBJ_DIR,
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        if timed_out:
            return False, f"Nirqoba compilation timed out after {COMPILE_TIMEOUT_SECONDS}s:\n{log}"
        return code == 0, log

    if impl == "c":
        output = BIN_DIR / f"{test_id}_c.exe"
        obj = OBJ_DIR / f"{test_id}_c.obj"
        code, log, timed_out = run_command(
            msvc_command(source, output, obj, cpp=False),
            shell=True,
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        if timed_out:
            return False, f"C compilation timed out after {COMPILE_TIMEOUT_SECONDS}s:\n{log}"
        return code == 0, log

    if impl == "cpp":
        output = BIN_DIR / f"{test_id}_cpp.exe"
        obj = OBJ_DIR / f"{test_id}_cpp.obj"
        code, log, timed_out = run_command(
            msvc_command(source, output, obj, cpp=True),
            shell=True,
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        if timed_out:
            return False, f"C++ compilation timed out after {COMPILE_TIMEOUT_SECONDS}s:\n{log}"
        return code == 0, log

    if impl == "rust":
        if not RUSTC_EXE.exists():
            return False, f"rustc not found: {RUSTC_EXE}"
        output = BIN_DIR / f"{test_id}_rust.exe"
        code, log, timed_out = run_command(
            [str(RUSTC_EXE), "-C", "opt-level=3", str(source), "-o", str(output)],
            timeout=COMPILE_TIMEOUT_SECONDS,
        )
        if timed_out:
            return False, f"Rust compilation timed out after {COMPILE_TIMEOUT_SECONDS}s:\n{log}"
        return code == 0, log

    if impl == "java":
        return False, "Java sources are compiled together by compile_java()"

    return False, f"Unknown implementation: {impl}"


def compile_java() -> tuple[bool, str]:
    """Compile all Java benchmarks into one class directory."""
    if not JAVAC_EXE.exists():
        return False, f"javac not found: {JAVAC_EXE}"
    if JAVA_CLASSES_DIR.exists():
        shutil.rmtree(JAVA_CLASSES_DIR)
    JAVA_CLASSES_DIR.mkdir(parents=True, exist_ok=True)
    sources = [str(TESTS[test_id]["sources"]["java"]) for test_id in TESTS]
    code, log, timed_out = run_command(
        [str(JAVAC_EXE), "-d", str(JAVA_CLASSES_DIR), *sources],
        timeout=COMPILE_TIMEOUT_SECONDS,
    )
    if timed_out:
        return False, f"Java compilation timed out after {COMPILE_TIMEOUT_SECONDS}s:\n{log}"
    return code == 0, log


def executable_for(test_id: str, impl: str) -> list[str]:
    """Return the argv used to launch one benchmark process."""
    if impl == "java":
        return [
            str(JAVA_EXE),
            "-Xms2g",
            "-Xmx2g",
            "-XX:+UseSerialGC",
            "-cp",
            str(JAVA_CLASSES_DIR),
            TESTS[test_id]["java_class"],
        ]
    return [str(BIN_DIR / f"{test_id}_{impl}.exe")]


# ---------------------------------------------------------------------------
# Output parsing and validation
# ---------------------------------------------------------------------------


def parse_output(text: str) -> dict[str, Any]:
    """Parse the label/value stream emitted by any track."""
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
                value = line[len(candidate) + 1 :].strip()
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


def as_float(value: Any) -> float:
    return float(value)


def as_int(value: Any) -> int:
    return int(float(value))


def validate_run(test_id: str, impl: str, parsed: dict[str, Any]) -> list[str]:
    """Return a list of human-readable validation failures."""
    definition = TESTS[test_id]
    problems: list[str] = []
    globals_ = parsed["globals"]
    rounds = parsed["rounds"]

    def require(key: str, expected: Any) -> None:
        if key not in globals_:
            problems.append(f"missing {key}")
            return
        actual = globals_[key]
        if str(actual) != str(expected):
            problems.append(f"{key}={actual!r}, expected {expected!r}")

    require("TEST", definition["name"])
    require("IMPL", impl)
    require("OBJECTS", definition["objects"])

    if "MEASURE_ROUNDS" in globals_:
        expected_rounds = as_int(globals_["MEASURE_ROUNDS"])
        if len(rounds) != expected_rounds:
            problems.append(
                f"parsed {len(rounds)} rounds, expected {expected_rounds}"
            )
    else:
        problems.append("missing MEASURE_ROUNDS")

    expected_checksum = definition["checksum"]
    if "CHECKSUM" in globals_ and as_int(globals_["CHECKSUM"]) != expected_checksum:
        problems.append(
            f"global CHECKSUM={globals_['CHECKSUM']}, expected {expected_checksum}"
        )
    for round_data in rounds:
        round_number = round_data.get("ROUND", "?")
        # Test 02 checksums the tree once, before the collection batches.
        if test_id in {"01", "03"}:
            if "CHECKSUM" not in round_data:
                problems.append(f"round {round_number}: missing CHECKSUM")
            elif as_int(round_data["CHECKSUM"]) != expected_checksum:
                problems.append(
                    f"round {round_number}: CHECKSUM={round_data['CHECKSUM']}, "
                    f"expected {expected_checksum}"
                )
        for metric in definition["phase_metrics"]:
            if metric not in round_data:
                problems.append(f"round {round_number}: missing {metric}")
            else:
                metric_val = as_float(round_data[metric])
                if metric_val <= 0.0:
                    problems.append(
                        f"round {round_number}: {metric}={round_data[metric]} is not positive"
                    )
                elif impl == "nirqoba" and metric_val > NIRQOBA_MAX_PHASE_MS:
                    problems.append(
                        f"round {round_number}: {metric}={round_data[metric]}ms exceeds {NIRQOBA_MAX_PHASE_MS:.0f}ms limit for nirqoba [FAIL]"
                    )

    if test_id in {"01", "02"}:
        for metric in definition.get("global_metrics", []):
            if metric not in globals_:
                problems.append(f"missing {metric}")
            elif impl == "nirqoba" and as_float(globals_[metric]) > NIRQOBA_MAX_PHASE_MS:
                problems.append(
                    f"global {metric}={globals_[metric]}ms exceeds {NIRQOBA_MAX_PHASE_MS:.0f}ms limit for nirqoba [FAIL]"
                )

    for round_data in rounds:
        round_number = round_data.get("ROUND", "?")
        if test_id == "01":
            expected_live = -1 if impl == "java" else 0
            if "LIVE_AFTER" not in round_data:
                problems.append(f"round {round_number}: missing LIVE_AFTER")
            elif as_int(round_data["LIVE_AFTER"]) != expected_live:
                problems.append(
                    f"round {round_number}: LIVE_AFTER={round_data['LIVE_AFTER']}, "
                    f"expected {expected_live}"
                )
        elif test_id == "02":
            if "LIVE_AFTER" not in round_data:
                problems.append(f"round {round_number}: missing LIVE_AFTER")
            elif as_int(round_data["LIVE_AFTER"]) != definition["objects"]:
                problems.append(
                    f"round {round_number}: LIVE_AFTER={round_data['LIVE_AFTER']}, "
                    f"expected {definition['objects']}"
                )
            if "COLLECTIONS_DELTA" not in round_data:
                problems.append(f"round {round_number}: missing COLLECTIONS_DELTA")
            else:
                expected_delta = as_int(globals_.get("COLLECT_BATCH", "5"))
                actual_delta = as_int(round_data["COLLECTIONS_DELTA"])
                if actual_delta != expected_delta:
                    problems.append(
                        f"round {round_number}: COLLECTIONS_DELTA={actual_delta}, "
                        f"expected {expected_delta}"
                    )
        elif test_id == "03":
            if "LIVE_AFTER" not in round_data:
                problems.append(f"round {round_number}: missing LIVE_AFTER")
            elif as_int(round_data["LIVE_AFTER"]) != 0:
                problems.append(
                    f"round {round_number}: LIVE_AFTER={round_data['LIVE_AFTER']}, "
                    "expected 0"
                )
            # C, C++, and Rust report LIVE_AFTER=0 directly. Java and Nirqoba
            # additionally report the number of reclaimed pairs.
            if "CYCLES_CLEARED" in round_data:
                expected_pairs = definition["objects"] // 2
                if as_int(round_data["CYCLES_CLEARED"]) != expected_pairs:
                    problems.append(
                        f"round {round_number}: "
                        f"CYCLES_CLEARED={round_data['CYCLES_CLEARED']}, "
                        f"expected {expected_pairs}"
                    )

    return problems


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def stats(values: list[float]) -> dict[str, Any]:
    """Summarise a sample list without hiding any of the samples."""
    clean = [float(value) for value in values]
    return {
        "samples": len(clean),
        "median": round(statistics.median(clean), 3),
        "mean": round(statistics.fmean(clean), 3),
        "min": round(min(clean), 3),
        "max": round(max(clean), 3),
        "stdev": round(statistics.stdev(clean), 3) if len(clean) > 1 else 0.0,
    }


def aggregate(test_id: str, runs: list[dict[str, Any]]) -> dict[str, Any]:
    """Collect per-phase samples across every measured round and repeat."""
    definition = TESTS[test_id]
    all_rounds = [round_data for run in runs for round_data in run["rounds"]]
    metrics: dict[str, Any] = {}

    for key in definition["phase_metrics"]:
        metrics[key] = stats([as_float(item[key]) for item in all_rounds])

    for key in definition.get("global_metrics", []):
        metrics[key] = stats(
            [as_float(run["parsed"]["globals"][key]) for run in runs]
        )

    if all_rounds and "CHECKSUM" in all_rounds[0]:
        checksums = sorted({as_int(item["CHECKSUM"]) for item in all_rounds})
    else:
        checksums = sorted(
            {
                as_int(run["parsed"]["globals"]["CHECKSUM"])
                for run in runs
                if "CHECKSUM" in run["parsed"]["globals"]
            }
        )

    return {
        "metrics": metrics,
        "round_samples": len(all_rounds),
        "checksums": checksums,
        "live_after": sorted(
            {as_int(item["LIVE_AFTER"]) for item in all_rounds}
            if all_rounds and "LIVE_AFTER" in all_rounds[0]
            else set()
        ),
    }


# ---------------------------------------------------------------------------
# Environment metadata
# ---------------------------------------------------------------------------


def environment_metadata() -> dict[str, Any]:
    """Capture enough context for the results to be reproducible."""

    def powershell(expression: str) -> str:
        try:
            code, output = run_command(
                ["powershell", "-NoProfile", "-Command", expression], timeout=30
            )
            return output.strip() if code == 0 else ""
        except Exception:
            return ""

    cpu = powershell("(Get-CimInstance Win32_Processor).Name") or platform.processor()
    memory_bytes = powershell(
        "[string](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"
    )
    os_caption = powershell(
        "(Get-CimInstance Win32_OperatingSystem).Caption + ' ' + "
        "(Get-CimInstance Win32_OperatingSystem).Version"
    )

    compilers: dict[str, str] = {}
    try:
        _, output = run_command([str(NRQ_EXE), "--version"], timeout=30)
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        compilers["nirqoba"] = lines[0] if lines else "unknown"
    except Exception:
        compilers["nirqoba"] = "unknown"
    try:
        _, output = run_command(
            f'call "{VCVARS_BAT}" >nul 2>&1 && cl 2>&1', shell=True, timeout=60
        )
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        compilers["msvc"] = lines[0] if lines else "unknown"
    except Exception:
        compilers["msvc"] = "unknown"
    try:
        _, output = run_command([str(RUSTC_EXE), "-Vv"], timeout=30)
        compilers["rustc"] = output.strip().splitlines()[0]
    except Exception:
        compilers["rustc"] = "unknown"
    try:
        _, output = run_command([str(JAVA_EXE), "-version"], timeout=30)
        compilers["java"] = output.strip().splitlines()[0]
    except Exception:
        compilers["java"] = "unknown"

    return {
        "generated_at": _datetime.datetime.now().astimezone().isoformat(),
        "host": platform.node(),
        "os": os_caption or platform.platform(),
        "architecture": platform.machine(),
        "cpu": cpu,
        "logical_processors": os.cpu_count(),
        "memory_bytes": int(memory_bytes) if memory_bytes.isdigit() else None,
        "python": sys.version.split()[0],
        "compilers": compilers,
        "source_root": str(PROJECT_ROOT),
        "benchmark_directory": str(BENCH_DIR),
    }


# ---------------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------------


def compile_all(selected_tests: list[str], selected_impls: list[str]) -> bool:
    """Compile every selected track. Returns False if any compile fails."""
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    OBJ_DIR.mkdir(parents=True, exist_ok=True)

    ok = True
    logs: list[str] = []

    if "java" in selected_impls:
        java_ok, java_log = compile_java()
        logs.append(f"[java] {'OK' if java_ok else 'FAILED'}\n{java_log}")
        ok = ok and java_ok

    for test_id in selected_tests:
        for impl in selected_impls:
            if impl == "java":
                continue
            started = time.perf_counter()
            compiled, log = compile_one(test_id, impl)
            elapsed = time.perf_counter() - started
            status = "OK" if compiled else "FAILED"
            logs.append(
                f"[{test_id} {impl}] {status} in {elapsed:.2f}s\n{log}"
            )
            ok = ok and compiled
            print(f"  compile {test_id} {impl:<8} {status} ({elapsed:.1f}s)")

    print()
    for log in logs:
        print(log.rstrip())
        print()
    return ok


def atomic_save_json(path: Path, data: dict[str, Any]) -> None:
    """Atomically save JSON data using a temporary file replaced into place."""
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_file = tempfile.NamedTemporaryFile(
        mode="w",
        encoding="utf-8",
        dir=str(path.parent),
        delete=False,
        suffix=".tmp",
    )
    temp_path = Path(temp_file.name)
    try:
        json.dump(data, temp_file, indent=2)
        temp_file.write("\n")
        temp_file.flush()
        temp_file.close()
        os.replace(temp_path, path)
    except Exception:
        temp_file.close()
        if temp_path.exists():
            try:
                temp_path.unlink()
            except Exception:
                pass
        raise


def run_suite(
    selected_tests: list[str],
    selected_impls: list[str],
    repeats: int,
    output_path: Path | None = None,
) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    results: dict[str, Any] = {}
    validation_failures: list[str] = []
    attempts: list[dict[str, Any]] = []

    checkpoint_path = (
        output_path.with_suffix(".checkpoint.json")
        if output_path
        else BENCH_DIR / "benchmark_results.checkpoint.json"
    )

    metadata = environment_metadata()
    current_payload: dict[str, Any] = {
        "metadata": metadata,
        "valid": True,
        "validation_failures": validation_failures,
        "results": results,
        "attempts": attempts,
    }

    for test_id in selected_tests:
        definition = TESTS[test_id]
        results[definition["name"]] = {}
        print(f"\n=== Test {test_id}: {definition['title']} ===")

        for impl in selected_impls:
            runs: list[dict[str, Any]] = []
            print(f"  {impl:<8}", end="", flush=True)

            for repeat in range(repeats):
                repeat_number = repeat + 1
                argv = executable_for(test_id, impl)
                if not Path(argv[0]).exists():
                    message = f"missing executable for {test_id} {impl}: {argv[0]}"
                    validation_failures.append(message)
                    attempts.append(
                        {
                            "test": test_id,
                            "test_name": definition["name"],
                            "impl": impl,
                            "repeat": repeat_number,
                            "command": argv,
                            "status": STATUS_NONZERO_EXIT,
                            "returncode": -1,
                            "wall_seconds": 0.0,
                            "timeout_seconds": RUN_TIMEOUT_SECONDS,
                            "error": message,
                        }
                    )
                    current_payload["valid"] = False
                    atomic_save_json(checkpoint_path, current_payload)
                    print("MISSING", end="")
                    break

                started = time.perf_counter()
                code, output, timed_out = run_command(
                    argv, timeout=RUN_TIMEOUT_SECONDS, shell=False
                )
                wall_seconds = time.perf_counter() - started

                raw_path = RAW_DIR / f"{test_id}_{impl}_run{repeat_number}.txt"
                raw_path.write_text(
                    f"$ {' '.join(argv)}\n"
                    f"exit_code={code}\n"
                    f"timed_out={timed_out}\n"
                    f"wall_seconds={wall_seconds:.3f}\n"
                    f"{output}\n",
                    encoding="utf-8",
                )

                if timed_out:
                    status = STATUS_TIMED_OUT
                    message = (
                        f"{test_id} {impl} run {repeat_number} timed out after "
                        f"{RUN_TIMEOUT_SECONDS}s (wall: {wall_seconds:.3f}s); see {raw_path}"
                    )
                    validation_failures.append(message)
                    attempts.append(
                        {
                            "test": test_id,
                            "test_name": definition["name"],
                            "impl": impl,
                            "repeat": repeat_number,
                            "command": argv,
                            "status": status,
                            "returncode": code,
                            "wall_seconds": round(wall_seconds, 3),
                            "timeout_seconds": RUN_TIMEOUT_SECONDS,
                            "partial_output": output,
                            "raw_log": str(raw_path),
                        }
                    )
                    current_payload["valid"] = False
                    atomic_save_json(checkpoint_path, current_payload)
                    print("TIMEOUT", end="", flush=True)
                    continue

                if code != 0:
                    status = STATUS_NONZERO_EXIT
                    message = (
                        f"{test_id} {impl} run {repeat_number} exited with {code}; "
                        f"see {raw_path}"
                    )
                    validation_failures.append(message)
                    attempts.append(
                        {
                            "test": test_id,
                            "test_name": definition["name"],
                            "impl": impl,
                            "repeat": repeat_number,
                            "command": argv,
                            "status": status,
                            "returncode": code,
                            "wall_seconds": round(wall_seconds, 3),
                            "timeout_seconds": RUN_TIMEOUT_SECONDS,
                            "output": output,
                            "raw_log": str(raw_path),
                        }
                    )
                    current_payload["valid"] = False
                    atomic_save_json(checkpoint_path, current_payload)
                    print("EXIT", end="", flush=True)
                    continue

                parsed = parse_output(output)
                problems = validate_run(test_id, impl, parsed)
                if problems:
                    status = STATUS_VALIDATION_FAILED
                    for problem in problems:
                        validation_failures.append(
                            f"{test_id} {impl} run {repeat_number}: {problem}"
                        )
                else:
                    status = STATUS_PASSED

                attempts.append(
                    {
                        "test": test_id,
                        "test_name": definition["name"],
                        "impl": impl,
                        "repeat": repeat_number,
                        "command": argv,
                        "status": status,
                        "returncode": code,
                        "wall_seconds": round(wall_seconds, 3),
                        "timeout_seconds": RUN_TIMEOUT_SECONDS,
                        "validation_problems": problems,
                        "raw_log": str(raw_path),
                    }
                )

                if status == STATUS_PASSED:
                    runs.append(
                        {
                            "repeat": repeat_number,
                            "wall_seconds": round(wall_seconds, 3),
                            "parsed": parsed,
                            "rounds": parsed["rounds"],
                        }
                    )
                    print(".", end="", flush=True)
                else:
                    current_payload["valid"] = False
                    print("FAIL", end="", flush=True)

                atomic_save_json(checkpoint_path, current_payload)

            print()
            if runs:
                results[definition["name"]][impl] = aggregate(test_id, runs)
                results[definition["name"]][impl]["runs"] = [
                    {
                        "repeat": run["repeat"],
                        "wall_seconds": run["wall_seconds"],
                        "globals": run["parsed"]["globals"],
                        "rounds": run["rounds"],
                    }
                    for run in runs
                ]
                atomic_save_json(checkpoint_path, current_payload)

    current_payload["valid"] = not validation_failures
    return current_payload


def print_summary(payload: dict[str, Any]) -> None:
    """Print a compact median table for quick inspection."""
    for test_name, by_impl in payload["results"].items():
        print(f"\n{test_name}")
        metric_names = []
        for impl_data in by_impl.values():
            metric_names = list(impl_data["metrics"].keys())
            break
        header = "language".ljust(10) + "".join(
            name.rjust(14) for name in metric_names
        )
        print(header)
        print("-" * len(header))
        for impl, impl_data in by_impl.items():
            row = impl.ljust(10)
            for name in metric_names:
                row += f"{impl_data['metrics'][name]['median']:>14.3f}"
            print(row)

    if payload["validation_failures"]:
        print("\nValidation failures:")
        for failure in payload["validation_failures"]:
            print(f"  - {failure}")
    else:
        print("\nAll validation checks passed.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--test",
        choices=sorted(TESTS),
        action="append",
        help="Run only this test id (repeatable).",
    )
    parser.add_argument(
        "--impl",
        choices=IMPLS,
        action="append",
        help="Run only this implementation (repeatable).",
    )
    parser.add_argument(
        "--repeats",
        type=int,
        default=DEFAULT_REPEATS,
        help=f"Process repeats per track (default: {DEFAULT_REPEATS}).",
    )
    parser.add_argument(
        "--skip-compile",
        action="store_true",
        help="Use existing binaries instead of rebuilding.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=RESULT_JSON,
        help=f"JSON output path (default: {RESULT_JSON}).",
    )
    parser.add_argument(
        "--force-overwrite",
        action="store_true",
        help="Overwrite the output JSON even if validation fails or runs timed out.",
    )
    args = parser.parse_args()

    if args.repeats < 1:
        parser.error("--repeats must be at least 1")

    selected_tests = args.test or sorted(TESTS)
    selected_impls = args.impl or IMPLS

    print("Stress Test 2 - GC benchmark suite")
    print(f"  benchmarks: {', '.join(selected_tests)}")
    print(f"  languages:  {', '.join(selected_impls)}")
    print(f"  repeats:    {args.repeats}")

    if not args.skip_compile:
        print("\nCompiling:")
        if not compile_all(selected_tests, selected_impls):
            print("\nCompilation failed; no measurements were taken.")
            return 2
    else:
        print("\nSkipping compilation.")

    target_output = args.output.resolve()
    default_output = RESULT_JSON.resolve()

    payload = run_suite(
        selected_tests, selected_impls, args.repeats, output_path=target_output
    )
    payload["configuration"] = {
        "repeats": args.repeats,
        "tests": selected_tests,
        "implementations": selected_impls,
        "run_timeout_seconds": RUN_TIMEOUT_SECONDS,
    }

    should_write_target = True
    if target_output == default_output and not payload["valid"] and not args.force_overwrite:
        if target_output.exists():
            should_write_target = False
            diag_output = target_output.with_name(
                f"benchmark_results_failed_{int(time.time())}.json"
            )
            atomic_save_json(diag_output, payload)
            print(
                f"\n[WARNING] Validation failed or runs were incomplete. Preserving previous "
                f"successful artifact at {target_output}. Diagnostic run saved to: {diag_output}"
            )

    if should_write_target:
        atomic_save_json(target_output, payload)
        print(f"\nResults saved to: {target_output}")

    print_summary(payload)

    return 0 if payload["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
