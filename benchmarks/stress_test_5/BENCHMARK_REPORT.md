# Stress Test 5 - Cross-Language CDMM Gauntlet: Results

**Host:** Windows 11 (10.0.26300), Intel64 Family 6 Model 154, Python 3.12.7
**Compiler:** Nirqoba `0.670-beta`, `nirqoba build -O3`
**Repeats:** 5 timed runs per language after one untimed warm-up
**Validation:** every track reproduced all four checksums on every run

## Results

Median of five timed runs. Peak RSS is the process high-water mark, read from the
operating system while the process ran.

| Language | Wall clock (ms) | Peak RSS (MiB) | vs C (time) | vs C (footprint) |
|---|---|---|---|---|
| **nirqoba** | 33.8 | 13.7 | 1.88x | 1.40x |
| c | 18.0 | 9.8 | 1.00x | 1.00x |
| cpp | 23.6 | 10.4 | 1.31x | 1.06x |
| rust | 17.8 | 10.5 | 0.99x | 1.07x |
| java | 76.9 | 90.1 | 4.27x | 9.19x |
| js | 99.7 | 11.5 | 5.54x | 1.17x |
| py | 71.9 | 17.9 | 3.99x | 1.83x |

Spread (median / min / max, ms):

| Language | Wall clock | Peak RSS |
|---|---|---|
| nirqoba | 33.8 / 29.3 / 34.9 | 13.7 / 13.7 / 13.7 |
| c | 18.0 / 17.5 / 18.2 | 9.8 / 9.8 / 9.8 |
| cpp | 23.6 / 22.7 / 56.1 | 10.4 / 10.4 / 10.4 |
| rust | 17.8 / 17.6 / 18.5 | 10.5 / 10.5 / 10.5 |
| java | 76.9 / 76.3 / 81.9 | 90.1 / 90.0 / 91.4 |
| js | 99.7 / 94.7 / 105.4 | 11.5 / 11.5 / 11.5 |
| py | 71.9 / 70.3 / 76.4 | 17.9 / 17.8 / 17.9 |

## What the numbers say

**Nirqoba sits with the native languages, not with the managed ones.** 33.8 ms
against C's 18.0, C++'s 23.6 and Rust's 17.8. It is 1.88x C - not at parity, and
this report does not claim parity. It is 2.3x faster than the JVM, 3.0x faster
than V8 and 2.1x faster than CPython, while holding 13.7 MiB against the JVM's
90.1 MiB.

**Where the 15.8 ms over C goes.** Running each phase alone (five runs, same
harness) separates start-up from work:

| Phase | Nirqoba (ms) | C (ms) | Difference |
|---|---|---|---|
| stack | 18.0 | 17.6 | +0.4 |
| pool | 18.4 | 15.7 | +2.7 |
| arena | 16.3 | 15.9 | +0.4 |
| gc | 33.7 | 25.2 | +8.5 |

Each single-phase binary pays the same ~16 ms of process and runtime start-up, so
the differences are the signal. Stack promotion and the arena block are at parity
with manual allocation (+0.4 ms each, inside noise). The pool loop costs 2.7 ms
more over 2,000 reuses. The collector phase costs 8.5 ms more over 40,000
published nodes and two full collections - which is 54% of the total gap, and is
the cost of reclamation that C does not perform at all but hands to 40,000
individual `free` calls.

**Footprint is where automation pays.** 13.7 MiB peak, 1.40x C. The JVM needs
90.1 MiB to run the same workload; CPython needs 17.9; V8 needs 11.5 but takes
three times as long. The CDMM tiers do not carry per-object headers big enough to
show up here: the stack and pool phases own no collector metadata at all, and the
arena is one contiguous block.

## Tier routing - the assertion

`nirqoba build nirqoba_impl.nrqba --inspect-cdmm` reports six ordered decisions,
one per site, and the workload was shaped until every tier in the arc was
represented:

```text
cdmm: 6 allocation site(s) classified
cdmm: nirqoba_impl.nrqba:48:5  '<temporary>' ExprNode* -> arena (returned to bounded callers, arena handle checked at runtime)
cdmm: nirqoba_impl.nrqba:56:17 'v'           Vec1*     -> stack (bounded to function, promoted to the stack, loop-scoped)
cdmm: nirqoba_impl.nrqba:71:21 'batch'       Event*    -> pool  (bounded to function, repeated layout (1 sites), loop-scoped)
cdmm: nirqoba_impl.nrqba:82:16 'tree'        ExprNode* -> arena (bounded to function)
cdmm: nirqoba_impl.nrqba:99:20 'node'        CycleNode*-> gc    (cyclic object graph detected, in loop)
cdmm: nirqoba_impl.nrqba:114:16 'leaf'       ExprNode* -> arena (bounded to the caller's frame, filled by make_leaf)
```

All four tiers are present, each named with its reason: `stack` for the loop's
short-lived value, `pool` for the repeated 64 KiB layout, `arena` for the bounded
block and for the two implicit-promotion sites, and `gc` for the cycle-capable
graph in foreign storage.

## Honest reading

- Nirqoba does **not** match C. It is 1.88x C and within 1.4x of C++ on this
  workload, which is the claim the data supports.
- The GC phase is the one place a real optimization would pay: 8.5 ms of the
  15.8 ms gap. It is also the phase where the language currently cannot close the
  cycle it was written to test - `shared ptr` fields cannot be assigned after
  construction, so the graph is cycle-capable but not literally cyclic at run
  time.
- Three compiler defects shaped the workload (element-0-only blocks, stale
  multi-field stack slots, and the selection/lowering disagreement when an arena
  and a collector site share a frame). They are listed in the README; fixing them
  is compiler work, not benchmark work.
- Five repeats on one machine. The ratio, not the millisecond, is the portable
  result.
