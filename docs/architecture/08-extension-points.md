# 08 — Extension Points

## 1. Dialect & Pass Extensions

The MLIR architecture in Phase 1 establishes the permanent extension points required by ROADMAP §3.3:
- **`register_all_dialects(mlir::DialectRegistry&)`**: Future phases add domain dialects (`nrq.tensor`, `nrq.graph`, `nrq.async`, `nrq.gpu`) by registering them into this registry.
- **`build_lowering_pipeline(mlir::PassManager&)`**: Lowering passes for new operations are added sequentially to this pipeline.

## 2. TIR Extensibility

`tir::OpKind` and `nirq::TypeKind` are extensible enums. When Phase 2 introduces arithmetic expressions, control flow, and collections, new op codes and type kinds are added without modifying existing IR semantics.

## 3. Runtime Hook Points

`nrq_runtime_init` and `nrq_runtime_shutdown` provide predefined entry and exit hooks for GC initialization, thread pools, and device runtime management in later phases.
