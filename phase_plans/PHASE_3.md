# Phase 3 — Functions, Scopes, and Control Flow

**Status:** Implementation plan  
**Author:** Muhammad Tahmid Ahnaf (@builtbyahnaf)  
**Plan version:** 1.0  
**Scope sources:** `project_context/ROADMAP.md` §Phase 3, `project_context/SYNTAX.md` §Phase 3, `project_context/AGENTS.md`  
**Target report:** `reports/phase-03.md`  

---

## 1. Decisions this plan locks

| Decision | Choice | Rationale |
|---|---|---|
| Function declaration keyword | `function` (never `fn`) | Locked per SYNTAX §0.1 & ROADMAP §4. |
| Function syntax convention | Return-type-first: `ReturnType function name(Type1 param1, Type2 param2) { ... }` | Preserves Nirqoba seed convention established in Phase 1. |
| Function return types | Any valid scalar type (`int`, `i8`..`i64`, `u8`..`u64`, `f32`, `f64`, `bool`, `char`, `string`) or `void` | Enables general-purpose procedural computations. |
| Forward function references | Supported: all top-level functions in a file are visible to each other regardless of declaration order | Enables mutual recursion, clean module organisation, and avoids header/prototype requirements. |
| Lexical scoping & shadowing | Nested blocks (`{}`) introduce child scopes; inner variables may shadow outer variables | Standard lexical block scoping; prevents unintended outer modifications while permitting local reuse. |
| Same-scope redeclaration | Rejected with `NRQ3012` | Declaring two variables of the same name in the exact same scope is an error. |
| Conditional statement syntax | `if (condition) block [else if (condition) block]* [else block]?` | Condition must strictly evaluate to `bool` (NRQ3015 if non-bool). |
| While loop syntax | `while (condition) block` | Condition must evaluate to `bool`. Evaluated before each iteration. |
| Conventional for loop syntax | `for (init; cond; step) block` where `init` is var decl or assignment, `cond` is bool expr, `step` is assignment/compound/expr | Familiar C/Java/JS-style 3-part for loop; loop variable scoped to the loop body. |
| Loop control statements | `break;` and `continue;` | Valid only within enclosing loop body; rejected with `NRQ3022` if used outside loops. |
| Return path analysis | Exhaustive control-flow reachability analysis; non-void functions must return on all paths | Guarantees no undefined runtime fall-through; reports `NRQ3006` if any branch path misses a return. |
| Function call semantics | Eager evaluation of arguments left-to-right; pass-by-value for all Phase 3 types | Standard deterministic call evaluation model. |
| Recursion | Full support for direct and mutual recursion | Mandatory for procedural algorithms (e.g. factorial, Fibonacci, tree traversal). |

---

## 2. Prerequisites and Environment Baseline

- **OS:** Windows 11 / Windows 10 x64.
- **Toolchain:** MSVC (v143+ toolset), Ninja, CMake >= 3.28.
- **LLVM / MLIR:** 23.1.4 SDK (installed in `toolchain/llvm-23.1.4/`).
- **Python:** 3.10+ (for integration test runner).
- **Previous Phases:** Phase 1 (foundation) and Phase 2 (scalar types & expressions) complete.

---

## 3. Repository Layout and Changes

Files created or modified in Phase 3:

```text
compiler/
  include/nirqoba/
    Diagnostics/
      Diagnostic.h                          # Added: NRQ3022 (break/continue outside loop), NRQ3023 (argument count mismatch), etc.
    Frontend/
      Ast.h                                 # Added: IfStmt, WhileStmt, ForStmt, BreakStmt, ContinueStmt
      Parser.h                              # Added: parse_if, parse_while, parse_for, parse_break, parse_continue
    Sema/
      SymbolTable.h                         # Function symbol catalog with signatures; scope chain enhancements
      TypeChecker.h                         # Forward collection pass, loop depth tracking, control flow exhaustiveness
    TIR/
      IR.h                                  # Call op for user functions, loop labels
      Builder.h                             # Lowers if/else, while, for loops, break/continue, call expressions
      Verifier.h                            # Checks call targets and loop structures
      Printing.h                            # Updated TIR dump with function calls & branch targets
    Lowering/
      TirToMlir.h, TirToMlir.cpp            # Translates user function calls (func.call) and structured control-flow blocks
tests/
  unit/
    parser_test.cpp                         # Tests for if/else, while, for, break, continue AST nodes
    sema_test.cpp                           # Tests for forward references, return completeness, loop controls, recursion
    tir_test.cpp                            # Tests for call lowering and loop jump generation
  integration/
    positive/
      control_flow/                         # if/else chains, nested loops, break/continue
      recursion/                            # factorial, fibonacci
      forward_calls/                        # mutual recursion, order-independent functions
    negative/
      missing_return/                       # unhandled branch path in non-void function
      break_outside_loop/                   # break outside loop rejected
      arg_mismatch/                         # wrong number or types of arguments to function
reports/
  phase-03.md                               # Complete technical report
```

---

## 4. Architecture and Stage Details

### 4.1 Frontend & AST Expansion

New AST Statements in `compiler/include/nirqoba/Frontend/Ast.h`:
1. **`IfStmt`**:
   - `ExprPtr condition`
   - `std::unique_ptr<BlockStmt> then_branch`
   - `StmtPtr else_branch` (either another `IfStmt` for `else if`, or `BlockStmt` for `else`, or `nullptr`)
2. **`WhileStmt`**:
   - `ExprPtr condition`
   - `std::unique_ptr<BlockStmt> body`
3. **`ForStmt`**:
   - `StmtPtr initializer` (e.g. `VariableDeclarationStmt` or `AssignmentStmt` or empty)
   - `ExprPtr condition` (optional, defaults to `true`)
   - `StmtPtr step` (e.g. `AssignmentStmt`, `CompoundAssignmentStmt`, `ExprStmt` or empty)
   - `std::unique_ptr<BlockStmt> body`
4. **`BreakStmt`**:
   - `SourceLocation location`
5. **`ContinueStmt`**:
   - `SourceLocation location`

New AST Expressions / Declarations:
- `CallExpr` already exists in `Ast.h`; will now be used for user function calls in addition to `print`.
- `FunctionDecl` already exists; parameter list is now fully wired up for execution.

### 4.2 Grammar & Parser

```text
statement        := block
                  | var_decl
                  | assignment
                  | compound_assign
                  | if_stmt
                  | while_stmt
                  | for_stmt
                  | break_stmt
                  | continue_stmt
                  | return_stmt
                  | expr_stmt

if_stmt          := 'if' '(' expr ')' block ('else' (if_stmt | block))?
while_stmt       := 'while' '(' expr ')' block
for_stmt         := 'for' '(' (var_decl | expr_stmt | ';') expr? ';' (assignment | compound_assign | expr)? ')' block
break_stmt       := 'break' ';'
continue_stmt    := 'continue' ';'
```

### 4.3 Semantic Analysis (Sema)

1. **Two-Pass Program Analysis:**
   - **Pass 1 (Function Signature Collection):** Walks top-level function declarations to record all function names, parameter types, and return types in the global function registry. Detects duplicate function definitions (`NRQ3016`). Allows forward references.
   - **Pass 2 (Body Type-Checking):** Checks function bodies with full visibility of all declared functions.
2. **Scoping & Shadowing:**
   - Blocks create lexical scopes. Lookups traverse from innermost scope to outer scopes up to function parameters.
   - Shadowing an outer variable in a nested block is legal. Redeclaring a variable in the same scope produces `NRQ3012`.
3. **Control Flow Validation:**
   - `if` and `while` conditions must be of type `bool` (`NRQ3015`).
   - `for` condition (if present) must be of type `bool` (`NRQ3015`).
   - `break` and `continue` maintain a loop nesting counter; if counter is 0, report `NRQ3022: 'break'/'continue' outside loop`.
4. **Return Path Exhaustiveness Analysis:**
   - `block_can_fall_through(const BlockStmt& block)`:
     - Non-void functions must end with a return or all execution paths must terminate with returns.
     - An `if-else` statement only terminates if *both* `then` and `else` branches terminate.
     - An infinite loop `while (true)` or `for (;;) without break` is a terminating construct.
     - Fall-through in a non-void function produces `NRQ3006`.
5. **Function Call Resolution:**
   - If callee is `print`: built-in scalar/string print checks apply.
   - If callee is user-defined:
     - Check callee exists in function registry (`NRQ3011` if undefined).
     - Check argument count matches parameter count (`NRQ3018` / `NRQ3023`).
     - Check each argument type matches the parameter type exactly (`NRQ3015`).
     - Call expression returns the function's return type.

### 4.4 Typed IR (TIR)

Extend TIR with:
1. `tir::OpKind::Call`:
   - `result` value with function return type
   - `std::string callee`
   - `std::vector<tir::Value> arguments`
2. Control Flow Translation:
   - `IfStmt`:
     - Evaluate condition -> `cond`
     - `BranchCond cond, ^then_bb, ^else_bb`
     - `^then_bb`: lower body, `Branch ^merge_bb`
     - `^else_bb`: lower else body (if any), `Branch ^merge_bb`
     - `^merge_bb`: continuation block
   - `WhileStmt`:
     - `Branch ^header_bb`
     - `^header_bb`: evaluate condition -> `cond`, `BranchCond cond, ^body_bb, ^exit_bb`
     - `^body_bb`: lower body, `Branch ^header_bb`
     - `^exit_bb`: continuation block
     - Loop stack keeps `header_bb` (target for `continue`) and `exit_bb` (target for `break`).
   - `ForStmt`:
     - Lower initializer
     - `Branch ^header_bb`
     - `^header_bb`: evaluate condition, `BranchCond cond, ^body_bb, ^exit_bb`
     - `^body_bb`: lower body, `Branch ^step_bb`
     - `^step_bb`: lower step statement, `Branch ^header_bb`
     - `^exit_bb`: continuation block
     - Loop stack keeps `step_bb` (target for `continue`) and `exit_bb` (target for `break`).

### 4.5 MLIR Lowering

1. **User Functions:**
   - Top-level `tir::Function` translated to `func.func @name(...) -> (...)` with typed parameters.
   - Parameter values in entry block stored into stack slots (`llvm.alloca` + `llvm.store`) so they behave like mutable local variables if needed.
2. **Function Calls:**
   - Lowered to `func.call @callee(arg1, arg2, ...)` in MLIR.
   - If non-void, call produces an MLIR SSA value.

---

## 5. Milestone Breakdown & Execution Strategy

- **Milestone 1 (Immediate):** AST & Parser additions for `if`, `else`, `while`, `for`, `break`, `continue`, user function calls.
- **Milestone 2 (Immediate):** Semantic Analysis — Function registry, two-pass resolution, lexical scoping, loop validation, and return path exhaustiveness.
- **Milestone 3:** TIR Representation & Structured Control Flow Lowering.
- **Milestone 4:** MLIR Lowering (`func.call`, function signatures, parameters, CFG blocks).
- **Milestone 5:** Test Suite (unit & integration tests).
- **Milestone 6:** Documentation & Phase 3 Report (`reports/phase-03.md`).
