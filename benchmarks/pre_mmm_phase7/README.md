# Pre-MMM Phase 7 — Compiler temporary-resource benchmark

This benchmark isolates the first Phase 7 subsystem, compiler-owned temporary
artifacts. It compares `0.473-beta` (`ae26bc1`) with `0.477-beta` on the same
machine and build mode.

## Workload

`hello.nrqba` is built, run, and scaffolded with the normal CLI:

- **build** — `nirqoba build hello.nrqba --out-dir <unique>`
- **run** — `nirqoba run hello.nrqba --out-dir <unique>`
- **new** — `nirqoba new <unique-project-dir>`

Each compiler invocation gets a fresh directory. Baseline and current
invocations are interleaved, so a slow period cannot land entirely in one
column. Peak RSS is sampled while the compiler process is alive. Runtime is wall
time around the process and includes startup; the differences are therefore
reported with their absolute values as well as percentages.

## Command

Recreate the baseline worktree first; it is removed after verification so it
cannot be mistaken for part of the release:

```text
git worktree add --detach .phase7-baseline ae26bc1
powershell -File scripts/build.ps1 -Configuration RelWithDebInfo  # in the worktree
python benchmarks/pre_mmm_phase7/run.py \
  --baseline .phase7-baseline/build/msvc-dev/bin/nirqoba.exe \
  --current build/msvc-dev/bin/nirqoba.exe --repeats 41
```

Raw samples are in `results.json`.

## Results (41 interleaved samples, medians)

| Measurement | 0.473-beta | 0.477-beta | Change |
|---|---:|---:|---:|
| Build wall time | 99.298 ms | 102.779 ms | **+3.51%** (+3.48 ms) |
| Run wall time | 157.214 ms | 155.831 ms | **-0.88%** (-1.38 ms) |
| `new` wall time | 33.933 ms | 36.325 ms | **+7.05%** (+2.39 ms) |
| Compiler peak RSS | 30,298,112 B | 30,371,840 B | **+0.24%** (+73,728 B) |
| Artifacts left by `run` | 2 files | **0 files** | **-2** |
| Bytes left by `run` | 10,300 B | **0 B** | **-100%** |
| Files in a created project | 3 | 3 | unchanged |
| Generated executable | 9,728 B | 9,728 B | identical SHA-256 |

The executable hash is identical in both configurations
(`6d4d6ff0763c68a54d4b57ca227ac7c4674f2b8cae3f3aaaae95696515e8d7a9`), so the
artifact migration changes lifetime, not code generation.

## Reading the result

- **`run` leaves no artifacts by default.** That is the intended migration:
  the executable and object are owned only while the process is alive. The
  fallback is `--keep-artifacts`, which publishes both at their normal paths.
- **Build and `new` pay a small publication cost.** A successful build renames
  both staged files; a scaffold renames one directory. The absolute costs
  (3.48 ms and 2.39 ms) are smaller than the run-to-run spread of this
  single-machine workload, so the percentages should not be treated as a
  stable regression claim. The compiler's peak RSS change is also within one
  page-sized noise band.
- **No runtime or generated-code performance is claimed.** No runtime source,
  allocator, collector, lowering rule, or optimizer changed. The identical
  executable confirms that.
- **`new` still writes three files.** Atomic publication does not change the
  produced project; it only prevents a half-written project from being visible.

## Reproduction limits

Single Windows 11 x64 machine, MSVC 14.51, `RelWithDebInfo`. The timing includes
process startup and filesystem/antivirus effects. No sanitizer build is
available in this configuration.
