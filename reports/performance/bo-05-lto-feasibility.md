# BO-05 — LTO feasibility with the pinned toolchain (step 2 of 4)

**Status:** complete — investigation only, no production change
**Version:** 0.294-beta
**Environment:** Windows x64; LLVM/MLIR 23.1.4 (`msvc` ABI, noassert, dynamic
release CRT); MSVC toolset 14.51.36231; `lld-link` from
`toolchain/llvm-23.1.4/bin`; MSVC `link.exe` fallback from the pinned toolset.
**Labels:** MEASURED / SOURCE / PROJECTION. No HYPOTHESIS is used as evidence.

## 1. Question and authorization limit

The roadmap asks whether whole-program optimisation is feasible with the pinned
MSVC/LLVM/lld combination, and what it would cost in code size and compile/link
time. It explicitly does **not** authorise enabling LTO: *"No LTO implementation
is authorized here."* Every experiment below is out of tree, under the
gitignored `build/bo05/`, and no repository build file was changed.

## 2. Toolchain facts (SOURCE)

- `toolchain/versions.lock.json` pins LLVM 23.1.4 with `abi: msvc`; the release
  ships `lld-link.exe` but no clang driver. The only clang-family executable in
  `toolchain/llvm-23.1.4/bin` is `clang-offload-packager.exe`.
- `toolchain/toolchain.json` records `linker = lld-link.exe` and
  `linkerFallback = ...\MSVC\14.51.36231\...\link.exe`; the driver prefers
  `lld-link` (`compiler/lib/Driver/LinkerEnvironment.cpp:110-113`).
- Consequence: the project has **no tool that can consume MSVC `/GL` objects into
  native code except MSVC `link.exe /LTCG`**. `lld-link` has no LTO backend for
  MSVC's proprietary `/GL` format, and there is no clang to emit LLVM bitcode.

## 3. Experiment design

Out-of-tree probe, one source of truth for both configurations:

1. Compile the 13 runtime sources twice: native (`/O2 /Ob2 /DNDEBUG`) and `/GL`.
2. Compile the same `boundary_probe.cpp` and `boundary_noop.cpp` the same way.
3. Link native with `link.exe` (no `/LTCG`) and `/GL` objects with
   `link.exe /LTCG`.
4. Also attempt the production linker (`lld-link`) against the `/GL` objects.
5. Compare code size, object size, per-TU compile time, link time, and the probe
   timings of the resulting executables.

Artifacts: `build/bo05/{lto_objs,nolto_objs}`, `boundary_lto.exe`,
`boundary_nolto_msvc.exe`, `boundary_*.map`, `measure_link.ps1`,
`measure_compile.ps1`.

## 4. Feasibility result (MEASURED)

`lld-link` rejects every `/GL` object:

```
lld-link: error: <obj>: is not a native COFF file. Recompile without /GL
```

This is not a flag that can be worked around: `/GL` objects are MSVC's
proprietary intermediate format, not COFF. With the pinned toolchain the only
working path is `link.exe /LTCG`, i.e. the fallback linker, not the one the
driver prefers and documents. Enabling LTO would therefore change the linker
identity of every Nirqoba link, not just add a flag.

`link.exe /LTCG` itself succeeds (exit 0), so the experiment can still quantify
the cost.

## 5. Code size (MEASURED, same linker)

`boundary_nolto_msvc.exe` vs `boundary_lto.exe`, both linked by the same
`link.exe`:

| section | native | `/GL` + `/LTCG` | delta |
| --- | ---: | ---: | ---: |
| `.text` | 55,192 | 57,248 | **+3.7%** |
| `.rdata` | 30,700 | 30,050 | −2.1% |
| `.data` | 1,536 | 1,536 | 0 |
| `.pdata` | 4,752 | 4,512 | −5.0% |
| executable file | 94,208 | 95,232 | **+1.1%** |

LTO did not shrink the binary. `.text` grew, consistent with cross-module
inlining/reordering that duplicates small bodies and changes layout; `.rdata` and
`.pdata` shrank slightly. PROJECTION: a larger program may land on either side of
this few-percent band; there is no evidence for a material size win.

## 6. Compile time and object size (MEASURED)

Per-TU `gc.cpp`, three interleaved runs, same flags except `/GL`:

| configuration | compile ms (3 runs) | median | object bytes |
| --- | --- | ---: | ---: |
| native `/O2 /Ob2` | 2,198 / 3,472 / 3,331 | 3,331 | 1,064,803 |
| `/GL` | 2,041 / 2,164 / 1,955 | 2,041 | 4,725,970 |

Per-TU compile was **not slower** with `/GL` here; `/GL` defers optimisation to
link time. The cost moves to the object files: the full object set is
**14,747,182 bytes (14.06 MiB) with `/GL` vs 3,822,309 bytes (3.65 MiB) native,
a 3.86× increase**. PROJECTION: the extra I/O and link-time code generation
dominate the saved front-end time on a full build; the earlier single-run
full-build numbers (11.7 s `/GL` vs 16.8 s native) were ordering/noise and are not
used as evidence.

## 7. Link time (MEASURED)

Three interleaved links of the probe, timed around the linker process only
(`measure_link.ps1`):

| link | ms (3 runs) | median |
| --- | --- | ---: |
| native `link.exe` | 58 / 83 / 86 | 83 |
| `link.exe /LTCG` | 2,893 / 2,512 / 2,511 | 2,512 |

That is **≈30× the link time** on this small probe (about 2.4 s of LTCG code
generation added). The ratio will fall on a larger program only if native link
time grows faster than LTCG code generation; directionally, LTCG is the expensive
phase. An earlier “+49%” figure was contaminated by process start-up and is
superseded by these isolated measurements.

## 8. What LTO actually optimised (MEASURED)

- The LTO map still contains **separate** `nrq_gc_root_frame_open`,
  `nrq_gc_root_frame_end`, `nrq_gc_root_store`, `nrq_gc_pin_transfer`, and
  `nrq_gc_unpin` symbols in `gc.obj`; the probe's `main` calls them directly at
  their new addresses. **LTO did not inline the root-protocol entries.**
- `nrq_boundary_noop` disappears from the LTO map, and the probe reports
  0.000 ns/op for the empty call: the only eliminated call is the trivial one.
- Probe timings, median of three alternating native/LTO runs (GC disabled):

| probe | native | `/GL` + `/LTCG` |
| --- | ---: | ---: |
| empty call | 0.58 | 0.00 (eliminated) |
| `frame_open + frame_end` | 87.4 | 89.9 |
| `frame_begin + frame_end` | 29.1 | 24.3 |
| `root_store` | 3.5 | 2.9 |
| `pin_transfer + unpin` | 33.0 | 33.2 |
| leaf protocol | 175.2 | 146.1 |
| node protocol | 214.7 | 251.4 |

Differences are within the host's run-to-run spread; the node protocol is worse
under LTO in this sample. **No runtime speedup is claimed.** An early run that
showed a ~2× LTO advantage did not reproduce under alternating runs and is
recorded here only as a caution against single-sample conclusions.

## 9. Runtime build compatibility and distribution impact

- Native objects and `/GL` objects cannot be mixed: every runtime TU and every
  user TU would have to switch together, and the linker must understand `/GL`.
- The pinned SDK's `lld-link` cannot do that; enabling LTO means either using the
  MSVC fallback linker for all links or replacing/augmenting the pinned toolchain
  with a clang driver that emits LLVM bitcode plus an LTO-capable `lld-link`.
  Both are architecture/toolchain changes, not build-flag tweaks.
- The runtime is built twice (production and debug,
  `runtime/CMakeLists.txt`). `/GL` doubles the large-object and link-time cost
  across both configurations and makes each library's objects non-portable across
  linkers.
- Debug information, PDB generation, and incremental linking would need separate
  validation under `/LTCG`; none was attempted because implementation is out of
  scope.

## 10. Verdict for this step

LTO is **not feasible with the pinned toolchain as shipped**, and the feasible
fallback (`link.exe /LTCG`) increases code size slightly, inflates objects 3.86×,
and multiplies link time by roughly 30× on the probe, while failing to inline the
hot root-protocol entries. This step recommends **no production LTO change**.
Debug/production ABI identity is examined in step 3.
