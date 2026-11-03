# GC cost probes

These programs back the measurements in `../GC_COST_INVESTIGATION.md`. They are
diagnostic tools, not part of the measured benchmark suite, and they do not
modify the compiler or runtime. Each links against the same runtime library used
by the benchmark executables.

## Prerequisites

Build the project so that `build/msvc-dev/lib/nrqoba_runtime.lib` exists, then
run the commands below from a Visual Studio Developer Command Prompt at the
repository root.

## Build

```bat
cl /O2 /MD /nologo /std:c++20 /EHsc ^
   /I "runtime\include" ^
   "benchmarks\stress_test_2\investigation\runtime_call_probe.cpp" ^
   /Fe:"runtime_call_probe.exe" ^
   /link "build\msvc-dev\lib\nrqoba_runtime.lib"
```

Use the same line for `traversal_probe.cpp`, `allocation_probe.cpp`, and
`layout_probe.cpp`.

## Run

```bat
runtime_call_probe.exe
traversal_probe.exe
layout_probe.exe
allocation_probe.exe alloc_raw
allocation_probe.exe alloc_rooted
allocation_probe.exe publish_each
allocation_probe.exe store_on
allocation_probe.exe store_off
allocation_probe.exe copy_on
allocation_probe.exe copy_off
allocation_probe.exe pin_unpin
allocation_probe.exe unpin_only
allocation_probe.exe field_store
```

Run each `allocation_probe.exe` mode in a fresh process; the modes intentionally
populate and reclaim the heap differently, so cross-mode reuse inside one
process would distort the measurements.

## What each probe measures

| Probe | Purpose |
|---|---|
| `runtime_call_probe.cpp` | Per-call cost of individual runtime operations and a first reconstruction of the `make_node` sequence. |
| `traversal_probe.cpp` | Raw chain/tree traversal versus faithful `sum_chain`/`sum_tree` IR reconstructions, with generational collection on and off. |
| `allocation_probe.cpp` | Raw TLAB allocation, bulk publication, per-return publication, and write/copy barriers over one million distinct objects. |
| `layout_probe.cpp` | Actual allocation stride for `Node` and `TreeNode`. |

The probes print ns/op. Absolute values depend on host state; compare variants
measured in the same process or immediately adjacent runs.
