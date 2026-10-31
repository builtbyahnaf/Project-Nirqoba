# Nirqoba Compiler CLI Reference

## Commands

```text
nirqoba build <file.nrqba>... [--out-dir DIR] [-o PATH] [--module-path DIR] [--emit=ast|tir|mlir|llvm|obj] [--verbose]
nirqoba run   <file.nrqba>... [--out-dir DIR] [--module-path DIR] [--verbose] [-- <args>...]
nirqoba check <file.nrqba>... [--module-path DIR] [--emit=ast|tir]
nirqoba version | --version
nirqoba help [command] | --help
```

### Multi-file projects

Every file named on the command line is a root module. Modules reached through
`import` statements are discovered automatically: the importer's directory is
searched first, then each `--module-path` directory, for `<module>.nrqba`.

```text
nirqoba run src/main.nrqba --module-path src/lib
```

The module that defines `main` is the entry module: its functions keep their
bare symbol names, while functions in every other module are emitted as
`module.function`. The first file on the command line names the output artifact.

```nirqoba
module geometry;

public int function calculate_area(int width, int height) {
    return width * height;
}
```

### Exit Codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Compilation or linking failure (diagnostics on stderr) |
| `2` | Invalid command line usage |
| `3` | Input / file I/O error |
| `4` | Environment / toolchain error |

## Diagnostic Codes

| Code | Severity | Description |
|---|---|---|
| `NRQ1005` | Error | Empty source file |
| `NRQ1006` | Error | Unknown escape sequence in string literal |
| `NRQ1007` | Error | Unterminated string literal |
| `NRQ1008` | Error | Unexpected character in source |
| `NRQ1009` | Error | Integer literal exceeds 64-bit integer range |
| `NRQ2001` | Error | Unexpected token |
| `NRQ2002` | Error | Expected specific token |
| `NRQ2003` | Error | Unknown type |
| `NRQ2100` | Error | Language construct specified for a future phase |
| `NRQ3001` | Error | Missing entry point (`main`) |
| `NRQ3002` | Error | Duplicate definition of `main` |
| `NRQ3003` | Error | Invalid return type for `main` (must be `int` or `void`) |
| `NRQ3004` | Error | Parameters not allowed on `main` in Phase 1 |
| `NRQ3005` | Error | Void function returns a value |
| `NRQ3006` | Error | Non-void function reaches end of body without return |
| `NRQ3010` | Error | `print` requires a `string` argument in Phase 1 |
| `NRQ3011` | Error | Undefined variable name |
| `NRQ3012` | Error | Duplicate variable declaration in scope |
| `NRQ3013` | Error | Expression is not callable |
| `NRQ3014` | Error | Assignment to immutable binding (`let` or `const`) |
| `NRQ3015` | Error | Type mismatch |
| `NRQ3016` | Error | Duplicate function definition |
| `NRQ3017` | Error | Value required in return statement |
| `NRQ3018` | Error | Incorrect arity in function call |
| `NRQ3028` | Error | Imported module not found in the search paths |
| `NRQ3029` | Error | Cyclic module dependency |
| `NRQ3030` | Error | Access to a `private` declaration from another module |
| `NRQ3031` | Error | Duplicate module declaration, or two files declaring the same module |
| `NRQ3032` | Error | Imported name conflicts with an existing declaration |
| `NRQ4001` | Error | Internal compiler error: unresolved symbol in TIR |
| `NRQ4002` | Error | Internal compiler error: unsupported expression in TIR |
| `NRQ4003` | Error | Internal compiler error: structural invariant violation in TIR |
| `NRQ4004` | Error | Internal compiler error: type mismatch in TIR verification |
| `NRQ4005` | Error | Internal compiler error: unsupported type during MLIR lowering |
| `NRQ5001` | Error | Cannot open source file |
| `NRQ5002` | Error | Invalid command line argument |
| `NRQ5003` | Error | Toolchain configuration (`toolchain.json`) not found |
| `NRQ5004` | Error | Internal compiler or LLVM target error |
| `NRQ5005` | Error | Linker failure or missing runtime library |
| `NRQ5006` | Error | Generated program failed to launch |
| `NRQ5007` | Error | Invalid source file extension |
