#include "nirqoba/Sema/TypeChecker.h"

#include <algorithm>
#include <unordered_set>

namespace nirq {
namespace {

constexpr const char* kPrintBuiltin = "print";

static bool statement_can_fall_through(const Stmt& stmt);

static bool block_falls_through(const BlockStmt& block) {
  if (block.statements().empty()) return true;
  for (const StmtPtr& statement : block.statements()) {
    if (!statement_can_fall_through(*statement)) {
      return false;
    }
  }
  return true;
}

static bool statement_can_fall_through(const Stmt& stmt) {
  switch (stmt.kind()) {
    case Stmt::Kind::Return:
      return false;
    case Stmt::Kind::Block:
      return block_falls_through(static_cast<const BlockStmt&>(stmt));
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(stmt);
      if (!if_stmt.has_else()) return true;
      const bool then_falls = block_falls_through(if_stmt.then_branch());
      const bool else_falls = statement_can_fall_through(*if_stmt.else_branch());
      return then_falls || else_falls;
    }
    default:
      return true;
  }
}

}  // namespace

bool TypeChecker::check(Program& program) {
  functions_.clear();
  const FunctionDecl* main_function = nullptr;
  int main_count = 0;

  // Pass 1: Collect function signatures for forward references & mutual recursion.
  for (const auto& function : program.functions()) {
    std::vector<Type> param_types;
    param_types.reserve(function->parameters().size());
    for (const Parameter& param : function->parameters()) {
      param_types.push_back(param.type);
    }

    FunctionSignature sig{
        function->name(),
        function->return_type(),
        std::move(param_types),
        function->location()
    };

    if (!functions_.emplace(function->name(), std::move(sig)).second) {
      diagnostics_.error(diag::kDuplicateFunction, function->location(),
                         "function '" + function->name() + "' is already defined");
    }

    if (function->name() == "main") {
      ++main_count;
      main_function = function.get();
    }
  }

  // Entry point validation.
  if (main_count == 0) {
    SourceLocation location;
    if (!program.functions().empty()) location = program.functions().front()->location();
    diagnostics_.error(diag::kMissingMain, location,
                       "program has no entry point: exactly one 'int function main()' is required");
  } else if (main_count > 1) {
    diagnostics_.error(diag::kDuplicateMain, main_function->location(),
                       "program defines 'main' more than once; exactly one entry point is allowed");
  } else if (main_function != nullptr) {
    if (!main_function->parameters().empty()) {
      diagnostics_.error(diag::kMainParameters, main_function->location(),
                         "'main' must not declare parameters in Phase 3");
    }
    if (!main_function->return_type().is_integer() && !main_function->return_type().is_void()) {
      diagnostics_.error(diag::kInvalidMainReturn, main_function->location(),
                         "'main' must return an integer or 'void', not '" +
                             main_function->return_type().to_string() + "'");
    }
  }

  // Pass 2: Type-check each function body.
  for (const auto& function : program.functions()) {
    check_function(*function);
  }

  return !diagnostics_.has_errors();
}

void TypeChecker::check_function(FunctionDecl& function) {
  symbols_.push_scope();
  current_return_type_ = function.return_type();
  loop_depth_ = 0;

  for (const Parameter& parameter : function.parameters()) {
    if (!symbols_.declare(Symbol{parameter.name, parameter.type, BindingKind::Let,
                                 parameter.location})) {
      diagnostics_.error(diag::kDuplicateName, parameter.location,
                         "parameter '" + parameter.name + "' is already declared");
    }
  }

  check_block(const_cast<BlockStmt&>(function.body()));

  if (!current_return_type_.is_void() && block_can_fall_through(function.body())) {
    diagnostics_.error(diag::kMissingReturn, function.location(),
                       "function '" + function.name() + "' returns '" +
                           current_return_type_.to_string() +
                           "' but control can reach the end of its body without returning a value");
  }

  symbols_.pop_scope();
  current_return_type_ = Type::error();
}

void TypeChecker::check_block(BlockStmt& block) {
  symbols_.push_scope();
  for (StmtPtr& statement : block.statements()) {
    check_statement(*statement);
  }
  symbols_.pop_scope();
}

void TypeChecker::check_statement(Stmt& statement) {
  switch (statement.kind()) {
    case Stmt::Kind::Block:
      check_block(static_cast<BlockStmt&>(statement));
      return;
    case Stmt::Kind::VariableDeclaration:
      check_variable_declaration(static_cast<VariableDeclarationStmt&>(statement));
      return;
    case Stmt::Kind::Assignment:
      check_assignment(static_cast<AssignmentStmt&>(statement));
      return;
    case Stmt::Kind::CompoundAssignment:
      check_compound_assignment(static_cast<CompoundAssignmentStmt&>(statement));
      return;
    case Stmt::Kind::If:
      check_if(static_cast<IfStmt&>(statement));
      return;
    case Stmt::Kind::While:
      check_while(static_cast<WhileStmt&>(statement));
      return;
    case Stmt::Kind::For:
      check_for(static_cast<ForStmt&>(statement));
      return;
    case Stmt::Kind::Break:
      check_break(static_cast<BreakStmt&>(statement));
      return;
    case Stmt::Kind::Continue:
      check_continue(static_cast<ContinueStmt&>(statement));
      return;
    case Stmt::Kind::Return:
      check_return(static_cast<ReturnStmt&>(statement));
      return;
    case Stmt::Kind::Expression:
      check_expression(const_cast<Expr&>(static_cast<ExprStmt&>(statement).expr()));
      return;
  }
}

void TypeChecker::check_variable_declaration(VariableDeclarationStmt& declaration) {
  const Type initializer_type =
      check_expression(const_cast<Expr&>(declaration.initializer()));
  Type binding_type = initializer_type;

  if (declaration.has_annotation()) {
    if (declaration.annotation().is_valid() && initializer_type.is_valid() &&
        declaration.annotation() != initializer_type) {
      diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                         "cannot initialize '" + declaration.name() + "' of type '" +
                             declaration.annotation().to_string() + "' with a value of type '" +
                             initializer_type.to_string() + "'");
    }
    binding_type = declaration.annotation();
  }

  if (!symbols_.declare(Symbol{declaration.name(), binding_type, declaration.binding(),
                               declaration.location()})) {
    diagnostics_.error(diag::kDuplicateName, declaration.location(),
                       "'" + declaration.name() + "' is already declared in this scope");
  }
}

void TypeChecker::check_assignment(AssignmentStmt& assignment) {
  const Type value_type = check_expression(const_cast<Expr&>(assignment.value()));
  const Symbol* symbol = symbols_.lookup(assignment.name());
  if (symbol == nullptr) {
    diagnostics_.error(diag::kUndefinedName, assignment.name_location(),
                       "assignment to undeclared variable '" + assignment.name() + "'");
    return;
  }
  if (!symbol->is_mutable()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.name_location(),
                       "'" + assignment.name() + "' was declared as " +
                           std::string(binding_kind_spelling(symbol->binding)) +
                           " and cannot be reassigned; declare it with 'var' to mutate it");
    return;
  }
  if (symbol->type.is_valid() && value_type.is_valid() && symbol->type != value_type) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot assign a value of type '" + value_type.to_string() +
                           "' to '" + assignment.name() + "' of type '" +
                           symbol->type.to_string() + "'");
  }
}

void TypeChecker::check_compound_assignment(CompoundAssignmentStmt& assignment) {
  const Type value_type = check_expression(const_cast<Expr&>(assignment.value()));
  const Symbol* symbol = symbols_.lookup(assignment.name());
  if (symbol == nullptr) {
    diagnostics_.error(diag::kUndefinedName, assignment.name_location(),
                       "compound assignment to undeclared variable '" + assignment.name() + "'");
    return;
  }
  if (!symbol->is_mutable()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.name_location(),
                       "'" + assignment.name() + "' was declared as " +
                           std::string(binding_kind_spelling(symbol->binding)) +
                           " and cannot be mutated; declare it with 'var' to mutate it");
    return;
  }
  if (!symbol->type.is_valid() || !value_type.is_valid()) return;

  if (symbol->type != value_type) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot apply compound operator to target of type '" +
                           symbol->type.to_string() + "' and operand of type '" +
                           value_type.to_string() + "'");
    return;
  }

  switch (assignment.op()) {
    case BinaryOp::Remainder:
    case BinaryOp::BitwiseAnd:
    case BinaryOp::BitwiseOr:
    case BinaryOp::BitwiseXor:
    case BinaryOp::ShiftLeft:
    case BinaryOp::ShiftRight:
      if (!symbol->type.is_integer()) {
        diagnostics_.error(diag::kTypeMismatch, assignment.location(),
                           "operator '" + std::string(binary_op_spelling(assignment.op())) +
                               "=' requires integer operands, found '" + symbol->type.to_string() + "'");
      }
      break;
    case BinaryOp::Add:
    case BinaryOp::Subtract:
    case BinaryOp::Multiply:
    case BinaryOp::Divide:
    case BinaryOp::Exponentiate:
      if (!symbol->type.is_numeric()) {
        diagnostics_.error(diag::kTypeMismatch, assignment.location(),
                           "operator '" + std::string(binary_op_spelling(assignment.op())) +
                               "=' requires numeric operands, found '" + symbol->type.to_string() + "'");
      }
      break;
    default:
      break;
  }
}

void TypeChecker::check_if(IfStmt& statement) {
  const Type cond_type = check_expression(const_cast<Expr&>(statement.condition()));
  if (cond_type.is_valid() && !cond_type.is_bool()) {
    diagnostics_.error(diag::kTypeMismatch, statement.condition().location(),
                       "'if' condition must be 'bool', not '" + cond_type.to_string() + "'");
  }
  check_block(const_cast<BlockStmt&>(statement.then_branch()));
  if (statement.has_else()) {
    check_statement(const_cast<Stmt&>(*statement.else_branch()));
  }
}

void TypeChecker::check_while(WhileStmt& statement) {
  const Type cond_type = check_expression(const_cast<Expr&>(statement.condition()));
  if (cond_type.is_valid() && !cond_type.is_bool()) {
    diagnostics_.error(diag::kTypeMismatch, statement.condition().location(),
                       "'while' condition must be 'bool', not '" + cond_type.to_string() + "'");
  }
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
}

void TypeChecker::check_for(ForStmt& statement) {
  symbols_.push_scope();
  if (statement.has_initializer()) {
    check_statement(const_cast<Stmt&>(*statement.initializer()));
  }
  if (statement.has_condition()) {
    const Type cond_type = check_expression(const_cast<Expr&>(*statement.condition()));
    if (cond_type.is_valid() && !cond_type.is_bool()) {
      diagnostics_.error(diag::kTypeMismatch, statement.condition()->location(),
                         "'for' condition must be 'bool', not '" + cond_type.to_string() + "'");
    }
  }
  if (statement.has_step()) {
    check_statement(const_cast<Stmt&>(*statement.step()));
  }
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
  symbols_.pop_scope();
}

void TypeChecker::check_break(BreakStmt& statement) {
  if (loop_depth_ <= 0) {
    diagnostics_.error(diag::kBreakOutsideLoop, statement.location(),
                       "'break' is only allowed inside an enclosing loop");
  }
}

void TypeChecker::check_continue(ContinueStmt& statement) {
  if (loop_depth_ <= 0) {
    diagnostics_.error(diag::kContinueOutsideLoop, statement.location(),
                       "'continue' is only allowed inside an enclosing loop");
  }
}

void TypeChecker::check_return(ReturnStmt& statement) {
  if (!current_return_type_.is_valid()) return;

  if (!statement.has_value()) {
    if (!current_return_type_.is_void()) {
      diagnostics_.error(diag::kReturnValueRequired, statement.location(),
                         "'return' without a value in a function returning '" +
                             current_return_type_.to_string() + "'");
    }
    return;
  }

  const Type value_type = check_expression(const_cast<Expr&>(*statement.value()));
  if (current_return_type_.is_void()) {
    diagnostics_.error(diag::kVoidReturnsValue, statement.location(),
                       "function returns 'void' but 'return' has a value of type '" +
                           value_type.to_string() + "'");
    return;
  }
  if (value_type.is_valid() && current_return_type_ != value_type) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "function returns '" + current_return_type_.to_string() +
                           "' but this 'return' yields '" + value_type.to_string() + "'");
  }
}

Type TypeChecker::check_expression(Expr& expr) {
  Type type = Type::error();
  switch (expr.kind()) {
    case Expr::Kind::IntegerLiteral: {
      const auto& lit = static_cast<const IntegerLiteralExpr&>(expr);
      if (lit.suffix() == "i8") type = Type::i8_type();
      else if (lit.suffix() == "i16") type = Type::i16_type();
      else if (lit.suffix() == "i32") type = Type::i32_type();
      else if (lit.suffix() == "i64") type = Type::i64_type();
      else if (lit.suffix() == "u8") type = Type::u8_type();
      else if (lit.suffix() == "u16") type = Type::u16_type();
      else if (lit.suffix() == "u32") type = Type::u32_type();
      else if (lit.suffix() == "u64") type = Type::u64_type();
      else type = Type::int_type();
      break;
    }
    case Expr::Kind::FloatLiteral: {
      const auto& lit = static_cast<const FloatLiteralExpr&>(expr);
      if (lit.suffix() == "f32") type = Type::f32_type();
      else type = Type::f64_type();
      break;
    }
    case Expr::Kind::CharLiteral:
      type = Type::char_type();
      break;
    case Expr::Kind::Cast: {
      auto& cast = static_cast<CastExpr&>(expr);
      const Type op_type = check_expression(const_cast<Expr&>(cast.operand()));
      if (op_type.is_valid() && cast.target_type().is_valid()) {
        bool valid_cast = false;
        if (op_type.is_numeric() && cast.target_type().is_numeric()) valid_cast = true;
        else if (op_type.is_bool() && cast.target_type().is_integer()) valid_cast = true;
        else if (op_type.is_integer() && cast.target_type().is_bool()) valid_cast = true;
        else if (op_type.is_char() && cast.target_type().is_integer()) valid_cast = true;
        else if (op_type.is_integer() && cast.target_type().is_char()) valid_cast = true;

        if (!valid_cast) {
          diagnostics_.error(diag::kTypeMismatch, cast.location(),
                             "cannot cast value of type '" + op_type.to_string() +
                             "' to '" + cast.target_type().to_string() + "'");
          type = Type::error();
        } else {
          type = cast.target_type();
        }
      }
      break;
    }
    case Expr::Kind::StringLiteral:
      type = Type::string_type();
      break;
    case Expr::Kind::BoolLiteral:
      type = Type::bool_type();
      break;
    case Expr::Kind::Name: {
      auto& name = static_cast<NameExpr&>(expr);
      const Symbol* symbol = symbols_.lookup(name.name());
      if (symbol == nullptr) {
        diagnostics_.error(diag::kUndefinedName, name.location(),
                           "use of undeclared variable '" + name.name() + "'");
      } else {
        type = symbol->type;
      }
      break;
    }
    case Expr::Kind::Call:
      type = check_call(static_cast<CallExpr&>(expr));
      break;
    case Expr::Kind::Unary: {
      auto& unary = static_cast<UnaryExpr&>(expr);
      const Type operand_type = check_expression(const_cast<Expr&>(unary.operand()));
      if (!operand_type.is_valid()) break;

      switch (unary.op()) {
        case UnaryOp::Negate:
        case UnaryOp::Plus:
          if (!operand_type.is_numeric()) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "operator '" + std::string(unary_op_spelling(unary.op())) +
                                   "' requires a numeric operand, not '" +
                                   operand_type.to_string() + "'");
            type = Type::error();
          } else {
            type = operand_type;
          }
          break;
        case UnaryOp::LogicalNot:
          if (!operand_type.is_bool()) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "operator '!' requires a 'bool' operand, not '" +
                                   operand_type.to_string() + "'");
            type = Type::error();
          } else {
            type = Type::bool_type();
          }
          break;
        case UnaryOp::BitwiseNot:
          if (!operand_type.is_integer()) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "operator '~' requires an integer operand, not '" +
                                   operand_type.to_string() + "'");
            type = Type::error();
          } else {
            type = operand_type;
          }
          break;
      }
      break;
    }
    case Expr::Kind::Binary: {
      auto& binary = static_cast<BinaryExpr&>(expr);
      const Type lhs_type = check_expression(const_cast<Expr&>(binary.lhs()));
      const Type rhs_type = check_expression(const_cast<Expr&>(binary.rhs()));
      if (!lhs_type.is_valid() || !rhs_type.is_valid()) break;

      const std::string spelling(binary_op_spelling(binary.op()));

      if (binary.op() == BinaryOp::Divide || binary.op() == BinaryOp::Remainder) {
        if (binary.rhs().kind() == Expr::Kind::IntegerLiteral) {
          const auto& lit = static_cast<const IntegerLiteralExpr&>(binary.rhs());
          if (lit.value() == 0) {
            diagnostics_.error(diag::kConstDivisionByZero, binary.rhs().location(),
                               "division by constant zero");
          }
        }
      }

      switch (binary.op()) {
        case BinaryOp::Add:
        case BinaryOp::Subtract:
        case BinaryOp::Multiply:
        case BinaryOp::Divide:
        case BinaryOp::Exponentiate:
          if (!lhs_type.is_numeric() || !rhs_type.is_numeric()) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "operator '" + spelling + "' requires numeric operands, found '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else if (lhs_type != rhs_type) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "mismatched operand types for '" + spelling + "': '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else {
            type = lhs_type;
          }
          break;
        case BinaryOp::Remainder:
        case BinaryOp::BitwiseAnd:
        case BinaryOp::BitwiseOr:
        case BinaryOp::BitwiseXor:
        case BinaryOp::ShiftLeft:
        case BinaryOp::ShiftRight:
          if (!lhs_type.is_integer() || !rhs_type.is_integer()) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "operator '" + spelling + "' requires integer operands, found '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else if (lhs_type != rhs_type) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "mismatched operand types for '" + spelling + "': '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else {
            type = lhs_type;
          }
          break;
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
          if (lhs_type != rhs_type) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "cannot compare values of different types for equality: '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else {
            type = Type::bool_type();
          }
          break;
        case BinaryOp::Less:
        case BinaryOp::LessEqual:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEqual:
          if (!lhs_type.is_numeric() || !rhs_type.is_numeric()) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "comparison operator '" + spelling + "' requires numeric operands, found '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else if (lhs_type != rhs_type) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "mismatched operand types for comparison '" + spelling + "': '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else {
            type = Type::bool_type();
          }
          break;
        case BinaryOp::LogicalAnd:
        case BinaryOp::LogicalOr:
          if (!lhs_type.is_bool() || !rhs_type.is_bool()) {
            diagnostics_.error(diag::kTypeMismatch, binary.location(),
                               "logical operator '" + spelling + "' requires 'bool' operands, found '" +
                                   lhs_type.to_string() + "' and '" + rhs_type.to_string() + "'");
            type = Type::error();
          } else {
            type = Type::bool_type();
          }
          break;
      }
      break;
    }
  }
  expr.set_type(type);
  return type;
}

Type TypeChecker::check_call(CallExpr& call) {
  // Builtin 'print'
  if (call.callee() == kPrintBuiltin) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'print' takes exactly one argument, but " +
                             std::to_string(call.arguments().size()) + " were given");
      for (const ExprPtr& argument : call.arguments()) {
        check_expression(const_cast<Expr&>(*argument));
      }
      return Type::void_type();
    }
    const Type argument_type =
        check_expression(const_cast<Expr&>(*call.arguments().front()));
    if (argument_type.is_valid()) {
      if (!argument_type.is_numeric() && !argument_type.is_bool() &&
          !argument_type.is_char() && !argument_type.is_string()) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments().front()->location(),
                           "'print' requires a scalar or string argument, not '" +
                               argument_type.to_string() + "'");
      }
    }
    return Type::void_type();
  }

  // User-defined function call
  auto it = functions_.find(call.callee());
  if (it == functions_.end()) {
    diagnostics_.error(diag::kUndefinedName, call.location(),
                       "call to undeclared function '" + call.callee() + "'");
    for (const ExprPtr& argument : call.arguments()) {
      check_expression(const_cast<Expr&>(*argument));
    }
    return Type::error();
  }

  const FunctionSignature& sig = it->second;
  if (call.arguments().size() != sig.parameter_types.size()) {
    diagnostics_.error(diag::kPrintArity, call.location(),
                       "function '" + call.callee() + "' expects " +
                           std::to_string(sig.parameter_types.size()) +
                           " argument" + (sig.parameter_types.size() == 1 ? "" : "s") +
                           ", but " + std::to_string(call.arguments().size()) +
                           " were given");
  }

  const std::size_t count = std::min(call.arguments().size(), sig.parameter_types.size());
  for (std::size_t i = 0; i < count; ++i) {
    const Type arg_type = check_expression(const_cast<Expr&>(*call.arguments()[i]));
    if (arg_type.is_valid() && arg_type != sig.parameter_types[i]) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "argument " + std::to_string(i + 1) + " of '" + call.callee() +
                             "' must be of type '" + sig.parameter_types[i].to_string() +
                             "', not '" + arg_type.to_string() + "'");
    }
  }

  for (std::size_t i = count; i < call.arguments().size(); ++i) {
    check_expression(const_cast<Expr&>(*call.arguments()[i]));
  }

  return sig.return_type;
}

void TypeChecker::require_type(const Expr& expr, Type expected, const char* context) {
  const Type actual = expr.type();
  if (!actual.is_valid() || !expected.is_valid()) return;
  if (actual != expected) {
    diagnostics_.error(diag::kTypeMismatch, expr.location(),
                       std::string(context) + " requires '" + expected.to_string() +
                           "', not '" + actual.to_string() + "'");
  }
}

bool TypeChecker::block_can_fall_through(const BlockStmt& block) const {
  return block_falls_through(block);
}

}  // namespace nirq
