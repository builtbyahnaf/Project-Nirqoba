"""Bisect the 0.257 -> 0.269 runtime chain to attribute the test-03 residual.

Same method as run_final_benchmark.py: one 0.269 compiler driver, one runtime
library per revision, runs interleaved. Only the requested modes run, so the
bisect stays cheap.

Usage: python run_bisect_benchmark.py [runs] [modes]
       modes is a comma list of 02 / 03 (default 03).
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
# Chronological runtime chain from 0.257 to 0.269. 5d62abf's runtime is
# byte-identical to 0.260 (904aee0); 9d5b6ac's is byte-identical to HEAD.
KITS = [
    ("0.257 (98a1099)", "98a1099"),
    ("f1d2b42", "f1d2b42"),
    ("1ca3c37", "1ca3c37"),
    ("e830b65", "e830b65"),
    ("1646b93", "1646b93"),
    ("0708f75", "0708f75"),
    ("0.260 (5d62abf)", "5d62abf"),
    ("0.269 (9d5b6ac)", "9d5b6ac"),
]
OUT = os.path.join(ROOT, "build/kits/bin")
RAW = os.path.join(ROOT, "build/kits/raw_bisect")
RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 8
MODES = (sys.argv[2] if len(sys.argv) > 2 else "03").split(",")
if len(sys.argv) > 3:
    wanted = sys.argv[3].split(",")
    KITS = [kit for kit in KITS if kit[1] in wanted]

os.makedirs(OUT, exist_ok=True)
os.makedirs(RAW, exist_ok=True)
backup = os.path.join(ROOT, "build/kits/nirqoba_runtime.lib.orig")
if not os.path.exists(backup):
    shutil.copy(LIB, backup)

for label, rev in KITS:
    kit_lib = os.path.join(ROOT, "build/kits", rev, "nirqoba_runtime.lib")
    shutil.copy(kit_lib, LIB)
    for mode in MODES:
        exe = os.path.join(OUT, "%s_%s.exe" % (rev, mode))
        result = subprocess.run([DRIVER, "build", TESTS[mode], "-o", exe, "-O3"],
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
        for mode in MODES:
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
