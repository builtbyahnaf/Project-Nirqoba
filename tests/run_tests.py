#!/usr/bin/env python3
"""
Nirqoba Phase 1 Integration Test Runner.
Uses only the Python standard library.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run_command(cmd, cwd=None):
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc.returncode, proc.stdout, proc.stderr


def check_coff_header(obj_path):
    """Verifies that obj_path starts with the AMD64 COFF machine header (0x8664 in little-endian: 0x64 0x86)."""
    with open(obj_path, "rb") as f:
        header = f.read(2)
        if len(header) < 2:
            return False, "File is too short to be a COFF object"
        if header[0] == 0x64 and header[1] == 0x86:
            return True, "Valid AMD64 COFF machine header (0x8664)"
        return False, f"Expected 0x64 0x86, got {header.hex()}"


def run_case(case_dir, compiler_exe, temp_dir):
    expect_path = case_dir / "expect.json"
    if not expect_path.exists():
        return True, "No expect.json, skipping"

    with open(expect_path, "r", encoding="utf-8") as f:
        spec = json.load(f)

    mode = spec.get("mode", "build_and_run")
    extra_args = spec.get("args", [])
    expected_exit = spec.get("expect_exit", 0)
    expected_stdout = spec.get("expect_stdout", None)
    expected_stderr_contains = spec.get("expect_stderr_contains", None)
    expected_stdout_contains = spec.get("expect_stdout_contains", None)
    source_file = case_dir / "main.nrqba"

    if not source_file.exists() and "source_file" in spec:
        source_file = Path(spec["source_file"])

    cmd = [str(compiler_exe)]

    if mode == "build_and_run":
        cmd.extend(["run", str(source_file), "--out-dir", str(temp_dir)])
        cmd.extend(extra_args)
    elif mode == "build":
        cmd.extend(["build", str(source_file), "--out-dir", str(temp_dir)])
        cmd.extend(extra_args)
    elif mode == "check":
        cmd.extend(["check", str(source_file)])
        cmd.extend(extra_args)
    elif mode == "raw":
        cmd.extend(extra_args)
    else:
        return False, f"Unknown mode: {mode}"

    code, out, err = run_command(cmd)

    if code != expected_exit:
        return False, f"Exit code mismatch: expected {expected_exit}, got {code}.\nStderr: {err}\nStdout: {out}"

    if expected_stdout is not None:
        normalized_out = out.replace("\r\n", "\n")
        normalized_exp = expected_stdout.replace("\r\n", "\n")
        if normalized_out != normalized_exp:
            return False, f"Stdout mismatch.\nExpected:\n{normalized_exp}\nGot:\n{normalized_out}"

    if expected_stdout_contains is not None:
        for needle in expected_stdout_contains:
            if needle not in out:
                return False, f"Stdout does not contain '{needle}'.\nGot:\n{out}"

    if expected_stderr_contains is not None:
        for needle in expected_stderr_contains:
            if needle not in err:
                return False, f"Stderr does not contain '{needle}'.\nGot:\n{err}"

    # Special check for MLIR proof
    expect_mlir = case_dir / "expect.mlir"
    if expect_mlir.exists() and mode == "build":
        with open(expect_mlir, "r", encoding="utf-8") as f:
            mlir_needles = [line.strip() for line in f if line.strip() and not line.startswith("//")]
        for needle in mlir_needles:
            if needle not in out:
                return False, f"MLIR output does not contain expected snippet: '{needle}'"

    # Special check for COFF object header if requested
    if spec.get("check_coff_header", False):
        stem = source_file.stem
        obj_file = temp_dir / f"{stem}.obj"
        if not obj_file.exists():
            return False, f"Expected object file '{obj_file}' was not produced"
        valid, msg = check_coff_header(obj_file)
        if not valid:
            return False, f"COFF header check failed: {msg}"

    return True, "PASSED"


def main():
    parser = argparse.ArgumentParser(description="Nirqoba Integration Test Runner")
    parser.add_argument("--compiler", required=True, help="Path to nirqoba.exe")
    parser.add_argument("--configuration", default="RelWithDebInfo", help="Build configuration")
    args = parser.parse_args()

    compiler_exe = Path(args.compiler).resolve()
    if not compiler_exe.exists():
        print(f"Error: Compiler executable not found at '{compiler_exe}'")
        sys.exit(1)

    repo_root = Path(__file__).parent.parent.resolve()
    integration_dir = repo_root / "tests" / "integration"

    if not integration_dir.exists():
        print(f"Error: Integration tests directory '{integration_dir}' not found")
        sys.exit(1)

    cases = sorted([d for d in integration_dir.glob("**/*") if d.is_dir() and (d / "expect.json").exists()])

    print(f"Discovered {len(cases)} integration test cases.")
    passed = 0
    failed = 0

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        for case in cases:
            rel_name = case.relative_to(integration_dir)
            print(f"Testing {rel_name}... ", end="", flush=True)
            ok, msg = run_case(case, compiler_exe, temp_path)
            if ok:
                print("PASS")
                passed += 1
            else:
                print("FAIL")
                print(f"  {msg}")
                failed += 1

    print("=" * 60)
    print(f"Total: {len(cases)} | Passed: {passed} | Failed: {failed}")
    if failed > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
