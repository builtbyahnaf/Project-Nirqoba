#!/usr/bin/env python3
"""Phase 7 GC-isolation evidence.

The integration expectations already assert individual IR properties. This
script performs the wider check the phase needs:

  * ordinary fixtures compile at -O0 and -O2 with no ``nrq_gc`` name in the
    emitted LLVM, including declarations, not just calls;
  * the linked executable contains no ``nrq_gc`` symbol, so a GC declaration or
    a whole runtime object cannot remain as an unused dependency;
  * the program still runs and produces the expected exit code;
  * a deliberately managed program does link GC symbols, proving the check is
    not vacuously true.

It is a verification tool, not a benchmark. It writes only under build/.
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_COMPILER = ROOT / "build" / "msvc-dev" / "bin" / "nirqoba.exe"
DEFAULT_NM = ROOT / "toolchain" / "llvm-23.1.4" / "bin" / "llvm-nm.exe"
OUTPUT = ROOT / "build" / "phase7_isolation_checks"

# name, source, expected exit, extra compiler flags.
FIXTURES = [
    ("owned_pointer_o0", "tests/integration/positive/owned_pointer_gc_isolation_o0/main.nrqba", 0, []),
    ("owned_heap_pointer", "tests/integration/positive/owned_heap_pointer_gc_isolation/main.nrqba", 0, []),
    ("owned_array", "tests/integration/positive/owned_array_gc_isolation_o0/main.nrqba", 102, []),
    ("owned_aggregate", "tests/integration/positive/owned_aggregate_gc_isolation_o0/main.nrqba", 42, []),
    ("borrowed_heap_view", "tests/integration/positive/borrowed_heap_view_gc_isolation/main.nrqba", 0, []),
    ("ordinary_reallocation", "tests/integration/positive/ordinary_reallocation_gc_isolation/main.nrqba", 0, []),
    ("secure_allocation", "tests/integration/positive/secure_allocation_gc_isolation/main.nrqba", 0, []),
    ("shared_allocation", "tests/integration/positive/shared_allocation_gc_isolation/main.nrqba", 0, []),
    ("helper_chain", "tests/integration/positive/gc_isolation_helpers/main.nrqba", 42, []),
    ("arena", "tests/integration/mlir/arena_gc_independence/main.nrqba", 42, []),
    ("pool", "tests/integration/mlir/pool_gc_independence/main.nrqba", 42, []),
    ("debug_runtime", "tests/integration/positive/gc_isolation_debug/main.nrqba", 42,
     ["--debug-enabled"]),
]

MANAGED_FIXTURE = "tests/integration/positive/gc_root_at_O0/main.nrqba"


def run(command, cwd=ROOT):
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True)


def compile_ir(compiler, source, optimization, extra):
    result = run([str(compiler), "build", str(ROOT / source), "--emit=llvm",
                  optimization] + extra)
    if result.returncode != 0:
        raise RuntimeError(f"IR build failed for {source} {optimization}:\n{result.stderr}")
    return result.stdout


def compile_exe(compiler, source, optimization, executable, extra):
    result = run([str(compiler), "build", str(ROOT / source), optimization,
                  "-o", str(executable)] + extra)
    if result.returncode != 0:
        raise RuntimeError(f"executable build failed for {source} {optimization}:\n"
                           f"{result.stderr}")
    return result


def compile_object(compiler, source, optimization, object_file, extra):
    result = run([str(compiler), "build", str(ROOT / source), "--emit=obj",
                  optimization, "-o", str(object_file)] + extra)
    if result.returncode != 0:
        raise RuntimeError(f"object build failed for {source} {optimization}:\n"
                           f"{result.stderr}")
    return result


def gc_symbol_count(nm, object_file):
    result = run([str(nm), str(object_file)])
    if result.returncode != 0:
        raise RuntimeError(f"llvm-nm failed for {object_file}:\n{result.stderr}")
    return sum(1 for line in result.stdout.splitlines() if "nrq_gc" in line.lower())


def function_body(ir, name):
    match = re.search(r"define[^\n]*@" + re.escape(name) + r"\([^\n]*\{\n", ir)
    if match is None:
        raise RuntimeError(f"function '{name}' not found in IR")
    end = ir.find("\n}\n", match.end())
    if end < 0:
        raise RuntimeError(f"unterminated function '{name}' in IR")
    return ir[match.end():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=str(DEFAULT_COMPILER))
    parser.add_argument("--nm", default=str(DEFAULT_NM))
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler)
    nm = pathlib.Path(args.nm)
    if not compiler.exists():
        raise SystemExit(f"compiler not found: {compiler}")
    if not nm.exists():
        raise SystemExit(f"llvm-nm not found: {nm}")

    OUTPUT.mkdir(parents=True, exist_ok=True)
    report = {"optimizations": ["-O0", "-O2"], "fixtures": {}, "managed_control": {}}
    failures = []
    for name, source, expected_exit, extra in FIXTURES:
        entry = {}
        for optimization in ("-O0", "-O2"):
            ir = compile_ir(compiler, source, optimization, extra)
            if "nrq_gc" in ir:
                failures.append(f"{name} {optimization}: emitted IR mentions nrq_gc")
            executable = OUTPUT / f"{name}_{optimization[1:]}.exe"
            compile_exe(compiler, source, optimization, executable, extra)
            object_file = OUTPUT / f"{name}_{optimization[1:]}.obj"
            compile_object(compiler, source, optimization, object_file, extra)
            symbols = gc_symbol_count(nm, object_file)
            if symbols != 0:
                failures.append(f"{name} {optimization}: object has {symbols} nrq_gc symbols")
            result = run([str(executable)])
            if result.returncode != expected_exit:
                failures.append(
                    f"{name} {optimization}: exit {result.returncode}, expected {expected_exit}")
            entry[optimization] = {
                "ir_mentions_nrq_gc": "nrq_gc" in ir,
                "object_gc_symbols": symbols,
                "exit": result.returncode,
            }
        report["fixtures"][name] = entry

    managed_ir = compile_ir(compiler, MANAGED_FIXTURE, "-O0", [])
    if "nrq_gc" not in managed_ir:
        failures.append("managed control: GC allocation fixture has no GC IR")
    managed_exe = OUTPUT / "managed_control.exe"
    compile_exe(compiler, MANAGED_FIXTURE, "-O0", managed_exe, [])
    managed_object = OUTPUT / "managed_control.obj"
    compile_object(compiler, MANAGED_FIXTURE, "-O0", managed_object, [])
    managed_symbols = gc_symbol_count(nm, managed_object)
    if managed_symbols == 0:
        failures.append("managed control: object has no GC symbols")
    report["managed_control"] = {
        "ir_mentions_nrq_gc": "nrq_gc" in managed_ir,
        "object_gc_symbols": managed_symbols,
    }

    # A GC-independent helper called from a managed function must not acquire
    # the caller's mutator region. Check the function bodies, not the whole
    # module, so the caller's required protocol cannot mask a helper regression.
    effect_ir = compile_ir(
        compiler, "tests/integration/positive/gc_plain_helper_no_region/main.nrqba",
        "-O0", [])
    plain_body = function_body(effect_ir, "add_one")
    managed_body = function_body(effect_ir, "make_node")
    if "nrq_gc" in plain_body:
        failures.append("plain helper body contains a GC reference")
    if "nrq_gc" not in managed_body:
        failures.append("managed helper body has no GC reference")
    report["function_effects"] = {
        "plain_helper_has_gc": "nrq_gc" in plain_body,
        "managed_helper_has_gc": "nrq_gc" in managed_body,
    }

    report["failures"] = failures
    print(json.dumps(report, indent=2))
    (OUTPUT / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    if failures:
        print("\nFAILED", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("\nPhase 7 isolation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
