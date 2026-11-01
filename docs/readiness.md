# Language Readiness Audit and Benchmark

**Audit date:** 2026-11-01
**Compiler:** Nirqoba 0.1.0 (Phase 6), LLVM/MLIR 23.1.4, `x86_64-pc-windows-msvc`
**Build:** RelWithDebInfo (`build/msvc-dev`)
**Host:** Windows x64, Intel Family 6 Model 154, 8+ logical cores
**Corpus:** `tests/readiness/` — 13 programs, 5 gap probes, 4 benchmarks
**Runner:** `python tests/readiness/run_readiness.py --runs 7`

---

## 1. Question

Phase 6 claims a working pipeline from source to native executable. That says the pieces
connect; it does not say a person can sit down and write a normal program with them. This
audit answers one question:

> **Can someone write and run ordinary, non-trivial logic programs in Nirqoba today?**

The method is deliberately not "does construct X compile". Each program below is a task a
beginner or an ordinary tool would write — FizzBuzz, sorting, a small inventory, a tokenizer —
and each is checked for the *correct answer*, not merely for a clean exit. Everything the
corpus could not use is captured as a gap probe with its exact diagnostic.

---

## 2. Result in one line

**Yes for self-contained batch programs over scalars, fixed arrays, structs and char buffers.
No for programs that need strings as text, dynamic collections, error handling, modules, or
several files.** 13 of 13 corpus programs now run correctly; they needed 4 language/runtime
defects fixed during the audit, and they still have to avoid five specified-but-unimplemented
features.

---

## 3. Corpus and results

Every program is compiled through the real `nirqoba run` pipeline and compared against a
recorded expected answer (`<name>.expect.txt`). "Time" is the whole pipeline plus execution,
so it is dominated by process start-up (~15 ms) — the benchmark section measures execution
separately.

| Program | What it exercises | Result | Time |
|---|---|---|---|
| `fizzbuzz` | `for`, `if`/`else if`, `%`, `print` per type | pass | 0.10 s |
| `sieve` | `bool[120]` array, nested `while`, accumulation | pass | 0.12 s |
| `fibonacci` | recursion vs iteration, loop-carried state | pass | 0.10 s |
| `sorting` | in-place insertion sort over `int[10]` | pass | 0.11 s |
| `records` | `struct`, `Product[4]`, field access, `f64` totals | pass | 0.11 s |
| `matrices` | `int[2][3]` × `int[3][2]`, triple-nested loops | pass | 0.11 s |
| `binary_search` | early `return`, sentinel `-1`, comparisons | pass | 0.10 s |
| `char_text` | `char[11]`, vowel test, in-place reversal | pass | 0.10 s |
| `state_machine` | symbolic states as functions, transition logic | pass | 0.11 s |
| `numeric` | gcd/lcm, Collatz search, `math.sqrt/abs` | pass | 0.11 s |
| `recursion` | mutual recursion, Hanoi count | pass | 0.10 s |
| `errors` | manual status codes and sentinel returns | pass | 0.10 s |
| `tokenizer` | char scanning, `char` arithmetic, digit runs | pass | 0.11 s |

Reproduce with:

```powershell
ninja -C build\msvc-dev
python tests/readiness/run_readiness.py --runs 7
```

### 3.1 What the corpus proved is usable

- Scalar types with distinct widths, `as` casts, integer and float arithmetic (integer
  division truncates toward zero: `-7 / 2` prints `-3`).
- `if`/`else if`/`else`, `while`, C-style `for`, `break`/`continue`, early `return`.
- Functions with recursion and mutual recursion (forward declarations are not needed),
  parameters are assignable inside the body.
- Fixed arrays of scalars, structs and nested arrays, with array literals and bounds-checked
  constant indices.
- Structs as locals, struct arrays, positional and named initialisers, field reads and writes.
- `char` and `bool` values, `char` relational comparisons (`c >= '0'`), `char`↔`int` casts.
- `string` values for printing and `.length`.
- `math.sqrt/abs/floor/ceil/sin/cos`, `time.now`, `file.read_text/write_text/exists`,
  `system.exit`, `input`.
- The Phase 6 memory surface (`memory.allocate<T>`, `gc.*`, allocators) — exercised by the
  existing integration suite, not repeated here.

---

## 4. Gap probes (specified, but unavailable)

`tests/readiness/gaps/` holds one minimal program per missing feature. The runner builds each
and records the diagnostic:

| Feature | Where it is specified | Actual result |
|---|---|---|
| `try` / `catch` | SYNTAX.md §5.4 | `NRQ2002: expected '}' to close struct initializer` — `try` is parsed as an identifier |
| String indexing `text[0]` | SYNTAX.md §4.7 | Type-checks, fails in lowering: `'llvm.getelementptr' op type '!llvm.ptr' cannot be indexed` |
| String concatenation `a + b` | SYNTAX.md §2.9 (interpolation) | `NRQ3015: operator '+' requires numeric operands, found 'string' and 'string'` |
| Dynamic arrays `int[]` + `.push` | SYNTAX.md §4.2 | `NRQ3015: cannot initialize 'numbers' of type 'int[]' with a value of type 'int[3]'` |
| Implicit `int` → `i32` | SYNTAX.md §2.1/§2.2 | `NRQ3015: field 'hits' of struct 'Counter' has type 'i32', but was given 'int'` |

### 4.1 Semantics that differ from what a newcomer expects

These compile and run, but behave in a way that is not written down anywhere, so a newcomer
cannot predict them:

1. **Aggregate arguments are passed by value.** Modifying an array or struct parameter does
   not affect the caller:

   ```nirqoba
   struct Counter { int value; }
   int function bump(Counter c) {
       c.value = c.value + 1;
       return c.value;
   }
   // bump(counter) prints 11, then counter.value prints 10
   ```

   This is why `sorting` sorts inside `main` and `errors` cannot report a status through an
   array parameter. There is no `&` address-of operator, so the only way to mutate through a
   function today is to allocate on the heap (`array.allocate<T>`, `memory.allocate<T>`) and
   pass the pointer. **The specification must either state value semantics explicitly and
   provide a reference/borrow parameter form, or switch aggregates to reference semantics.**

2. **No forward declarations.** `int function f(int n);` is a parse error
   (`NRQ2002: expected '{' to start the function body`). Mutual recursion still works because
   signatures are collected first, but header-style declaration is impossible.

3. **No implicit numeric conversions at all.** `int` and `i32` are distinct types, `bool` does
   not convert to `int`, and `int` does not widen to `f64` without `as`. The corpus had to
   write `bool function is_vowel(...)` because `int` was not accepted for a boolean result.

4. **`int[]` and `int[3]` are unrelated types.** A fixed array cannot be passed where a
   dynamic array is expected, so the specified `string[] args` entry point cannot receive a
   literal list.

5. **Division by zero is a compile-time diagnostic only for literals.** `10 / 0` with literals
   is rejected (`division by constant zero`); at runtime nothing traps, and there is no way to
   catch it — see §4, `try/catch`.

6. **Modules exist as declarations but are not a working multi-file workflow** in practice:
   there is no `public` re-export story for structs across modules beyond what Phase 5 tested,
   and the corpus stayed single-file.

---

## 5. Defects found and fixed during the audit

The audit was not allowed to end with "the corpus fails"; anything the corpus exposed that was
a bug rather than a missing feature was fixed.

| # | Defect | Impact | Fix |
|---|---|---|---|
| 1 | `alloca` was emitted **inside loop bodies** (`tir.alloca` translated at its insertion point) | Any loop with a local variable grew the stack every iteration and aborted with `STATUS_STACK_BUFFER_OVERRUN` after ≈65 000 iterations — FizzBuzz-scale loops were fine, real loops were not | `compiler/lib/Lowering/TirToMlir.cpp`: every stack slot is now allocated in the function entry block (`alloca_at_entry`) |
| 2 | GC root registration ran **per loop iteration**, re-registering the same slot | Root frames grew without bound and collections got slower over time | Registrations are queued and emitted once per activation, in the entry block |
| 3 | Struct annotations carried no field layout (`Type::struct_type(name)` only) | `var p: Point = ...` failed with `Cannot allocate unsized type`; struct arrays failed with `GEP into unsized type`; member reads could crash the compiler | `TypeChecker::resolve_aggregate_type` expands struct names (recursively through arrays/tuples, cycle-safe) and records the resolved type back on the declaration |
| 4 | Nested array suffixes were associated in reverse | `int[2][3]` built `3 × int[2]`, so every non-square 2-D array failed to initialise | `compiler/lib/Frontend/Parser.cpp`: suffixes are applied outermost-first |
| 5 | Parameters were `let` bindings | No function could assign to its own parameter, so no helper could fill a buffer or update a struct copy | Parameters are `var` bindings (`TypeChecker::check_function`) |
| 6 | `char` was rejected in `<`, `<=`, `>`, `>=` | Digit/letter range checks (`c >= '0' && c <= '9'`) were impossible | Ordering comparisons accept `char` operands |

Regression coverage added: `tests/integration/positive/locals_in_loops` (200 000 iterations
with a local and an allocation per iteration) and `tests/integration/positive/struct_locals`
(struct locals, struct arrays, 2-D arrays, parameter assignment, `char` comparison).
Full suites after these changes: **55/55 unit, 29/29 integration**.

---

## 6. Benchmarks

Method: the runner builds each program once into a scratch directory, then times the
executable directly (`nargs=7`, median reported), so compilation is excluded. An empty program
is timed the same way to expose process start-up cost.

| Benchmark | Work | Median | Min | Max | Per operation |
|---|---|---|---|---|---|
| start-up baseline | empty `main` | 0.0144 s | — | — | — |
| `bench_loop` | 100,000,000 iterations of `+=`, `%`, `^=`, `&=` | 0.1081 s | 0.1040 s | 0.1781 s | ≈1.08 ns/iteration |
| `bench_fib` | `fib(32)` recursion, ≈7.0 M calls | 0.1438 s | 0.1246 s | 0.2078 s | ≈20 ns/call |
| `bench_alloc` | 1,000,000 `memory.allocate<byte>(64)` + `deallocate` | 0.1278 s | 0.1208 s | 0.2012 s | ≈114 ns per alloc+free pair |
| `bench_gc_array` | 1 M-element managed `int` array: fill, sum, 20 explicit collections | 0.0200 s | 0.0191 s | 0.0688 s | ≈2 ns per element visit |

Reading the numbers:

- **Arithmetic and control flow are essentially free** once compiled: 100 M loop iterations in
  108 ms means the generated code is comparable to what a plain C loop would do with the same
  body. There is no interpreter in the path.
- **Function calls cost ≈20 ns** in this recursion, dominated by the GC root frame
  begin/leave around every activation. That is the single clearest optimisation target: the
  root frame protocol costs more than the arithmetic in `fib`.
- **Explicit allocation costs ≈57 ns per call plus ≈57 ns per free**, of which the
  registry lookup under one mutex dominates. Acceptable for bulk allocation, costly for
  churn: allocating a 16-byte node per iteration caps out around 8–9 M allocations/s.
- **Managed arrays are cheap to touch** (≈2 ns/element for a write sweep plus a read sweep),
  and collecting a leaf array is O(1) in references — the 20 collections add almost nothing.
  This is expected for a non-moving mark-sweep tracing *typed* references only.
- Variance is dominated by process launch and CI-host noise (min vs max differ by up to 60 %),
  which is why medians, not means, are reported.

These are single-machine observations, not comparative claims against any other language.

---

## 7. Sufficiency by task size

| Task shape | Ready? | Notes |
|---|---|---|
| Algorithm exercises (loops, arrays, recursion) | **Yes** | All 13 corpus programs are of this shape |
| Small tools over numeric/char data | **Mostly** | Works with `char` buffers; text is the limit |
| Record-oriented programs (inventory, grades) | **Yes**, single file | Structs and struct arrays work; multi-file does not |
| Programs that report failure to the user | **No** | No exceptions, no `Result`, `try/catch` unimplemented |
| Programs that build or search text | **No** | No string indexing, concatenation, slicing, or search |
| Programs with data-driven collections | **No** | No growable arrays, maps, or lists |
| Multi-file programs / libraries | **No** | Modules parse, but the workflow is untested beyond Phase 5 fixtures |
| Programs reading configuration files | **Partially** | `file.read_text` returns a `string` you cannot inspect character by character |
| Interactive programs | **Partially** | `input()` exists; string handling limits what you can do with it |

---

## 8. Priority recommendations

In dependency order — each unlocks the next set of programs:

1. **Error handling.** `try`/`catch` (in progress) plus a `Result` type. Without it every
   fallible operation is a silent wrong answer.
2. **Strings as text.** Indexing, length, slicing, concatenation, comparison, and
   `contains/find/split`. Until then the string type is a print buffer, and roughly half of
   §7 stays red.
3. **Growable collections.** Dynamic arrays with `push`/`pop`/`remove_at` were specified in
   Phase 4 and never implemented; they also unblock `string[] args`.
4. **Aggregate parameter semantics.** Decide value vs reference in SYNTAX.md and implement
   the chosen form (a reference parameter or `&`). Today a helper cannot fill a buffer, which
   forces everything into `main`.
5. **Numeric conversions.** Either allow `int`→`i32`/`f64` widening implicitly or make the
   distinction a documented rule with a compiler hint; the current silence costs every
   beginner a diagnostic round-trip.
6. **Front-end ergonomics.** Forward declarations, and a diagnostic that names the missing
   feature (`try` currently reports "expected '}' to close struct initializer").

---

## 9. Reproducing this audit

```powershell
# build
cmd /c "call ...\vcvars64.bat && ninja -C build\msvc-dev"

# full corpus, gap probes and benchmarks
python tests/readiness/run_readiness.py --runs 7 --json readiness.json

# the gate this audit changed
build\msvc-dev\tests\nirqoba_unit_tests.exe
python tests\run_tests.py --compiler build\msvc-dev\bin\nirqoba.exe
```

The runner returns 0 only when every corpus program matches its expected answer and every
benchmark executes; gap probes are reported but do not fail the run, because they describe the
language rather than the build.
