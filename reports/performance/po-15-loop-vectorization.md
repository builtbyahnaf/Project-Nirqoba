# PO-15 — Loop Optimization and SIMD Vectorization

**Task:** P0 3.6 — Loop Optimization and SIMD Vectorization
**Date:** 2026-11-05
**Status:** Completed & Verified (IR/assembly inspection and execution tests passed;
runtime benchmarks deferred per project policy)
**Compiler:** Nirqoba 0.1.0, LLVM/MLIR 23.1.4, MSVC x64 RelWithDebInfo

---

## 1. Starting Point

The task asks the compiler to expose loop structure, alignment and memory
independence to LLVM rather than to re-implement loop transformations. Before
changing anything, the emitted IR and LLVM's own remarks were inspected for the
loop shapes the language produces:

```text
int function main() {
    var values = memory.owned_array<int>(64);   // int is i64
    ... while (i < 64) { values[i] = i * 2; i += 1; }
    ... while (j < 64) { total += values[j]; j += 1; }
}
```

Findings at `-O2` before this task:

1. **LLVM already vectorizes canonical loops.** A fixed-size transform loop was
   turned into `<2 x i64>` vector stores plus a `paddq` reduction, and a
   runtime-count reduction became a full vectorized loop with
   `llvm.vector.reduce.add.v2i64`. Nothing needs to be forced in the compiler.
2. **The vector accesses were unaligned.** Promoted arrays lived in
   `alloca [64 x i64], align 8`, so LLVM emitted `store <2 x i64> ..., align 8`
   (and `align 4` for heap-accessed elements). Only the element's natural
   alignment was ever communicated.
3. **Floating-point reductions were rejected.** With strict IEEE semantics the
   vectorizer reports
   `loop not vectorized: cannot prove it is safe to reorder floating-point operations`
   and every `f64` accumulation stayed scalar.
4. **A branchy body creates a second loop exit.** In
   `if (src[j] > 10) { dst[j] = src[j] - 10; } else { dst[j] = 0; }` the shared
   cold bounds-failure block becomes an early exit, and the vectorizer reports
   `loop not vectorized: Early exit loop with store but no supported condition load`.
   The guard cannot be folded away because an owned array's length is
   `select(allocation_failed, 0, count)` and therefore is not the same SSA value
   as the loop bound.
5. **Vector width is a target property.** The default `generic` CPU selects
   128-bit registers, so the widest available i64 vector is `<2 x i64>`; the
   existing `--target-cpu` already reaches the cost model
   (`x86-64-v3` → `<4 x i64>`, `znver4` → `<8 x i64>`).

Nothing here duplicates LLVM work; every change below either makes a fact
visible to LLVM or asks LLVM for a decision it already knows how to make.

## 2. Changes

### 2.1 Optimization remarks (`--remarks`)

`compiler/lib/Backend/Optimization.cpp`, `compiler/lib/CLI/Driver.cpp`

LLVM's optimization remarks were unreachable, so "did this loop vectorize, and
if not why" could only be answered by running `opt` by hand on printed IR. The
backend now installs a `llvm::DiagnosticHandler` that enables pass, missed and
analysis remarks, and the driver exposes them:

```text
--remarks               all remarks
--remarks=missed        only declined/analysis remarks
--remarks=loop-vectorize  remarks from passes whose name matches
```

Remarks are diagnostics only: they do not alter the module, and the handler is
installed per compilation unit (one `LLVMContext` per compilation).

### 2.2 Aligned stack slots for SIMD accesses

`compiler/lib/Lowering/TirToMlir.cpp`

Promoted owned arrays and `memory.allocate` buffers get an alignment floor of
16 bytes (one SSE2 vector register) instead of their element's natural
alignment. A stricter `memory.owned_array_aligned` request still wins, and
buffers smaller than a vector register keep their natural alignment.

Before / after for the same program:

```text
-  %0 = alloca [64 x i64], align 8
-  store <2 x i64> <i64 0, i64 2>, ptr %0, align 8
+  %0 = alloca [64 x i64], align 16
+  store <2 x i64> <i64 0, i64 2>, ptr %0, align 16
```

Static assembly inspection of the emitted object (`llvm-objdump -d`, 89
instructions for the whole program):

```text
64  movaps     (aligned 16-byte moves)
 2  movdqa
14  paddq      (packed 64-bit adds from the reduction)
```

### 2.3 Opt-in floating-point reassociation (`--fast-math`)

`compiler/lib/Lowering/TirToMlir.cpp`, driver/pipeline plumbing

`--fast-math` marks floating-point arithmetic with `reassoc` and `contract` in
the LLVM dialect. This is the flag that lets the vectorizer reassociate an
accumulation; the fastmath flags are passed through the op builders because MLIR
stores them in op properties (a plain `fastmath` attribute is printed but
ignored by the LLVM translation).

```text
--fast-math:  %24 = fadd reassoc contract <2 x double> %vec.phi, %22
              %bin.rdx = fadd reassoc contract <2 x double> %25, %24
              %27 = tail call reassoc contract double
                    @llvm.vector.reduce.fadd.v2f64(double -0.000000e+00, <2 x double> %bin.rdx)
```

The dot product of `i` with `i` for `i < 64` still prints `85344`, and the
strict-mode build emits no `reassoc` anywhere (asserted by
`fp_reduction_strict`). Default semantics are unchanged: without the flag the
vectorizer keeps declining the same loop for the same reason.

### 2.4 Semantics coverage for vectorized loops

`tests/integration/positive/{vectorized_reduction_tail,vectorized_reduction_wrap,vectorized_bounds_trap}`

Trip counts come from `args.length` so they cannot be constant-folded; each
program emits `vector.body` and therefore exercises generated SIMD code:

| test | what it locks down | result |
| --- | --- | --- |
| `vectorized_reduction_tail` | 37-element reduction, scalar epilogue | `1998` |
| `vectorized_reduction_wrap` | eight `i64::MAX - i` values wrap | `-36` |
| `vectorized_bounds_trap` | 10 iterations over an 8-element array | exit 70, bounds message |

The last one is the important guard: adding vectorization must not remove the
bounds check from vectorized code.

## 3. Verification

- `./build/msvc-dev/tests/nirqoba_unit_tests.exe` — **123/123** (new cases:
  `CliTest.ParsesRemarkModes`, `CliTest.ParsesFastMath`).
- `python tests/run_tests.py --compiler build/msvc-dev/bin/nirqoba.exe` —
  **203/203**.
- Vectorization evidence is taken from LLVM remarks, emitted IR and
  `llvm-objdump` output rather than wall-clock measurements, per the project
  policy that this task must not be benchmarked.

## 4. Limitations and Follow-ups

1. **Branchy loop bodies still lose vectorization to the bounds guard.** The
   sound fixes are loop versioning (`if (bound <= length) fast-loop else
   guarded-loop`, which is how managed runtimes handle this) or a range proof
   that also establishes non-negativity and that the guard's length equals the
   bound including the allocation-failure `select`. Eliding or hoisting the
   guard without those facts would move a trap earlier than the language
   promises, so it was deliberately not done here. `--remarks=loop-vectorize`
   now makes the remaining cases visible.
2. **Heap arrays still carry element alignment.** Requesting 16 bytes from
   `nrq_memory_allocate_array` and declaring the result alignment would let
   heap loops use aligned moves; it also changes allocation padding, so it
   belongs in its own change together with the runtime contract.
3. **Floating-point reductions are opt-in.** Reassociation changes results, so
   it stays behind `--fast-math`.
4. **Widths above 128 bits need a target.** `--target-cpu x86-64-v3`/`znver4`
   or `--native` already widen the vectors; the default stays portable.
