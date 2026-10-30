# 01 — Frontend Architecture

## 1. Source Mapping

`SourceFile` owns the source buffer text and builds a 0-based offset to 1-based line/column index upon construction.
Columns count UTF-8 bytes in Phase 1 (full Unicode grapheme cluster counting is planned for Phase 4). Every token, AST node, and diagnostic holds an immutable `SourceLocation` span (`file`, `offset`, `line`, `column`, `length`).

## 2. Lexer Design

The lexer uses maximal munch over `std::string_view`.
- String literals decode escape sequences (`\n`, `\t`, `\r`, `\\`, `\"`, `\0`), storing both raw source text and unescaped value.
- Decimal integer literals are validated against 64-bit integer limits (`int` is fixed `i64`).
- All Phase 2 operators (`+`, `-`, `*`, `/`, `%`, `**`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&&`, `||`, `!`, `&`, `|`, `^`, `~`, `<<`, `>>`, `=`, `+=`, `-=`, etc.) and keywords (`if`, `else`, `while`, `for`, `break`, `continue`, `struct`, `module`, `import`, `async`, `await`, etc.) are recognized at the lexer level to avoid lexer churn in future phases.

## 3. Parser & AST

The parser is a hand-written recursive-descent parser.
- Constructs strongly typed AST node classes with tag discriminators.
- Recovers from syntax errors by synchronizing to statement boundaries (semicolons, closing braces, declaration keywords) rather than crashing or terminating early.
- Explicitly rejects Phase 2 operators and reserved keywords with diagnostic code `NRQ2100`, naming the phase in which they are scheduled.
