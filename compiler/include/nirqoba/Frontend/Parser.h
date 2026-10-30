// Recursive-descent parser for the Phase 2 grammar.
//
// Grammar implemented (SYNTAX 2.0, ROADMAP Phase 2):
//
//   program      := function_decl*
//   function_decl:= type 'function' IDENT '(' [param (',' param)*] ')' block
//   param        := type IDENT
//   block        := '{' statement* '}'
//   statement    := var_decl | compound_assign | assignment | return_stmt
//                 | expr_stmt | block
//   var_decl     := ('let'|'var'|'const') IDENT [':' type] '=' expr ';'
//   assignment   := IDENT '=' expr ';'
//   compound_assign := IDENT compound_op expr ';'
//   compound_op  := '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^='
//                 | '<<=' | '>>='
//   return_stmt  := 'return' [expr] ';'
//   expr_stmt    := expr ';'
//   expr         := logical_or
//   logical_or   := logical_and ('||' logical_and)*
//   logical_and  := bitwise_or ('&&' bitwise_or)*
//   bitwise_or   := bitwise_xor ('|' bitwise_xor)*
//   bitwise_xor  := bitwise_and ('^' bitwise_and)*
//   bitwise_and  := equality ('&' equality)*
//   equality     := relational (('=='|'!=') relational)*
//   relational   := shift (('<'|'<='|'>'|'>=') shift)*
//   shift        := additive (('<<'|'>>') additive)*
//   additive     := multiplicative (('+'|'-') multiplicative)*
//   multiplicative := cast (('*'|'/'|'%') cast)*
//   cast         := exponentiation ('as' type)*
//   exponentiation := unary ('**' exponentiation)?   // right-associative
//   unary        := ('-'|'+'|'!'|'~') unary | primary
//   primary      := INT | FLOAT | CHAR | STRING | 'true' | 'false'
//                 | IDENT ['(' [expr (',' expr)*] ')']
//                 | 'print' '(' [expr (',' expr)*] ')'
//                 | '(' expr ')'
//
// Everything the lexer recognizes but Phase 2 does not implement is rejected
// with NRQ2100 naming the phase that owns it; nothing is silently skipped.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/Frontend/Token.h"

namespace nirq {

class Parser {
 public:
  Parser(std::vector<Token> tokens, DiagnosticEngine& diagnostics);

  // Returns nullptr only when the token stream contains no function at all
  // after recovery; otherwise a tree that may still be incomplete.
  std::unique_ptr<Program> parse_program();

 private:
  // Token helpers.
  const Token& peek(std::size_t lookahead = 0) const;
  const Token& previous() const;
  bool check(TokenKind kind) const;
  bool match(TokenKind kind);
  Token advance();
  const Token& expect(TokenKind kind, std::string_view context);

  // Diagnostics.
  void error_at(const Token& token, std::string_view code, std::string message);
  // Reports NRQ2100 for a recognised-but-unimplemented construct.
  void report_phase_gap(const Token& token, std::string_view feature, std::string_view phase);
  // Skips to the end of the current statement or block after a syntax error so
  // one mistake produces one diagnostic instead of a cascade.
  void synchronize();

  // Declarations.
  std::unique_ptr<FunctionDecl> parse_function();
  Type parse_type_annotation();

  // Statements.
  std::unique_ptr<BlockStmt> parse_block();
  StmtPtr parse_statement();
  StmtPtr parse_variable_declaration();
  StmtPtr parse_assignment_or_expression();

  // Expressions — full Pratt hierarchy.
  ExprPtr parse_expression();
  ExprPtr parse_logical_or();
  ExprPtr parse_logical_and();
  ExprPtr parse_bitwise_or();
  ExprPtr parse_bitwise_xor();
  ExprPtr parse_bitwise_and();
  ExprPtr parse_equality();
  ExprPtr parse_relational();
  ExprPtr parse_shift();
  ExprPtr parse_additive();
  ExprPtr parse_multiplicative();
  ExprPtr parse_cast();
  ExprPtr parse_exponentiation();
  ExprPtr parse_unary();
  ExprPtr parse_primary();
  std::vector<ExprPtr> parse_argument_list();

  std::vector<Token> tokens_;
  std::size_t position_ = 0;
  DiagnosticEngine& diagnostics_;
};

// Phase that owns a reserved keyword or operator, used by NRQ2100 messages.
std::string_view phase_for_keyword(TokenKind kind);

}  // namespace nirq
