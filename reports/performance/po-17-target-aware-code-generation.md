# PO-17 - Target-Aware Code Generation

**Task:** P1 3.8 - Target-Aware Code Generation  
**Date:** 2026-11-05  
**Status:** Completed & Verified (target-selection, portable/native assembly, and
cross-target object tests passed; runtime benchmarks deferred per Section 5
policy)  
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 Debug and Release
(`msvc-dev`, `msvc-release`)

---

## 1. Problem Identified

Before this task the backend had CPU/feature flags but no model of what they
were for. `--native`, `--target-cpu`, and `--target-features` were independent
booleans and strings, and they were applied directly inside
`create_target_machine`:

- `--native --target-features=+avx2` silently merged the host feature set with
  the requested features, producing a target no real deployment machine was
  guaranteed to match.
- An unknown CPU or feature produced only an LLVM warning on stderr; the build
  still succeeded and emitted code for a different instruction set than the
  developer intended.
- `--target <triple>` did not exist. The only way to change the triple was a
  source change, and the backend never recorded which profile/CPU/features
  produced an object.
- A portable build and a native build were indistinguishable in `--emit=llvm`
  output, so a tuned binary could be shipped by mistake.

The task requires three clear profiles (portable, native, explicit), CPU
feature detection, instruction selection, SIMD availability, target-aware
optimization, ABI/data-layout correctness, and a guarantee that feature
selection matches the deployment target.

## 2. Root Cause

- Target selection was split across `CommandLineOptions`, `PipelineOptions`,
  and `OptimizationOptions` as separate fields plus a `host_cpu` boolean. There
  was no single authority for "what target is this build for".
- `--target-cpu=native` and `--native` were normalized at the wrong layer, so
  conflicts could survive until code generation.
- Validation relied on LLVM's permissive path: `MCSubtargetInfo` warns about an
  unrecognized processor or feature and continues with the previous value.
- The triple was chosen inside `ObjectEmitter` from `options.target_triple`
  (or the default), while host CPU/features were chosen independently. Nothing
  forced the CPU, features, and triple to describe the same machine.

## 3. Files Modified

- `compiler/include/nirqoba/Backend/Optimization.h`
  - Added `TargetProfile` (`Portable`, `Native`, `Explicit`), its name, and its
    parser.
  - Replaced `host_cpu` with `target_profile` in `OptimizationOptions`.
- `compiler/include/nirqoba/Backend/TargetInfo.h`,
  `compiler/lib/Backend/TargetInfo.cpp`
  - Added `TargetConfiguration`, `host_target_triple()`,
    `resolve_target_configuration()`, and `describe_target_configuration()`.
  - Resolution now owns profile inference, host detection, triple lookup, CPU
    validation, and feature-token validation.
- `compiler/include/nirqoba/CLI/CommandLine.h`,
  `compiler/lib/CLI/Driver.cpp`
  - Added `--target-profile=<p>`, `--target <triple>` / `--target-triple`.
  - Normalized `--native` and `--target-cpu=native` to the native profile.
  - Added inference: no target flags -> portable; any explicit CPU/features/
    triple -> explicit.
  - Updated build/run help text.
- `compiler/include/nirqoba/Driver/CompilationPipeline.h`,
  `compiler/lib/Driver/CompilationPipeline.cpp`
  - Carries the profile and triple through the pipeline.
  - Resolves and validates the target before optimization (including for
    `--emit=llvm-raw`).
  - Verbose output prints the resolved profile, triple, CPU, and features.
- `compiler/include/nirqoba/Backend/ObjectEmitter.h`,
  `compiler/lib/Backend/ObjectEmitter.cpp`
  - `create_target_machine` now consumes the resolved `TargetConfiguration`
    instead of re-deriving CPU/features from options.
  - Adds `nrqoba.target.profile`, `.triple`, `.cpu`, and `.features` module
    flags to the LLVM module.
  - Data layout and triple always come from the same `TargetMachine`.
- `tests/unit/target_test.cpp` (new), `tests/unit/cli_test.cpp`,
  `tests/CMakeLists.txt`
  - Unit coverage for profile parsing, resolution, host detection, conflicts,
    unknown CPUs/features, and unsupported triples.
- `tests/run_tests.py`
  - Added `assembly_check` mode: compiles an object, disassembles it with the
    packaged `llvm-objdump`, and asserts required/forbidden instruction
    markers.
- `tests/integration/positive/target_profile_*` (7 new suites)
- `tests/integration/negative/target_profile_*` (5 new suites)
- `README.md`
  - Documents the three profiles and shows portable/native/explicit usage.
- This report.

## 4. Design and Algorithm

### 4.1 Profile semantics

| Profile | Triple | CPU | Features | Intended use |
|---|---|---|---|---|
| `portable` | published default (`x86_64-pc-windows-msvc`) | `generic` | none | Distributed binaries |
| `native` | host triple | `llvm::sys::getHostCPUName()` | `llvm::sys::getHostCPUFeatures()` | Local builds only |
| `explicit` | `--target`, else default | `--target-cpu`, else `generic` | `--target-features` | Known deployment fleet |

The CLI infers a profile when the user does not name one:

```text
no target flags                         -> portable
--native or --target-cpu=native         -> native
--target / --target-cpu=<not generic> / --target-features -> explicit
```

`--target-profile` always wins over inference. The resolved profile is the
single source of truth from that point on.

### 4.2 Resolution and validation

`resolve_target_configuration()` performs these checks before any optimization
or code generation:

1. **Profile-specific conflicts.**
   - `portable` rejects a non-default triple, a CPU other than `generic`, and
     any feature string.
   - `native` rejects an explicit triple, an explicit CPU, and an explicit
     feature string because all three come from the host.
   - `explicit` accepts the requested values and fills defaults where omitted.
2. **Triple lookup.** `TargetRegistry::lookupTarget` must recognize the triple;
   otherwise the diagnostic names the triple and LLVM's reason.
3. **CPU validation.** A temporary `MCSubtargetInfo` is created with an empty
   CPU (avoiding LLVM's own warning) and `isCPUStringValid()` checks the
   requested model.
4. **Feature validation.** Each comma-separated token must start with `+` or
   `-` and must appear in `getAllProcessorFeatures()`. This turns LLVM's
   "ignoring feature" warning into a hard `NRQ5002` error.
5. **Host sanity.** Native resolution rejects an undetectable host CPU.

Because the same function produces the triple/CPU/features tuple, the optimizer
and the object emitter cannot see different targets.

### 4.3 Backend and metadata

`create_target_machine()` now receives the resolved configuration. It creates
the `TargetMachine` from that tuple, copies the machine's data layout and triple
into the LLVM module, and records the profile as module flags:

```llvm
!1 = !{i32 1, !"nrqoba.target.profile", !"portable"}
!2 = !{i32 1, !"nrqoba.target.triple", !"x86_64-pc-windows-msvc"}
!3 = !{i32 1, !"nrqoba.target.cpu", !"generic"}
!4 = !{i32 1, !"nrqoba.target.features", !""}
```

These flags make portable, native, and explicit builds auditable from
`--emit=llvm` output. They are ordinary module flags and do not affect
optimization.

### 4.4 Why LLVM still owns optimization

No custom instruction selector, vectorizer, or ABI lowering was added. The
profiles only decide which target facts LLVM receives. Once the target machine
is correct, LLVM's existing cost models, loop vectorizer, and x86 backend choose
SSE2/AVX2/AVX-512 instructions and the target ABI's stack layout.

## 5. Why the Approach Is Correct and Safe

- **Portable cannot accidentally become native.** The profile has no CPU or
  feature input; any attempt to combine it with `--target-cpu` or
  `--target-features` is rejected.
- **Native cannot be mistaken for portable.** Native resolves to the host
  triple/CPU/features, is recorded as `native`, and rejects explicit overrides
  that would make it non-native. A native object is therefore never presented
  as a universal build by this compiler.
- **Explicit cannot silently fall back.** Unknown CPUs/features and
  unrecognized triples are errors, not warnings. A typo cannot produce code for
  a different instruction set.
- **CPU/features/triple always agree.** They come from one resolved tuple, and
  the `TargetMachine` that drives both optimization and object emission uses
  that tuple directly.
- **ABI and layout follow the target.** `setDataLayout(target_machine->...)`
  and `setTargetTriple(target_machine->...)` use the same machine that emits the
  object, so calling conventions, pointer width, alignment, and vector types
  cannot disagree with the selected target.
- **No new distribution hazard.** The default remains the portable profile;
  native tuning is opt-in.

### Known limitation

Cross-target triples are supported through `--emit=llvm` and `--emit=obj` (for
example ELF output for `x86_64-unknown-linux-gnu`). Linking an executable still
uses the Windows linker path, so non-Windows triples are intentionally limited
to object/IR inspection until a cross-linker is configured.

## 6. Tests Added or Modified

### Unit tests (`target_test.cpp`, 10 new tests)

| Test | What it pins |
|---|---|
| `ParsesProfileNames` | Round-trips `portable`/`native`/`explicit`; rejects unknown names. |
| `PortableUsesPublishedBaseline` | Default resolves to the published triple, `generic`, no features. |
| `PortableAcceptsRedundantDefaultTriple` | Naming the default triple is harmless. |
| `NativeDerivesCpuAndFeaturesFromHost` | Host CPU/features are used and marked `from_host`. |
| `ExplicitUsesRequestedCpuTripleAndFeatures` | Explicit values survive resolution unchanged. |
| `ExplicitDefaultsToGenericCpuAndPublishedTriple` | Omitted explicit inputs get safe defaults. |
| `PortableRejectsTargetOverrides` | CPU, features, and non-default triple are conflicts. |
| `NativeRejectsExplicitTargetOverrides` | Triple, CPU, and features are conflicts. |
| `ExplicitRejectsUnknownCpuAndFeatures` | Unknown CPU/feature and malformed feature tokens fail. |
| `ExplicitRejectsUnsupportedTriple` | Unknown triples fail with a diagnostic. |

`cli_test.cpp` now checks profile inference, `--target`, `--target-profile`,
`--target-cpu=native`, and invalid profile spellings.

### Integration tests (12 new suites)

| Suite | Mode | What it pins |
|---|---|---|
| `positive/target_profile_portable_asm` | objdump | Portable object uses `xmm` and never `ymm`/`zmm`/VEX floating-point forms. |
| `positive/target_profile_explicit_avx2_asm` | objdump | `x86-64-v3` object uses `ymm` and no `zmm`. |
| `positive/target_profile_explicit_avx512_asm` | objdump | `znver4` object uses `zmm`. |
| `positive/target_profile_cross_target_linux` | objdump | Linux ELF object is produced and uses `ymm`, no `zmm`. |
| `positive/target_profile_metadata` | `--emit=llvm` | Portable module records `portable`/`generic` and 128-bit vectors. |
| `positive/target_profile_native_metadata` | `--emit=llvm` | Native module records `native` and a non-`generic` host CPU. |
| `positive/target_profile_explicit_ir` | `--emit=llvm` | Explicit `x86-64-v3` records its CPU and 256-bit vectors. |
| `negative/target_profile_portable_cpu_conflict` | build | Portable + named CPU fails with `NRQ5002`. |
| `negative/target_profile_native_feature_conflict` | build | Native + explicit features fails. |
| `negative/target_profile_unknown_cpu` | build | Typo CPU fails instead of warning. |
| `negative/target_profile_unknown_feature` | build | Typo feature fails instead of warning. |
| `negative/target_profile_unknown_triple` | build | Unsupported triple fails. |

The integration runner gained `assembly_check`, which object-emits, invokes the
packaged `llvm-objdump -d --no-show-raw-insn`, and applies
`expect_asm_contains` / `expect_asm_not_contains`.

## 7. Evidence

### 7.1 Portable vs native vs explicit vector width

Source: `tests/integration/positive/fp_reduction_vectorized/main.nrqba`
(`-O2`, no fast-math). The initialization loop is vectorized by LLVM; the
selected target controls the width.

```text
portable (generic):       uitofp nneg <2 x i64> ... to <2 x double>
native (alderlake):       uitofp nneg <4 x i64> ... to <4 x double>
explicit x86-64-v3:       uitofp nneg <4 x i64> ... to <4 x double>
explicit znver4:          uitofp nneg <8 x i64> ... to <8 x double>
```

This is a direct portable-vs-native-vs-explicit comparison: 128-bit SSE2,
256-bit AVX2, and 512-bit AVX-512, chosen without any source change.

### 7.2 Assembly inspection

Portable object (`--target-profile=portable --emit=obj -O2`):

```asm
movdqa  (%rip), %xmm0
movdqa  (%rip), %xmm1
movdqa  %xmm0, %xmm3
paddq   %xmm1, %xmm3
xorps   %xmm4, %xmm4
```

Explicit `x86-64-v3` object (same source, same flags):

```asm
vmovdqa       (%rip), %ymm0
vpbroadcastq  (%rip), %ymm1
vpaddq        %ymm1, %ymm0, %ymm6
vextracti128  $0x1, %ymm0, %xmm5
```

Portable output stays on the non-VEX SSE2 baseline; the explicit target uses
VEX-encoded AVX2 and `ymm`. The `znver4` test additionally requires `zmm`.

### 7.3 Recorded target metadata

Portable:

```llvm
!1 = !{i32 1, !"nrqoba.target.profile", !"portable"}
!2 = !{i32 1, !"nrqoba.target.triple", !"x86_64-pc-windows-msvc"}
!3 = !{i32 1, !"nrqoba.target.cpu", !"generic"}
!4 = !{i32 1, !"nrqoba.target.features", !""}
```

Native on the verification host (12th Gen Intel Core i5-1235U):

```llvm
!1 = !{i32 1, !"nrqoba.target.profile", !"native"}
!2 = !{i32 1, !"nrqoba.target.triple", !"x86_64-pc-windows-msvc"}
!3 = !{i32 1, !"nrqoba.target.cpu", !"alderlake"}
!4 = !{i32 1, !"nrqoba.target.features", !"+64bit,+adx,+aes,...,+avx2,..."}
```

The native feature string contains the detected host set, including `+avx2`
and explicit `-avx512*` disabling entries on this AVX2-only machine.

### 7.4 Cross-target correctness

```text
nirqoba build ... --emit=obj -O2 \
  --target-profile=explicit \
  --target x86_64-unknown-linux-gnu \
  --target-cpu=x86-64-v3
```

`llvm-objdump` reports `file format elf64-x86-64` and the disassembly contains
`ymm` registers, proving the triple, CPU, data layout, and instruction selection
all followed the explicit target. Linking remains Windows-only (see 5).

### 7.5 Diagnostics

```text
portable + --target-cpu=znver4
  error[NRQ5002]: target profile 'portable' does not allow a named CPU
  ('znver4'); use --target-profile=explicit or --native

--native + --target-features=+avx2
  error[NRQ5002]: target profile 'native' derives features from the host;
  remove --target-features='+avx2'

explicit + --target-cpu=notacpu
  error[NRQ5002]: unknown target CPU 'notacpu' for 'x86_64-pc-windows-msvc'

explicit + --target-features=+notafeature
  error[NRQ5002]: unknown target feature '+notafeature' for
  'x86_64-pc-windows-msvc'
```

No LLVM "ignoring processor/feature" warning is printed before these errors.

## 8. Verification Results

| Verification | Result | Evidence |
|---|---|---|
| Debug build | PASS | `nirqoba` and `nirqoba_unit_tests` built with `msvc-dev`. |
| Release build | PASS | `nirqoba` and `nirqoba_unit_tests` built with `msvc-release`. |
| Unit suite (Debug) | PASS | 133/133 tests. |
| Unit suite (Release) | PASS | 133/133 tests. |
| Integration suite (Debug) | PASS | 224/224 cases. |
| Integration suite (Release) | PASS | 224/224 cases. |
| Portable assembly baseline | PASS | `xmm` present; `ymm`/`zmm` absent in portable object. |
| Explicit AVX2 selection | PASS | `x86-64-v3` object contains `ymm`, no `zmm`. |
| Explicit AVX-512 selection | PASS | `znver4` object contains `zmm`. |
| Cross-target object | PASS | Linux ELF object produced and disassembled. |
| Conflict diagnostics | PASS | 5 negative suites return `NRQ5002` with precise messages. |
| Runtime benchmarks | DEFERRED | Section 5 verification requires explicit project-owner permission. |

## 9. Acceptance Criteria

| Requirement | Status |
|---|---|
| Clear portable / native / explicit target profiles | Met: profile enum, CLI, inference, docs. |
| CPU feature detection | Met: host CPU and feature map used only by `native`. |
| Instruction selection | Met: LLVM selects instructions from the resolved target. |
| SIMD availability | Met: 128-bit / 256-bit / 512-bit evidence for portable / AVX2 / AVX-512. |
| Target-specific optimization | Met: loop vectorizer width follows the target profile. |
| ABI and calling-convention correctness | Met: data layout and triple come from the same `TargetMachine`. |
| Alignment and data layout | Met: machine data layout is installed before optimization. |
| CPU feature selection matches deployment target | Met: portable rejects overrides; native uses host; explicit validates CPU/features. |
| Portable binaries not distributed as native | Met: separate profiles and recorded `nrqoba.target.*` flags. |
| Portable builds emit no unsupported instructions | Met: portable test forbids `ymm`/`zmm`/VEX forms. |
| Target-selection tests | Met: 10 unit tests and 12 integration suites. |
| Assembly inspection | Met: `llvm-objdump` assertions in the integration runner. |
| Portable-vs-native comparison | Met: IR widths and disassembly. |
| Cross-target correctness where supported | Met: Linux ELF object via `--emit=obj`. |

---

**End of report.**
