#!/usr/bin/env python3
"""
Nirqoba Phase 1 Integration Test Runner.
Uses only the Python standard library.
"""

import argparse
import json
import os
import shutil
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


def find_llvm_profdata(compiler_exe):
    """Locates llvm-profdata next to the compiler or in the packaged toolchain."""
    candidates = [
        compiler_exe.parent / "llvm-profdata.exe",
        compiler_exe.resolve().parents[3] / "toolchain" / "llvm-23.1.4" / "bin" / "llvm-profdata.exe",
    ]
    on_path = shutil.which("llvm-profdata")
    if on_path:
        candidates.append(Path(on_path))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def find_llvm_objdump(compiler_exe):
    """Locates llvm-objdump next to the compiler or in the packaged toolchain."""
    candidates = [
        compiler_exe.parent / "llvm-objdump.exe",
        compiler_exe.resolve().parents[3] / "toolchain" / "llvm-23.1.4" / "bin" / "llvm-objdump.exe",
    ]
    on_path = shutil.which("llvm-objdump")
    if on_path:
        candidates.append(Path(on_path))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def check_coff_header(obj_path):
    """Verifies that obj_path starts with the AMD64 COFF machine header (0x8664 in little-endian: 0x64 0x86)."""
    with open(obj_path, "rb") as f:
        header = f.read(2)
        if len(header) < 2:
            return False, "File is too short to be a COFF object"
        if header[0] == 0x64 and header[1] == 0x86:
            return True, "Valid AMD64 COFF machine header (0x8664)"
        return False, f"Expected 0x64 0x86, got {header.hex()}"


def check_outputs(spec, out, err, stage):
    """Applies the stdout/stderr assertions shared by the normal and PGO modes."""
    if spec.get("expect_stdout") is not None:
        normalized_out = out.replace("\r\n", "\n")
        normalized_exp = spec["expect_stdout"].replace("\r\n", "\n")
        if normalized_out != normalized_exp:
            return False, (
                f"{stage}: stdout mismatch.\nExpected:\n{normalized_exp}\nGot:\n{normalized_out}"
            )

    for needle in spec.get("expect_stdout_contains", []):
        if needle not in out:
            return False, f"{stage}: stdout does not contain '{needle}'.\nGot:\n{out}"

    for needle in spec.get("expect_stdout_not_contains", []):
        if needle in out:
            return False, f"{stage}: stdout unexpectedly contains '{needle}'.\nGot:\n{out}"

    if spec.get("expect_stderr_contains") is not None:
        needle = spec["expect_stderr_contains"]
        if needle not in err:
            return False, f"{stage}: stderr does not contain '{needle}'.\nGot:\n{err}"

    return True, "PASSED"


def run_pgo_roundtrip(case_dir, source_file, compiler_exe, temp_dir, spec):
    """Builds instrumented code, runs it, merges the profile, and rebuilds.

    The round trip is what proves reproducibility and consumption: the profile
    is produced by executing the program, not checked in, and every stage uses
    the compiler's public command line.
    """
    extra_args = spec.get("args", [])
    pgo_dir = temp_dir / f"pgo_{case_dir.name}"
    pgo_dir.mkdir(parents=True, exist_ok=True)

    # 1. Instrumented build. The executable links the profile runtime.
    code, out, err = run_command(
        [str(compiler_exe), "build", str(source_file),
         "--out-dir", str(pgo_dir), "--pgo-generate"] + extra_args
    )
    if code != 0:
        # --pgo-generate links LLVM's profile runtime, which the pinned
        # LLVM/MLIR SDK does not ship (compiler-rt is a separate package).
        # A toolchain without it cannot run this case at all, so it is
        # skipped with the reason instead of reported as a compiler
        # failure; every other failure of the same build still fails.
        if "clang_rt.profile" in err:
            return None, (
                "skipped: this toolchain has no PGO profile runtime "
                "(clang_rt.profile-x86_64.lib); install compiler-rt into "
                "toolchain/<name>/lib/clang/<version>/lib/windows/ to run it")
        return False, f"Instrumented build failed with exit {code}.\nStderr: {err}\nStdout: {out}"

    executable = pgo_dir / f"{source_file.stem}.exe"
    if not executable.exists():
        return False, f"Instrumented build did not produce '{executable}'"

    # 2. Execute in the profile directory so default.profraw lands there.
    expected_program_exit = spec.get("expect_program_exit", 0)
    code, run_out, run_err = run_command([str(executable)], cwd=str(pgo_dir))
    if code != expected_program_exit:
        return False, (
            f"Instrumented program exited with {code}, expected {expected_program_exit}.\n"
            f"Stderr: {run_err}\nStdout: {run_out}"
        )
    ok, message = check_outputs(spec, run_out, run_err, "instrumented run")
    if not ok:
        return False, message

    # 3. The runtime must have written raw counters.
    profraw = pgo_dir / "default.profraw"
    if not profraw.exists() or profraw.stat().st_size == 0:
        return False, f"Profile runtime did not write a non-empty '{profraw}'"

    # 4. Merge the raw counters into the format LLVM's use pass expects.
    profdata_tool = find_llvm_profdata(compiler_exe)
    if profdata_tool is None:
        return False, "llvm-profdata was not found next to the compiler or in the toolchain"
    profdata = pgo_dir / "app.profdata"
    code, out, err = run_command(
        [str(profdata_tool), "merge", "-o", str(profdata), str(profraw)]
    )
    if code != 0 or not profdata.exists() or profdata.stat().st_size == 0:
        return False, f"llvm-profdata merge failed with exit {code}.\nStderr: {err}\nStdout: {out}"

    # 5. Inspect the optimized IR. A consumed profile adds function/branch
    #    metadata and removes the instrumentation counters.
    code, ir, err = run_command(
        [str(compiler_exe), "build", str(source_file), "--out-dir", str(pgo_dir),
         "--emit=llvm", f"--pgo-use={profdata}"] + extra_args
    )
    if code != 0:
        return False, f"Profile-use IR build failed with exit {code}.\nStderr: {err}\nStdout: {ir}"
    for needle in spec.get("expect_ir_contains", []):
        if needle not in ir:
            return False, f"Profile-use IR does not contain '{needle}'.\nGot:\n{ir}"
    for needle in spec.get("expect_ir_not_contains", []):
        if needle in ir:
            return False, f"Profile-use IR unexpectedly contains '{needle}'.\nGot:\n{ir}"

    # 6. Final optimized build and execution: the profile must not change the
    #    program's observable behaviour.
    code, out, err = run_command(
        [str(compiler_exe), "build", str(source_file), "--out-dir", str(pgo_dir),
         f"--pgo-use={profdata}"] + extra_args
    )
    if code != 0:
        return False, f"Profile-use build failed with exit {code}.\nStderr: {err}\nStdout: {out}"

    code, run_out, run_err = run_command([str(executable)], cwd=str(pgo_dir))
    if code != expected_program_exit:
        return False, (
            f"Profile-optimized program exited with {code}, expected {expected_program_exit}.\n"
            f"Stderr: {run_err}\nStdout: {run_out}"
        )
    return check_outputs(spec, run_out, run_err, "profile-optimized run")


def run_pgo_sample(case_dir, source_file, compiler_exe, temp_dir, spec):
    """Builds with a checked-in sample profile and verifies consumption.

    Sample profiles come from external samplers (perf, ETW, Instruments), so the
    fixture is checked in rather than generated. The test still proves the
    compiler loads the profile, records the sample mode, and preserves the
    program's observable behaviour.
    """
    profile_file = spec.get("profile_file", "app.sample")
    sample_path = case_dir / profile_file
    if not sample_path.exists():
        return False, f"Sample profile '{sample_path}' does not exist"

    extra_args = spec.get("args", [])
    sample_dir = temp_dir / f"pgo_sample_{case_dir.name}"
    sample_dir.mkdir(parents=True, exist_ok=True)

    # 1. Inspect the optimized IR to prove the profile was consumed.
    code, ir, err = run_command(
        [str(compiler_exe), "build", str(source_file),
         "--out-dir", str(sample_dir), "--emit=llvm",
         f"--pgo-sample={sample_path}"] + extra_args
    )
    if code != 0:
        return False, (
            f"Sample-profile IR build failed with exit {code}.\n"
            f"Stderr: {err}\nStdout: {ir}"
        )
    for needle in spec.get("expect_ir_contains", []):
        if needle not in ir:
            return False, f"Sample-profile IR does not contain '{needle}'.\nGot:\n{ir}"
    for needle in spec.get("expect_ir_not_contains", []):
        if needle in ir:
            return False, f"Sample-profile IR unexpectedly contains '{needle}'.\nGot:\n{ir}"

    # 2. Build and run the real program with the same profile.
    code, out, err = run_command(
        [str(compiler_exe), "build", str(source_file),
         "--out-dir", str(sample_dir), f"--pgo-sample={sample_path}"] + extra_args
    )
    if code != 0:
        return False, (
            f"Sample-profile build failed with exit {code}.\n"
            f"Stderr: {err}\nStdout: {out}"
        )

    executable = sample_dir / f"{source_file.stem}.exe"
    if not executable.exists():
        return False, f"Sample-profile build did not produce '{executable}'"

    expected_program_exit = spec.get("expect_program_exit", 0)
    code, run_out, run_err = run_command([str(executable)], cwd=str(sample_dir))
    if code != expected_program_exit:
        return False, (
            f"Sample-profile program exited with {code}, expected "
            f"{expected_program_exit}.\nStderr: {run_err}\nStdout: {run_out}"
        )
    return check_outputs(spec, run_out, run_err, "sample-profile run")


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
    expected_stdout_not_contains = spec.get("expect_stdout_not_contains", None)
    source_file = case_dir / "main.nrqba"

    if not source_file.exists() and "source_file" in spec:
        source_file = Path(spec["source_file"])

    if mode == "pgo_roundtrip":
        return run_pgo_roundtrip(case_dir, source_file, compiler_exe, temp_dir, spec)
    if mode == "pgo_sample":
        return run_pgo_sample(case_dir, source_file, compiler_exe, temp_dir, spec)

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
    elif mode in ("assembly_check", "target_assembly"):
        cmd.extend(["build", str(source_file), "--out-dir", str(temp_dir)])
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

    if expected_stdout_not_contains is not None:
        for needle in expected_stdout_not_contains:
            if needle in out:
                return False, f"Stdout unexpectedly contains '{needle}'.\nGot:\n{out}"

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

    # Code generation fixtures can assert on the IR the compiler produced. The
    # check builds again with --emit=llvm-raw, which stops before LLVM's own
    # passes could inline or delete the operations under test, so a count here
    # describes what lowering emitted rather than what survived optimization.
    asserts_ir = (spec.get("expect_ir_counts") or
                  spec.get("expect_ir_contains") is not None or
                  spec.get("expect_ir_not_contains") is not None)
    if asserts_ir and mode in ("build", "build_and_run", "assembly_check", "target_assembly"):
        ir_code, ir_out, ir_err = run_command(
            [str(compiler_exe), "build", str(source_file),
             "--out-dir", str(temp_dir), "--emit=llvm-raw"] + extra_args
        )
        if ir_code != 0:
            return False, (
                f"IR emission failed with exit {ir_code}.\nStderr: {ir_err}\nStdout: {ir_out}"
            )
        for needle in spec.get("expect_ir_contains", []):
            if needle not in ir_out:
                return False, f"Emitted IR does not contain '{needle}'.\nGot:\n{ir_out}"
        for needle in spec.get("expect_ir_not_contains", []):
            if needle in ir_out:
                return False, f"Emitted IR unexpectedly contains '{needle}'.\nGot:\n{ir_out}"
        for needle, expected in spec.get("expect_ir_counts", {}).items():
            actual = ir_out.count(needle)
            if actual != expected:
                return False, (
                    f"Emitted IR contains '{needle}' {actual} time(s), expected "
                    f"{expected}.\nGot:\n{ir_out}"
                )

    # The checks above describe what lowering emitted. LLVM's own passes then
    # inline, promote, unroll and vectorize that IR, so a fixture that claims one
    # of those transforms has to assert on the optimized module instead. The
    # second build stops after the LLVM pipeline (--emit=llvm), which is the
    # module handed to the target machine.
    asserts_optimized = (spec.get("expect_optimized_ir_counts") or
                         spec.get("expect_optimized_ir_contains") is not None or
                         spec.get("expect_optimized_ir_not_contains") is not None)
    if asserts_optimized and mode in ("build", "build_and_run", "assembly_check", "target_assembly"):
        opt_code, opt_out, opt_err = run_command(
            [str(compiler_exe), "build", str(source_file),
             "--out-dir", str(temp_dir), "--emit=llvm"] + extra_args
        )
        if opt_code != 0:
            return False, (
                f"Optimized IR emission failed with exit {opt_code}.\nStderr: {opt_err}\nStdout: {opt_out}"
            )
        for needle in spec.get("expect_optimized_ir_contains", []):
            if needle not in opt_out:
                return False, (
                    f"Optimized IR does not contain '{needle}'.\nGot:\n{opt_out}"
                )
        for needle in spec.get("expect_optimized_ir_not_contains", []):
            if needle in opt_out:
                return False, (
                    f"Optimized IR unexpectedly contains '{needle}'.\nGot:\n{opt_out}"
                )
        for needle, expected in spec.get("expect_optimized_ir_counts", {}).items():
            actual = opt_out.count(needle)
            if actual != expected:
                return False, (
                    f"Optimized IR contains '{needle}' {actual} time(s), expected "
                    f"{expected}.\nGot:\n{opt_out}"
                )

    # Special check for COFF object header if requested
    if spec.get("check_coff_header", False):
        stem = source_file.stem
        obj_file = temp_dir / f"{stem}.obj"
        if not obj_file.exists():
            return False, f"Expected object file '{obj_file}' was not produced"
        valid, msg = check_coff_header(obj_file)
        if not valid:
            return False, f"COFF header check failed: {msg}"

    # Disassemble the object to prove the selected profile controls instruction
    # selection. Portable must stay on the baseline; explicit wides must use
    # the wider registers they requested.
    if mode in ("assembly_check", "target_assembly"):
        obj_file = temp_dir / f"{source_file.stem}.obj"
        if not obj_file.exists():
            return False, f"Expected object file '{obj_file}' was not produced"

        objdump = find_llvm_objdump(compiler_exe)
        if objdump is None:
            return False, "llvm-objdump was not found next to the compiler or in the toolchain"

        dump_code, dump, dump_err = run_command(
            [str(objdump), "-d", "--no-show-raw-insn", str(obj_file)]
        )
        if dump_code != 0:
            return False, f"llvm-objdump failed: {dump_err}"

        for needle in spec.get("expect_asm_contains", []):
            if needle not in dump:
                return False, f"Disassembly does not contain '{needle}'.\nGot:\n{dump}"
        for needle in spec.get("expect_asm_not_contains", []):
            if needle in dump:
                return False, f"Disassembly unexpectedly contains '{needle}'.\nGot:\n{dump}"

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
    skipped = []

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_path = Path(temp_dir)
        for case in cases:
            rel_name = case.relative_to(integration_dir)
            print(f"Testing {rel_name}... ", end="", flush=True)
            ok, msg = run_case(case, compiler_exe, temp_path)
            if ok is None:
                print("SKIP")
                skipped.append((rel_name, msg))
            elif ok:
                print("PASS")
                passed += 1
            else:
                print("FAIL")
                print(f"  {msg}")
                failed += 1

    print("=" * 60)
    for rel_name, reason in skipped:
        print(f"Skipped {rel_name}: {reason}")
    print(f"Total: {len(cases)} | Passed: {passed} | "
          f"Failed: {failed} | Skipped: {len(skipped)}")
    if failed > 0:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
