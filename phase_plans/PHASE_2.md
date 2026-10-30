# Phase 2 — Core Syntax, Scalar Types, Variables, and Expressions

**Status:** Implementation plan  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Plan version:** 1.0  
**Scope sources:** `project_context/ROADMAP.md` §Phase 2, `project_context/SYNTAX.md` §Phase 2, `project_context/AGENTS.md`  
**Target report:** `reports/phase-02.md`  

---

## 1. Decisions this plan locks

| Decision | Choice | Rationale |
|---|---|---|
| Primitive integer types | `i8`, `i16`, `i32`, `i64`, `int` (= `i64`), `u8`, `u16`, `u32`, `u64`, `byte` (= `u8`) | Exact widths eliminate platform ambiguity. `int` is locked to `i64` per SYNTAX §2.1 and Phase 1 decision ADR 0001. |
| Floating-point types | `f32` (IEEE 754 single precision), `f64` (IEEE 754 double precision) | Standard native hardware floating-point formats on modern x86_64 CPUs. |
| Character type | `char` (32-bit unsigned Unicode scalar value / code point) | Prepared for UTF-8/Unicode integration in Phase 4 without representation confusion. |
| Explicit cast syntax | `expr as Type` (e.g. `x as f64`, `y as i32`) | Confirms SYNTAX §2.8; unambiguous grammar, high readability, avoids C-style paren ambiguities. |
| Type conversion policy | No silent narrowing conversions; explicit `as` cast required for mixed-type operations | Strict typing preserves mathematical correctness and eliminates silent truncation bugs. |
| Exponentiation operator (`**`) | Right-associative; lowers to runtime `nrq_pow_i64` / `nrq_pow_f64` | Distinct from multiplication; standard mathematical right-associativity (`2 ** 3 ** 2 == 512`). |
| Short-circuit evaluation | `&&` and `||` evaluate right-hand side only if left-hand side does not determine the result | Mandatory per SYNTAX §2.5; lowered via branching blocks in TIR. |
| Division-by-zero behavior | Integer division/remainder by zero traps via `nrq_runtime_fatal("division by zero")`; float division follows IEEE 754 (`inf`, `-inf`, `nan`) | Predictable, safe execution without undefined host-architecture crashes. |
| Integer overflow behavior | Two's complement modular wrap-around by default | Matches native x86-64 machine arithmetic performance; diagnostics for constant-folded overflow. |
| `print` expansion | Builtin `print` accepts any scalar type (`string`, `int`, `i32`, `f64`, `f32`, `bool`, `char`) | Overcomes Phase 1 string-literal-only restriction, establishing runtime formatting primitives. |
| Control flow boundary | Statements `if`, `while`, `for` remain reserved for Phase 3 | Comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) evaluate to `bool` values in Phase 2; statement-level branching belongs to Phase 3. |

---

## 2. Prerequisites and Environment Baseline

- **OS:** Windows 11 / Windows 10 x64.
- **Toolchain:** MSVC (v143+ toolset), Ninja, CMake >= 3.28 (using local CMake 4.5.0-rc2).
- **LLVM / MLIR:** 23.1.4 SDK (installed in `toolchain/llvm-23.1.4/` with dynamic Release CRT).
- **Python:** 3.10+ (using local Python 3.12).
- **Dependencies:** None beyond the pinned LLVM/MLIR SDK and standard Windows C/C++ build tools.

---

## 3. Repository Layout and Changes

Files created or modified in Phase 2:

```text
compiler/
  include/nirqoba/
    Common/
      Type.h                                # Expanded: i8..i64, u8..u64, f32, f64, char, byte
    Frontend/
      Token.h                               # Suffixes for numeric literals (e.g. 42i32, 3.14f32)
      Ast.h                                 # CastExpr, FloatLiteralExpr, CharLiteralExpr, CompoundAssignStmt, LogicalExpr
      Parser.h                              # Precedence-climbing Pratt parser for all operators
    Sema/
      TypeChecker.h                         # Full scalar type promotion, cast validation, immutability check
    TIR/
      IR.h                                  # Float, bitwise, shift, comparison, cast, and branch TIR ops
      Builder.h                             # Lowers all scalar ops & short-circuit control flow
      Verifier.h                            # Checks type safety on widened op set
      Printing.h                            # Updated stable textual dump
    Lowering/
      NrqDialect.h, NrqOps.td, NrqOps.h     # Ops for formatted scalar printing
      PassPipeline.h, PassPipeline.cpp      # Arithmetic & control flow conversion passes
      TirToMlir.h, TirToMlir.cpp            # Lowers scalar ops to MLIR arith, math, and LLVM dialects
runtime/
  include/nirqoba/runtime/
    io.h                                    # nrq_print_int, nrq_print_f64, nrq_print_bool, nrq_print_char
    math.h                                  # nrq_pow_i64, nrq_pow_f64
  src/
    io_windows.cpp                          # Formatted scalar string conversion & WriteFile output
    math.cpp                                # Integer and floating-point exponentiation helpers
tests/
  unit/
    lexer_test.cpp                          # Numbers with radix (hex 0x, bin 0b), floats, char literals, suffixes
    parser_test.cpp                         # Precedence table, associativity, cast syntax, compound assignments
    sema_test.cpp                           # Valid/invalid casts, immutability enforcement, bitwise on non-ints
    tir_test.cpp                            # Short-circuit blocks, arithmetic/comparison IR ops
  integration/
    positive/
      scalar_arithmetic/                    # Full operator matrix evaluation
      float_math/                           # IEEE 754 f32/f64 operations
      short_circuit/                        # Verifies non-execution of side effects in && and ||
      compound_assignment/                  # Validates +=, -=, *=, /=, |=, etc. on var
      explicit_casts/                       # Widening and narrowing casts
    negative/
      div_by_zero/                          # Trapping integer division by zero
      let_reassignment/                     # Reassigning immutable binding rejected
      invalid_cast/                         # Non-convertible types rejected with NRQ3015
      uninitialized_var/                    # Use before initialization rejected
    run_tests.py                            # Preserved and updated
docs/
  architecture/
    07-models.md                            # Updated: scalar type hierarchy and arithmetic semantics
  language/
    types.md                                # Scalar types reference
    operators.md                            # Complete precedence and associativity table
    variables.md                            # let, var, const semantics and compound assignments
reports/
  phase-02.md                               # Complete 12-section technical report
```

---

## 4. Architecture and Stage Details

### 4.1 Lexer Expansion

1. **Numeric Literals:**
   - **Radix prefixes:**
     - Hexadecimal: `0x[0-9a-fA-F_]+`
     - Binary: `0b[01_]+`
     - Decimal: `[0-9][0-9_]*`
   - **Floats:** `[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?` and `[0-9]+[eE][+-]?[0-9]+`
   - **Type suffixes:**
     - Integers: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64` (e.g. `100u8`, `0xFFi32`)
     - Floats: `f32`, `f64` (e.g. `3.14f32`, `1.0f64`)
2. **Character Literals:**
   - Single-quoted character `'.'`: handles escapes (`'\n'`, `'\t'`, `'\''`, `'\\'`, `'\0'`) and raw Unicode scalar values up to `U+10FFFF`.
3. **Operators & Tokens:**
   - Tokenize `as` keyword for casts.
   - Tokenize compound assignment operators (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`).
   - Tokenize exponentiation `**`.

### 4.2 Parser & Grammar

Implement precedence-climbing (or Pratt parsing) for expressions.

#### Complete Precedence Table (Highest to Lowest):

| Precedence | Level | Operators | Associativity |
|---|---|---|---|
| 12 | Primary | Literals, Identifiers, Grouping `( )`, Function Call | - |
| 11 | Exponentiation | `**` | Right |
| 10 | Unary Prefix | `-`, `+`, `!`, `~` | Right |
| 9 | Cast | `as` | Left |
| 8 | Multiplicative | `*`, `/`, `%` | Left |
| 7 | Additive | `+`, `-` | Left |
| 6 | Shift | `<<`, `>>` | Left |
| 5 | Relational | `<`, `<=`, `>`, `>=` | Left |
| 4 | Equality | `==`, `!=` | Left |
| 3 | Bitwise AND | `&` | Left |
| 2 | Bitwise XOR | `^` | Left |
| 1 | Bitwise OR | `\|` | Left |
| 0 | Logical AND | `&&` | Left |
| -1 | Logical OR | `\|\|` | Left |

#### Grammar Additions:

```text
statement      := var_decl | assignment | compound_assign | return_stmt | expr_stmt | block
compound_assign:= IDENT compound_op expression ';'
compound_op    := '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>='
expression     := logical_or
logical_or     := logical_and ('||' logical_and)*
logical_and    := bitwise_or ('&&' bitwise_or)*
bitwise_or     := bitwise_xor ('|' bitwise_xor)*
bitwise_xor    := bitwise_and ('^' bitwise_and)*
bitwise_and    := equality ('&' equality)*
equality       := relational (('==' | '!=') relational)*
relational     := shift (('<' | '<=' | '>' | '>=') shift)*
shift          := additive (('<<' | '>>') additive)*
additive       := multiplicative (('+' | '-') multiplicative)*
multiplicative := cast (('*' | '/' | '%') cast)*
cast           := exponentiation ('as' type)*
exponentiation := unary ('**' exponentiation) | unary
unary          := ('-' | '+' | '!' | '~') unary | primary
primary        := literal | IDENT | call_expr | '(' expression ')'
literal        := INT_LITERAL | FLOAT_LITERAL | CHAR_LITERAL | STRING_LITERAL | 'true' | 'false'
```

### 4.3 Semantic Analysis (Sema)

1. **Type Compatibility & Inferences:**
   - Default integer literal type is `int` (`i64`) unless suffixed or constrained by explicit annotation.
   - Default floating-point literal type is `f64` unless suffixed.
   - Binary arithmetic operators require matching operand types; mixed arithmetic (`x + y` where `x: i32`, `y: i64`) produces diagnostic `NRQ3015: binary operator '+' cannot be applied to operands of type 'i32' and 'i64'; use explicit 'as' cast`.
2. **Explicit Cast Rules (`as`):**
   - Integer to Integer (truncation or sign/zero extension).
   - Integer to Float (conversion `sitofp` / `uitofp`).
   - Float to Integer (truncation `fptosi` / `fptoui`).
   - Float to Float (`fpext` / `fptrunc`).
   - `bool` to Integer (`false -> 0`, `true -> 1`) and Integer to `bool` (`x != 0`).
   - Incompatible casts (e.g. `string as int`, `void as f64`) rejected with `NRQ3015`.
3. **Variable Mutation & Initialization:**
   - Reassignment (`=` or compound assignments) on `let` or `const` bindings rejected with `NRQ3014`.
   - Variable read before initialization rejected with `NRQ3020`.
4. **Compile-time Constant Evaluation (`const`):**
   - Arithmetic on literal constants in `const` declarations evaluated at compile time; constant division by zero produces `NRQ3021`.

### 4.4 Typed IR (TIR)

Extend TIR with:
- `tir::TypeKind`: `I8`, `I16`, `I32`, `I64`, `U8`, `U16`, `U32`, `U64`, `F32`, `F64`, `Char`, `Bool`, `String`, `Void`.
- `tir::OpKind`:
  - `ConstantFloat`, `ConstantChar`
  - `Cast`: source value, target type, cast kind (`IntTrunc`, `IntSExt`, `IntZExt`, `SIToFP`, `UIToFP`, `FPToSI`, `FPToUI`, `FPExt`, `FPTrunc`)
  - `Binary`: arithmetic (`Add`, `Sub`, `Mul`, `Div`, `Rem`, `Pow`), bitwise (`And`, `Or`, `Xor`, `Shl`, `Shr`), comparisons (`CmpEq`, `CmpNe`, `CmpLt`, `CmpLe`, `CmpGt`, `CmpGe`)
  - `Branch`, `BranchCond`: to model short-circuit control flow for `&&` and `||`.
  - `PrintValue`: prints any scalar value by invoking runtime helpers.

### 4.5 MLIR Lowering & Dialect Integration

1. **MLIR Dialects Utilized:**
   - `arith`: `arith.addi`, `arith.subi`, `arith.muli`, `arith.divsi`, `arith.divui`, `arith.remui`, `arith.remsi`, `arith.addf`, `arith.subf`, `arith.mulf`, `arith.divf`, `arith.cmpf`, `arith.cmpi`, `arith.extsi`, `arith.extui`, `arith.trunci`, `arith.sitofp`, `arith.fptosi`.
   - `math`: `math.powf` (for floating-point `**`).
   - `cf`: `cf.br`, `cf.cond_br` (for short-circuit `&&` / `||`).
   - `llvm`: for pointers, globals, and runtime C ABI function calls.
2. **Lowering Pipeline:**
   - Prepend `convert-arith-to-llvm`, `convert-math-to-llvm`, `convert-cf-to-llvm` to `build_lowering_pipeline()`.
   - Update `NrqToLLVMPass` to support formatted printing of scalar types (`nrq_print_int`, `nrq_print_f64`, `nrq_print_bool`, `nrq_print_char`).

### 4.6 Runtime Library Extensions

Add C ABI symbols to `nrqoba_runtime.lib`:
```c
int32_t nrq_print_int(int64_t val);
int32_t nrq_print_uint(uint64_t val);
int32_t nrq_print_f64(double val);
int32_t nrq_print_bool(int32_t val);
int32_t nrq_print_char(uint32_t code_point);
int64_t nrq_pow_i64(int64_t base, int64_t exp);
double  nrq_pow_f64(double base, double exp);
```
Implemented directly using Win32 `WriteFile` without standard library `printf` dependencies.

---

## 5. Command-Line Interface (Unchanged & Preserved)

The CLI interface defined in Phase 1 remains completely compatible:
- `nirqoba build <file.nrqba> [-o PATH] [--out-dir DIR] [--emit=ast|tir|mlir|llvm|obj]`
- `nirqoba run <file.nrqba> [--out-dir DIR]`
- `nirqoba check <file.nrqba> [--emit=ast|tir]`
- `nirqoba version` (reports Phase 2)
- `nirqoba help`

---

## 6. Testing Plan

### 6.1 Unit Tests (`nirqoba_unit_tests`)

1. **Lexer:**
   - Decimal, hexadecimal (`0x`), binary (`0b`) integer literals.
   - Floating-point literals with fractions and exponents (`1.5e-3`).
   - Suffix parsing (`42i32`, `3.14f32`).
   - Character literals and escape sequences (`'a'`, `'\n'`, `'\\'`, `'\''`).
   - All compound assignment tokens (`+=`, `*=`, `<<=`, etc.).
2. **Parser:**
   - Operator precedence correctness (e.g. `1 + 2 * 3 == 7`, `2 ** 3 ** 2 == 512`).
   - Explicit `as` cast expressions (`x as f64`).
   - Compound assignment statements (`x += 5;`).
   - Unary operators (`!flag`, `~mask`, `-value`).
3. **Sema:**
   - Type-checking on all scalar binary operations.
   - Rejecting mixed-type operations without explicit cast.
   - Rejecting mutation of `let` and `const`.
   - Validating legal vs illegal type casts.
   - Detecting constant-expression division by zero.
4. **TIR & Lowering:**
   - Builder generates branching blocks for `&&` and `||`.
   - Verified TIR emission for floats, signed/unsigned ints, and casts.

### 6.2 Integration Tests (`tests/run_tests.py`)

- `positive/scalar_arithmetic`: Comprehensive evaluation of integer arithmetic and bitwise logic.
- `positive/float_math`: Verified single and double precision floating-point arithmetic.
- `positive/short_circuit`: Verifies that `false && (1/0 == 0)` does not divide by zero or execute side effects.
- `positive/compound_assignment`: Verifies `var x = 10; x += 5; x *= 2;` computes 30.
- `positive/explicit_casts`: Verifies integer truncation, sign extension, and float conversions.
- `positive/exponentiation`: Verifies integer and floating-point `**`.
- `positive/print_scalars`: Verifies `print(42)`, `print(3.14)`, `print(true)`, `print('A')`.
- `negative/div_by_zero`: Runtime trap verification for integer division by zero.
- `negative/type_mismatch`: Compile-time rejection of mixed uncast arithmetic.
- `negative/let_reassignment`: Reassignment of immutable binding rejected with `NRQ3014`.

---

## 7. Documentation Deliverables

1. `docs/language/types.md`: Comprehensive scalar type reference (`int`, `i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `char`, `bool`, `string`).
2. `docs/language/operators.md`: Full operator precedence and associativity table, short-circuit semantics, bitwise operations.
3. `docs/language/variables.md`: `let`, `var`, `const` semantics, initialization requirements, and compound assignments.
4. `docs/architecture/07-models.md`: Updated type model and arithmetic execution semantics.
5. `reports/phase-02.md`: 12-section technical implementation report.

---

## 8. Implementation Milestones and Commit Checkpoints

| # | Milestone | Suggested Commit Message |
|---|---|---|
| 1 | Primitive scalar types and literal lexing (hex, bin, floats, chars, suffixes) | `feat(frontend): add scalar types, numeric radix, float and char literals` |
| 2 | Pratt parser for all operators, precedence climbing, `as` casts, compound assignment | `feat(parser): implement operator precedence, casts and compound assignments` |
| 3 | Sema type analysis: scalar promotion rules, cast validation, immutability checks | `feat(sema): type-check scalar operators, explicit casts and bindings` |
| 4 | TIR extension: scalar ops, cast kinds, comparisons, and short-circuit branching | `feat(tir): lower scalar operations and short-circuit evaluation to TIR` |
| 5 | MLIR lowering: arith/math dialect conversion and runtime scalar print helpers | `feat(lowering): translate scalar operations to MLIR arith and math dialects` |
| 6 | Unit and integration test suites for scalar arithmetic, casts, and short-circuiting | `test(phase-2): add comprehensive unit and integration test coverage` |
| 7 | Documentation and `reports/phase-02.md` | `docs(phase-2): add scalar language reference and Phase 2 report` |

---

## 9. Explicit Exclusions (Deferred to Future Phases)

- Control flow statements: `if`, `else`, `while`, `for`, `break`, `continue` (Phase 3).
- User-defined functions, parameters, return value passing, function overloading (Phase 3).
- Strings beyond UTF-8 literals and scalar printing (string concatenation, slicing, methods) (Phase 4).
- `f_string` template interpolation parsing (Phase 4).
- Arrays, tuples, structs, records (Phase 4).
- Modules, packages, and imports (Phase 5).
- Heap allocation, pointers, references, and GC (Phase 6.5+).

---

## 10. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| MLIR `arith` to `llvm` conversion dialect mismatches | Use standard upstream `convert-arith-to-llvm` pass, already linked in SDK. |
| Inconsistent floating-point precision on Windows x64 | Rely on standard x86-64 SSE2/AVX hardware float instructions generated by LLVM TargetMachine. |
| Integer division by zero crashing OS process silently | Emit explicit check before division/remainder in TIR lowering; invoke `nrq_runtime_fatal` on zero divisor. |
| Complex precedence bugs in recursive descent parser | Implement standard Pratt / precedence climbing algorithm with exhaustive unit test assertions. |

---

## 11. Acceptance Criteria

| Requirement | Evidence Produced |
|---|---|
| Complete scalar types (`bool`, `i8`..`i64`, `int`, `u8`..`u64`, `byte`, `f32`, `f64`, `char`, `string`) | Type unit tests and AST/TIR checks |
| Integer literals (hex `0x`, binary `0b`, decimal) and floats | Lexer and parser unit tests |
| All arithmetic (`+`, `-`, `*`, `/`, `%`, `**`) and bitwise operators | Unit tests & integration test suite |
| All comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) yielding `bool` | Evaluation tests and verification |
| Strict short-circuit evaluation for `&&` and `||` | Integration tests verifying unexecuted side effects |
| Explicit `expr as Type` casting for all valid scalar pairs | Sema tests and runtime execution tests |
| Reassignment allowed on `var`, rejected on `let`/`const` | Sema diagnostics `NRQ3014` test |
| Integer division by zero safely trapped | Negative integration test verifying fatal runtime exit |
| Scalar printing (`print(42)`, `print(3.14)`, `print(true)`) | Integration tests verifying exact output |
| Architecture docs and `reports/phase-02.md` complete | 12-section technical report |

---

## 12. Definition of Done

Phase 2 is complete only when:
- All scalar types, literals, operators, casts, and compound assignments parse and compile cleanly.
- Operator precedence strictly adheres to the language grammar table.
- Strict short-circuiting for `&&` and `||` is verified by tests.
- Reassignment of `let` is rejected with `NRQ3014`.
- All unit and integration test suites pass with 100% success rate.
- `reports/phase-02.md` is authored with evidence and concludes with a **PASS** verdict.
