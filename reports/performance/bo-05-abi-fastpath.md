# BO-05 — Debug/production ABI identity and the fast-path option (step 3 of 4)

**Status:** complete — investigation only, no production change
**Version:** 0.294-beta; production (`NRQ_RUNTIME_DEBUG_ENABLED=0`) and
development (`NRQ_RUNTIME_DEBUG_ENABLED=1`) configurations
**Labels:** MEASURED / SOURCE / PROJECTION. No HYPOTHESIS is used as evidence.

## 1. Question

Two deliverables remain from the roadmap: whether the debug and production
runtimes present the same ABI to generated code, and whether a narrow
compiler-emitted fast path plus a runtime slow path would be justified. This
step answers both from the current tree; it does not implement a fast path.

## 2. The ABI surface (SOURCE)

Generated code calls only `extern "C"` entry points declared in
`runtime/include/nirqoba/runtime/gc.h`. No C++ class, template, exception, or
standard-library type crosses the boundary. The entries used on the Stress 02
build path:

```llvm
declare i64  @nrq_gc_root_frame_open(ptr, i64, ptr) local_unnamed_addr #2
declare void @nrq_gc_root_frame_end(i64)            local_unnamed_addr #3
declare void @nrq_gc_root_store(ptr, ptr)           local_unnamed_addr #2
declare ptr  @nrq_gc_pin_transfer(ptr)              local_unnamed_addr #2
declare void @nrq_gc_unpin(ptr captures(none))      local_unnamed_addr #3
```

with the BO-01 memory-effect groups unchanged:

```llvm
attributes #2 = { nounwind memory(readwrite, errnomem: none, target_mem: none) }
attributes #3 = { mustprogress nounwind willreturn memory(readwrite, argmem: none, ...) }
```

The boundary contract is therefore: fixed C prototypes, fixed `uint64_t` frame
marker, fixed `void*` slot/value pointers, and caller-owned descriptor tables.

## 3. Debug and production are the same ABI (MEASURED)

### 3.1 Symbol inventory

The two archives were enumerated with the pinned `llvm-nm`/driver symbol dump
(`build/bo05/{prod,debug}_symbols.txt`):

| archive | unique symbols |
| --- | ---: |
| `nirqoba_runtime.lib` | 460 |
| `nirqoba_runtime_debug.lib` | 477 |

After removing the 13 archive-member path labels that appear only because the
two libraries are enumerated separately, the real differences are:

- **Debug-only (18):** the ten public observation entries
  `nrq_gc_debug_{declare_site, describe_site, drain_events, drain_tags,
  dropped_events, dropped_tags, event_mask, set_event_mask, snapshot_roots,
  tag_allocation}`, seven incidental standard-library helpers pulled in by the
  event recorder (`call_once`, `_Init_once_completer`, `steady_clock::now`,
  `debug_record_event`, `debug_record_tag`, the event-mask atomic, and one
  `_Atomic_address_as` instantiation), and the marker
  `nrq_runtime_abi_v1_development`.
- **Production-only (1):** the marker `nrq_runtime_abi_v1_production`.

Every symbol generated code actually calls is present in **both** archives with
the same name. Nothing on the root-protocol surface is debug-only.

### 3.2 Generated IR identity

`build/bo05/02_debug_opt.ll` was produced from the same Stress 02 source with
`--debug-enabled --emit=llvm -O3`. A line diff against `02_current_opt.ll`
(`02_current` = production) yields only:

- one debug-only string literal (`__nrq_strlit_0`, the allocation site path);
- three added declarations: the development marker,
  `nrq_gc_debug_tag_allocation`, `nrq_gc_debug_declare_site`;
- three added calls: `declare_site`, `tag_allocation`, and the development
  marker;
- the production marker declaration/call replaced by the development marker.

The five root-protocol declarations, their `#2`/`#3` attribute groups, and every
other instruction are **line-for-line identical**. Debug mode adds observation;
it does not reinterpret the frame protocol.

### 3.3 Machine-code identity

Disassembling `gc.cpp.obj` from both archives
(`build/bo05/gc_{prod,debug}.asm`) and comparing the shared entries:

| entry | production vs development |
| --- | --- |
| `nrq_gc_root_store` | byte-identical |
| `nrq_gc_pin_transfer` | byte-identical |
| `nrq_gc_unpin` | byte-identical |
| `nrq_gc_root_frame_open` | one instruction differs: `xor r9d,r9d` vs `mov r9b,1` (the `validate` flag) |
| `nrq_gc_root_frame_end` | same ABI and normal path; debug adds a saved-register prologue and an event-emit block before the barrier |

`frame_open` passes `validate = nirq::runtime_debug_enabled`
(`runtime/src/gc.cpp:3271-3272`), which is the single intentional difference:
production skips descriptor re-validation; development records it. The frame
marker, slot array, descriptor pointer, return type, and calling convention are
unchanged in both.

**Conclusion:** debug and production are ABI-identical on the surface generated
code uses. They differ only by observation and by the validation flag, which is
exactly the contract stated in `runtime/CMakeLists.txt`.

## 4. What a compiler-emitted fast path would have to do

The measured entry cost (step 1) lives in the runtime bodies, not in call/return
(≈0.6 ns). A fast path that only replaces the call with inline code would
therefore save at most a few nanoseconds per entry. To capture the 70–120 ns
`frame_open + frame_end` cost, the compiler would have to inline the bodies:

- read the thread-local `thread_shadow_stack` pointer (`gc.cpp:830`) and handle
  the null/ensure path (`ensure_shadow_stack`, `3245`);
- push a `ShadowStack::Frame{marker, root_depth}` onto a C++ `std::vector`,
  including marker overflow and allocation-failure handling (`3241-3256`);
- reserve root capacity, build per-root metadata, and publish slots
  (`register_frame_roots`, `2950-3030`);
- on end, validate LIFO/marker/depth, pop, emit the debug event, and run the
  fast-barrier/mutex protocol while shrinking `stack.roots` (`3276-3309`);
- keep every one of those steps in lockstep with the two runtime configurations.

That is not a narrow fast path. It publishes the runtime's private layout,
vector invariants, and barrier protocol into generated code, and it splits one
safety contract between two owners (compiler and runtime). It would also make
debug/production codegen diverge, because the debug path records events and
re-validates descriptors.

A genuinely narrow option exists at the *runtime* level, not the compiler level:
the production path already skips descriptor validation, and a validated-
descriptor cache already exists (`thread_validated_descriptors`, `2967-2997`).
Caching or flattening the remaining frame bookkeeping would be a runtime
optimisation for a later stage; it does not require touching the archive
boundary, and it is outside BO-05's report-only authorization.

## 5. Is a fast path justified now?

**No.** Three independent reasons:

1. **Ceiling.** The measured pure-boundary cost is ≈0.6 ns per entry; the two
   frame entries together are ≈1.2 ns of ≈284 ns/node (≈0.4%). Even a perfect
   call-elimination fast path would not be visible at whole-program scale.
2. **Attribution gap.** No measurement attributes `BUILD_MS` to frame open/end
   versus allocation or collection; the probe runs with GC disabled. Acting on
   the probe alone would be a PROJECTION, not evidence.
3. **Coupling.** Inlining the bodies to reach the real cost would expose runtime
   internals, duplicate safety logic, and break the clean archive boundary that
   the current design deliberately preserves. The roadmap forbids bundling such
   an architecture change into this work.

## 6. Limitations

- The probe is single-threaded and GC-disabled; collector-active paths are not
  sampled.
- No fast-path prototype was built, because implementation is not authorized;
  the ceiling argument uses measured call cost only.
- Machine-code identity was compared on `gc.cpp.obj` from the two archives; other
  runtime translation units have the same observation-only split but were not
  byte-compared individually.

## 7. Verdict for this step

Debug/production ABI identity holds: same symbols, same prototypes, same
attribute groups, and identical machine code for three of the five hot entries,
with the other two differing only by the documented debug flag and event
recording. A compiler-emitted fast path is **not justified** by the measured
boundary cost, and the version that would be worth measuring would dissolve the
archive boundary rather than narrow it. Keep the current boundary; if the frame
body is ever shown to dominate end-to-end, optimise it inside the runtime first.
