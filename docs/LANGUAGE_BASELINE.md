# Nirqoba Language Baseline — Version 1.0 Audit

**Version audited:** `0.670-beta` (HEAD at audit time)
**Method:** read-only audit of `compiler/lib/Frontend` (lexer, parser, AST),
`compiler/lib/Sema`, `compiler/include/nirqoba/Common/Type.h`, and `runtime/`,
cross-checked against sixteen probe programs compiled with the shipped compiler.
No compiler, runtime or test file was modified.
**Probe sources:** `build/tmp/14a/*.nrqba` (scratch, not committed).

Rows marked **✔ run** were compiled and executed during the audit; the printed
result is quoted. Rows marked **code** are established from the implementation
without a probe. Anything absent is absent, not merely undocumented.

---

## 1. Lexer

### 1.1 Literals

| Form | Status | Evidence |
|---|---|---|
| Decimal integer | ✔ run | `print(17 % 5)` → `2` |
| Hex integer `0x…` | code | `Lexer.cpp:211` (`0x` / `0b` prefixes) |
| Binary integer `0b…` | ✔ run | `0b1111 \| (0b1010 & 0b1100) ^ 0b0001` → `14` |
| Octal `0o…` | ✘ absent | no prefix branch in the lexer |
| Float | ✔ run | `print(3.5 as int)` → `3` |
| Char `'a'` | code | `TokenKind::CharLiteral` |
| String `"…"` | ✔ run | `print("body")` → `body` |
| Type suffix on literals | code | `Token::suffix` field |
| Escape sequences in strings/chars | code | handled in `Lexer.cpp` string scanning |

### 1.2 Keywords

**Parsed today:** `function`, `let`, `var`, `const`, `return`, `print`, `if`,
`else`, `while`, `for`, `break`, `continue`, `struct`, `module`, `import`,
`extern`, `public`, `private`, `defer`, `as`, `int`, `string`, `string_owned`,
`bool`, `void`, `true`, `false`, `i8 i16 i32 i64 u8 u16 u32 u64 byte char f32 f64`.

**Reserved, reported as a phase gap (not parsed):** `protected`, `async`,
`await`, `fn`. The parser emits a diagnostic naming the feature and its phase
rather than a generic syntax error (`Parser::report_phase_gap`).

**Do not exist as keywords at all:** `switch`, `case`, `match`, `do`, `loop`,
`enum`, `interface`, `class`, `trait`, `impl`, `type`, `static`, `goto`,
`namespace`, `using`, `try`, `catch`, `throw`, `yield`, `return_owned`. Such
words lex as identifiers; using them as statement starters produces a parse
error, not a "not implemented" diagnostic.

### 1.3 Comments

Line comments `//` and block comments `/* … */` (lexer trivia).

---

## 2. Operators

### 2.1 Present and working

| Category | Operators | Status |
|---|---|---|
| Arithmetic | `+ - * / %` | ✔ run |
| Exponentiation | `**` (right-associative; `2 ** 10` → `1024`) | ✔ run |
| Unary | `-x`, `+x`, `!x` (bool only), `~x` (→ `-1` for `~0`) | ✔ run |
| Address-of | `&x` | ✔ run (`bump(&x)` → `42`) |
| Dereference | `*p`, `**pp` (nested) | ✔ run |
| Indexing | `a[i]` | ✔ run |
| Member access | `a.b`, `a.0` (tuple index) | ✔ run |
| Assignment | `=` | ✔ run |
| Compound assignment | `+= -= *= /= %= &= \|= ^= <<= >>=` | code (`parse_assignment_or_expression`) |
| Comparison | `== != < <= > >=` | ✔ run |
| Logical | `&& \|\| !` (short-circuit, bool operands required) | ✔ run |
| Bitwise | `& \| ^ ~` | ✔ run |
| Shift | `<< >>` | ✔ run (`1 << 4` → `16`, `256 >> 3` → `32`) |
| Cast | `expr as T` | ✔ run |
| Grouping | `( … )` | ✔ run |

### 2.2 Missing standard operators — the brief's assumptions, corrected

| Operator | Status | Evidence |
|---|---|---|
| `++` (pre/post) | **absent** | `i++;` → `NRQ2001: expected an expression, found ';'`. No `PlusPlus` token exists. |
| `--` (pre/post) | **absent** | no `MinusMinus` token. `for` loops must spell `i += 1`. |
| `<<` / `>>` | **present** | tokens `LessLess`/`GreaterGreater`, `BinaryOp::ShiftLeft/ShiftRight`; verified at run time. The brief listed these as missing; they are not. |
| Ternary `?:` | **absent** | `?` is not a lexable character: `NRQ1008: unexpected character '?'`. |
| Null-coalescing `??` | absent | same lexer error |
| Null-safe access `?.` | absent | no token |
| Range `..` / `...` | absent | no token |
| Arrow `->` | **tokenized but unused** | `TokenKind::Arrow` exists; no parser path consumes it |
| `sizeof` | absent | no token, no parser path |
| Comma operator | absent | `,` is a separator only |
| Spaceship `<=>` | absent | no token |
| String concatenation `+` | **absent** | `"hello" + " world"` → `NRQ3015: operator '+' requires numeric operands`. There is no string builder and no `concat`. |

---

## 3. Statements and control flow

| Construct | Status | Evidence |
|---|---|---|
| `if` / `else if` / `else` | ✔ run | `parse_if`, `Parser.cpp:1008` |
| `while` | ✔ run | all stress-test code |
| `for (init; cond; step)` | ✔ run | `for (var i = 0; i < 10; i += 1)` → `31` after `continue`/`break` |
| `break` / `continue` | ✔ run | same probe |
| Early `return`, bare `return;` in `void` | ✔ run | used throughout |
| `defer <call>;` | ✔ run | runs at scope exit: printed `body` then `outer` |
| Block statements, nested scopes | ✔ run | |
| Destructuring `var (a, b) = (1, 2);` | ✔ run | → `3` |
| `@Nullable` annotation on a declaration | partially | see §5.6 |
| `switch` / `case` | **absent** | `switch (x) { case 1: … }` → parse errors (`expected ';' after the expression, found '{'`) |
| `match` / pattern matching | **absent** | not a keyword |
| `do … while` | **absent** | `do { … } while (…)` → `expected '}' to close struct initializer` |
| `loop { }` | absent | not a keyword |
| Labelled break/continue | absent | no label syntax |
| `try` / `catch` / `throw` | absent | no keywords, no AST nodes |
| `goto` | absent | not a keyword |

---

## 4. Declarations, modules, visibility

### 4.1 Program structure

- One optional `module <name>;` per file, and it must be the first item.
- Zero or more `import <module>;` lines, which must precede every declaration.
- Declarations: `struct`, function definitions, and `extern … function …;`.

### 4.2 Functions

```nirqoba
int function name(borrow Point* p, shared ptr Node n, int count) { … }
extern int function puts(ptr byte text);
```

- Return type is written **before** the keyword: `<type> function <name>(…)`.
- Parameters may carry ownership qualifiers: `borrow`, `borrow mut`, `shared`,
  `owned` (with optional resource domain `file`, `socket`, `mutex`, `mapping`,
  `secure`), plain `ptr`/`T*`.
- `extern` declares a symbol resolved from the linked C objects; the declaration
  ends at `;`.
- No default parameter values, no variadics, no overloading, no generics, no
  function pointers, no closures, no methods on structs (verified: no `fn`
  keyword support, and struct bodies parse fields only).

### 4.3 Structs

- Fields only. Each field is `[qualifier] <type> <name>;` where the qualifier may
  be `borrow [mut] [domain]`, `shared`, or `owned [domain]`.
- Initialisation by literal: `Point { x: 3, y: 4 }` ✔ run.
- No field defaults, no constructors, no destructors, no inheritance, no
  per-field visibility, no nested type declarations inside a struct.

### 4.4 Visibility and modules

- `public` and `private` are parsed before `struct` and `function`; the default is
  `public`.
- Enforcement is real, not decorative: `TypeChecker.cpp:791`, `:806` and `:6195`
  reject a **private** function or struct of another module, and a private symbol
  accessed from outside its module.
- The module system is a resolver, not raw includes:
  `compiler/lib/Driver/ModuleResolver.cpp` loads imports iteratively with
  `--module-path`, and `CompilationPipeline.cpp` orders checking so a consumer is
  checked after the module it imports.
- Cross-module type names are written `module.Type` and kept verbatim by the
  parser.
- `protected` is reserved and unused. There is no `pub(crate)`, no re-export, no
  explicit module visibility declaration beyond per-symbol `private`.

---

## 5. Type system

`TypeKind` (`compiler/include/nirqoba/Common/Type.h:70`):

| Group | Types |
|---|---|
| Scalars | `void`, `bool`, `int` (default integer), `i8 i16 i32 i64`, `u8 u16 u32 u64`, `char`, `f32 f64` |
| Text | `string` (borrowed, process-lifetime), `string_owned` (language-owned buffer) |
| Pointer | `T*`, `ptr T`, `owned_ptr<T>`, `shared ptr T`, `managed<T>` |
| Aggregate | `struct`, tuple `(T1, T2, …)` |
| Containers | `dynamic_array<T>`, `owned_array<T>`, `soa<T>` (struct-of-arrays), `array_view<T>` |

### 5.1 Numeric and cast rules
- Mixed-width arithmetic requires explicit `as` casts; implicit narrowing is a
  diagnostic.
- `as` casts exist between numeric types and `bool`, verified by `3.5 as int`.
- `string` → `string_owned` cast is rejected: `NRQ3015: cannot cast value of type
  'string' to 'string_owned'`.

### 5.2 Struct fields and pointer ownership
- Pointer-typed fields must carry a qualifier:
  `pointer field 'left' needs an ownership qualifier: declare it 'owned', 'borrow'
  or 'borrow mut'`.
- **`owned`, `shared` and `borrow` pointers cannot be stored in aggregate fields**
  (`owned, shared, and borrowed pointers cannot be stored in aggregate fields`).
  Only `shared ptr`/`managed<T>` fields survive, and a `shared` field may only be
  initialised to `nullptr` — assigning one after construction is rejected by the
  same rule. Consequence: a recursive or cyclic structure cannot be built in
  source today (see §10, blocker B3).

### 5.3 Address-of limitation
`&x` works for scalars. Taking the address of a struct-typed local is rejected:
`taking the address of 'Point' is not supported yet`.

### 5.4 Containers
- `memory.dynamic_array<T>()` + `memory.array_append(a, v)` + `a.length` + `a[i]`
  ✔ run.
- Adding, `append`, indexing and length all work (verified: `2`, `8`).
- `array_view<T>`: `memory.array_view(x)` is **not** a declared builtin at call
  site (`NRQ3011: call to undeclared function 'memory.array_view'`), although the
  type and some Sema strings exist.
- `dynamic_array<T>` cannot hold owned elements yet: the parser rejects
  `dynamic_array<string_owned>` / `owned_array<T>` / `soa<T>` elements with
  "the element teardown loop is not implemented".
- `soa<T>` requires a struct element (`NRQ:S oaNotReady` diagnostic).

### 5.5 Tuples
Type kind, literals `(1, 2)`, `.0`/`.1` access, and destructuring declarations all
exist and were verified by run.

### 5.6 `@Nullable`
Parsed as an annotation before a declaration. Verified behaviour: `@Nullable int
maybe = 5;` then `maybe != nullptr` is rejected — `nullptr can only be compared
with a pointer, not 'int'`. The annotation is therefore usable only on pointer
types; on scalars it is accepted at declaration and useless at the check.

---

## 6. Builtins (compiler-injected, `memory.`/`gc.`/`file.`/`…` namespaces)

The checker resolves these names directly (`TypeChecker.cpp`); they are not
declared in source and cannot be shadowed by an import.

### 6.1 `memory.` — 70 entries
Allocation: `allocate`, `manual_allocate`, `manual_release`, `reallocate`,
`secure_allocate`, `secure_release`, `secure_zero`, `deallocate`,
`shared_allocate`, `shared_retain`, `shared_release`, `shared_count`,
`get_ref_count`, `pin`, `unpin`, `pin_count`, `pin_violations`.
Allocators: `general_allocator`, `arena_allocator`, `pool_allocator`,
`stack_allocator`, `slab_allocator`, `buddy_allocator`, `free_list_allocator`,
`tlsf_allocator`, `region`, `arena`, `pool`, `use_allocator`, `reset_allocator`,
`destroy_allocator`, `get_allocator`, `get_default_allocator`,
`set_default_allocator`, `allocator_kind`, `arena_reset`, `arena_destroy`,
`pool_reset`, `pool_release`, `pool_destroy`.
Containers: `dynamic_array`, `owned_array`, `owned_array_aligned`, `owned_soa`,
`array_append`, `array_live_count`, `array_view`, `array_view_mut`.
Files/mapping: `map_file`, `unmap_file`, `mapped_read`, `mapped_write`,
`mapping_size`.
Introspection: `runtime_version`, `total_bytes`, `used_bytes`,
`available_bytes`, `cpu_count`, `cpu_features`, `thread_count`,
`device_memory_total_bytes`, `device_memory_used_bytes`, `start_ref_count`,
`stop_ref_count`.

### 6.2 `gc.`
`collect`, `enable`, `disable`, `is_enabled`, `set_threshold`, `get_statistics`
(returns a struct with `live_objects` and related counters, ✔ run).

### 6.3 `file.`
`open`, `close`, `read`, `write`, `exists` ✔ run, `read_text` ✔ run,
`read_text_owned`, `write_text` ✔ run, `append_text`, `free_text`,
`text_live_count`.
Text buffers: `file.read_text` returns a **borrowed, process-lifetime** string
that must never be freed. Freeing either kind explicitly terminates the process
with the runtime fatal (exit 70) — see §8.

### 6.4 `math.`
`abs`, `sqrt` (✔ run on floats), `sin`, `cos`, `floor`, `ceil`.
`math.abs` on an **integer** fails to lower: `'llvm.call' op operand type mismatch
for operand 0: 'i64' != 'f64'` (`NRQ5004`). Float arguments work.

### 6.5 Others
- `mutex.create/destroy/lock/unlock` — represented, resource-domain checked.
- `socket.connect/close/send/receive`.
- `time.now`, `time.cycles`.
- `system.exit`.
- `array.allocate`, `array.owned_release`, `soa.owned_release`,
  `aggregate.owned_drop` (internal ownership plumbing).
- `print` — one argument; accepts numeric, `bool`, `char`, `string`; prints a
  newline. There is no `print` formatting, no `println` distinction, no
  `eprint`.
- `argv`-style access exists at the runtime level (`nrq_args_build`,
  `nrq_args_length`, `nrq_args_get`) but is **not exposed** as a language builtin
  — a program cannot read its own arguments today.

---

## 7. Runtime

`runtime/` (headers under `runtime/include/nirqoba/runtime/`, 302 exported `nrq_`
symbols) provides:

`allocator`, `alloc_hint`, `args`, `array`, `cache_line`, `cdmm`, `fatal`, `ffi`,
`file`, `gc`, `gc_debug`, `gc_inline`, `io`, `math`, `socket`, `sync`, `system`,
`version`.

- **CDMM** (`cdmm.h`) — the stack/pool/arena/collector selector's runtime half.
- **FFI** (`ffi.h`) — one exported path, `nrq_ffi_free`, for releasing storage a C
  function transferred to Nirqoba; nothing else crosses the boundary implicitly.
- **Fatal** (`fatal.h`) — `nrq_runtime_fatal` writes to stderr and exits `70`.
  Documented as *internal invariant* protection, not a language feature.
- I/O: console (`io`), files (`file`), sockets (`socket`), memory-mapped files.
- Synchronisation: mutex only (no atomics, channels, threads, or thread spawning).
- Diagnostics: `gc_debug` instrumentation, `--inspect-cdmm` classification report.

---

## 8. Error handling

There is **no user-facing error mechanism** in the language:

| Mechanism | Status |
|---|---|
| Exceptions (`try`/`catch`/`throw`) | absent |
| `Result`/`Option` types | absent |
| `panic()` / `abort()` / `assert()` builtins | absent |
| `unreachable()` | absent (the string `"unreachable"` exists only in internal TIR lowering) |
| Process exit | `system.exit(code)` ✔ code |
| Runtime fatal | `nrq_runtime_fatal` → exit `70`, not reachable from source |
| File/resource failure | builtins return sentinels (`file.exists` → bool); no error channel |

Observed consequences worth stating plainly:

1. Calling `file.free_text` on a borrowed `file.read_text` result compiles and
   then aborts the process with exit `70` — the ownership rule is enforced at run
   time by the fatal, not at compile time.
2. `file.write_text` + `file.read_text` + `print` works (exit 0). The same program
   with an added `file.free_text(text)` prints the text, then dies with exit `70`.

A language with no exceptions, no `Result` and no panic leaves I/O error handling
entirely to sentinel values and process death. This is a Version 1.0 blocker.

---

## 9. Verified probe matrix

| # | Program | Result |
|---|---|---|
| A | `for` + `if` + `continue` + `break` + bitwise + shifts + `**` + `%` + `~` + `as` + `defer` | `31 14 16 32 1024 2 -1 3 body outer`, exit 0 |
| B | struct + `borrow Point*` parameter via `&p` | rejected: `taking the address of 'Point' is not supported yet` |
| C | `file.*` + `math.sqrt` + `math.abs(-4)` | lowering error `i64 != f64` |
| D | `borrow int*` + `&x` + `dynamic_array` + tuple destructure | `42 2 8 3` (with `@Nullable` line removed; see F) |
| E | `file.write_text` / `exists` / `read_text` / `free_text` | prints text, then exit **70** |
| F | `borrow int*`, `dynamic_array`, tuple, `math.abs(-4.5)` | `42 2 8 3 4.5`, exit 0 |
| G | `read_text_owned` + `free_text` + `text_live_count` | text, `0`, then exit **70** |
| H | `read_text` alone | `hello file`, exit 0 |
| I | `read_text_owned` alone | `hello file`, exit 0 |
| J | `&&`, `\|\|`, `!` on `bool` | `and or false false`, exit 0 |
| K | `extern int function puts(ptr byte text);` called with a string | `argument 1 … must be of type 'u8*', not 'string'` |
| L | `"x" as string_owned` | `cannot cast value of type 'string' to 'string_owned'` |
| M–O | `extern int function abs(int)` called with a literal / a variable / `i32` | `llvm.call operand type mismatch 'i64' != 'f64'` for all three |
| — | `i++` | `NRQ2001: expected an expression, found ';'` |
| — | `switch` | parse error |
| — | `x > 0 ? 5 : 6` | `NRQ1008: unexpected character '?'` |
| — | `do { } while (…)` | parse error |
| — | `"hello" + " world"` | `NRQ3015: operator '+' requires numeric operands` |

---

## 10. Version 1.0 blockers

Ordered by how much they cost a real program, not by how hard they are.

**B1 — No error handling of any kind.** No exceptions, no `Result`, no `panic`,
no `assert`. Every fallible operation is a sentinel plus hope, and the only
failure path that exists ends the process. Nothing in the standard library can
report a failure to its caller.

**B2 — FFI is declared but cannot be called with scalar arguments.** `extern`
parses and typechecks; the moment a call carries an `int` the lowering fails with
`'llvm.call' op operand type mismatch for operand 0: 'i64' != 'f64'` (`NRQ5004`).
Strings cannot cross the boundary either, because there is no `string` → `u8*`
conversion. A language that cannot call a C function with an integer argument
cannot claim a runtime boundary at all. This is a codegen defect, not a missing
feature.

**B3 — Pointers cannot live in aggregates, so recursive data is unbuildable.**
`owned`/`shared`/`borrow` pointer fields are rejected outright, and a `shared`
field cannot be assigned after construction (`(*a).next = b` →
`owned, shared, and borrowed pointers cannot be stored in aggregate fields`). Every
tree, list, graph and cycle in the current test suite is therefore either a flat
block or absent. This is the single largest gap between Nirqoba and the languages
in `stress_test_5`.

**B4 — No `++`/`--`, no ternary, no `?:`.** `for` loops must spell `i += 1`; the
one-line conditional must become a five-line `if`. These are the constructs a
competent programmer reaches for in the first ten minutes, and the brief's example
of a missing feature is real.

**B5 — No `switch`/`match`.** Only `if`/`else if` chains. No exhaustiveness
checking, no destructuring in patterns.

**B6 — String handling is a dead end.** No concatenation (`+` is numeric only), no
comparison beyond `==`, no length builtin exposed to source, no formatting, no
`substring`/`contains`/`split`, no `int`↔`string` conversion. A program can print a
string and write it to a file; it cannot build one.

**B7 — Array views and multi-element indexing.** `array_view<T>` exists as a type
with no constructor builtin at call site. And `memory.allocate<T>(n)` is reliable
at element 0 only: writing `values[1]` and reading it back returns the wrong value.
Every benchmark in this repository is shaped around that defect.

**B8 — No user-facing panic or assertion.** Even without full error handling, a V1
language needs a documented way to abort with a message. Today the only abort is
`nrq_runtime_fatal`, described in its own header as internal.

**B9 — No program arguments.** The runtime builds `argc`/`argv`, but no builtin
exposes them; a Nirqoba program cannot read its command line.

**B10 — `math` is half-broken.** `math.abs` on an integer fails to lower; only the
float overload works. `math` has six functions and no `min`/`max`/`pow`/`round`.

### Secondary defects found while auditing (not blockers)

- `@Nullable` on a scalar is accepted at declaration and then cannot be
  null-checked (`nullptr can only be compared with a pointer, not 'int'`).
- `file.free_text` on a borrowed `read_text` result compiles, then exits 70.
- `&` cannot be taken of a struct-typed local.
- The `->` token is lexed but no grammar rule consumes it.
- Multi-field structs promoted to a loop's stack slot read their fields back stale
  (`0.670-beta`, characterized in `benchmarks/stress_test_5/README.md`).

---

## 11. What is genuinely solid

For balance, the parts a developer can rely on today:

- A bounded, checked arithmetic and bitwise surface with explicit casts.
- Real ownership analysis: `borrow`/`borrow mut`/`shared`/`owned` parameters,
  move checking, resource domains, deferred cleanup (`defer`) with deterministic
  ordering.
- CDMM: automatic stack/pool/arena/collector selection, reported per site,
  measured at parity with manual allocation for stack and arena tiers.
- A working module system with `private` enforcement and `--module-path` imports.
- Deterministic file I/O, memory-mapped files, sockets and mutexes in the runtime.
- Generational/cycle-capable collector with `gc.get_statistics`, enable/disable and
  threshold control.

The foundation is a systems language with a strong memory model and a thin,
partially broken surface language. The V1 work is not in the model; it is in the
grammar, the aggregates, the strings and the error channel.
