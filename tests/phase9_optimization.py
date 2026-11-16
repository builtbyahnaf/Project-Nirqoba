#!/usr/bin/env python3
"""Phase 9 optimization compatibility checks.

Builds a representative ownership-heavy fixture set at -O0, -O2 and the debug
runtime, runs each result, and compares its behavior. It also inspects emitted
LLVM at -O0, where no backend pass can hide a duplicated release or an unwanted
collector dependency.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = [
    "partial_initialization",
    "manual_lifecycle",
    "manual_managed_payload",
    "move_local",
    "move_chain",
    "conditional_move",
    "arena_lifecycle",
    "shared_copy_retains",
    "shared_multiple_owners",
    "nested_aggregate",
]
GC_FREE = {
    "partial_initialization",
    "move_local",
    "move_chain",
    "conditional_move",
    "arena_lifecycle",
    "shared_copy_retains",
    "shared_multiple_owners",
    "nested_aggregate",
}
EXPECTED_RELEASES = {
    "manual_lifecycle": "nrq_memory_deallocate_checked",
    "manual_managed_payload": "nrq_memory_deallocate_checked",
}


def run(command):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"{' '.join(map(str, command))}: exit {result.returncode}\n"
            f"{result.stdout}\n{result.stderr}")
    return result.stdout


def expected(directory):
    spec = json.loads((directory / "expect.json").read_text())
    if spec.get("mode") != "build_and_run":
        raise RuntimeError(f"{directory}: unsupported mode {spec.get('mode')}")
    return spec


def check_run(compiler, directory, optimization, debug, output):
    spec = expected(directory)
    command = [compiler, "build", str(directory / "main.nrqba"),
               f"-O{optimization}", "-o", str(output)]
    if debug:
        command.append("--debug-enabled")
    run(command)
    result = subprocess.run([str(output)], cwd=ROOT, capture_output=True, text=True)
    if result.returncode != spec.get("expect_exit", 0):
        raise RuntimeError(
            f"{directory.name} -O{optimization} debug={debug}: "
            f"exit {result.returncode}, expected {spec.get('expect_exit', 0)}\n"
            f"{result.stdout}\n{result.stderr}")
    if "expect_stdout" in spec and result.stdout != spec["expect_stdout"]:
        raise RuntimeError(
            f"{directory.name} -O{optimization} debug={debug}: stdout "
            f"{result.stdout!r}, expected {spec['expect_stdout']!r}")
    for fragment in spec.get("expect_stdout_contains", []):
        if fragment not in result.stdout:
            raise RuntimeError(
                f"{directory.name} -O{optimization} debug={debug}: "
                f"stdout lacks {fragment!r}: {result.stdout!r}")
    return result.returncode, result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=str(
        ROOT / "build/msvc-dev/bin/nirqoba.exe"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="nirqoba-phase9-") as temporary:
        output_dir = pathlib.Path(temporary)
        for name in FIXTURES:
            directory = ROOT / "tests/memory_model" / name
            results = {}
            for optimization in ("0", "2"):
                for debug in (False, True):
                    output = output_dir / f"{name}-O{optimization}-{int(debug)}.exe"
                    results[(optimization, debug)] = check_run(
                        args.compiler, directory, optimization, debug, output)
            if len(set(results.values())) != 1:
                raise RuntimeError(f"{name}: optimization/debug behavior diverged")

            llvm = run([args.compiler, "build", str(directory / "main.nrqba"),
                        "--emit=llvm", "-O0"])
            if name in GC_FREE and re.search(r"@nrq_gc_|@nrq_gc\b", llvm):
                raise RuntimeError(f"{name}: ordinary path acquired a GC dependency")
            release = EXPECTED_RELEASES.get(name)
            if release:
                calls = len(re.findall(rf"call void @{release}\(", llvm))
                if calls != 1:
                    raise RuntimeError(
                        f"{name}: expected one {release} call at -O0, found {calls}")
            print(f"{name}: O0/O2 production+debug equivalent")
    print("Phase 9 optimization compatibility checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
