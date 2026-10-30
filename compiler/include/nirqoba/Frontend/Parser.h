// Recursive-descent parser for Phase 1, 2 & 3 grammar.
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
  void report_phase_gap(const Token& token, std::string_view feature, std::string_view phase);
  void synchronize();

  // Declarations.
  std::unique_ptr<FunctionDecl> parse_function();
  Type parse_type_annotation();

  // Statements.
  std::unique_ptr<BlockStmt> parse_block();
  StmtPtr parse_statement();
  StmtPtr parse_variable_declaration();
  StmtPtr parse_assignment_or_expression();
  StmtPtr parse_if();
  StmtPtr parse_while();
  StmtPtr parse_for();
  StmtPtr parse_break();
  StmtPtr parse_continue();

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

std::string_view phase_for_keyword(TokenKind kind);

}  // namespace nirq
