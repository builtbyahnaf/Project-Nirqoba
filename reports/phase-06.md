# Phase 6 Report — Memory Runtime and Windows Installer Foundation

## 1. Phase Overview

- **Phase:** 6 — Memory/runtime hardening and Windows installer foundation
- **Date:** 2026-10-31
- **Status:** Implemented — regression gate green in both presets (see §7)
- **Plan:** [`phase_plans/PHASE_6.md`](../phase_plans/PHASE_6.md)
- **Objective:** Implement Phase 6 tracing GC, explicit allocation and allocator strategies, advanced memory APIs, verification, documentation, and Windows packaging.

## 2. Scope and status

Implemented: precise non-moving mark-and-sweep GC; GC controls and statistics; typed explicit allocations; eight built-in allocator strategies; allocator ownership registry; per-allocation allocator selection; arena-backed regions; limited direct-alias use-after-free diagnostics; LIFO `defer` calls for block cleanup; reference diagnostics; explicit-allocation profiling with GC snapshots; Windows file mappings; explicit pin/unpin with scoped release using `defer`; secure allocation/zeroization; available-memory, runtime-version, logical-CPU, and allocator-kind introspection; tests/docs; and Windows package staging plus an Inno Setup definition.

Still incomplete: a complete owned/borrowed/shared type and lifetime model; native resource wrappers and cleanup during unwind; source-level custom allocator declarations; container/module/application allocator policies; complete static pointer lifetime checking; profiler fragmentation and allocator/hotspot attribution; excessive-pin diagnostics; broad runtime introspection; non-Windows runtime support; and actual installer compilation and install/uninstall validation. The runtime now exposes a native C callback custom allocator; function and block allocator selection can use `defer` to restore the prior thread-local selection.

## 3. Architecture

Managed objects use a distinct precise non-moving mark-and-sweep heap. Generated root frames and typed reference offsets identify live managed references. Explicit allocations use a separate registry that associates each allocation with its owning allocator. Allocator handles are opaque; thread-local selection applies to explicit allocations, and an optional per-allocation handle overrides it. Regions are arena allocator handles.

## 4. Safety and implementation limits

Invalid and duplicate frees are diagnosed by the runtime. The compiler detects use after supported deallocation operations through direct variable aliases tracked in a function, including simple assignments. It does not catch general raw-pointer dereferences, aliases hidden in aggregates, or interprocedural escape/free cases. This is a useful diagnostic, not a substitute for full ownership and borrowing enforcement.

The GC supports cycles, automatic collection, explicit collection, configurable thresholds, and statistics. Allocator strategies include general, pool, arena, stack, slab, buddy, free-list, and TLSF. Advanced utilities and constraints are recorded in `reports/phase-06.5.md`, `reports/phase-06.7.md`, and `reports/phase-06.9.md`.

## 5. Validation

- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Configuration RelWithDebInfo`: build passed; CTest 1/1; integration 23/23.
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1 -Configuration Release -Clean`: Release build passed with pinned MSVC 19.51/14.51.
- `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Configuration Release`: build passed; CTest 1/1; integration 23/23.
- `powershell -NoProfile -ExecutionPolicy Bypass -File windows_installer/build_scripts/stage_package.ps1 -Configuration Release`: package staging passed.
- `git diff --check`: passed.
- Release initially had a stale cache using MSVC 14.44 while including MSVC 14.51 headers. A clean configure resolved that. Release also exposed MLIR 23's deprecated `OpBuilder::create` warning under `/WX`; MSVC warning C4996 is suppressed while other warnings remain errors.
- `ISCC.exe` is unavailable here, so the Inno Setup script was not compiled and the installer was not installed or uninstalled.

## 6. Performance and platform

No benchmark or sanitizer run was performed. The runtime implementation remains Windows-specific, and no relative performance claims are made.

## 7. Acceptance snapshot

| Requirement | Status |
|---|---|
| Precise tracing GC, cycles, controls, statistics | Implemented; current tests pass |
| Eight built-in allocator strategies | Implemented; basic and unit coverage present |
| Per-allocation override and arena regions | Implemented |
| Use-after-free prevention | Partial; direct aliases and same-function operations only |
| Lexical cleanup calls (normal exit, return, break, continue) | Implemented; tested |
| Full owned/borrowed/shared model and native resource cleanup | Incomplete |
| Native custom allocator callbacks | Implemented and unit tested; Nirqoba declarations and container/module/application selection remain incomplete |
| Phase 6.9 utilities | Partial; see phase-06.9 report |
| Release and RelWithDebInfo compiler/unit/integration suite | PASS; build and CTest 1/1; 23/23 integration each |
| Package stage | PASS; toolchain paths remain tied to build host |
| Installer compilation and installation validation | Not run; ISCC unavailable |

## 8. Verdict

**Phase 6 is not complete.** The GC, allocator, compiler integration, advanced utility subset, and package staging build and pass the local suites. The remaining requirements above need implementation before claiming full Phase 6 acceptance.
