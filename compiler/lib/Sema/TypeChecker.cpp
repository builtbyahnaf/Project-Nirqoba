#include "nirqoba/Sema/TypeChecker.h"

#include <unordered_set>

namespace nirq {
namespace {

// Builtin `print` is the only callable in Phase 1; it is spelled like a
// function so Phase 3 can replace this special case with real overloads
// without changing the surface syntax.
constexpr const char* kPrintBuiltin = "print";

}  // namespace

bool TypeChecker::check(Program& program) {
  std::unordered_set<std::string> function_names;
  const FunctionDecl* main_function = nullptr;
  int main_count = 0;

  for (const auto& function : program.functions()) {
    if (!function_names.insert(function->name()).second) {
      diagnostics_.error(diag::kDuplicateFunction, function->location(),
                         "function '" + function->name() + "' is already defined");
    }
    if (function->name() == "main") {
      ++main_count;
      main_function = function.get();
    }
  }

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
                         "'main' must not declare parameters in Phase 1");
    }
    if (!main_function->return_type().is_int() && !main_function->return_type().is_void()) {
      diagnostics_.error(diag::kInvalidMainReturn, main_function->location(),
                         "'main' must return 'int' or 'void', not '" +
                             main_function->return_type().to_string() + "'");
    }
  }

  for (const auto& function : program.functions()) {
    if (function->return_type().is_int() || function->return_type().is_void()) {
      check_function(*function);
    }
  }
  return !diagnostics_.has_errors();
}

void TypeChecker::check_function(FunctionDecl& function) {
  symbols_.push_scope();
  current_return_type_ = function.return_type();

  for (const Parameter& parameter : function.parameters()) {
    if (!symbols_.declare(Symbol{parameter.name, parameter.type, BindingKind::Let,
                                 parameter.location})) {
      diagnostics_.error(diag::kDuplicateName, parameter.location,
                         "parameter '" + parameter.name + "' is already declared");
    }
  }

  check_block(const_cast<BlockStmt&>(function.body()));

  if (current_return_type_.is_int() && block_can_fall_through(function.body())) {
    diagnostics_.error(diag::kMissingReturn, function.location(),
                       "function '" + function.name() +
                           "' returns 'int' but control can reach the end of its body");
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

void TypeChecker::check_return(ReturnStmt& statement) {
  if (!current_return_type_.is_valid()) return;  // function already failed to resolve

  if (!statement.has_value()) {
    if (current_return_type_.is_int()) {
      diagnostics_.error(diag::kReturnValueRequired, statement.location(),
                         "'return' without a value in a function returning 'int'");
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
    case Expr::Kind::IntegerLiteral:
      type = Type::int_type();
      break;
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
      if (operand_type.is_valid() && !operand_type.is_int()) {
        diagnostics_.error(diag::kTypeMismatch, unary.location(),
                           "operator '" + std::string(unary_op_spelling(unary.op())) +
                               "' requires an 'int' operand, not '" +
                               operand_type.to_string() + "'");
      }
      type = operand_type.is_int() ? Type::int_type() : Type::error();
      break;
    }
    case Expr::Kind::Binary: {
      auto& binary = static_cast<BinaryExpr&>(expr);
      const Type lhs_type = check_expression(const_cast<Expr&>(binary.lhs()));
      const Type rhs_type = check_expression(const_cast<Expr&>(binary.rhs()));
      const std::string spelling(binary_op_spelling(binary.op()));
      if (lhs_type.is_valid() && !lhs_type.is_int()) {
        diagnostics_.error(diag::kTypeMismatch, binary.lhs().location(),
                           "left operand of '" + spelling + "' must be 'int', not '" +
                               lhs_type.to_string() + "'");
      }
      if (rhs_type.is_valid() && !rhs_type.is_int()) {
        diagnostics_.error(diag::kTypeMismatch, binary.rhs().location(),
                           "right operand of '" + spelling + "' must be 'int', not '" +
                               rhs_type.to_string() + "'");
      }
      type = (lhs_type.is_int() && rhs_type.is_int()) ? Type::int_type() : Type::error();
      break;
    }
  }
  expr.set_type(type);
  return type;
}

Type TypeChecker::check_call(CallExpr& call) {
  if (call.callee() == kPrintBuiltin) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'print' takes exactly one 'string' argument, but " +
                             std::to_string(call.arguments().size()) + " were given");
      for (const ExprPtr& argument : call.arguments()) {
        check_expression(const_cast<Expr&>(*argument));
      }
      return Type::void_type();
    }
    const Type argument_type =
        check_expression(const_cast<Expr&>(*call.arguments().front()));
    if (argument_type.is_valid() && !argument_type.is_string()) {
      diagnostics_.error(diag::kPrintRequiresString, call.arguments().front()->location(),
                         "'print' requires a 'string' argument, not '" +
                             argument_type.to_string() +
                             "'; formatting other types is specified for Phase 2");
    }
    return Type::void_type();
  }

  // Any other callee: report the phase gap, but still walk the arguments so
  // name errors inside them are reported too.
  for (const ExprPtr& argument : call.arguments()) {
    check_expression(const_cast<Expr&>(*argument));
  }
  diagnostics_.error(diag::kNotCallable, call.location(),
                     "call to '" + call.callee() +
                         "' is not supported in Phase 1; user-defined function calls are "
                         "specified for Phase 3");
  return Type::error();
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
  if (block.statements().empty()) return true;
  const Stmt& last = *block.statements().back();
  switch (last.kind()) {
    case Stmt::Kind::Return:
      return false;
    case Stmt::Kind::Block:
      return block_can_fall_through(static_cast<const BlockStmt&>(last));
    default:
      // Phase 1 has no branching, so any other final statement can reach the
      // closing brace. Phase 3 replaces this with a terminator analysis.
      return true;
  }
}

}  // namespace nirq
