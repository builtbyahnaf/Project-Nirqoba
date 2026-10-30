#include "nirqoba/Frontend/Parser.h"

#include <utility>

namespace nirq {
namespace {

constexpr const char* kUnknownType = "NRQ2003";

bool is_type_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwInt:
    case TokenKind::KwI8:
    case TokenKind::KwI16:
    case TokenKind::KwI32:
    case TokenKind::KwI64:
    case TokenKind::KwU8:
    case TokenKind::KwU16:
    case TokenKind::KwU32:
    case TokenKind::KwU64:
    case TokenKind::KwByte:
    case TokenKind::KwChar:
    case TokenKind::KwF32:
    case TokenKind::KwF64:
    case TokenKind::KwString:
    case TokenKind::KwBool:
    case TokenKind::KwVoid:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string_view phase_for_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwFn:
      return "Phase 3 (use 'function', not 'fn')";
    case TokenKind::KwStruct:
      return "Phase 4";
    case TokenKind::KwModule:
    case TokenKind::KwImport:
    case TokenKind::KwPublic:
    case TokenKind::KwPrivate:
    case TokenKind::KwProtected:
      return "Phase 5";
    case TokenKind::KwAsync:
    case TokenKind::KwAwait:
      return "Phase 10";
    default:
      return "a later phase";
  }
}

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diagnostics)
    : tokens_(std::move(tokens)), diagnostics_(diagnostics) {}

const Token& Parser::peek(std::size_t lookahead) const {
  const std::size_t index = position_ + lookahead;
  if (index >= tokens_.size()) return tokens_.back();
  return tokens_[index];
}

const Token& Parser::previous() const {
  return position_ == 0 ? tokens_.front() : tokens_[position_ - 1];
}

bool Parser::check(TokenKind kind) const { return peek().kind == kind; }

bool Parser::match(TokenKind kind) {
  if (!check(kind)) return false;
  ++position_;
  return true;
}

Token Parser::advance() {
  const Token token = peek();
  if (position_ + 1 < tokens_.size()) ++position_;
  return token;
}

const Token& Parser::expect(TokenKind kind, std::string_view context) {
  if (check(kind)) return tokens_[position_++];
  error_at(peek(), diag::kExpectedToken,
           "expected '" + std::string(token_kind_name(kind)) + "' " + std::string(context) +
               ", found " + describe_token(peek()));
  return peek();
}

void Parser::error_at(const Token& token, std::string_view code, std::string message) {
  diagnostics_.error(std::string(code), token.location, std::move(message));
}

void Parser::report_phase_gap(const Token& token, std::string_view feature,
                             std::string_view phase) {
  diagnostics_.error(diag::kNotImplementedInPhase1, token.location,
                     "'" + std::string(feature) + "' is specified for " + std::string(phase) +
                         " and is not available in Phase 3");
}

void Parser::synchronize() {
  while (!check(TokenKind::EndOfFile)) {
    if (previous().kind == TokenKind::Semicolon) return;
    switch (peek().kind) {
      case TokenKind::Semicolon:
        advance();
        return;
      case TokenKind::RBrace:
        return;
      case TokenKind::KwFunction:
      case TokenKind::KwLet:
      case TokenKind::KwVar:
      case TokenKind::KwConst:
      case TokenKind::KwIf:
      case TokenKind::KwWhile:
      case TokenKind::KwFor:
      case TokenKind::KwBreak:
      case TokenKind::KwContinue:
      case TokenKind::KwReturn:
        return;
      default:
        advance();
        break;
    }
  }
}

std::unique_ptr<Program> Parser::parse_program() {
  auto program = std::make_unique<Program>();
  while (!check(TokenKind::EndOfFile)) {
    if (check(TokenKind::KwFunction)) {
      error_at(peek(), kUnknownType,
               "function declaration requires an explicit return type before 'function'");
      advance();
      synchronize();
      continue;
    }
    if (is_type_keyword(peek().kind)) {
      const std::size_t before = position_;
      std::unique_ptr<FunctionDecl> function = parse_function();
      if (function) {
        program->functions().push_back(std::move(function));
      } else if (position_ == before) {
        advance();
      }
      continue;
    }
    if (check(TokenKind::KwModule) || check(TokenKind::KwImport) || check(TokenKind::KwStruct)) {
      report_phase_gap(peek(), token_kind_name(peek().kind), phase_for_keyword(peek().kind));
      advance();
      synchronize();
      continue;
    }
    error_at(peek(), diag::kUnexpectedToken,
             "expected a function declaration, found " + describe_token(peek()));
    advance();
    synchronize();
  }

  if (program->functions().empty() && !diagnostics_.has_errors()) {
    error_at(peek(), diag::kMissingMain, "program contains no function declarations");
  }
  return program;
}

std::unique_ptr<FunctionDecl> Parser::parse_function() {
  const Token& type_token = peek();
  const Type return_type = parse_type_annotation();
  expect(TokenKind::KwFunction, "after the return type");
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken,
             "expected a function name after 'function', found " + describe_token(peek()));
    synchronize();
    return nullptr;
  }
  const Token name_token = advance();

  std::vector<Parameter> parameters;
  if (expect(TokenKind::LParen, "after the function name").kind == TokenKind::LParen) {
    if (!check(TokenKind::RParen)) {
      do {
        if (check(TokenKind::RParen)) break;
        if (!is_type_keyword(peek().kind)) {
          error_at(peek(), kUnknownType,
                   "expected a parameter type, found " + describe_token(peek()));
          synchronize();
          return nullptr;
        }
        const Token& parameter_type_token = peek();
        const Type parameter_type = parse_type_annotation();
        if (!check(TokenKind::Identifier)) {
          error_at(peek(), diag::kExpectedToken,
                   "expected a parameter name, found " + describe_token(peek()));
          synchronize();
          return nullptr;
        }
        const Token parameter_name = advance();
        parameters.push_back(
            Parameter{parameter_name.text, parameter_type, parameter_type_token.location});
      } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "after the parameter list");
  }

  if (!check(TokenKind::LBrace)) {
    error_at(peek(), diag::kExpectedToken,
             "expected '{' to start the function body, found " + describe_token(peek()));
    synchronize();
    return nullptr;
  }
  auto body = parse_block();
  if (!body) return nullptr;
  return std::make_unique<FunctionDecl>(return_type, name_token.text, std::move(parameters),
                                        std::move(body),
                                        type_token.location.is_valid() ? type_token.location
                                                                       : name_token.location);
}

Type Parser::parse_type_annotation() {
  const Token& token = peek();
  if (!is_type_keyword(token.kind)) {
    error_at(token, kUnknownType, "expected a type, found " + describe_token(token));
    return Type::error();
  }
  advance();
  if (check(TokenKind::LBracket)) {
    report_phase_gap(peek(), "array type", "Phase 4");
    int depth = 0;
    while (!check(TokenKind::EndOfFile)) {
      if (check(TokenKind::LBracket)) ++depth;
      if (check(TokenKind::RBracket)) {
        --depth;
        if (depth == 0) {
          advance();
          break;
        }
      }
      advance();
    }
  }
  return parse_type_name(token.text);
}

std::unique_ptr<BlockStmt> Parser::parse_block() {
  const Token open = advance();
  auto block = std::make_unique<BlockStmt>(open.location);
  while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
    const std::size_t before = position_;
    StmtPtr statement = parse_statement();
    if (statement) {
      block->statements().push_back(std::move(statement));
    } else if (position_ == before) {
      advance();
    }
  }
  expect(TokenKind::RBrace, "to close the block");
  return block;
}

StmtPtr Parser::parse_statement() {
  const Token& token = peek();

  if (check(TokenKind::LBrace)) return parse_block();
  if (check(TokenKind::KwLet) || check(TokenKind::KwVar) || check(TokenKind::KwConst)) {
    return parse_variable_declaration();
  }
  if (check(TokenKind::KwIf)) {
    return parse_if();
  }
  if (check(TokenKind::KwWhile)) {
    return parse_while();
  }
  if (check(TokenKind::KwFor)) {
    return parse_for();
  }
  if (check(TokenKind::KwBreak)) {
    return parse_break();
  }
  if (check(TokenKind::KwContinue)) {
    return parse_continue();
  }
  if (check(TokenKind::KwReturn)) {
    const Token keyword = advance();
    ExprPtr value;
    if (!check(TokenKind::Semicolon)) value = parse_expression();
    expect(TokenKind::Semicolon, "after the return statement");
    return std::make_unique<ReturnStmt>(keyword.location, std::move(value));
  }
  if (is_reserved_future_keyword(token.kind)) {
    report_phase_gap(token, token_kind_name(token.kind), phase_for_keyword(token.kind));
    advance();
    synchronize();
    return nullptr;
  }
  if (check(TokenKind::Semicolon)) {
    advance();
    return nullptr;
  }
  return parse_assignment_or_expression();
}

StmtPtr Parser::parse_variable_declaration() {
  const Token keyword = advance();
  const BindingKind binding = keyword.kind == TokenKind::KwLet     ? BindingKind::Let
                              : keyword.kind == TokenKind::KwVar   ? BindingKind::Var
                                                                   : BindingKind::Const;
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken,
             "expected a variable name after '" + keyword.text + "', found " +
                 describe_token(peek()));
    synchronize();
    return nullptr;
  }
  const Token name = advance();

  bool has_annotation = false;
  Type annotation;
  if (match(TokenKind::Colon)) {
    has_annotation = true;
    annotation = parse_type_annotation();
  }

  if (!check(TokenKind::Equal)) {
    error_at(peek(), kUnknownType,
             "'" + name.text + "' must be initialized: variables require an initializer");
    synchronize();
    return nullptr;
  }
  advance();  // '='
  ExprPtr initializer = parse_expression();
  expect(TokenKind::Semicolon, "after the variable initializer");
  return std::make_unique<VariableDeclarationStmt>(keyword.location, binding, name.text,
                                                   has_annotation, annotation,
                                                   std::move(initializer));
}

StmtPtr Parser::parse_assignment_or_expression() {
  if (check(TokenKind::Identifier)) {
    if (peek(1).kind == TokenKind::Equal) {
      const Token name = advance();
      advance();  // '='
      ExprPtr value = parse_expression();
      expect(TokenKind::Semicolon, "after the assignment");
      return std::make_unique<AssignmentStmt>(name.location, name.text, std::move(value),
                                              name.location);
    }
    BinaryOp comp_op;
    bool is_compound = false;
    switch (peek(1).kind) {
      case TokenKind::PlusEqual: comp_op = BinaryOp::Add; is_compound = true; break;
      case TokenKind::MinusEqual: comp_op = BinaryOp::Subtract; is_compound = true; break;
      case TokenKind::StarEqual: comp_op = BinaryOp::Multiply; is_compound = true; break;
      case TokenKind::SlashEqual: comp_op = BinaryOp::Divide; is_compound = true; break;
      case TokenKind::PercentEqual: comp_op = BinaryOp::Remainder; is_compound = true; break;
      case TokenKind::AmpEqual: comp_op = BinaryOp::BitwiseAnd; is_compound = true; break;
      case TokenKind::PipeEqual: comp_op = BinaryOp::BitwiseOr; is_compound = true; break;
      case TokenKind::CaretEqual: comp_op = BinaryOp::BitwiseXor; is_compound = true; break;
      case TokenKind::LessLessEqual: comp_op = BinaryOp::ShiftLeft; is_compound = true; break;
      case TokenKind::GreaterGreaterEqual: comp_op = BinaryOp::ShiftRight; is_compound = true; break;
      default: break;
    }
    if (is_compound) {
      const Token name = advance();
      advance();  // compound op
      ExprPtr value = parse_expression();
      expect(TokenKind::Semicolon, "after the compound assignment");
      return std::make_unique<CompoundAssignmentStmt>(name.location, comp_op, name.text,
                                                      std::move(value), name.location);
    }
  }
  const Token start = peek();
  ExprPtr expr = parse_expression();
  expect(TokenKind::Semicolon, "after the expression");
  return std::make_unique<ExprStmt>(start.location, std::move(expr));
}

StmtPtr Parser::parse_if() {
  const Token if_tok = advance();
  expect(TokenKind::LParen, "after 'if'");
  ExprPtr condition = parse_expression();
  expect(TokenKind::RParen, "after the 'if' condition");
  if (!check(TokenKind::LBrace)) {
    error_at(peek(), diag::kExpectedToken, "expected '{' to start the 'if' body");
    synchronize();
    return nullptr;
  }
  auto then_branch = parse_block();
  StmtPtr else_branch = nullptr;
  if (match(TokenKind::KwElse)) {
    if (check(TokenKind::KwIf)) {
      else_branch = parse_if();
    } else if (check(TokenKind::LBrace)) {
      else_branch = parse_block();
    } else {
      error_at(peek(), diag::kExpectedToken, "expected '{' or 'if' after 'else'");
      synchronize();
    }
  }
  return std::make_unique<IfStmt>(if_tok.location, std::move(condition),
                                  std::move(then_branch), std::move(else_branch));
}

StmtPtr Parser::parse_while() {
  const Token while_tok = advance();
  expect(TokenKind::LParen, "after 'while'");
  ExprPtr condition = parse_expression();
  expect(TokenKind::RParen, "after the 'while' condition");
  if (!check(TokenKind::LBrace)) {
    error_at(peek(), diag::kExpectedToken, "expected '{' to start the 'while' body");
    synchronize();
    return nullptr;
  }
  auto body = parse_block();
  return std::make_unique<WhileStmt>(while_tok.location, std::move(condition), std::move(body));
}

StmtPtr Parser::parse_for() {
  const Token for_tok = advance();
  expect(TokenKind::LParen, "after 'for'");

  StmtPtr initializer = nullptr;
  if (check(TokenKind::KwLet) || check(TokenKind::KwVar) || check(TokenKind::KwConst)) {
    initializer = parse_variable_declaration();
  } else if (match(TokenKind::Semicolon)) {
    initializer = nullptr;
  } else {
    initializer = parse_assignment_or_expression();
  }

  ExprPtr condition = nullptr;
  if (!check(TokenKind::Semicolon)) {
    condition = parse_expression();
  }
  expect(TokenKind::Semicolon, "after the 'for' condition");

  StmtPtr step = nullptr;
  if (!check(TokenKind::RParen)) {
    if (check(TokenKind::Identifier)) {
      if (peek(1).kind == TokenKind::Equal) {
        const Token name = advance();
        advance();  // '='
        ExprPtr value = parse_expression();
        step = std::make_unique<AssignmentStmt>(name.location, name.text, std::move(value),
                                                name.location);
      } else {
        BinaryOp comp_op;
        bool is_compound = false;
        switch (peek(1).kind) {
          case TokenKind::PlusEqual: comp_op = BinaryOp::Add; is_compound = true; break;
          case TokenKind::MinusEqual: comp_op = BinaryOp::Subtract; is_compound = true; break;
          case TokenKind::StarEqual: comp_op = BinaryOp::Multiply; is_compound = true; break;
          case TokenKind::SlashEqual: comp_op = BinaryOp::Divide; is_compound = true; break;
          case TokenKind::PercentEqual: comp_op = BinaryOp::Remainder; is_compound = true; break;
          case TokenKind::AmpEqual: comp_op = BinaryOp::BitwiseAnd; is_compound = true; break;
          case TokenKind::PipeEqual: comp_op = BinaryOp::BitwiseOr; is_compound = true; break;
          case TokenKind::CaretEqual: comp_op = BinaryOp::BitwiseXor; is_compound = true; break;
          case TokenKind::LessLessEqual: comp_op = BinaryOp::ShiftLeft; is_compound = true; break;
          case TokenKind::GreaterGreaterEqual: comp_op = BinaryOp::ShiftRight; is_compound = true; break;
          default: break;
        }
        if (is_compound) {
          const Token name = advance();
          advance();  // compound op
          ExprPtr value = parse_expression();
          step = std::make_unique<CompoundAssignmentStmt>(name.location, comp_op, name.text,
                                                          std::move(value), name.location);
        } else {
          const Token start = peek();
          ExprPtr expr = parse_expression();
          step = std::make_unique<ExprStmt>(start.location, std::move(expr));
        }
      }
    } else {
      const Token start = peek();
      ExprPtr expr = parse_expression();
      step = std::make_unique<ExprStmt>(start.location, std::move(expr));
    }
  }
  expect(TokenKind::RParen, "after the 'for' loop clauses");

  if (!check(TokenKind::LBrace)) {
    error_at(peek(), diag::kExpectedToken, "expected '{' to start the 'for' body");
    synchronize();
    return nullptr;
  }
  auto body = parse_block();
  return std::make_unique<ForStmt>(for_tok.location, std::move(initializer),
                                  std::move(condition), std::move(step), std::move(body));
}

StmtPtr Parser::parse_break() {
  const Token tok = advance();
  expect(TokenKind::Semicolon, "after 'break'");
  return std::make_unique<BreakStmt>(tok.location);
}

StmtPtr Parser::parse_continue() {
  const Token tok = advance();
  expect(TokenKind::Semicolon, "after 'continue'");
  return std::make_unique<ContinueStmt>(tok.location);
}

std::vector<ExprPtr> Parser::parse_argument_list() {
  std::vector<ExprPtr> arguments;
  if (check(TokenKind::RParen)) return arguments;
  do {
    if (check(TokenKind::RParen)) break;
    arguments.push_back(parse_expression());
  } while (match(TokenKind::Comma));
  return arguments;
}

ExprPtr Parser::parse_expression() { return parse_logical_or(); }

ExprPtr Parser::parse_logical_or() {
  ExprPtr lhs = parse_logical_and();
  while (check(TokenKind::PipePipe)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_logical_and();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::LogicalOr, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_logical_and() {
  ExprPtr lhs = parse_bitwise_or();
  while (check(TokenKind::AmpAmp)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_or();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::LogicalAnd, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_or() {
  ExprPtr lhs = parse_bitwise_xor();
  while (check(TokenKind::Pipe)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_xor();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseOr, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_xor() {
  ExprPtr lhs = parse_bitwise_and();
  while (check(TokenKind::Caret)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_and();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseXor, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_and() {
  ExprPtr lhs = parse_equality();
  while (check(TokenKind::Amp)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_equality();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseAnd, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_equality() {
  ExprPtr lhs = parse_relational();
  while (check(TokenKind::EqualEqual) || check(TokenKind::BangEqual)) {
    const Token op_tok = advance();
    const BinaryOp op = op_tok.kind == TokenKind::EqualEqual ? BinaryOp::Equal : BinaryOp::NotEqual;
    ExprPtr rhs = parse_relational();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_relational() {
  ExprPtr lhs = parse_shift();
  while (check(TokenKind::Less) || check(TokenKind::LessEqual) ||
         check(TokenKind::Greater) || check(TokenKind::GreaterEqual)) {
    const Token op_tok = advance();
    BinaryOp op = BinaryOp::Less;
    switch (op_tok.kind) {
      case TokenKind::Less: op = BinaryOp::Less; break;
      case TokenKind::LessEqual: op = BinaryOp::LessEqual; break;
      case TokenKind::Greater: op = BinaryOp::Greater; break;
      case TokenKind::GreaterEqual: op = BinaryOp::GreaterEqual; break;
      default: break;
    }
    ExprPtr rhs = parse_shift();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_shift() {
  ExprPtr lhs = parse_additive();
  while (check(TokenKind::LessLess) || check(TokenKind::GreaterGreater)) {
    const Token op_tok = advance();
    const BinaryOp op =
        op_tok.kind == TokenKind::LessLess ? BinaryOp::ShiftLeft : BinaryOp::ShiftRight;
    ExprPtr rhs = parse_additive();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_additive() {
  ExprPtr lhs = parse_multiplicative();
  while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
    const Token op_tok = advance();
    const BinaryOp op = op_tok.kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Subtract;
    ExprPtr rhs = parse_multiplicative();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_multiplicative() {
  ExprPtr lhs = parse_cast();
  while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
    const Token op_tok = advance();
    const BinaryOp op = op_tok.kind == TokenKind::Star    ? BinaryOp::Multiply
                        : op_tok.kind == TokenKind::Slash ? BinaryOp::Divide
                                                          : BinaryOp::Remainder;
    ExprPtr rhs = parse_cast();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_cast() {
  ExprPtr lhs = parse_unary();
  while (check(TokenKind::KwAs)) {
    const Token as_tok = advance();
    const Type target = parse_type_annotation();
    lhs = std::make_unique<CastExpr>(as_tok.location, std::move(lhs), target);
  }
  return lhs;
}

ExprPtr Parser::parse_unary() {
  if (check(TokenKind::Minus)) {
    const Token op_tok = advance();
    return std::make_unique<UnaryExpr>(op_tok.location, UnaryOp::Negate, parse_unary());
  }
  if (check(TokenKind::Plus)) {
    const Token op_tok = advance();
    return std::make_unique<UnaryExpr>(op_tok.location, UnaryOp::Plus, parse_unary());
  }
  if (check(TokenKind::Bang)) {
    const Token op_tok = advance();
    return std::make_unique<UnaryExpr>(op_tok.location, UnaryOp::LogicalNot, parse_unary());
  }
  if (check(TokenKind::Tilde)) {
    const Token op_tok = advance();
    return std::make_unique<UnaryExpr>(op_tok.location, UnaryOp::BitwiseNot, parse_unary());
  }
  return parse_exponentiation();
}

ExprPtr Parser::parse_exponentiation() {
  ExprPtr lhs = parse_primary();
  if (check(TokenKind::StarStar)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_unary();
    lhs = std::make_unique<BinaryExpr>(op_tok.location, BinaryOp::Exponentiate, std::move(lhs),
                                       std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_primary() {
  const Token token = peek();
  switch (token.kind) {
    case TokenKind::IntegerLiteral:
      advance();
      return std::make_unique<IntegerLiteralExpr>(token.location, token.integer_value,
                                                 token.suffix);
    case TokenKind::FloatLiteral:
      advance();
      return std::make_unique<FloatLiteralExpr>(token.location, token.float_value, token.suffix);
    case TokenKind::CharLiteral:
      advance();
      return std::make_unique<CharLiteralExpr>(token.location, token.char_value);
    case TokenKind::StringLiteral:
      advance();
      return std::make_unique<StringLiteralExpr>(token.location, token.string_value);
    case TokenKind::KwTrue:
      advance();
      return std::make_unique<BoolLiteralExpr>(token.location, true);
    case TokenKind::KwFalse:
      advance();
      return std::make_unique<BoolLiteralExpr>(token.location, false);
    case TokenKind::KwPrint: {
      advance();
      expect(TokenKind::LParen, "after 'print'");
      std::vector<ExprPtr> arguments = parse_argument_list();
      expect(TokenKind::RParen, "to close the 'print' call");
      return std::make_unique<CallExpr>(token.location, "print", std::move(arguments));
    }
    case TokenKind::Identifier: {
      advance();
      if (match(TokenKind::LParen)) {
        std::vector<ExprPtr> arguments = parse_argument_list();
        expect(TokenKind::RParen, "to close the call");
        return std::make_unique<CallExpr>(token.location, token.text, std::move(arguments));
      }
      return std::make_unique<NameExpr>(token.location, token.text);
    }
    case TokenKind::LParen: {
      advance();
      ExprPtr inner = parse_expression();
      expect(TokenKind::RParen, "to close the parenthesized expression");
      return inner;
    }
    case TokenKind::EndOfFile:
      error_at(token, diag::kUnexpectedToken, "unexpected end of file in an expression");
      break;
    default:
      if (is_reserved_future_keyword(token.kind)) {
        report_phase_gap(token, token_kind_name(token.kind), phase_for_keyword(token.kind));
      } else {
        error_at(token, diag::kUnexpectedToken,
                 "expected an expression, found " + describe_token(token));
      }
      advance();
      break;
  }
  auto placeholder = std::make_unique<IntegerLiteralExpr>(token.location, 0);
  placeholder->set_type(Type::error());
  return placeholder;
}

std::string_view unary_op_spelling(UnaryOp op) {
  switch (op) {
    case UnaryOp::Negate: return "-";
    case UnaryOp::Plus: return "+";
    case UnaryOp::LogicalNot: return "!";
    case UnaryOp::BitwiseNot: return "~";
  }
  return "?";
}

std::string_view binary_op_spelling(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Subtract: return "-";
    case BinaryOp::Multiply: return "*";
    case BinaryOp::Divide: return "/";
    case BinaryOp::Remainder: return "%";
    case BinaryOp::Exponentiate: return "**";
    case BinaryOp::BitwiseAnd: return "&";
    case BinaryOp::BitwiseOr: return "|";
    case BinaryOp::BitwiseXor: return "^";
    case BinaryOp::ShiftLeft: return "<<";
    case BinaryOp::ShiftRight: return ">>";
    case BinaryOp::Equal: return "==";
    case BinaryOp::NotEqual: return "!=";
    case BinaryOp::Less: return "<";
    case BinaryOp::LessEqual: return "<=";
    case BinaryOp::Greater: return ">";
    case BinaryOp::GreaterEqual: return ">=";
    case BinaryOp::LogicalAnd: return "&&";
    case BinaryOp::LogicalOr: return "||";
  }
  return "?";
}

std::string_view binding_kind_spelling(BindingKind kind) {
  switch (kind) {
    case BindingKind::Let: return "let";
    case BindingKind::Var: return "var";
    case BindingKind::Const: return "const";
  }
  return "let";
}

}  // namespace nirq
