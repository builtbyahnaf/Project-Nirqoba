#include "nirqoba/Frontend/Parser.h"

#include <utility>

namespace nirq {
using namespace std;
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

string_view phase_for_keyword(TokenKind kind) {
  switch (kind) {
    case TokenKind::KwFn:
      return "Phase 3 (use 'function', not 'fn')";
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

Parser::Parser(vector<Token> tokens, DiagnosticEngine& diagnostics)
    : tokens_(move(tokens)), diagnostics_(diagnostics) {}

const Token& Parser::peek(size_t lookahead) const {
  const size_t index = position_ + lookahead;
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

const Token& Parser::expect(TokenKind kind, string_view context) {
  if (check(kind)) return tokens_[position_++];
  error_at(peek(), diag::kExpectedToken,
           "expected '" + string(token_kind_name(kind)) + "' " + string(context) +
               ", found " + describe_token(peek()));
  return peek();
}

void Parser::error_at(const Token& token, string_view code, string message) {
  diagnostics_.error(string(code), token.location, move(message));
}

void Parser::report_phase_gap(const Token& token, string_view feature,
                             string_view phase) {
  diagnostics_.error(diag::kNotImplementedInPhase1, token.location,
                     "'" + string(feature) + "' is specified for " + string(phase) +
                         " and is not available in Phase 4");
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
      case TokenKind::KwStruct:
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

unique_ptr<Program> Parser::parse_program() {
  auto program = make_unique<Program>();
  // `module` must open the file and every `import` must precede the first
  // declaration; both rules keep a module's dependency list readable at a
  // glance instead of scattering it through the file.
  bool seen_declaration = false;
  bool seen_import = false;
  while (!check(TokenKind::EndOfFile)) {
    if (check(TokenKind::KwModule)) {
      auto mod = parse_module();
      if (mod) {
        if (program->module_decl()) {
          diagnostics_.error(diag::kDuplicateModule, mod->location(),
                             "only one 'module' declaration is allowed per source file");
        } else if (seen_declaration || seen_import) {
          diagnostics_.error(diag::kUnexpectedToken, mod->location(),
                             "the 'module' declaration must be the first item in a source file");
        } else {
          program->set_module(move(mod));
        }
      }
      continue;
    }
    if (check(TokenKind::KwImport)) {
      auto imp = parse_import();
      if (imp) {
        if (seen_declaration) {
          diagnostics_.error(diag::kUnexpectedToken, imp->location(),
                             "import statements must appear before any declaration");
        }
        program->imports().push_back(move(imp));
      }
      seen_import = true;
      continue;
    }

    Visibility vis = Visibility::Public;
    if (check(TokenKind::KwPublic) || check(TokenKind::KwPrivate)) {
      vis = match(TokenKind::KwPrivate) ? Visibility::Private : Visibility::Public;
      if (vis == Visibility::Public) advance();
    }

    if (check(TokenKind::KwStruct)) {
      seen_declaration = true;
      unique_ptr<StructDecl> s = parse_struct(vis);
      if (s) program->structs().push_back(move(s));
      continue;
    }
    if (check(TokenKind::KwFunction)) {
      seen_declaration = true;
      error_at(peek(), kUnknownType,
               "function declaration requires an explicit return type before 'function'");
      advance();
      synchronize();
      continue;
    }
    if ((check(TokenKind::Identifier) && peek().text == "shared" &&
         (is_type_keyword(peek(1).kind) || peek(1).kind == TokenKind::Identifier)) ||
        (check(TokenKind::Identifier) && peek().text == "owned_array" &&
         peek(1).kind == TokenKind::Less) ||
        (check(TokenKind::Identifier) && peek().text == "array_view" &&
         peek(1).kind == TokenKind::Less) ||
        is_type_keyword(peek().kind) ||
        (check(TokenKind::Identifier) &&
         (peek(1).kind == TokenKind::KwFunction ||
          (peek().text == "managed" && peek(1).kind == TokenKind::Less)))) {
      seen_declaration = true;
      const size_t before = position_;
      unique_ptr<FunctionDecl> function = parse_function(vis);
      if (function) {
        program->functions().push_back(move(function));
      } else if (position_ == before) {
        advance();
      }
      continue;
    }
    error_at(peek(), diag::kUnexpectedToken,
             "expected a declaration, found " + describe_token(peek()));
    advance();
    synchronize();
  }

  return program;
}

unique_ptr<ModuleDecl> Parser::parse_module() {
  const Token kw = advance();  // 'module'
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken, "expected module name after 'module'");
    synchronize();
    return nullptr;
  }
  const Token name_tok = advance();
  expect(TokenKind::Semicolon, "after module declaration");
  return make_unique<ModuleDecl>(name_tok.text, kw.location);
}

unique_ptr<ImportDecl> Parser::parse_import() {
  const Token kw = advance();  // 'import'
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken, "expected module name after 'import'");
    synchronize();
    return nullptr;
  }
  const Token mod_tok = advance();
  string item_name;
  string alias;

  if (match(TokenKind::Dot)) {
    if (!check(TokenKind::Identifier)) {
      error_at(peek(), diag::kExpectedToken, "expected member name after '.' in import");
      synchronize();
      return nullptr;
    }
    item_name = advance().text;
  }

  if (match(TokenKind::KwAs)) {
    if (!check(TokenKind::Identifier)) {
      error_at(peek(), diag::kExpectedToken, "expected alias name after 'as' in import");
      synchronize();
      return nullptr;
    }
    alias = advance().text;
  }

  expect(TokenKind::Semicolon, "after import statement");
  return make_unique<ImportDecl>(mod_tok.text, move(item_name), move(alias), kw.location);
}

unique_ptr<StructDecl> Parser::parse_struct(Visibility visibility) {
  const Token struct_tok = advance();  // 'struct'
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken, "expected struct name after 'struct'");
    synchronize();
    return nullptr;
  }
  const Token name_tok = advance();
  expect(TokenKind::LBrace, "to open struct body");

  vector<Parameter> fields;
  while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
    Type field_type = parse_type_annotation();
    if (!check(TokenKind::Identifier)) {
      error_at(peek(), diag::kExpectedToken, "expected field name in struct");
      synchronize();
      break;
    }
    Token field_name = advance();
    expect(TokenKind::Semicolon, "after struct field declaration");
    fields.push_back(Parameter{field_name.text, field_type, field_name.location});
  }
  expect(TokenKind::RBrace, "to close struct body");
  return make_unique<StructDecl>(name_tok.text, move(fields), struct_tok.location, visibility);
}

unique_ptr<FunctionDecl> Parser::parse_function(Visibility visibility) {
  const Token& type_token = peek();
  bool returns_shared = false;
  if (check(TokenKind::Identifier) && peek().text == "shared") {
    advance();
    returns_shared = true;
  }
  const Type return_type = parse_type_annotation();
  expect(TokenKind::KwFunction, "after the return type");
  if (!check(TokenKind::Identifier)) {
    error_at(peek(), diag::kExpectedToken,
             "expected a function name after 'function', found " + describe_token(peek()));
    synchronize();
    return nullptr;
  }
  const Token name_token = advance();

  vector<Parameter> parameters;
  if (expect(TokenKind::LParen, "after the function name").kind == TokenKind::LParen) {
    if (!check(TokenKind::RParen)) {
      do {
        if (check(TokenKind::RParen)) break;
        const Token& parameter_type_token = peek();
        bool borrowed = false;
        bool mutable_borrow = false;
        bool shared = false;
        if (check(TokenKind::Identifier) && peek().text == "borrow") {
          advance();
          borrowed = true;
          if (check(TokenKind::Identifier) && peek().text == "mut") {
            advance();
            mutable_borrow = true;
          }
        } else if (check(TokenKind::Identifier) && peek().text == "shared") {
          advance();
          shared = true;
        }
        const Type parameter_type = parse_type_annotation();
        if (!check(TokenKind::Identifier)) {
          error_at(peek(), diag::kExpectedToken,
                   "expected a parameter name, found " + describe_token(peek()));
          synchronize();
          return nullptr;
        }
        const Token parameter_name = advance();
        parameters.push_back(
            Parameter{parameter_name.text, parameter_type, parameter_type_token.location,
                      borrowed, mutable_borrow, shared});
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
  return make_unique<FunctionDecl>(return_type, name_token.text, move(parameters),
                                   move(body),
                                   type_token.location.is_valid() ? type_token.location
                                                                  : name_token.location,
                                   visibility, returns_shared);
}

Type Parser::parse_type_annotation() {
  Type base_type = Type::error();
  if (check(TokenKind::Identifier) && peek().text == "managed" &&
      peek(1).kind == TokenKind::Less) {
    advance();
    advance();
    Type managed_type = parse_type_annotation();
    expect(TokenKind::Greater, "after the managed type");
    base_type = Type::managed_pointer_type(managed_type);
  } else if (check(TokenKind::Identifier) && peek().text == "owned_array" &&
             peek(1).kind == TokenKind::Less) {
    advance();
    advance();
    Type element_type = parse_type_annotation();
    expect(TokenKind::Greater, "after the owned array element type");
    base_type = Type::owned_array_type(element_type);
  } else if (check(TokenKind::Identifier) && peek().text == "array_view" &&
             peek(1).kind == TokenKind::Less) {
    advance();
    advance();
    Type element_type = parse_type_annotation();
    expect(TokenKind::Greater, "after the array view element type");
    base_type = Type::array_view_type(element_type);
  } else if (is_type_keyword(peek().kind)) {
    const Token token = advance();
    base_type = parse_type_name(token.text);
  } else if (check(TokenKind::Identifier)) {
    const Token token = advance();
    // A struct declared in another module is named `module.Type` (Phase 5).
    // The qualified spelling is kept verbatim in the type so semantic analysis
    // and the TIR keep one name for the type across modules.
    string name = token.text;
    while (check(TokenKind::Dot)) {
      advance();
      if (!check(TokenKind::Identifier)) {
        error_at(peek(), kUnknownType,
                 "expected a type name after '.' in qualified type");
        break;
      }
      name += "." + advance().text;
    }
    base_type = Type::struct_type(name);
  } else if (match(TokenKind::LParen)) {
    vector<Type> tuple_elements;
    if (!check(TokenKind::RParen)) {
      do {
        tuple_elements.push_back(parse_type_annotation());
      } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "to close tuple type");
    base_type = Type::tuple_type(move(tuple_elements));
  } else {
    error_at(peek(), kUnknownType, "expected a type, found " + describe_token(peek()));
    advance();
    return Type::error();
  }

  // Parse any array suffixes: `[N]` or `[]`. Suffixes nest left to right, so
  // `int[2][3]` is two rows of three: the first suffix is the outermost
  // dimension. Building innermost-last keeps that reading and matches how an
  // array literal infers its type.
  struct ArraySuffix {
    bool dynamic;
    size_t length;
  };
  vector<ArraySuffix> suffixes;
  while (match(TokenKind::LBracket)) {
    if (check(TokenKind::IntegerLiteral)) {
      const Token len_tok = advance();
      expect(TokenKind::RBracket, "to close array type");
      suffixes.push_back(ArraySuffix{false, static_cast<size_t>(len_tok.integer_value)});
    } else {
      expect(TokenKind::RBracket, "to close dynamic array type");
      suffixes.push_back(ArraySuffix{true, 0});
    }
  }
  for (auto suffix = suffixes.rbegin(); suffix != suffixes.rend(); ++suffix) {
    base_type = suffix->dynamic ? Type::dynamic_array_type(base_type)
                                : Type::array_type(base_type, suffix->length);
  }
  while (match(TokenKind::Star)) {
    base_type = Type::pointer_type(base_type);
  }

  return base_type;
}

unique_ptr<BlockStmt> Parser::parse_block() {
  const Token open = advance();
  auto block = make_unique<BlockStmt>(open.location);
  while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
    const size_t before = position_;
    StmtPtr statement = parse_statement();
    if (statement) {
      block->statements().push_back(move(statement));
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
  if ((check(TokenKind::KwLet) || check(TokenKind::KwVar)) &&
      (peek(1).kind == TokenKind::LParen || peek(1).kind == TokenKind::LBracket)) {
    return parse_destructuring();
  }
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
  if (check(TokenKind::KwDefer)) {
    const Token keyword = advance();
    ExprPtr expression = parse_expression();
    expect(TokenKind::Semicolon, "after the deferred cleanup call");
    return make_unique<DeferredCleanupStmt>(keyword.location,
                                            move(expression));
  }
  if (check(TokenKind::KwReturn)) {
    const Token keyword = advance();
    ExprPtr value;
    if (!check(TokenKind::Semicolon)) value = parse_expression();
    expect(TokenKind::Semicolon, "after the return statement");
    return make_unique<ReturnStmt>(keyword.location, move(value));
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

StmtPtr Parser::parse_destructuring() {
  const Token kw = advance();
  const BindingKind binding = kw.kind == TokenKind::KwLet ? BindingKind::Let : BindingKind::Var;
  const bool is_paren = match(TokenKind::LParen);
  if (!is_paren) expect(TokenKind::LBracket, "for destructuring");

  vector<string> names;
  do {
    if (is_paren && check(TokenKind::RParen)) break;
    if (!is_paren && check(TokenKind::RBracket)) break;
    if (!check(TokenKind::Identifier)) {
      error_at(peek(), diag::kExpectedToken, "expected variable name in destructuring pattern");
      synchronize();
      return nullptr;
    }
    names.push_back(advance().text);
  } while (match(TokenKind::Comma));

  if (is_paren) expect(TokenKind::RParen, "to close destructuring pattern");
  else expect(TokenKind::RBracket, "to close destructuring pattern");

  expect(TokenKind::Equal, "before destructuring initializer");
  ExprPtr value = parse_expression();
  expect(TokenKind::Semicolon, "after destructuring statement");
  return make_unique<DestructuringStmt>(kw.location, binding, move(names), move(value));
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
  return make_unique<VariableDeclarationStmt>(keyword.location, binding, name.text,
                                              has_annotation, annotation,
                                              move(initializer));
}

StmtPtr Parser::parse_assignment_or_expression() {
  const Token start = peek();
  ExprPtr expr = parse_expression();
  if (!expr) return nullptr;

  if (match(TokenKind::Equal)) {
    ExprPtr value = parse_expression();
    expect(TokenKind::Semicolon, "after the assignment");
    return make_unique<AssignmentStmt>(start.location, move(expr), move(value));
  }

  BinaryOp comp_op;
  bool is_compound = false;
  switch (peek().kind) {
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
    advance();  // compound op
    ExprPtr value = parse_expression();
    expect(TokenKind::Semicolon, "after the compound assignment");
    return make_unique<CompoundAssignmentStmt>(start.location, comp_op, move(expr), move(value));
  }

  expect(TokenKind::Semicolon, "after the expression");
  return make_unique<ExprStmt>(start.location, move(expr));
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
  return make_unique<IfStmt>(if_tok.location, move(condition),
                             move(then_branch), move(else_branch));
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
  return make_unique<WhileStmt>(while_tok.location, move(condition), move(body));
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
    const Token start = peek();
    ExprPtr expr = parse_expression();
    if (expr) {
      if (match(TokenKind::Equal)) {
        ExprPtr value = parse_expression();
        step = make_unique<AssignmentStmt>(start.location, move(expr), move(value));
      } else {
        BinaryOp comp_op;
        bool is_compound = false;
        switch (peek().kind) {
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
          advance();  // compound op
          ExprPtr value = parse_expression();
          step = make_unique<CompoundAssignmentStmt>(start.location, comp_op, move(expr), move(value));
        } else {
          step = make_unique<ExprStmt>(start.location, move(expr));
        }
      }
    }
  }
  expect(TokenKind::RParen, "after the 'for' loop clauses");

  if (!check(TokenKind::LBrace)) {
    error_at(peek(), diag::kExpectedToken, "expected '{' to start the 'for' body");
    synchronize();
    return nullptr;
  }
  auto body = parse_block();
  return make_unique<ForStmt>(for_tok.location, move(initializer),
                              move(condition), move(step), move(body));
}

StmtPtr Parser::parse_break() {
  const Token tok = advance();
  expect(TokenKind::Semicolon, "after 'break'");
  return make_unique<BreakStmt>(tok.location);
}

StmtPtr Parser::parse_continue() {
  const Token tok = advance();
  expect(TokenKind::Semicolon, "after 'continue'");
  return make_unique<ContinueStmt>(tok.location);
}

vector<ExprPtr> Parser::parse_argument_list() {
  vector<ExprPtr> arguments;
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
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::LogicalOr, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_logical_and() {
  ExprPtr lhs = parse_bitwise_or();
  while (check(TokenKind::AmpAmp)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_or();
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::LogicalAnd, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_or() {
  ExprPtr lhs = parse_bitwise_xor();
  while (check(TokenKind::Pipe)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_xor();
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseOr, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_xor() {
  ExprPtr lhs = parse_bitwise_and();
  while (check(TokenKind::Caret)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_bitwise_and();
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseXor, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_bitwise_and() {
  ExprPtr lhs = parse_equality();
  while (check(TokenKind::Amp)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_equality();
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::BitwiseAnd, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_equality() {
  ExprPtr lhs = parse_relational();
  while (check(TokenKind::EqualEqual) || check(TokenKind::BangEqual)) {
    const Token op_tok = advance();
    const BinaryOp op = op_tok.kind == TokenKind::EqualEqual ? BinaryOp::Equal : BinaryOp::NotEqual;
    ExprPtr rhs = parse_relational();
    lhs = make_unique<BinaryExpr>(op_tok.location, op, move(lhs), move(rhs));
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
    lhs = make_unique<BinaryExpr>(op_tok.location, op, move(lhs), move(rhs));
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
    lhs = make_unique<BinaryExpr>(op_tok.location, op, move(lhs), move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_additive() {
  ExprPtr lhs = parse_multiplicative();
  while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
    const Token op_tok = advance();
    const BinaryOp op = op_tok.kind == TokenKind::Plus ? BinaryOp::Add : BinaryOp::Subtract;
    ExprPtr rhs = parse_multiplicative();
    lhs = make_unique<BinaryExpr>(op_tok.location, op, move(lhs), move(rhs));
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
    lhs = make_unique<BinaryExpr>(op_tok.location, op, move(lhs), move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_cast() {
  ExprPtr lhs = parse_unary();
  while (check(TokenKind::KwAs)) {
    const Token as_tok = advance();
    const Type target = parse_type_annotation();
    lhs = make_unique<CastExpr>(as_tok.location, move(lhs), target);
  }
  return lhs;
}

ExprPtr Parser::parse_unary() {
  if (check(TokenKind::Minus)) {
    const Token op_tok = advance();
    return make_unique<UnaryExpr>(op_tok.location, UnaryOp::Negate, parse_unary());
  }
  if (check(TokenKind::Plus)) {
    const Token op_tok = advance();
    return make_unique<UnaryExpr>(op_tok.location, UnaryOp::Plus, parse_unary());
  }
  if (check(TokenKind::Bang)) {
    const Token op_tok = advance();
    return make_unique<UnaryExpr>(op_tok.location, UnaryOp::LogicalNot, parse_unary());
  }
  if (check(TokenKind::Tilde)) {
    const Token op_tok = advance();
    return make_unique<UnaryExpr>(op_tok.location, UnaryOp::BitwiseNot, parse_unary());
  }
  return parse_exponentiation();
}

ExprPtr Parser::parse_exponentiation() {
  ExprPtr lhs = parse_postfix();
  if (check(TokenKind::StarStar)) {
    const Token op_tok = advance();
    ExprPtr rhs = parse_unary();
    lhs = make_unique<BinaryExpr>(op_tok.location, BinaryOp::Exponentiate, move(lhs),
                                       move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_postfix() {
  ExprPtr expr = parse_primary();
  while (true) {
    if (match(TokenKind::LBracket)) {
      ExprPtr index = parse_expression();
      expect(TokenKind::RBracket, "to close array index");
      expr = make_unique<IndexExpr>(expr->location(), move(expr), move(index));
    } else if (match(TokenKind::Dot)) {
      const Token mem_tok = peek();
      if (check(TokenKind::IntegerLiteral)) {
        advance();
        expr = make_unique<MemberAccessExpr>(
            expr->location(), move(expr), "", static_cast<int>(mem_tok.integer_value));
      } else if (check(TokenKind::Identifier)) {
        advance();
        expr = make_unique<MemberAccessExpr>(
            expr->location(), move(expr), mem_tok.text, -1);
      } else {
        error_at(mem_tok, diag::kExpectedToken, "expected member name or tuple index after '.'");
        break;
      }
    } else {
      break;
    }
  }
  return expr;
}

ExprPtr Parser::parse_array_literal() {
  const Token open = advance();  // '['
  vector<ExprPtr> elements;
  if (!check(TokenKind::RBracket)) {
    do {
      if (check(TokenKind::RBracket)) break;
      elements.push_back(parse_expression());
    } while (match(TokenKind::Comma));
  }
  expect(TokenKind::RBracket, "to close array literal");
  return make_unique<ArrayLiteralExpr>(open.location, move(elements));
}

ExprPtr Parser::parse_struct_init(string struct_name, SourceLocation location) {
  expect(TokenKind::LBrace, "to open struct initializer");
  if (match(TokenKind::RBrace)) {
    return make_unique<StructInitExpr>(location, move(struct_name),
                                       vector<ExprPtr>{});
  }

  if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
    vector<pair<string, ExprPtr>> field_inits;
    do {
      if (check(TokenKind::RBrace)) break;
      if (!check(TokenKind::Identifier)) {
        error_at(peek(), diag::kExpectedToken, "expected field name in struct initializer");
        break;
      }
      Token field_tok = advance();
      expect(TokenKind::Colon, "after field name");
      ExprPtr init_expr = parse_expression();
      field_inits.push_back({field_tok.text, move(init_expr)});
    } while (match(TokenKind::Comma));
    expect(TokenKind::RBrace, "to close struct initializer");
    return make_unique<StructInitExpr>(location, move(struct_name),
                                       move(field_inits));
  } else {
    vector<ExprPtr> positional_inits;
    do {
      if (check(TokenKind::RBrace)) break;
      positional_inits.push_back(parse_expression());
    } while (match(TokenKind::Comma));
    expect(TokenKind::RBrace, "to close struct initializer");
    return make_unique<StructInitExpr>(location, move(struct_name),
                                       move(positional_inits));
  }
}

ExprPtr Parser::parse_primary() {
  const Token token = peek();
  switch (token.kind) {
    case TokenKind::IntegerLiteral:
      advance();
      return make_unique<IntegerLiteralExpr>(token.location, token.integer_value,
                                             token.suffix);
    case TokenKind::FloatLiteral:
      advance();
      return make_unique<FloatLiteralExpr>(token.location, token.float_value, token.suffix);
    case TokenKind::CharLiteral:
      advance();
      return make_unique<CharLiteralExpr>(token.location, token.char_value);
    case TokenKind::StringLiteral:
      advance();
      return make_unique<StringLiteralExpr>(token.location, token.string_value);
    case TokenKind::KwTrue:
      advance();
      return make_unique<BoolLiteralExpr>(token.location, true);
    case TokenKind::KwFalse:
      advance();
      return make_unique<BoolLiteralExpr>(token.location, false);
    case TokenKind::LBracket:
      return parse_array_literal();
    case TokenKind::KwPrint: {
      advance();
      expect(TokenKind::LParen, "after 'print'");
      vector<ExprPtr> arguments = parse_argument_list();
      expect(TokenKind::RParen, "to close the 'print' call");
      return make_unique<CallExpr>(token.location, "print", move(arguments));
    }
    case TokenKind::Identifier: {
      advance();
      if ((token.text == "memory" || token.text == "array") && check(TokenKind::Dot) &&
          peek(1).kind == TokenKind::Identifier &&
          (peek(1).text == "allocate" || peek(1).text == "reallocate" ||
           peek(1).text == "secure_allocate" ||
           peek(1).text == "shared_allocate" ||
           peek(1).text == "owned_array" ||
           peek(1).text == "owned_array_aligned" ||
           peek(1).text == "array_view" ||
           peek(1).text == "mapped_read" || peek(1).text == "mapped_write") &&
          peek(2).kind == TokenKind::Less) {
        advance();  // '.'
        const Token operation = advance();
        advance();  // '<'
        Type allocated_type = parse_type_annotation();
        expect(TokenKind::Greater, "after the allocated element type");
        expect(TokenKind::LParen, "after the typed memory allocation operation");
        vector<ExprPtr> arguments = parse_argument_list();
        expect(TokenKind::RParen, "to close the typed memory allocation operation");
        return make_unique<CallExpr>(token.location,
                                     token.text + "." + operation.text,
                                     move(arguments), allocated_type);
      }
      // A dotted callable such as memory.profiler.capture() is distinguished
      // from member access by the final parenthesis. Preserve the complete
      // qualified name for semantic analysis and lowering.
      size_t qualified_parts = 0;
      size_t lookahead = 0;
      while (peek(lookahead).kind == TokenKind::Dot &&
             peek(lookahead + 1).kind == TokenKind::Identifier) {
        ++qualified_parts;
        lookahead += 2;
      }
      if (qualified_parts != 0 && peek(lookahead).kind == TokenKind::LParen) {
        string qualified_name = token.text;
        for (size_t part = 0; part < qualified_parts; ++part) {
          advance();  // '.'
          qualified_name += "." + advance().text;
        }
        advance();  // '('
        vector<ExprPtr> arguments = parse_argument_list();
        expect(TokenKind::RParen, "to close the qualified call");
        return make_unique<CallExpr>(token.location, move(qualified_name),
                                     move(arguments));
      }
      // `module.name` is a member access, except when it is immediately called:
      // then it is a call to a function in another module (Phase 5).
      if (check(TokenKind::Dot) && peek(1).kind == TokenKind::Identifier &&
          peek(2).kind == TokenKind::LParen) {
        advance();  // '.'
        const Token member = advance();
        advance();  // '('
        vector<ExprPtr> arguments = parse_argument_list();
        expect(TokenKind::RParen, "to close the call");
        return make_unique<CallExpr>(token.location, token.text + "." + member.text,
                                     move(arguments));
      }
      if (check(TokenKind::LBrace)) {
        return parse_struct_init(token.text, token.location);
      }
      if (match(TokenKind::LParen)) {
        vector<ExprPtr> arguments = parse_argument_list();
        expect(TokenKind::RParen, "to close the call");
        return make_unique<CallExpr>(token.location, token.text, move(arguments));
      }
      return make_unique<NameExpr>(token.location, token.text);
    }
    case TokenKind::LParen: {
      advance();
      if (check(TokenKind::RParen)) {
        advance();
        return make_unique<TupleLiteralExpr>(token.location, vector<ExprPtr>{});
      }
      ExprPtr first = parse_expression();
      if (match(TokenKind::Comma)) {
        vector<ExprPtr> tuple_elements;
        tuple_elements.push_back(move(first));
        if (!check(TokenKind::RParen)) {
          do {
            if (check(TokenKind::RParen)) break;
            tuple_elements.push_back(parse_expression());
          } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "to close tuple expression");
        return make_unique<TupleLiteralExpr>(token.location, move(tuple_elements));
      }
      expect(TokenKind::RParen, "to close the parenthesized expression");
      return first;
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
  auto placeholder = make_unique<IntegerLiteralExpr>(token.location, 0);
  placeholder->set_type(Type::error());
  return placeholder;
}

string_view unary_op_spelling(UnaryOp op) {
  switch (op) {
    case UnaryOp::Negate: return "-";
    case UnaryOp::Plus: return "+";
    case UnaryOp::LogicalNot: return "!";
    case UnaryOp::BitwiseNot: return "~";
  }
  return "?";
}

string_view binary_op_spelling(BinaryOp op) {
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

string_view binding_kind_spelling(BindingKind kind) {
  switch (kind) {
    case BindingKind::Let: return "let";
    case BindingKind::Var: return "var";
    case BindingKind::Const: return "const";
  }
  return "let";
}

}  // namespace nirq
