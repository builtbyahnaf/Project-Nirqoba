# BO-05 — Runtime/compiler boundary audit (step 1 of 4)

**Status:** complete — investigation only, no production change
**Version:** 0.294-beta, production configuration (`NRQ_RUNTIME_DEBUG_ENABLED=0`)
**Environment:** Windows x64; MSVC toolset 14.51.36231 (cl 19.51); LLVM/MLIR 23.1.4;
`lld-link` from `toolchain/llvm-23.1.4/bin`; runtime built `RelWithDebInfo`
(`/O2 /Ob2`).
**Labels:** MEASURED / SOURCE / PROJECTION. No HYPOTHESIS is used as evidence.

## 1. Question

BO-01/02 removed work *inside* the runtime and fused the frame-open pair. This
step asks what the call boundary itself now costs: how many runtime entries
remain on the Stress 02 build path, what they cost per activation, and how much
prologue/epilogue and register pressure the boundary forces on generated code.

The roadmap's authorization is a report, not a change: *"No LTO implementation is
authorized here."*

## 2. Current archive boundary (SOURCE)

- User code is optimised as one LLVM module, then linked to a prebuilt MSVC static
  archive (`nirqoba_runtime.lib`) by `lld-link`; the archive is not bitcode and the
  final link does not use `/LTCG`.
- `compiler/lib/Driver/CompilationPipeline.cpp` selects the library by runtime
  configuration (`runtime_library_name`) and passes it to the linker at
  `CompilationPipeline.cpp:387-440`.
- `compiler/lib/Driver/LinkerEnvironment.cpp:110-113` prefers `lld-link` and falls
  back to MSVC `link.exe`.
- No `clang`/`clang-cl` driver ships in the pinned SDK; the only clang-family
  binary is `clang-offload-packager.exe`.
- Consequences: generated code can only call across the boundary; it cannot
  inline runtime bodies, and the runtime cannot see user modules.

## 3. Fresh IR and assembly (MEASURED)

Regenerated from current HEAD (`269f1de`) with the shipped driver:

| artifact | file |
| --- | --- |
| Stress 02 raw IR | `build/bo05/02_current_raw.ll` |
| Stress 02 `-O3` IR | `build/bo05/02_current_opt.ll` |
| Stress 02 assembly (`llc -O2`) | `build/bo05/02_current.s` |
| Stress 03 raw / opt / asm | `build/bo05/03_current_{raw,opt}.ll`, `03_current.s` |

`build_tree` in `02_current_raw.ll` has **11 call sites**: 9 runtime calls and 2
recursive self-calls. In source order:

```
frame_open, allocate_array_typed_rooted, root_store,
build_tree, unpin, root_store, build_tree, unpin, root_store,
pin_transfer, frame_end
```

The leaf path is **5 runtime calls**: `frame_open`, `allocate_array_typed_rooted`,
`root_store`, `pin_transfer`, `frame_end`. This matches the BO-02 call-site map;
no new boundary entry has appeared.

`02_current.s` contains the same 11 calls. Prologue:

```
pushq %r15 ; pushq %r14 ; pushq %rsi ; pushq %rdi ; pushq %rbx
subq  $80, %rsp
```

Epilogue is symmetric (`addq $80,%rsp` plus five `popq`), giving a 120-byte
frame including the saves. There are no spill/reload pairs in the body. Two
callee-saved registers are live across the whole function because they hold the
addresses of the two published root slots (`leaq 56(%rsp), %r14`;
`leaq 48(%rsp), %r15`), which must survive every call. That is register pressure
*induced by the boundary*, not an avoidable spill: the addresses are loop/frame
invariant and would otherwise be recomputed after each call.

## 4. Call-boundary cost (MEASURED)

**Method.** `build/bo05/boundary_probe.cpp` links the shipped production archive
exactly as generated code does (external calls, no LTO) and times each protocol
entry with `steady_clock`, 10% warm-up, GC disabled so no collection perturbs the
loop. `build/bo05/boundary_noop.cpp` provides a same-signature external function
whose body is empty, isolating call/return cost from runtime logic. Values below
are the **median of three alternating runs**; the min–max range is shown because
this host is noisy.

| probe | median ns/op | range |
| --- | ---: | ---: |
| empty external call | 0.58 | 0.51–0.63 |
| `frame_open + frame_end` (2 slots) | 87.4 | 73.3–117.7 |
| `frame_begin + frame_end` | 29.1 | 21.1–34.3 |
| `begin + register + end` (split pair) | 119.9 | 73.1–156.9 |
| `root_store` (alternating value) | 3.5 | 2.3–5.8 |
| `pin_transfer + unpin` | 33.0 | 25.6–47.3 |
| leaf protocol, no allocation | 175.2 | 110.9–232.1 |
| node protocol, no allocation | 214.7 | 136.6–286.9 |

**Interpretation.**

- The pure boundary is the empty-call row: **≈0.6 ns per entry**. At 9 runtime
  entries per internal node, call/return is ≈5 ns of a node; the boundary is not
  where the measured time goes.
- The expensive rows are runtime *entry bodies*: descriptor registration and
  marker bookkeeping in `frame_open` (`runtime/src/gc.cpp:3268-3274`,
  `3241-3256`), LIFO validation and the root-stack barrier in `frame_end`
  (`3276-3309`), and pin bookkeeping in `pin_transfer`/`unpin`.
- `root_store` is close to a boundary-only cost (~3 ns including its own logic),
  so the compiler-side protocol around it is already thin.

**Whole-program scale, not attribution.** Stress 02 reports `BUILD_MS`
445, 668, 595, 570, 598 (median 595) for 2,097,151 nodes, i.e. ≈284 ns/node
(MEASURED). The node probe is in the same order of magnitude, but the probe runs
with GC disabled and excludes allocation; the two numbers must not be subtracted
to claim an attributable share. PROJECTION: the boundary alone is a single-digit
nanosecond per-node component.

## 5. What this step does not establish

- No whole-program attribution of `BUILD_MS` to individual entries; the probe
  isolates entry cost, not end-to-end contribution.
- The probe is single-threaded and GC-disabled; collector-active paths
  (`begin_fast_barrier` failure, mutex fallback) are not sampled.
- Host variance is large (up to ~2× between runs on the same binary); only the
  median/range ordering is used, never a single run.
- This step says nothing about LTO, which is step 2, or ABI parity, which is
  step 3.

## 6. Interim conclusion

The archive boundary is real but cheap per call. The remaining per-node cost is
runtime entry logic, not call/return, and the generated function carries only the
register pressure required to keep published root-slot addresses live across the
nine calls. On this evidence there is no boundary bottleneck that would justify
changing the archive/LTO architecture; steps 2–4 test the alternatives explicitly.
