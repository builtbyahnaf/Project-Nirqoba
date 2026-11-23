# Stress Test 5 - Methodology

This document states what is measured, how the tracks are kept comparable, and
what the numbers cannot show.

## What is timed

Each language is measured as a whole process:

- **Wall clock** - from the moment the process is created to the moment it exits,
  measured with `time.perf_counter()` around the child process.
- **Peak RSS** - the peak working set the operating system recorded for that
  process, read with `GetProcessMemoryInfo` while it runs. The peak is a property
  of the process, so a late read still reports the high-water mark.

Both numbers include runtime start-up, which is the honest cost of running the
workload in that language. A JVM that needs 76 ms of its 77 ms to boot and run is
not faster than a native binary that needs 18 ms; it is simply a different
product, and the harness measures the product. This is the opposite choice from
Stress Test 2, which timed phases inside the process to isolate collector
behaviour. Both choices are recorded here so the two suites are not confused.

## Fairness rules

1. **Same work, same checksums.** A track that prints the wrong numbers is not
   timed at all. The runner compares all four checksums against the values the
   workload defines and aborts the run on a mismatch.
2. **Same shapes.** 200,000 short-lived values; 2,000 uniform 8,192-slot batches
   touched at slot 0; one 8,191-node block plus one promoted node; 40,000 dropped
   nodes. Sizes match the tiers the Nirqoba compiler chose for the same phases, so
   the allocation counts and payload sizes are equal.
3. **Idiomatic per language, not identical instructions.** C uses `malloc`/`free`,
   C++ `std::make_unique`, Rust `Box` and `Rc<RefCell<..>>`, Java `new`,
   JavaScript object literals, Python slotted classes. Where a runtime zero-fills
   on allocation (C++ `make_unique<T[]>`, Java arrays, Rust `vec!`), that cost is
   part of that language's idiom and is not compensated for.
4. **Release settings.** `cl /O2`, `gcc -O3`, `g++ -O3 -std=c++20`,
   `rustc -O -C target-cpu=native`, `nirqoba build -O3`, Java `-XX:+UseG1GC` with
   no warm-up JIT loop, Node with `--expose-gc`, CPython 3.12 unmodified.
5. **Warm-up excluded.** One untimed run per language precedes the timed repeats,
   because the first execution of a freshly written executable pays the virus
   scanner and the file cache, not the workload. The effect is large enough to
   matter: the Rust track's first run has measured several hundred milliseconds
   against a median of eighteen.
6. **Repeats.** Default five timed repeats; the table reports the median, and the
   JSON carries min, max and standard deviation so a reader can judge the spread.

## Validation

Every track must reproduce these four checksums, in this order:

| Phase | Value | Meaning |
|---|---|---|
| stack | 19999900000 | sum of iteration ids |
| pool | 5997000 | sum of `kind + payload` |
| arena | 12 | node 0 (5 + 1 + 2) plus the promoted leaf (4) |
| gc | 799980000 | sum of node values |

A run that exits non-zero, or prints anything else, fails the suite.

## Limitations

- One workload, four shapes. It says nothing about allocation-heavy tree
  building, string handling, or anything concurrent: no track uses threads.
- Wall clock includes start-up. A reader who wants steady-state throughput should
  run the phases in a loop inside one process; this suite deliberately does not.
- Peak RSS is the whole process image, including the runtime's own footprint.
  The JVM's 90 MiB is mostly the JVM, not the workload.
- The machine is a single Windows host; absolute milliseconds are not portable.
  Ratios against C on the same run are the portable part.
- The four phases in the Nirqoba track are shaped around three compiler defects
  (see the README). The defects do not change the allocation counts or the tier
  decisions, but they do mean the benchmark cannot exercise multi-element blocks
  or multi-field stack slots.
- C has no collector, so its "gc" phase is 40,000 `free` calls. That is the
  closest deterministic equivalent and is the baseline the other tracks are read
  against, not a claim that manual reclamation is free.
