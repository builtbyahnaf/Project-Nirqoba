# Pre-MMM Phase 6 — Ownership/GC lowering benchmark

This benchmark isolates the cost of publishing a GC root and write barrier for
stack-placed owned storage that no collection can observe.

`root_elision/main.nrqba` creates 32 `Node` blocks per `work` call. Each `Node`
has a managed `Child` field, but `work` contains no call that may collect and no
loop. `main` calls `work` 100,000 times and prints the checksum.

## Commands

```text
nirqoba build benchmarks/pre_mmm_phase6/root_elision/main.nrqba --out-dir <dir>
```

Baseline: `569a8ab` (`0.469-beta`), built in a separate worktree. Current:
`0.473-beta`.

## Results

Windows 11 x64, MSVC 14.51, `RelWithDebInfo`, wall-clock measurements. Runtime
is seven process runs; compile-time and peak RSS are five and three runs.

| Measurement | 0.469-beta | 0.473-beta | Change |
|---|---:|---:|---:|
| Runtime median | 104.13 ms | 16.63 ms | **-84.0%** |
| Compile median (`--emit=llvm`) | 56.9 ms | 48.8 ms | **-14.2%** |
| Compiler peak RSS median | 29.8 MB | 28.7 MB | **-3.7%** |
| Executable size | 94,208 B | 90,112 B | **-4.3%** |

The runtime result is deliberately large because the baseline's root and barrier
calls prevent LLVM from promoting the owned stack blocks; the current lowering
removes both calls when no collection point exists, exposing the blocks to
ordinary SROA. It is not a claim that every workload gets the same speedup. Raw
samples are in `results.json`.

## Why this is safe

`work` has no call that may collect and no loop backedge, so no collector can
snapshot its activation. The stack storage cannot escape: the stack-allocation
analysis already rejects escaping uses. A function that calls `gc.collect`, an
allocator, an unknown runtime entry, or a user function that may collect retains
the root and barrier. Existing `stack_owned_pointer_gc_roots` and
`owned_pointer_gc_roots` cover that branch.
