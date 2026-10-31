#include "nirqoba/Sema/TypeChecker.h"

#include <algorithm>
#include <unordered_set>

#include "nirqoba/Driver/ModuleResolver.h"

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
  structs_.clear();
  current_module_ = program.module_name();
  const FunctionDecl* main_function = nullptr;
  int main_count = 0;

  // Pass 0: Register struct declarations.
  for (const auto& decl : program.structs()) {
    // Validate unique field names.
    std::unordered_set<std::string> seen_fields;
    for (const Parameter& field : decl->fields()) {
      if (!seen_fields.insert(field.name).second) {
        diagnostics_.error(diag::kDuplicateName, field.location,
                           "struct '" + decl->name() + "' has duplicate field '" +
                               field.name + "'");
      }
    }
    StructInfo info;
    info.name = decl->name();
    info.fields = decl->fields();
    info.location = decl->location();
    info.module_name = current_module_;
    info.visibility = decl->visibility();
    if (!structs_.emplace(decl->name(), std::move(info)).second) {
      diagnostics_.error(diag::kDuplicateName, decl->location(),
                         "struct '" + decl->name() + "' is already defined");
    }
  }

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
        function->location(),
        current_module_,
        function->visibility(),
        function->name()  // single-module compilation links symbols by name
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
                       "program has no entry point: exactly one 'main' function is required");
  } else if (main_count > 1) {
    diagnostics_.error(diag::kDuplicateMain, main_function->location(),
                       "program defines 'main' more than once; exactly one entry point is allowed");
  } else if (main_function != nullptr) {
    if (!main_function->parameters().empty()) {
      bool valid_args = false;
      if (main_function->parameters().size() == 1) {
        const Type& ptype = main_function->parameters()[0].type;
        if (ptype.is_dynamic_array() && ptype.element_type().is_string()) {
          valid_args = true;
        }
      }
      if (!valid_args) {
        diagnostics_.error(diag::kMainParameters, main_function->location(),
                           "'main' may only declare 'string[] args' or no parameters");
      }
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

bool TypeChecker::check_modules(const std::vector<std::unique_ptr<ModuleUnit>>& modules) {
  functions_.clear();
  structs_.clear();
  current_module_.clear();
  const FunctionDecl* main_function = nullptr;
  int main_count = 0;

  // The module that defines `main` is the entry module. Only it claims the
  // bare symbol names (so the entry point links as `main`), which also means a
  // second file named on the command line cannot silently shadow its functions.
  const ModuleUnit* entry_unit = nullptr;
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    for (const auto& function : unit->ast->functions()) {
      if (function->name() == "main") {
        if (entry_unit == nullptr) entry_unit = unit.get();
        ++main_count;
      }
    }
  }

  // Pass 0: register every struct under its module-qualified name. The entry
  // module also owns the bare names, which keeps a single-file program
  // resolving exactly as it did before modules existed.
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    for (const auto& decl : unit->ast->structs()) {
      StructInfo info;
      info.name = decl->name();
      info.fields = decl->fields();
      info.location = decl->location();
      info.module_name = unit->name;
      info.visibility = decl->visibility();

      if (!structs_.emplace(unit->name + "." + decl->name(), info).second) {
        diagnostics_.error(diag::kDuplicateName, decl->location(),
                           "struct '" + decl->name() + "' is already defined in module '" +
                               unit->name + "'");
      }
      if (unit.get() == entry_unit) {
        structs_.emplace(decl->name(), info);
      }
    }
  }

  // Pass 1: register function signatures. Symbols are `module.name` except in
  // the root module, whose functions keep bare names so the entry point is
  // emitted as `main`.
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    for (const auto& function : unit->ast->functions()) {
      std::vector<Type> param_types;
      param_types.reserve(function->parameters().size());
      for (const Parameter& param : function->parameters()) {
        param_types.push_back(param.type);
      }

      const bool is_entry = (unit.get() == entry_unit);
      const std::string qualified = unit->name + "." + function->name();

      FunctionSignature sig{
          function->name(),
          function->return_type(),
          std::move(param_types),
          function->location(),
          unit->name,
          function->visibility(),
          is_entry ? function->name() : qualified
      };

      // Every module is reachable by its qualified name, including the entry
      // module, so `import app; app.helper()` keeps working.
      if (!functions_.emplace(qualified, sig).second) {
        diagnostics_.error(diag::kDuplicateFunction, function->location(),
                           "function '" + function->name() +
                               "' is already defined in module '" + unit->name + "'");
      }
      if (is_entry) {
        functions_.emplace(function->name(), sig);
      }

      if (function->name() == "main" && is_entry && main_function == nullptr) {
        main_function = function.get();
      }
    }
  }

  // Pass 2: apply imports. A selective import binds one item into the importing
  // module's namespace; an aliased import binds every item of the target under
  // the alias. Both only expose `public` declarations.
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    for (const auto& imp : unit->ast->imports()) {
      const std::string module = imp->module_name();

      if (imp->is_selective()) {
        const std::string qualified = module + "." + imp->item_name();
        auto fn = functions_.find(qualified);
        if (fn != functions_.end()) {
          if (fn->second.visibility == Visibility::Private) {
            diagnostics_.error(diag::kPrivateAccessViolation, imp->location(),
                               "cannot import private function '" + qualified + "'");
            continue;
          }
          auto inserted = functions_.emplace(imp->item_name(), fn->second);
          if (!inserted.second && inserted.first->second.symbol != fn->second.symbol) {
            diagnostics_.error(diag::kImportConflict, imp->location(),
                               "import of '" + qualified + "' conflicts with an existing '" +
                                   imp->item_name() + "'");
          }
          continue;
        }
        auto st = structs_.find(qualified);
        if (st != structs_.end()) {
          if (st->second.visibility == Visibility::Private) {
            diagnostics_.error(diag::kPrivateAccessViolation, imp->location(),
                               "cannot import private struct '" + qualified + "'");
            continue;
          }
          auto inserted = structs_.emplace(imp->item_name(), st->second);
          if (!inserted.second &&
              inserted.first->second.module_name != st->second.module_name) {
            diagnostics_.error(diag::kImportConflict, imp->location(),
                               "import of '" + qualified + "' conflicts with an existing '" +
                                   imp->item_name() + "'");
          }
          continue;
        }
        diagnostics_.error(diag::kUndefinedName, imp->location(),
                           "module '" + module + "' has no public item '" + imp->item_name() + "'");
        continue;
      }

      if (imp->is_aliased()) {
        // Snapshot first: the loop below inserts into the same map.
        std::vector<FunctionSignature> module_functions;
        for (const auto& kv : functions_) {
          if (kv.second.module_name == module && kv.second.visibility == Visibility::Public) {
            module_functions.push_back(kv.second);
          }
        }
        for (const FunctionSignature& sig : module_functions) {
          functions_.emplace(imp->alias() + "." + sig.name, sig);
        }

        std::vector<StructInfo> module_structs;
        for (const auto& kv : structs_) {
          if (kv.second.module_name == module && kv.second.visibility == Visibility::Public) {
            module_structs.push_back(kv.second);
          }
        }
        for (const StructInfo& info : module_structs) {
          structs_.emplace(imp->alias() + "." + info.name, info);
        }
      }
    }
  }

  // Entry point validation.
  if (main_count == 0) {
    diagnostics_.error(diag::kMissingMain, SourceLocation{},
                       "program has no entry point: exactly one 'main' function is required");
  } else if (main_count > 1) {
    diagnostics_.error(diag::kDuplicateMain, main_function->location(),
                       "program defines 'main' more than once");
  } else if (main_function != nullptr) {
    if (!main_function->parameters().empty()) {
      bool valid_args = false;
      if (main_function->parameters().size() == 1) {
        const Type& ptype = main_function->parameters()[0].type;
        if (ptype.is_dynamic_array() && ptype.element_type().is_string()) {
          valid_args = true;
        }
      }
      if (!valid_args) {
        diagnostics_.error(diag::kMainParameters, main_function->location(),
                           "'main' may only declare 'string[] args' or no parameters");
      }
    }
    if (!main_function->return_type().is_integer() && !main_function->return_type().is_void()) {
      diagnostics_.error(diag::kInvalidMainReturn, main_function->location(),
                         "'main' must return an integer or 'void', not '" +
                             main_function->return_type().to_string() + "'");
    }
  }

  // Pass 2: Type-check each function body in each module.
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    current_module_ = unit->name;
    for (const auto& function : unit->ast->functions()) {
      check_function(*function);
    }
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
    case Stmt::Kind::Destructuring:
      check_destructuring(static_cast<DestructuringStmt&>(statement));
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

void TypeChecker::check_destructuring(DestructuringStmt& statement) {
  const Type value_type = check_expression(const_cast<Expr&>(statement.value()));
  const std::vector<std::string>& names = statement.names();

  if (!value_type.is_valid()) {
    // Still declare all names as Error to avoid cascading diagnostics.
    for (const std::string& name : names) {
      symbols_.declare(Symbol{name, Type::error(), statement.binding(), statement.location()});
    }
    return;
  }

  // Tuple destructuring: let (a, b) = tup;
  if (value_type.is_tuple()) {
    const std::vector<Type>& elements = value_type.tuple_elements();
    if (names.size() != elements.size()) {
      diagnostics_.error(diag::kDestructuringMismatch, statement.location(),
                         "tuple destructuring expects " + std::to_string(elements.size()) +
                             " variable(s), but " + std::to_string(names.size()) +
                             " were given");
      // Declare with element types up to available count.
      for (std::size_t i = 0; i < names.size(); ++i) {
        Type t = i < elements.size() ? elements[i] : Type::error();
        symbols_.declare(Symbol{names[i], t, statement.binding(), statement.location()});
      }
      return;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (!symbols_.declare(Symbol{names[i], elements[i], statement.binding(),
                                   statement.location()})) {
        diagnostics_.error(diag::kDuplicateName, statement.location(),
                           "'" + names[i] + "' is already declared in this scope");
      }
    }
    return;
  }

  // Array destructuring: let [a, b] = arr;
  if (value_type.is_array()) {
    const std::size_t arr_len = value_type.array_length();
    if (names.size() != arr_len) {
      diagnostics_.error(diag::kDestructuringMismatch, statement.location(),
                         "array destructuring expects " + std::to_string(arr_len) +
                             " variable(s), but " + std::to_string(names.size()) +
                             " were given");
    }
    const Type elem_type = value_type.element_type();
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (!symbols_.declare(Symbol{names[i], elem_type, statement.binding(),
                                   statement.location()})) {
        diagnostics_.error(diag::kDuplicateName, statement.location(),
                           "'" + names[i] + "' is already declared in this scope");
      }
    }
    return;
  }

  diagnostics_.error(diag::kTypeMismatch, statement.value().location(),
                     "cannot destructure a value of type '" + value_type.to_string() +
                         "'; only tuples and fixed arrays support destructuring");
  for (const std::string& name : names) {
    symbols_.declare(Symbol{name, Type::error(), statement.binding(), statement.location()});
  }
}

static const Symbol* find_root_symbol(const Expr& expr, const SymbolTable& symbols) {
  if (expr.kind() == Expr::Kind::Name) {
    return symbols.lookup(static_cast<const NameExpr&>(expr).name());
  }
  if (expr.kind() == Expr::Kind::Index) {
    return find_root_symbol(static_cast<const IndexExpr&>(expr).base(), symbols);
  }
  if (expr.kind() == Expr::Kind::MemberAccess) {
    return find_root_symbol(static_cast<const MemberAccessExpr&>(expr).base(), symbols);
  }
  return nullptr;
}

void TypeChecker::check_assignment(AssignmentStmt& assignment) {
  const Type value_type = check_expression(const_cast<Expr&>(assignment.value()));
  const Type target_type = check_expression(const_cast<Expr&>(assignment.target()));

  const Symbol* symbol = find_root_symbol(assignment.target(), symbols_);
  if (symbol == nullptr && assignment.is_simple_name()) {
    diagnostics_.error(diag::kUndefinedName, assignment.name_location(),
                       "assignment to undeclared variable '" + assignment.name() + "'");
    return;
  }
  if (symbol != nullptr && !symbol->is_mutable()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "'" + symbol->name + "' was declared as " +
                           std::string(binding_kind_spelling(symbol->binding)) +
                           " and cannot be mutated; declare it with 'var' to mutate it");
    return;
  }
  if (target_type.is_valid() && value_type.is_valid() && target_type != value_type) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot assign a value of type '" + value_type.to_string() +
                           "' to target of type '" + target_type.to_string() + "'");
  }
}

void TypeChecker::check_compound_assignment(CompoundAssignmentStmt& assignment) {
  const Type value_type = check_expression(const_cast<Expr&>(assignment.value()));
  const Type target_type = check_expression(const_cast<Expr&>(assignment.target()));

  const Symbol* symbol = find_root_symbol(assignment.target(), symbols_);
  if (symbol == nullptr && assignment.is_simple_name()) {
    diagnostics_.error(diag::kUndefinedName, assignment.name_location(),
                       "compound assignment to undeclared variable '" + assignment.name() + "'");
    return;
  }
  if (symbol != nullptr && !symbol->is_mutable()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "'" + symbol->name + "' was declared as " +
                           std::string(binding_kind_spelling(symbol->binding)) +
                           " and cannot be mutated; declare it with 'var' to mutate it");
    return;
  }
  if (!target_type.is_valid() || !value_type.is_valid()) return;

  if (target_type != value_type) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot apply compound operator to target of type '" +
                           target_type.to_string() + "' and operand of type '" +
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
    case Expr::Kind::ArrayLiteral:
      type = check_array_literal(static_cast<ArrayLiteralExpr&>(expr));
      break;
    case Expr::Kind::TupleLiteral:
      type = check_tuple_literal(static_cast<TupleLiteralExpr&>(expr));
      break;
    case Expr::Kind::Index:
      type = check_index_expr(static_cast<IndexExpr&>(expr));
      break;
    case Expr::Kind::MemberAccess:
      type = check_member_access(static_cast<MemberAccessExpr&>(expr));
      break;
    case Expr::Kind::StructInit:
      type = check_struct_init(static_cast<StructInitExpr&>(expr));
      break;
  }
  expr.set_type(type);
  return type;
}

const StructInfo* TypeChecker::find_struct(const std::string& name) const {
  // Qualified names are looked up verbatim.
  if (name.find('.') != std::string::npos) {
    auto it = structs_.find(name);
    return it != structs_.end() ? &it->second : nullptr;
  }
  // A bare name first means "the struct in the module I am checking", which is
  // what a file uses for its own declarations. Falling back to the root module
  // keeps single-file programs and the root module's own names working.
  if (!current_module_.empty()) {
    auto local = structs_.find(current_module_ + "." + name);
    if (local != structs_.end()) return &local->second;
  }
  auto it = structs_.find(name);
  return it != structs_.end() ? &it->second : nullptr;
}

const FunctionSignature* TypeChecker::find_function(const std::string& name) const {
  auto it = functions_.find(name);
  if (it != functions_.end()) return &it->second;
  if (name.find('.') == std::string::npos && !current_module_.empty()) {
    auto local = functions_.find(current_module_ + "." + name);
    if (local != functions_.end()) return &local->second;
  }
  return nullptr;
}

Type TypeChecker::check_array_literal(ArrayLiteralExpr& expr) {
  const std::vector<ExprPtr>& elements = expr.elements();
  if (elements.empty()) {
    // Empty array literal: type is inferred from context; return error for now.
    diagnostics_.error(diag::kTypeMismatch, expr.location(),
                       "empty array literal requires an explicit type annotation");
    return Type::error();
  }

  Type elem_type = check_expression(const_cast<Expr&>(*elements[0]));
  bool ok = elem_type.is_valid();
  for (std::size_t i = 1; i < elements.size(); ++i) {
    Type t = check_expression(const_cast<Expr&>(*elements[i]));
    if (ok && t.is_valid() && t != elem_type) {
      diagnostics_.error(diag::kTypeMismatch, elements[i]->location(),
                         "array literal element " + std::to_string(i) +
                             " has type '" + t.to_string() +
                             "', expected '" + elem_type.to_string() + "'");
      ok = false;
    }
  }

  if (!ok) return Type::error();
  return Type::array_type(elem_type, elements.size());
}

Type TypeChecker::check_tuple_literal(TupleLiteralExpr& expr) {
  std::vector<Type> element_types;
  element_types.reserve(expr.elements().size());
  bool ok = true;
  for (const ExprPtr& elem : expr.elements()) {
    Type t = check_expression(const_cast<Expr&>(*elem));
    element_types.push_back(t);
    if (!t.is_valid()) ok = false;
  }
  if (!ok) return Type::error();
  return Type::tuple_type(std::move(element_types));
}

Type TypeChecker::check_index_expr(IndexExpr& expr) {
  const Type base_type = check_expression(const_cast<Expr&>(expr.base()));
  const Type index_type = check_expression(const_cast<Expr&>(expr.index()));

  if (!base_type.is_valid() || !index_type.is_valid()) return Type::error();

  if (!index_type.is_integer()) {
    diagnostics_.error(diag::kTypeMismatch, expr.index().location(),
                       "array index must be an integer, not '" + index_type.to_string() + "'");
    return Type::error();
  }

  if (base_type.is_array() || base_type.is_dynamic_array()) {
    // Constant-index bounds check for fixed arrays.
    if (base_type.is_array() && expr.index().kind() == Expr::Kind::IntegerLiteral) {
      const auto& lit = static_cast<const IntegerLiteralExpr&>(expr.index());
      const long long idx = lit.value();
      if (idx < 0 || static_cast<std::size_t>(idx) >= base_type.array_length()) {
        diagnostics_.error(diag::kIndexOutOfBounds, expr.index().location(),
                           "index " + std::to_string(idx) + " is out of bounds for array of " +
                               "length " + std::to_string(base_type.array_length()));
        return Type::error();
      }
    }
    return base_type.element_type();
  }

  if (base_type.is_string()) {
    return Type::char_type();
  }

  diagnostics_.error(diag::kTypeMismatch, expr.base().location(),
                     "cannot index into value of type '" + base_type.to_string() +
                         "'; only arrays and strings support indexing");
  return Type::error();
}

Type TypeChecker::check_member_access(MemberAccessExpr& expr) {
  const Type base_type = check_expression(const_cast<Expr&>(expr.base()));
  if (!base_type.is_valid()) return Type::error();

  // Tuple element access: tup.0
  if (expr.is_tuple_access()) {
    if (!base_type.is_tuple()) {
      diagnostics_.error(diag::kTypeMismatch, expr.location(),
                         "numeric field access '.0' requires a tuple, not '" +
                             base_type.to_string() + "'");
      return Type::error();
    }
    const int idx = expr.tuple_index();
    const std::vector<Type>& elems = base_type.tuple_elements();
    if (idx < 0 || static_cast<std::size_t>(idx) >= elems.size()) {
      diagnostics_.error(diag::kIndexOutOfBounds, expr.location(),
                         "tuple index " + std::to_string(idx) +
                             " is out of range for a " + std::to_string(elems.size()) +
                             "-element tuple");
      return Type::error();
    }
    return elems[static_cast<std::size_t>(idx)];
  }

  // Struct field access: obj.field
  if (base_type.is_struct()) {
    const std::string& sname = base_type.struct_name();
    const StructInfo* info = find_struct(sname);
    if (info == nullptr) {
      diagnostics_.error(diag::kUndefinedName, expr.location(),
                         "struct '" + sname + "' is not defined");
      return Type::error();
    }
    for (const Parameter& field : info->fields) {
      if (field.name == expr.member()) return field.type;
    }
    diagnostics_.error(diag::kUnknownField, expr.location(),
                       "struct '" + sname + "' has no field '" + expr.member() + "'");
    return Type::error();
  }

  // String .length property
  if (base_type.is_string() && expr.member() == "length") {
    return Type::int_type();
  }

  diagnostics_.error(diag::kTypeMismatch, expr.location(),
                     "value of type '" + base_type.to_string() +
                         "' has no member '" + expr.member() + "'");
  return Type::error();
}

Type TypeChecker::check_struct_init(StructInitExpr& expr) {
  const StructInfo* info = find_struct(expr.struct_name());
  if (info == nullptr) {
    diagnostics_.error(diag::kUndefinedName, expr.location(),
                       "undefined struct '" + expr.struct_name() + "'");
    // Still check sub-expressions.
    if (expr.is_positional()) {
      for (const ExprPtr& e : expr.positional_inits())
        check_expression(const_cast<Expr&>(*e));
    } else {
      for (const auto& kv : expr.field_inits())
        check_expression(const_cast<Expr&>(*kv.second));
    }
    return Type::error();
  }

  const std::vector<Parameter>& fields = info->fields;

  if (expr.is_positional()) {
    if (expr.positional_inits().size() != fields.size()) {
      diagnostics_.error(diag::kDestructuringMismatch, expr.location(),
                         "struct '" + expr.struct_name() + "' has " +
                             std::to_string(fields.size()) + " field(s) but " +
                             std::to_string(expr.positional_inits().size()) +
                             " value(s) were given");
    }
    const std::size_t count = std::min(expr.positional_inits().size(), fields.size());
    for (std::size_t i = 0; i < count; ++i) {
      const Type init_type = check_expression(const_cast<Expr&>(*expr.positional_inits()[i]));
      if (init_type.is_valid() && init_type != fields[i].type) {
        diagnostics_.error(diag::kTypeMismatch, expr.positional_inits()[i]->location(),
                           "field '" + fields[i].name + "' of struct '" +
                               expr.struct_name() + "' has type '" +
                               fields[i].type.to_string() + "', but was given '" +
                               init_type.to_string() + "'");
      }
    }
    for (std::size_t i = count; i < expr.positional_inits().size(); ++i)
      check_expression(const_cast<Expr&>(*expr.positional_inits()[i]));
  } else {
    // Named-field init: validate each provided field name and type.
    for (const auto& kv : expr.field_inits()) {
      const std::string& field_name = kv.first;
      const Type init_type = check_expression(const_cast<Expr&>(*kv.second));
      // Find the field in the struct definition.
      const Parameter* field_def = nullptr;
      for (const Parameter& f : fields) {
        if (f.name == field_name) { field_def = &f; break; }
      }
      if (field_def == nullptr) {
        diagnostics_.error(diag::kUnknownField, kv.second->location(),
                           "struct '" + expr.struct_name() + "' has no field '" +
                               field_name + "'");
      } else if (init_type.is_valid() && init_type != field_def->type) {
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "field '" + field_name + "' of struct '" + expr.struct_name() +
                               "' has type '" + field_def->type.to_string() +
                               "', but was given '" + init_type.to_string() + "'");
      }
    }
  }

  return Type::struct_type(expr.struct_name());
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

  // Builtin 'input'
  if (call.callee() == "input") {
    if (call.arguments().size() > 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'input' takes at most one argument, but " +
                             std::to_string(call.arguments().size()) + " were given");
    }
    if (call.arguments().size() == 1) {
      const Type prompt_type = check_expression(const_cast<Expr&>(*call.arguments().front()));
      if (prompt_type.is_valid() && !prompt_type.is_string()) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments().front()->location(),
                           "'input' prompt must be a 'string', not '" + prompt_type.to_string() + "'");
      }
    }
    return Type::string_type();
  }

  // Builtin 'length'
  if (call.callee() == "length") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'length' takes exactly one argument, but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::int_type();
    }
    const Type arg_type = check_expression(const_cast<Expr&>(*call.arguments().front()));
    if (arg_type.is_valid() && !arg_type.is_string() && !arg_type.is_array() && !arg_type.is_dynamic_array()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments().front()->location(),
                         "'length' requires a string or array argument, not '" + arg_type.to_string() + "'");
    }
    return Type::int_type();
  }

  // Math builtins: sqrt, sin, cos, abs, floor, ceil (and math.<name>)
  std::string fn_name = call.callee();
  if (fn_name.rfind("math.", 0) == 0) fn_name = fn_name.substr(5);
  if (fn_name == "sqrt" || fn_name == "sin" || fn_name == "cos" ||
      fn_name == "abs" || fn_name == "floor" || fn_name == "ceil") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "math function '" + call.callee() + "' takes exactly one argument, but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::f64_type();
    }
    const Type arg_type = check_expression(const_cast<Expr&>(*call.arguments().front()));
    if (arg_type.is_valid() && !arg_type.is_numeric()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments().front()->location(),
                         "math function '" + call.callee() + "' requires a numeric argument, not '" +
                             arg_type.to_string() + "'");
    }
    return Type::f64_type();
  }

  // File I/O builtins: file.read_text, file.write_text, file.append_text, file.exists
  if (call.callee() == "file.read_text") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'file.read_text' expects 1 argument (path: string), but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::string_type();
    }
    const Type path_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (path_type.is_valid() && !path_type.is_string()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'file.read_text' path must be a 'string', not '" + path_type.to_string() + "'");
    }
    return Type::string_type();
  }

  if (call.callee() == "file.write_text" || call.callee() == "file.append_text") {
    if (call.arguments().size() != 2) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' expects 2 arguments (path: string, content: string), but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::int_type();
    }
    const Type path_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Type content_type = check_expression(const_cast<Expr&>(*call.arguments()[1]));
    if (path_type.is_valid() && !path_type.is_string()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "path must be a 'string', not '" + path_type.to_string() + "'");
    }
    if (content_type.is_valid() && !content_type.is_string()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                         "content must be a 'string', not '" + content_type.to_string() + "'");
    }
    return Type::int_type();
  }

  if (call.callee() == "file.exists") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'file.exists' expects 1 argument (path: string), but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::bool_type();
    }
    const Type path_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (path_type.is_valid() && !path_type.is_string()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'file.exists' path must be a 'string', not '" + path_type.to_string() + "'");
    }
    return Type::bool_type();
  }

  // Time & System builtins: time.now, system.exit
  if (call.callee() == "time.now") {
    if (!call.arguments().empty()) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'time.now' takes no arguments");
    }
    return Type::int_type();
  }

  if (call.callee() == "system.exit") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'system.exit' expects 1 argument (code: int), but " +
                             std::to_string(call.arguments().size()) + " were given");
      return Type::void_type();
    }
    const Type code_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (code_type.is_valid() && !code_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'system.exit' exit code must be an integer, not '" + code_type.to_string() + "'");
    }
    return Type::void_type();
  }

  // User-defined function call
  const FunctionSignature* resolved = find_function(call.callee());
  if (resolved == nullptr) {
    diagnostics_.error(diag::kUndefinedName, call.location(),
                       "call to undeclared function '" + call.callee() + "'");
    for (const ExprPtr& argument : call.arguments()) {
      check_expression(const_cast<Expr&>(*argument));
    }
    return Type::error();
  }

  const FunctionSignature& sig = *resolved;
  if (!current_module_.empty() && !sig.module_name.empty() &&
      current_module_ != sig.module_name && sig.visibility == Visibility::Private) {
    diagnostics_.error(diag::kPrivateAccessViolation, call.location(),
                       "cannot access private function '" + sig.name +
                           "' from outside module '" + sig.module_name + "'");
  }
  // Record the link-time symbol so TIR lowering does not have to repeat the
  // module-aware name resolution.
  call.set_resolved_symbol(sig.symbol);
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
