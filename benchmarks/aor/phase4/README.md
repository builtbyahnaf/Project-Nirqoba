# AOR Phase 4 representation benchmark

`phase4_benchmark` measures the three object representations through the public
GC API: allocation, a full collection with the whole live set reachable, and a
full collection after every reference is dropped. Each shape keeps one managed
reference so marking walks the same graph in all three cases.

The executable links `nirqoba_runtime_debug`, the library the unit tests audit.
That is stated here rather than implied: the numbers describe the development
configuration.

## Build

The benchmark is its own CMake target, so the repository's default build (which
names `nirqoba_unit_tests` and `nirqoba`) does not build it. From a Visual Studio
x64 developer shell:

```bat
cmake --build build\msvc-dev --target nirqoba_phase4_benchmark
```

The executable lands in `build\msvc-dev\benchmarks\aor\phase4\`.

## Run

```bat
build\msvc-dev\benchmarks\aor\phase4\nirqoba_phase4_benchmark.exe 200000 9
```

- argument 1: objects per shape (default 200000, minimum 16)
- argument 2: timed runs after one warm-up (default 5)

Output is one row per shape with the median of the timed runs:

```
shape=H0-64B-record n=200000 alloc_median_ns=... live_gc_median_ns=... dead_gc_median_ns=... header_bytes=16 h16_objects=1
```

`header_bytes` is the header bytes spent by the live set at the first timed
sample (H0 spends 0, H8 8 per object, H16 16 per object).

The measured results and the derived per-object comparison are recorded in
`reports/aor/phase-4-performance.md`. Run the benchmark on the machine you are
quoting; do not copy these numbers to another target as if they were measured
there.
