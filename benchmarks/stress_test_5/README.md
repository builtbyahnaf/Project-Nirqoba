# Stress Test 5 - The Cross-Language CDMM Gauntlet

One workload, four memory tiers, seven implementations. The question is not who
wins a microbenchmark; it is whether an automated memory model costs anything
against the languages programmers reach for when they want control, and whether
it keeps the footprint those languages keep.

| Language | Runtime model | Source |
|---|---|---|
| Nirqoba | CDMM: stack, pool, arena, collector - chosen by the compiler | `nirqoba_impl.nrqba` |
| C | manual `malloc` / `free` | `c_impl.c` |
| C++ | `std::make_unique` / `std::unique_ptr` | `cpp_impl.cpp` |
| Rust | `Box` / `Rc<RefCell<..>>` | `rust_impl.rs` |
| Java | G1 collector | `java_impl.java` |
| JavaScript | Node.js 24, V8 | `js_impl.js` |
| Python | CPython 3.12, reference counting + cycle GC | `py_impl.py` |

The measured numbers are in [`BENCHMARK_REPORT.md`](BENCHMARK_REPORT.md). The
timing protocol, validation rules and limitations are in
[`METHODOLOGY.md`](METHODOLOGY.md). Machine-readable results are in
[`benchmark_results.json`](benchmark_results.json).

## The workload

Four phases, each in its own function so each own its frame:

1. **Stack** - 200,000 short-lived one-field structs in a tight loop, written and
   read back inside the iteration. Nothing escapes, so the CDMM selector promotes
   the allocation to the loop's stack slot.
2. **Pool** - 2,000 iterations of a 8,192-slot uniform batch, touched at slot 0
   and released at the end of the iteration. The slot is too large for the stack
   limit, and a fresh arena block per iteration would grow without bound, so the
   selector uses the pool and hands the same slot back.
3. **Arena** - one 8,191-node block, released whole when the frame ends, plus a
   node promoted into the caller's arena by implicit return promotion.
4. **GC** - 40,000 cycle-capable nodes published into foreign storage, then the
   roots are dropped and the collector is asked to sweep.

Every phase returns a checksum; `main` prints all four, in order. Identical
numbers across seven runtimes is the proof that the same work happened.

## Directory layout

```text
benchmarks/stress_test_5/
├── BENCHMARK_REPORT.md      # results and analysis
├── METHODOLOGY.md           # protocol, fairness, limitations
├── README.md                # this file
├── run_stress_test_5.py     # compile + run + validate + summarise
├── benchmark_results.json   # latest machine-readable results
├── nirqoba_impl.nrqba       # CDMM track
├── c_impl.c
├── cpp_impl.cpp
├── rust_impl.rs
├── java_impl.java
├── js_impl.js
├── py_impl.py
├── bin/                     # generated executables (ignored by git)
└── results/raw/             # captured stdout of every repeat
```

## Prerequisites

| Tool | Expected location / command |
|---|---|
| Nirqoba | `build/msvc-dev/bin/nirqoba.exe` |
| C | `gcc` on `PATH` (falls back to MSVC `cl` in a vcvars64 environment) |
| C++ | `g++` on `PATH` (same fallback) |
| Rust | `C:\Users\USER\.cargo\bin\rustc.exe` |
| Java | JDK 17 at the Gradle toolchain path defined at the top of the runner |
| Node.js | `node` on `PATH` |
| Python | `python` on `PATH` |

Paths are defined at the top of `run_stress_test_5.py`.

## Quick start

From this directory:

```powershell
python run_stress_test_5.py
```

The default run compiles every track, executes one untimed warm-up and then five
timed repeats of each process, and writes `benchmark_results.json` plus raw
output under `results/raw/`.

Common options:

```powershell
# Re-run measurements without recompiling
python run_stress_test_5.py --skip-compile

# Two languages only
python run_stress_test_5.py --impl nirqoba --impl c

# More repeats
python run_stress_test_5.py --repeats 9
```

The runner exits with code `0` only when every track prints the four checksums
the workload defines.

## What each phase costs, by design

| Phase | Nirqoba | C | Notes |
|---|---|---|---|
| stack | 200,000 promotions to a loop stack slot | 200,000 `malloc`/`free` pairs | |
| pool | 2,000 reused 64 KiB slots | 2,000 `malloc`/`free` of 64 KiB | |
| arena | one 96 KiB block + one promoted node | one `malloc`/`free` of 96 KiB | |
| gc | 40,000 published nodes, two full collections | 40,000 `malloc`, 40,000 `free` | no C equivalent: reclamation is manual |

## Compiler defects this workload is shaped around

Recorded here because the benchmark would be dishonest without them, and
because they belong in the compiler's own issue list:

1. `memory.allocate<T>(n)` is reliable at element 0 only - writing element 1 and
   reading it back returns the wrong value. Every block phase therefore touches
   element 0 of a large block, which is what the tier decision is about anyway.
2. A multi-field struct promoted to a loop's stack slot reads its fields back
   stale, so the vector phase uses a one-field struct.
3. Two allocation sites in one loop body fail lowering with "selected allocation
   has no region handle; hybrid strategy selection and lowering disagree", and an
   arena allocation and a collector allocation in the same frame fail the same
   way. Both are why each phase is its own function and the GC phase allocates one
   node per iteration.
