import os, re, statistics, subprocess, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CC = os.path.join(ROOT, "build", "msvc-dev", "bin", "nirqoba.exe")
OUT = os.path.join(ROOT, "build", "ao")
REPS = ["aos", "soa", "aosoa"]
CASES = [("cache", 65536, 200), ("bandwidth", 2000000, 20)]
RUNS = 5
WORKLOADS = {"aos": ["single", "multi", "random", "write"],
             "soa": ["single", "multi", "random", "write"],
             "aosoa": ["single", "multi", "write"]}

def build(rep, n, rounds):
    src = os.path.join(ROOT, "benchmarks", "soa_stage4", rep + ".nrqba")
    text = open(src, encoding="utf-8").read()
    text = re.sub(r"var n = \d+;", "var n = %d;" % n, text, count=1)
    text = re.sub(r"var rounds = \d+;", "var rounds = %d;" % rounds, text, count=1)
    tmp = os.path.join(OUT, "bench_%s_%d.nrqba" % (rep, n))
    open(tmp, "w", encoding="utf-8").write(text)
    exe = os.path.join(OUT, "bench_%s_%d.exe" % (rep, n))
    r = subprocess.run([CC, "build", tmp, "-o", exe], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("build failed for %s/%d: %s" % (rep, n, r.stdout + r.stderr))
    return exe

def run(exe):
    r = subprocess.run([exe], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("run failed: %s" % r.stderr)
    return [int(x) for x in re.findall(r"-?\d+", r.stdout)]

rows = []
for case, n, rounds in CASES:
    for rep in REPS:
        exe = build(rep, n, rounds)
        samples = []
        checks = None
        for _ in range(RUNS):
            v = run(exe)
            pairs = list(zip(v[0::2], v[1::2]))
            samples.append([p[0] for p in pairs])
            c = [p[1] for p in pairs]
            if checks is None:
                checks = c
            elif checks != c:
                sys.exit("checksum mismatch for %s/%d" % (rep, n))
        for i, name in enumerate(WORKLOADS[rep]):
            col = [s[i] for s in samples]
            rows.append((case, n, rep, name, min(col), statistics.median(col),
                         sum(col) / len(col), checks[i], col))
lines = ["# Stage 4 raw measurements", "",
         "Internal `time.now()` milliseconds, %d runs per configuration." % RUNS,
         "Median is the middle run; best is the fastest. Checksums are equal across",
         "representations for the same workload, which shows identical work.", "",
         "| case | rows | rep | workload | best ms | median ms | mean ms | checksum | runs |",
         "|---|---|---|---|---|---|---|---|---|"]
for case, n, rep, name, best, med, mean, chk, runs in rows:
    lines.append("| %s | %d | %s | %s | %d | %.1f | %.1f | %d | %s |" %
                 (case, n, rep, name, best, med, mean, chk, ",".join(map(str, runs))))
open(os.path.join(ROOT, "benchmarks", "soa_stage4", "RAW.md"), "w",
     encoding="utf-8").write("\n".join(lines) + "\n")
for case, n, rep, name, best, med, mean, chk, runs in rows:
    print("%-9s %8d %-6s %-7s best=%3d med=%6.1f chk=%d" % (case, n, rep, name, best, med, chk))
