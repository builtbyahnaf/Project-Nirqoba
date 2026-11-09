# BO-05 — Runtime/compiler boundary and LTO: consolidated investigation

**Phase identifier:** BO-05
**Title:** Runtime/compiler boundary and LTO — separate investigation
**Date:** 2026-11-09
**Status:** complete (report only)
**Objective:** determine whether the prebuilt runtime archive is a meaningful
call-boundary bottleneck, whether LTO is feasible with the pinned toolchain, and
whether a compiler-emitted fast path is warranted — **without implementing any of
them**.
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)
**Labels:** MEASURED / SOURCE / PROJECTION. No HYPOTHESIS is used as evidence.

## 1. Scope

**Required by the roadmap (`BO_ROADMAP_2.md` §7.1/§7.2):**

1. Current archive boundary.
2. LTO feasibility with pinned MSVC/LLVM/lld.
3. Code-size and compile-time impact.
4. Debug/production ABI identity.
5. A narrow compiler-emitted fast path plus runtime slow path.
6. Call-boundary cost; register spills and prologue/epilogue cost; runtime build
   compatibility; linker and distribution impact.

**Implemented:** all six as measurements and analysis, split into three
independently reviewable reports:

- `reports/performance/bo-05-boundary-audit.md`
- `reports/performance/bo-05-lto-feasibility.md`
- `reports/performance/bo-05-abi-fastpath.md`

**Intentionally excluded:** any `/GL`, `/LTCG`, ThinLTO, FullLTO, bitcode runtime,
linker change, or compiler-emitted fast path. The roadmap is explicit: *"No LTO
implementation is authorized here."* No repository source, build file, workload,
or benchmark artifact was changed.

## 2. Architecture and design (SOURCE)

User code is optimised as one LLVM module and linked by `lld-link` against a
prebuilt MSVC static archive. The runtime is built twice from the same sources
(production and development) by `runtime/CMakeLists.txt`, selected by
`--debug-enabled` in `compiler/lib/Driver/CompilationPipeline.cpp`. The driver
prefers `lld-link` and falls back to MSVC `link.exe`
(`compiler/lib/Driver/LinkerEnvironment.cpp:110-113`). The pinned SDK ships no
clang driver; the only clang-family executable is `clang-offload-packager.exe`.

This is a deliberate replaceability boundary: generated code sees fixed C
prototypes, and the runtime owns the root-stack and collector internals. Every
finding below is judged against that design, not against a hypothetical
single-module build.

## 3. Findings

### 3.1 Call-boundary cost (MEASURED)

- `build_tree` (Stress 02) has **11 call sites**: 9 runtime entries plus 2
  recursive self-calls. Leaf path: 5 runtime calls.
- Empty external call: **≈0.6 ns/entry** (median 0.58, range 0.51–0.63).
- `root_store`: ≈3.5 ns; `pin_transfer + unpin`: ≈33 ns;
  `frame_open + frame_end`: ≈87 ns; node protocol: ≈215 ns
  (medians of three alternating runs, GC disabled).
- The expensive entries are runtime **bodies**, not call/return. At 9 calls per
  internal node, call/return is ≈5 ns of a ≈284 ns/node build (Stress 02
  `BUILD_MS` median 595 ms / 2,097,151 nodes).
- **No whole-program attribution** is claimed; the probe isolates entry cost.

### 3.2 Register pressure and prologue/epilogue (MEASURED)

- `build_tree` prologue: five `pushq` (`r15, r14, rsi, rdi, rbx`) plus
  `subq $80, %rsp` = 120 bytes including saves; symmetric epilogue.
- No spill/reload pairs in the body.
- `r14`/`r15` are live across the whole function because they hold the addresses
  of the two published root slots, which must survive every call. This is the
  only boundary-induced register pressure; it is not an avoidable spill.

### 3.3 LTO feasibility (MEASURED)

- `lld-link` rejects every MSVC `/GL` object:
  `is not a native COFF file. Recompile without /GL`.
- No clang driver is pinned, so there is no path to emit LLVM bitcode. The only
  working LTO route is the fallback `link.exe /LTCG`, i.e. changing the linker
  identity of every link.
- LTO did **not** inline `nrq_gc_root_frame_open/end`, `nrq_gc_root_store`,
  `nrq_gc_pin_transfer`, or `nrq_gc_unpin`; the map still shows separate
  functions and `main` calls them directly. It eliminated only the trivial
  `nrq_boundary_noop`.
- No reproducible runtime speedup; an early ~2× sample did not survive
  alternating repeats.

### 3.4 Code size and compile/link time (MEASURED)

| metric | native | `/GL` + `/LTCG` | delta |
| --- | ---: | ---: | ---: |
| `.text` | 55,192 | 57,248 | **+3.7%** |
| executable | 94,208 | 95,232 | **+1.1%** |
| object set | 3.65 MiB | 14.06 MiB | **3.86×** |
| `gc.cpp` per-TU compile (median) | 3,331 ms | 2,041 ms | faster per TU; cost moves to link |
| probe link (median) | 83 ms | 2,512 ms | **≈30×** |

PROJECTION: the extra object I/O and link-time code generation dominate any
front-end saving on a full build. There is no code-size win.

### 3.5 Debug/production ABI identity (MEASURED)

- 460 vs 477 unique symbols; after excluding archive-member labels, the real
  difference is 18 debug-only observation symbols plus the ABI marker
  (`development` vs `production`).
- Generated IR is line-for-line identical except the ABI marker, two debug calls,
  one site string, and the debug declarations. The root-protocol declarations
  and `#2`/`#3` memory-effect groups are unchanged.
- Machine code: `root_store`, `pin_transfer`, `unpin` are byte-identical;
  `frame_open` differs by one validation flag (`xor r9d,r9d` vs `mov r9b,1`);
  `frame_end` keeps the same ABI and normal path, adding debug event recording.
- **Conclusion:** the two configurations are ABI-identical on the surface
  generated code uses. Only observation differs, matching the documented
  contract.

### 3.6 Compiler fast path vs runtime slow path (analysis)

- A call-elimination fast path saves at most ≈0.6 ns/entry (≈0.4% of a node);
  invisible at whole-program scale.
- Reaching the real 87 ns frame cost would require inlining the runtime bodies:
  TLS shadow-stack access, `std::vector` frame push, descriptor/root publication,
  LIFO validation, and the fast-barrier/mutex protocol. That publishes private
  layout into generated code, duplicates safety logic, and forces debug/
  production codegen to diverge — it dissolves the archive boundary instead of
  narrowing it.
- The narrow, safe alternative is a **runtime-side** optimisation (the production
  path already skips descriptor validation; a validated-descriptor cache already
  exists). That belongs to a later runtime stage, not BO-05.
- **Verdict:** not justified by current evidence, and not authorized here.

## 4. Modified files

| file | role |
| --- | --- |
| `reports/performance/bo-05-boundary-audit.md` | step 1: archive boundary, IR/asm, call-cost probe |
| `reports/performance/bo-05-lto-feasibility.md` | step 2: `/GL`/`/LTCG` experiment, size/time data |
| `reports/performance/bo-05-abi-fastpath.md` | step 3: ABI parity and fast-path assessment |
| `reports/performance/bo-05-investigation.md` | this consolidated verdict |

No source, build, test, workload, or benchmark file was modified. Experimental
artifacts live under gitignored `build/bo05/`.

## 5. Language/API changes

None. No syntax, runtime API, ABI, or build flag changed.

## 6. Testing

This deliverable changes documentation only; there is no code path to test, and
the suite was not re-run for BO-05. Prior stage BO-04 recorded 194/194 unit and
263/263 integration tests passing on the same runtime and compiler sources. All
BO-05 executable evidence is the measurement set above, produced from the current
HEAD (`269f1de`) and the shipped `build/msvc-dev` runtime.

## 7. Performance summary

| roadmap question | answer | label |
| --- | --- | --- |
| Call-boundary cost | ≈0.6 ns/entry; ≈5 ns/node for 9 entries | MEASURED |
| Register spills / prologue | no spills; 5 pushes + 80-byte frame; 2 slot-address registers live across calls | MEASURED |
| Code-size impact (LTO) | `.text` +3.7%, exe +1.1% | MEASURED |
| Compile-time impact (LTO) | per-TU not slower; object set 3.86×; link ≈30× | MEASURED |
| Runtime build compatibility | `lld-link` cannot consume `/GL`; no clang pinned; dual libs multiply cost | MEASURED / SOURCE |
| Debug/production parity | same ABI; observation-only differences | MEASURED |
| Linker/distribution impact | LTO requires the fallback linker or a toolchain replacement | SOURCE / PROJECTION |
| Fast path | not justified; ceiling ≈0.4% of node time | MEASURED / PROJECTION |

## 8. Known issues and limitations

- Probe timings are single-threaded, GC-disabled, and noisy (up to ~2× run to
  run). Only medians/ranges are used.
- No end-to-end attribution of `BUILD_MS` to individual runtime entries.
- LTO timing was measured on a small probe with the fallback linker, not on a
  full Nirqoba program; the direction (expensive link, larger objects, no hot
  inlining) is robust, the exact ratio is not transferable.
- No fast-path prototype was built, because implementation is not authorized;
  its rejection rests on the measured call-cost ceiling and the coupling
  argument, both stated explicitly.

## 9. Acceptance criteria

| requirement | status | evidence |
| --- | --- | --- |
| Investigate current archive boundary | PASS | §3.1, step 1 report |
| Investigate LTO feasibility (MSVC/LLVM/lld) | PASS | §3.3, step 2 report |
| Quantify code size and compile time | PASS | §3.4 |
| Verify debug/production ABI identity | PASS | §3.5, step 3 report |
| Assess narrow fast path + slow path | PASS | §3.6 |
| Quantify call cost, spills, prologue/epilogue | PASS | §3.1–3.2 |
| Quantify runtime build compatibility | PASS | §3.3–3.4 |
| Quantify linker/distribution impact | PASS | §3.4, step 2 §9 |
| No production LTO / fast-path change | PASS | no code or build file modified |
| Do not modify workloads or commit artifacts | PASS | only four report files committed |

## 10. Final verdict

**PASS** — report-only deliverable complete.

**Recommendation: accept the investigation, implement nothing.** BO-01/02 left no
meaningful call-boundary bottleneck: the boundary costs ≈0.6 ns per entry, and
the remaining per-node time is runtime entry logic. LTO is not usable with the
pinned linker, and its feasible fallback increases size, objects, and link time
without inlining the hot entries. A compiler fast path would either save
sub-nanosecond call overhead or duplicate runtime internals and break the
archive boundary. Revisit only if a future stage (a) ships a toolchain with
clang + LTO-capable `lld-link`, and (b) produces whole-program attribution
showing the frame body dominates — neither condition holds today.
