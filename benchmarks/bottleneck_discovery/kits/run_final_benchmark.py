"""Final integrated benchmark: 0.257 vs 0.260 vs 0.269 runtime kits.

The compiler driver is the current 0.269 build for every kit, so the emitted
program is identical; only the linked runtime library differs. That isolates the
collector, which is what the 0.266 regression changed (COLLECT is runtime-only).
"""
import os
import re
import shutil
import statistics
import subprocess
import sys

ROOT = "D:/Actual Project Files/Project Nirqoba"
DRIVER = os.path.join(ROOT, "build/msvc-dev/bin/nirqoba.exe")
LIB = os.path.join(ROOT, "build/msvc-dev/lib/nirqoba_runtime.lib")
TESTS = {
    "02": os.path.join(ROOT, "benchmarks/stress_test_2/nirqoba/02_mark_and_sweep.nrqba"),
    "03": os.path.join(ROOT, "benchmarks/stress_test_2/nirqoba/03_cyclic_graph_reclamation.nrqba"),
}
KITS = [("0.257 (98a1099)", "98a1099"), ("0.260 (904aee0)", "904aee0"),
        ("0.269 (d23a2b4)", "d23a2b4")]
OUT = os.path.join(ROOT, "build/kits/bin")
RAW = os.path.join(ROOT, "build/kits/raw")
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 8

os.makedirs(OUT, exist_ok=True)
os.makedirs(RAW, exist_ok=True)
backup = os.path.join(ROOT, "build/kits/nirqoba_runtime.lib.orig")
if not os.path.exists(backup):
    shutil.copy(LIB, backup)

for label, rev in KITS:
    kit_lib = os.path.join(ROOT, "build/kits", rev, "nirqoba_runtime.lib")
    shutil.copy(kit_lib, LIB)
    for mode, source in TESTS.items():
        exe = os.path.join(OUT, "%s_%s.exe" % (rev, mode))
        result = subprocess.run([DRIVER, "build", source, "-o", exe, "-O3"],
                                cwd=os.path.join(ROOT, "benchmarks/stress_test_2/bin"),
                                capture_output=True, text=True)
        if result.returncode != 0:
            print("compile failed", rev, mode)
            print(result.stdout[-2000:], result.stderr[-2000:])
            shutil.copy(backup, LIB)
            sys.exit(1)
        print("compiled", rev, mode)

metrics = {}
for round_index in range(1, RUNS + 1):
    for label, rev in KITS:
        for mode in TESTS:
            exe = os.path.join(OUT, "%s_%s.exe" % (rev, mode))
            out = subprocess.run([exe], capture_output=True, text=True).stdout
            open(os.path.join(RAW, "%s_%s_run%d.txt" % (rev, mode, round_index)),
                 "w", encoding="utf-8").write(out)
            values = [float(v) for v in re.findall(r"COLLECT_MS\n([0-9.]+)", out)]
            if not values:
                print("no COLLECT_MS in", rev, mode)
                shutil.copy(backup, LIB)
                sys.exit(1)
            metrics.setdefault((label, mode), []).append(statistics.median(values))

shutil.copy(backup, LIB)

for (label, mode), values in metrics.items():
    ordered = sorted(values)
    print("%-18s %s min %6.1f med %6.1f max %6.1f  %s" %
          (label, mode, ordered[0], statistics.median(ordered), ordered[-1],
           " ".join("%.1f" % v for v in values)))
