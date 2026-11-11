import os, re, subprocess
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CC = os.path.join(ROOT, "build", "msvc-dev", "bin", "nirqoba.exe")
lines = ["# Vectorisation observed in emitted IR", "",
         "Counts come from `--emit=llvm` output, so this is what the compiler actually",
         "produced, not an inference from timings.", "",
         "| representation | vector types | vector loads | vector stores | vector reduces |",
         "|---|---|---|---|---|"]
for rep in ["aos", "soa", "aosoa"]:
    src = os.path.join(ROOT, "benchmarks", "soa_stage4", rep + ".nrqba")
    out = subprocess.run([CC, "build", src, "--emit=llvm"], capture_output=True, text=True).stdout
    types = len(re.findall(r"<\d+ x i\d+>", out))
    loads = len(re.findall(r"load <\d+ x", out))
    stores = len(re.findall(r"store <\d+ x", out))
    reduces = len(re.findall(r"vector\.reduce", out))
    lines.append("| %s | %d | %d | %d | %d |" % (rep, types, loads, stores, reduces))
    print("%-6s vector_types=%d vec_loads=%d vec_stores=%d reduces=%d" % (rep, types, loads, stores, reduces))
open(os.path.join(ROOT, "benchmarks", "soa_stage4", "VECTORIZATION.md"), "w",
     encoding="utf-8").write("\n".join(lines) + "\n")
