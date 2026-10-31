# Phase 4 — Collections, Structured Data, and Standard I/O

**Status:** Implementation plan  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Plan version:** 1.0  
**Scope sources:** `project_context/ROADMAP.md` §Phase 4, `project_context/SYNTAX.md` §Phase 4, `project_context/AGENTS.md`  
**Target report:** `reports/phase-04.md`  

---

## 1. Decisions this plan locks

| Decision | Choice | Rationale |
|---|---|---|
| Fixed-size array syntax | `T[N]` (e.g. `int[5]`, `f64[3]`) | Compile-time fixed length encoded directly in the type; zero allocation overhead; value semantics. |
| Fixed-size array literals | `[elem0, elem1, ...]` or `{elem0, elem1, ...}` with elements matching declared length | Unambiguous literal notation consistent with both syntax proposals in SYNTAX §4.1 and ROADMAP §Phase 4. |
| Dynamic array syntax | `T[]` (e.g. `int[]`, `string[]`) | Distinct from fixed-size arrays; runtime capacity and length; managed buffer. |
| Array indexing & bounds checking | `arr[index]` with runtime bounds check: out-of-bounds traps via `nrq_runtime_fatal("index out of bounds")` | Safe, predictable execution without memory corruption or undefined host behavior. |
| Tuples & member access | `(T1, T2, ...)` (e.g. `(int, string)`); 0-indexed member access: `tup.0`, `tup.1` | Lightweight anonymous heterogeneous groupings with compile-time known types. |
| Tuple destructuring | `let (a, b) = pair;` and `var (x, y) = coords;` | Natural unpack syntax for multiple-value returns without requiring synthetic struct definitions. |
| Struct declaration | `struct Name { Type1 field1; Type2 field2; }` | Explicit, C-family record definition establishing named types in the type system. |
| Struct instantiation | `Name { field1: val1, field2: val2 }` and positional `Name { val1, val2 }` | High readability with named initializers, concise positional construction when convenient. |
| Field access & mutation | `obj.field` for read; `obj.field = val;` for mutation if binding is `var` | Familiar dot notation; mutation strictly gated on binding mutability (`var` required). |
| String length & concatenation | `str.length()` or builtin `length(str)`; string concatenation via `+` operator | Standard ergonomic string manipulation built on native UTF-8 runtime buffers. |
| Console input | `input()` and `input(prompt)` returning a `string` | Standard console input using Win32 `ReadFile` without CRT stdio. |
| Mathematical standard functions | `math.sqrt`, `math.sin`, `math.cos`, `math.abs`, `math.floor`, `math.ceil` | Standard native hardware math operations exposed through compiler builtins/runtime. |
| Array Initialization | int[5] ages = {21, 23, 11, 34, 45};
---

## 2. Prerequisites and Environment Baseline

- **OS:** Windows 11 / Windows 10 x64.
- **Toolchain:** MSVC (v143+ toolset), Ninja, CMake >= 3.28.
- **LLVM / MLIR:** 23.1.4 SDK (`toolchain/llvm-23.1.4/`).
- **Python:** 3.10+ (for integration test runner).
- **Previous Phases:**
  - Phase 1: Native compilation pipeline & executable slice.
  - Phase 2: All scalar types, numeric suffixes, Pratt expressions, explicit casts.
  - Phase 3: Functions, parameters, recursion, scopes, `if`/`while`/`for`/`break`/`continue`.

---

## 3. Repository Layout and Changes

Files created or modified in Phase 4:

```text
compiler/
  include/nirqoba/
    Common/
      Type.h                                # Added: ArrayType (fixed & dynamic), TupleType, StructType
    Diagnostics/
      Diagnostic.h                          # Added: NRQ3025 (unknown struct field), NRQ3026 (index out of bounds), NRQ3027 (destructuring arity)
    Frontend/
      Ast.h                                 # Added: StructDecl, ArrayLiteralExpr, IndexExpr, MemberAccessExpr, TupleLiteralExpr, DestructuringStmt
      Parser.h                              # Added: parse_struct_decl, parse_array_literal, parse_tuple_or_grouping, parse_index_or_member
    Sema/
      SymbolTable.h                         # Struct definition catalog and field lookup tables
      TypeChecker.h                         # Struct, array, tuple type-checking; bounds checks; destructuring verification
    TIR/
      IR.h                                  # Array/tuple allocation, GEP/element extraction, struct field load/store
      Builder.h                             # Lowers array indexing, struct instantiation, tuple packing/unpacking
      Verifier.h                            # Verifies structured types and memory layouts
      Printing.h                            # Formatted output for structured types
    Lowering/
      TirToMlir.h, TirToMlir.cpp            # Lowers struct types (llvm.struct), GEP (llvm.getelementptr), memory copies
runtime/
  include/nirqoba/runtime/
    io.h                                    # Added: nrq_read_line (input primitive)
    math.h                                  # Added: nrq_sqrt, nrq_sin, nrq_cos, nrq_abs, nrq_floor, nrq_ceil
  src/
    io_windows.cpp                          # Implements nrq_read_line using Win32 ReadFile
    math.cpp                                # Implements math functions
tests/
  unit/
    parser_test.cpp                         # Tests for struct decls, array/tuple literals, indexing syntax
    sema_test.cpp                           # Struct field resolution, array type matching, destructuring checks
    tir_test.cpp                            # Indexing bounds guards, GEP generation
  integration/
    positive/
      arrays/                               # Fixed-size array passing, indexing, return
      tuples/                               # Tuple return, destructuring
      structs/                              # Struct creation, field access, mutation
      input_output/                         # Interactive console input & output
    negative/
      invalid_field/                        # Accessing non-existent field rejected
      type_mismatch_index/                  # Non-integer index rejected
      destructure_mismatch/                 # Arity mismatch on tuple unpack
reports/
  phase-04.md                               # Complete technical report
```

---

## 4. Architecture and Stage Details

### 4.1 Type System Expansion

In `compiler/include/nirqoba/Common/Type.h`:
1. **Fixed-Size Array Type:**
   - Base element type `Type element_type`.
   - Compile-time length `size_t length`.
   - String representation: `int[5]`, `f64[3]`.
2. **Dynamic Array Type:**
   - Base element type `Type element_type`.
   - String representation: `int[]`, `string[]`.
3. **Tuple Type:**
   - Element types list `std::vector<Type> element_types`.
   - String representation: `(int, string, bool)`.
4. **Struct Type:**
   - Struct identifier `std::string name`.
   - Fields list `std::vector<std::pair<std::string, Type>> fields`.
   - String representation: `struct Point`.

### 4.2 Grammar & Parser

1. **Struct Declarations (Top-Level):**
   ```text
   struct_decl   := 'struct' IDENT '{' (type IDENT ';')* '}'
   ```
2. **Type Annotations:**
   ```text
   type          := scalar_type
                  | type '[' INT_LITERAL? ']'
                  | '(' type (',' type)* ')'
                  | IDENT
   ```
3. **Array & Tuple Expressions:**
   ```text
   primary       := '{' (expr (',' expr)*)? '}'      // Array literal
                  | '(' expr ',' (expr (',' expr)*)? ')' // Tuple literal
                  | IDENT '{' struct_inits? '}'        // Struct instantiation
                  | ...
   ```
4. **Postfix Indexing & Member Access:**
   ```text
   postfix       := primary ( '[' expr ']' | '.' IDENT | '.' INT_LITERAL )*
   ```
5. **Tuple / Array Destructuring Assignment:**
   ```text
   destructure   := ('let' | 'var') '(' IDENT (',' IDENT)* ')' '=' expr ';'
                  | ('let' | 'var') '[' IDENT (',' IDENT)* ']' '=' expr ';'
   ```

### 4.3 Semantic Analysis (Sema)

1. **Struct Registration:**
   - Top-level pass collects all `struct` definitions, validates field uniqueness (`NRQ3012`), and registers memory layout.
2. **Array Validation:**
   - Elements of an array literal must be homogeneous; mismatched elements produce `NRQ3015`.
   - Fixed-size array assignment verifies exact element count match.
   - Array index expressions must be integer (`NRQ3015`).
   - Constant index out of bounds produces compile-time warning or error (`NRQ3026`).
3. **Tuple Validation:**
   - Member access `tuple.N` validates `0 <= N < tuple.size()` (`NRQ3026`).
   - Tuple destructuring validates exact arity match between target pattern and tuple expression (`NRQ3027`).
4. **Struct Field Access:**
   - Validates field existence on the struct type (`NRQ3025`).
   - Field mutation (`struct.field = val`) validates that the struct instance is declared with `var` (`NRQ3014`).

### 4.4 Typed IR (TIR)

Extend TIR with:
1. `tir::OpKind::GetElementPtr`: computes pointer to array element or struct/tuple member.
2. `tir::OpKind::ArrayBoundsCheck`: guards runtime array indexing with condition `0 <= index < length`, branching to a runtime fatal abort on failure.
3. `tir::OpKind::StructInit`: constructs an aggregate in a stack slot or registers.
4. `tir::OpKind::TuplePack` / `tir::OpKind::TupleExtract`: pack/unpack operations for multi-value returns.

### 4.5 MLIR Lowering

1. **Aggregate Types:**
   - Fixed-size arrays mapped to `!llvm.array<N x elem_type>`.
   - Structs mapped to identified/literal `!llvm.struct<(field1_type, field2_type, ...)>`.
   - Tuples mapped to anonymous `!llvm.struct<(type0, type1, ...)>`.
2. **Access Lowering:**
   - Element indexing and field access lowered to `llvm.getelementptr` followed by `llvm.load` or `llvm.store`.
3. **Runtime Function Lowering:**
   - `input()` lowered to call `@nrq_read_line()`.
   - Math functions lowered to `@nrq_sqrt`, `@nrq_sin`, etc., or native LLVM intrinsics (`llvm.sqrt`).

### 4.6 Runtime Library Extensions

Add C ABI functions to `runtime/include/nirqoba/runtime/io.h` and `math.h`:
```c
// Console input: reads a line from stdin using Win32 ReadFile, returning UTF-8 buffer.
int32_t nrq_read_line(char* out_buf, size_t max_bytes, size_t* out_bytes_read);

// Math primitives:
double nrq_sqrt(double val);
double nrq_sin(double val);
double nrq_cos(double val);
double nrq_abs(double val);
double nrq_floor(double val);
double nrq_ceil(double val);
```

---

## 5. Testing Plan

### 5.1 Unit Tests (`nirqoba_unit_tests`)
1. **Parser Tests:**
   - Struct definitions and nested field parsing.
   - Array literals (`[1, 2, 3]`), 2D array literals (`[[1, 2], [3, 4]]`).
   - Tuple literals (`(10, "text")`) and tuple element indexing (`tup.0`).
   - Destructuring syntax `let (a, b) = tup;`.
2. **Sema Tests:**
   - Correct typing of fixed arrays and struct fields.
   - Rejection of invalid field access (`NRQ3025`).
   - Rejection of non-integer array index (`NRQ3015`).
   - Arity mismatch in tuple destructuring (`NRQ3027`).
3. **TIR Tests:**
   - Bounds-check op insertion for dynamic/fixed arrays.
   - GEP op verification for struct field offset calculation.

### 5.2 Integration Tests
1. **Positive Tests:**
   - Array sorting / sum iteration using loops.
   - Returning tuples from functions (e.g. `get_monitor_pixels()` returning `(1920, 1080)`).
   - Point/Vector struct operations with field mutation.
   - Standard math evaluations (`math.sqrt(16.0) == 4.0`).
2. **Negative Tests:**
   - Runtime trap on array index out of bounds.
   - Mutation of field in immutable (`let`) struct.

---

## 6. Milestones & Execution Strategy

- **Milestone 1:** Type System & AST Expansion (Fixed arrays, dynamic arrays, tuples, structs).
- **Milestone 2:** Parser & Grammar (Struct declarations, array/tuple literals, indexing, destructuring).
- **Milestone 3:** Semantic Analysis (Aggregate type checking, field validation, bounds checks).
- **Milestone 4:** TIR Representation & Lowering (GEP, bounds check, aggregate allocation).
- **Milestone 5:** MLIR Lowering & Runtime I/O (`llvm.struct`, `llvm.getelementptr`, console input `nrq_read_line`, math runtime).
- **Milestone 6:** Unit and Integration Tests.
- **Milestone 7:** Documentation & Phase 4 Report (`reports/phase-04.md`).
