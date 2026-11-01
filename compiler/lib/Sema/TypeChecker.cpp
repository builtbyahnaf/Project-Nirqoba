#include "nirqoba/Sema/TypeChecker.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

#include "nirqoba/Driver/ModuleResolver.h"

namespace nirq {
using namespace std;
namespace {

constexpr const char* kPrintBuiltin = "print";

static bool contains_owned_array(const Type& type,
                                 unordered_set<string>& active_structs) {
  if (type.is_owned_array()) return true;
  if (type.is_array() || type.is_tuple()) {
    if (type.is_array()) return contains_owned_array(type.element_type(), active_structs);
    for (const Type& element : type.tuple_elements())
      if (contains_owned_array(element, active_structs)) return true;
  }
  if (type.is_struct() && !type.struct_fields().empty() &&
      active_structs.insert(type.struct_name()).second) {
    for (const auto& field : type.struct_fields())
      if (contains_owned_array(field.second, active_structs)) {
        active_structs.erase(type.struct_name());
        return true;
      }
    active_structs.erase(type.struct_name());
  }
  return false;
}

static bool contains_owned_array(const Type& type) {
  unordered_set<string> active_structs;
  return contains_owned_array(type, active_structs);
}

static bool contains_nested_owned_array(const Type& type,
                                        unordered_set<string>& active_structs) {
  if (type.is_owned_array()) return contains_owned_array(type.element_type());
  if (type.is_array()) return contains_nested_owned_array(type.element_type(), active_structs);
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_nested_owned_array(element, active_structs)) return true;
  }
  if (type.is_struct() && !type.struct_fields().empty() &&
      active_structs.insert(type.struct_name()).second) {
    for (const auto& field : type.struct_fields())
      if (contains_nested_owned_array(field.second, active_structs)) {
        active_structs.erase(type.struct_name());
        return true;
      }
    active_structs.erase(type.struct_name());
  }
  return false;
}

static bool contains_nested_owned_array(const Type& type) {
  unordered_set<string> active_structs;
  return contains_nested_owned_array(type, active_structs);
}

static bool contains_array_view(const Type& type,
                                unordered_set<string>& active_structs) {
  if (type.is_array_view()) return true;
  if (type.is_owned_array()) return contains_array_view(type.element_type(), active_structs);
  if (type.is_array()) return contains_array_view(type.element_type(), active_structs);
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_array_view(element, active_structs)) return true;
  }
  if (type.is_struct() && !type.struct_fields().empty() &&
      active_structs.insert(type.struct_name()).second) {
    for (const auto& field : type.struct_fields())
      if (contains_array_view(field.second, active_structs)) {
        active_structs.erase(type.struct_name());
        return true;
      }
    active_structs.erase(type.struct_name());
  }
  return false;
}

static bool contains_array_view(const Type& type) {
  unordered_set<string> active_structs;
  return contains_array_view(type, active_structs);
}

static string cleanup_for_type(const Type& type) {
  if (type.is_owned_array()) return "array.owned_release";
  return contains_owned_array(type) ? "aggregate.owned_drop" : string{};
}

static bool has_untracked_pointer_ownership(const Type& type,
                                            ValueOwnership ownership) {
  if (!type.is_pointer()) return false;
  return ownership == ValueOwnership::Owned ||
         ownership == ValueOwnership::Shared ||
         ownership == ValueOwnership::Borrowed ||
         ownership == ValueOwnership::MutableBorrowed;
}

static bool statement_can_fall_through(const Stmt& stmt);

static bool expression_creates_owned_pointer(const Expr& expr) {
  if (expr.kind() != Expr::Kind::Call) return false;
  const std::string& callee = static_cast<const CallExpr&>(expr).callee();
  return callee == "file.open" || callee == "memory.allocate" ||
         callee == "memory.secure_allocate" || callee == "memory.reallocate" ||
         callee == "memory.map_file" || callee == "memory.shared_allocate" ||
         callee == "memory.shared_retain" || callee == "mutex.create" ||
         callee == "memory.owned_array" ||
         callee == "memory.owned_array_aligned";
}

static std::string cleanup_for_owner_producer(const std::string& callee,
                                             bool shared_return = false) {
  if (shared_return || callee == "memory.shared_allocate" ||
      callee == "memory.shared_retain") return "memory.shared_release";
  if (callee == "file.open") return "file.close";
  if (callee == "memory.map_file") return "memory.unmap_file";
  if (callee == "memory.secure_allocate") return "memory.secure_release";
  if (callee == "memory.allocate" || callee == "memory.reallocate")
    return "memory.deallocate";
  if (callee == "mutex.create") return "mutex.destroy";
  if (callee == "memory.owned_array" ||
      callee == "memory.owned_array_aligned") return "array.owned_release";
  return {};
}

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

static const Symbol* find_root_symbol(const Expr& expr, const SymbolTable& symbols);

bool TypeChecker::check(Program& program) {
  functions_.clear();
  structs_.clear();
  current_module_ = program.module_name();
  const FunctionDecl* main_function = nullptr;
  int main_count = 0;

  // Pass 0: Register struct declarations.
  for (const auto& decl : program.structs()) {
    // Validate unique field names.
    unordered_set<string> seen_fields;
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
    if (!structs_.emplace(decl->name(), move(info)).second) {
      diagnostics_.error(diag::kDuplicateName, decl->location(),
                         "struct '" + decl->name() + "' is already defined");
    }
  }

  for (const auto& decl : program.structs()) {
    for (const Parameter& field : decl->fields()) {
      const Type field_type = resolve_aggregate_type(field.type);
      if (contains_nested_owned_array(field_type))
        diagnostics_.error(diag::kTypeMismatch, field.location,
                           "owned arrays cannot contain elements that own nested allocations yet");
      if (contains_array_view(field_type))
        diagnostics_.error(diag::kTypeMismatch, field.location,
                           "borrowed array views cannot be stored in aggregates");
    }
  }

  // Pass 1: Collect function signatures for forward references & mutual recursion.
  for (const auto& function : program.functions()) {
    resolve_declaration_types(*function);
    if (contains_nested_owned_array(function->return_type()))
      diagnostics_.error(diag::kTypeMismatch, function->location(),
                         "owned arrays cannot contain elements that own nested allocations yet");
    if (contains_array_view(function->return_type()))
      diagnostics_.error(diag::kTypeMismatch, function->location(),
                         "borrowed array views cannot escape through a return value");
    vector<Type> param_types;
    param_types.reserve(function->parameters().size());
    for (const Parameter& param : function->parameters()) {
      if (contains_nested_owned_array(param.type))
        diagnostics_.error(diag::kTypeMismatch, param.location,
                           "owned arrays cannot contain elements that own nested allocations yet");
      if (contains_array_view(param.type) && !param.type.is_array_view())
        diagnostics_.error(diag::kTypeMismatch, param.location,
                           "borrowed array views cannot be nested in aggregate parameters");
      param_types.push_back(param.type);
    }

    FunctionSignature sig{
        function->name(),
        function->return_type(),
        move(param_types),
        function->location(),
        current_module_,
        function->visibility(),
        function->name()  // single-module compilation links symbols by name
    };
    sig.returns_shared = function->returns_shared();
    for (const Parameter& param : function->parameters()) {
      sig.parameter_borrowed.push_back(param.borrowed);
      sig.parameter_mutable_borrow.push_back(param.mutable_borrow);
      sig.parameter_shared.push_back(param.shared);
    }

    if (!functions_.emplace(function->name(), move(sig)).second) {
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

bool TypeChecker::check_modules(const vector<unique_ptr<ModuleUnit>>& modules) {
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

  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    current_module_ = unit->name;
    for (const auto& decl : unit->ast->structs()) {
      for (const Parameter& field : decl->fields()) {
        const Type field_type = resolve_aggregate_type(field.type);
        if (contains_nested_owned_array(field_type))
          diagnostics_.error(diag::kTypeMismatch, field.location,
                             "owned arrays cannot contain elements that own nested allocations yet");
        if (contains_array_view(field_type))
          diagnostics_.error(diag::kTypeMismatch, field.location,
                             "borrowed array views cannot be stored in aggregates");
      }
    }
  }

  // Pass 1: register function signatures. Symbols are `module.name` except in
  // the root module, whose functions keep bare names so the entry point is
  // emitted as `main`.
  for (const auto& unit : modules) {
    if (!unit || !unit->ast) continue;
    current_module_ = unit->name;
    for (const auto& function : unit->ast->functions()) {
      resolve_declaration_types(*function);
      if (contains_nested_owned_array(function->return_type()))
        diagnostics_.error(diag::kTypeMismatch, function->location(),
                           "owned arrays cannot contain elements that own nested allocations yet");
      if (contains_array_view(function->return_type()))
        diagnostics_.error(diag::kTypeMismatch, function->location(),
                           "borrowed array views cannot escape through a return value");
      vector<Type> param_types;
      param_types.reserve(function->parameters().size());
      for (const Parameter& param : function->parameters()) {
        if (contains_nested_owned_array(param.type))
          diagnostics_.error(diag::kTypeMismatch, param.location,
                             "owned arrays cannot contain elements that own nested allocations yet");
        if (contains_array_view(param.type) && !param.type.is_array_view())
          diagnostics_.error(diag::kTypeMismatch, param.location,
                             "borrowed array views cannot be nested in aggregate parameters");
        param_types.push_back(param.type);
      }

      const bool is_entry = (unit.get() == entry_unit);
      const string qualified = unit->name + "." + function->name();

      FunctionSignature sig{
          function->name(),
          function->return_type(),
          move(param_types),
          function->location(),
          unit->name,
          function->visibility(),
          is_entry ? function->name() : qualified
      };
      sig.returns_shared = function->returns_shared();
      for (const Parameter& param : function->parameters()) {
        sig.parameter_borrowed.push_back(param.borrowed);
        sig.parameter_mutable_borrow.push_back(param.mutable_borrow);
        sig.parameter_shared.push_back(param.shared);
      }

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
      const string module = imp->module_name();

      if (imp->is_selective()) {
        const string qualified = module + "." + imp->item_name();
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
        vector<FunctionSignature> module_functions;
        for (const auto& kv : functions_) {
          if (kv.second.module_name == module && kv.second.visibility == Visibility::Public) {
            module_functions.push_back(kv.second);
          }
        }
        for (const FunctionSignature& sig : module_functions) {
          functions_.emplace(imp->alias() + "." + sig.name, sig);
        }

        vector<StructInfo> module_structs;
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
  current_return_shared_ = function.returns_shared();
  if (current_return_shared_ && !current_return_type_.is_pointer()) {
    diagnostics_.error(diag::kTypeMismatch, function.location(),
                       "shared return types must be pointers");
  }
  loop_depth_ = 0;

  for (const Parameter& parameter : function.parameters()) {
    // Parameters are assignable inside the body. Nothing in the specification
    // makes them `let`, and there is no syntax to opt into mutability; treating
    // them as immutable would make it impossible to modify an array or struct
    // passed to a helper function at all.
    if (parameter.borrowed && !parameter.type.is_pointer()) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "borrow parameters must have pointer type");
    }
    if (parameter.mutable_borrow && !parameter.borrowed) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "'mut' requires a borrowed parameter");
    }
    if (parameter.shared && (parameter.borrowed || !parameter.type.is_pointer())) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "shared parameters must have pointer type and cannot be borrowed");
    }
    Symbol symbol{parameter.name, parameter.type,
                  parameter.borrowed && !parameter.mutable_borrow
                      ? BindingKind::Let : BindingKind::Var,
                  parameter.location};
    if (parameter.type.is_pointer() || parameter.type.is_owned_array() ||
        contains_owned_array(parameter.type) || parameter.type.is_array_view()) {
      symbol.allocation_id = next_allocation_id_++;
      symbol.borrowed = parameter.borrowed || parameter.type.is_array_view();
      symbol.mutable_borrow = parameter.mutable_borrow;
      symbol.owns_allocation = !symbol.borrowed;
      symbol.shared_allocation = parameter.shared;
    }
    if (!symbols_.declare(move(symbol))) {
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
  current_return_shared_ = false;
}

void TypeChecker::check_block(BlockStmt& block) {
  symbols_.push_scope();
  deferred_calls_.emplace_back();
  for (StmtPtr& statement : block.statements()) {
    check_statement(*statement);
  }
  for (auto cleanup = deferred_calls_.back().rbegin();
       cleanup != deferred_calls_.back().rend(); ++cleanup) {
    const CallExpr* call = cleanup->call;
    const string& callee = call->callee();
    if ((callee == "memory.deallocate" || callee == "memory.shared_release" || callee == "array.owned_release" || callee == "file.close" || callee == "mutex.destroy" || callee == "memory.secure_release" ||
         callee == "memory.unmap_file") && cleanup->allocation_id != 0) {
      if (symbols_.allocation_is_moved(cleanup->allocation_id)) {
          diagnostics_.error(diag::kUseAfterFree, call->location(),
                             "deferred cleanup releases an allocation that was already released");
      } else if (callee == "array.owned_release" &&
                 symbols_.has_live_borrow(cleanup->allocation_id)) {
        diagnostics_.error(diag::kTypeMismatch, call->location(),
                           "deferred array release would invalidate a live array view");
      } else {
        symbols_.mark_allocation_moved(cleanup->allocation_id);
      }
    }
  }
  deferred_calls_.pop_back();
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
    case Stmt::Kind::DeferredCleanup:
      check_deferred_cleanup(static_cast<DeferredCleanupStmt&>(statement));
      return;
    case Stmt::Kind::Expression:
      check_expression(const_cast<Expr&>(static_cast<ExprStmt&>(statement).expr()));
      return;
  }
}

void TypeChecker::check_deferred_cleanup(DeferredCleanupStmt& statement) {
  Expr& expression = const_cast<Expr&>(statement.expression());
  if (expression.kind() != Expr::Kind::Call) {
    diagnostics_.error(diag::kTypeMismatch, expression.location(),
                       "defer requires a function call expression");
    check_expression(expression);
    return;
  }
  checking_deferred_cleanup_ = true;
  check_expression(expression);
  checking_deferred_cleanup_ = false;
  const auto* call = static_cast<const CallExpr*>(&expression);
  uint64_t allocation_id = 0;
  if ((call->callee() == "memory.deallocate" || call->callee() == "memory.shared_release" || call->callee() == "array.owned_release" || call->callee() == "file.close" || call->callee() == "mutex.destroy" ||
       call->callee() == "memory.secure_release" ||
       call->callee() == "memory.unmap_file") && !call->arguments().empty()) {
    const Symbol* pointer = find_root_symbol(*call->arguments()[0], symbols_);
    if (pointer && (pointer->type.is_pointer() || pointer->type.is_owned_array()))
      allocation_id = pointer->allocation_id;
  }
  deferred_calls_.back().push_back(DeferredCleanup{call, allocation_id});
}

void TypeChecker::check_variable_declaration(VariableDeclarationStmt& declaration) {
  const Type initializer_type =
      check_expression(const_cast<Expr&>(declaration.initializer()));
  Type binding_type = initializer_type;

  if (declaration.has_annotation()) {
    const Type annotation = resolve_aggregate_type(declaration.annotation());
    declaration.set_annotation(annotation);
    if (annotation.is_valid() && initializer_type.is_valid() &&
        annotation != initializer_type) {
      diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                         "cannot initialize '" + declaration.name() + "' of type '" +
                             annotation.to_string() + "' with a value of type '" +
                             initializer_type.to_string() + "'");
    }
    binding_type = annotation;
  }

  Symbol symbol{declaration.name(), binding_type, declaration.binding(),
                declaration.location()};
  if ((binding_type.is_owned_array() || contains_owned_array(binding_type)) &&
      (declaration.initializer().kind() == Expr::Kind::MemberAccess ||
       declaration.initializer().kind() == Expr::Kind::Index)) {
    diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                       "owned fields cannot be moved out of an aggregate individually");
  }
  const Symbol* transferred_owner = nullptr;
  if (binding_type.is_pointer() || binding_type.is_owned_array() ||
      contains_owned_array(binding_type)) {
    symbol.owns_allocation = expression_creates_owned_pointer(declaration.initializer());
    if (contains_owned_array(binding_type) &&
        declaration.initializer().ownership() == ValueOwnership::Owned) {
      symbol.owns_allocation = true;
      symbol.cleanup_callee = cleanup_for_type(binding_type);
    }
    if (declaration.initializer().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(declaration.initializer());
      symbol.shared_allocation = call.callee() == "memory.shared_allocate" ||
                                 call.callee() == "memory.shared_retain";
      if (const FunctionSignature* signature = find_function(call.callee())) {
        symbol.shared_allocation = symbol.shared_allocation || signature->returns_shared;
        if (signature->return_type.is_pointer() || signature->return_type.is_owned_array() ||
            contains_owned_array(signature->return_type)) {
          symbol.cleanup_callee = cleanup_for_owner_producer(
              call.callee(), signature->returns_shared);
          if (symbol.cleanup_callee.empty())
            symbol.cleanup_callee = cleanup_for_type(signature->return_type);
        }
      }
      if (symbol.cleanup_callee.empty())
        symbol.cleanup_callee = cleanup_for_owner_producer(call.callee());
    }
    if (!symbol.owns_allocation && declaration.initializer().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(declaration.initializer());
      const FunctionSignature* signature = find_function(call.callee());
      symbol.owns_allocation = signature &&
          (signature->return_type.is_pointer() || signature->return_type.is_owned_array() ||
           contains_owned_array(signature->return_type));
      if (symbol.owns_allocation && symbol.cleanup_callee.empty() &&
          !signature->returns_shared)
        symbol.cleanup_callee = cleanup_for_type(signature->return_type);
        if (symbol.cleanup_callee.empty()) symbol.cleanup_callee = "memory.deallocate";
    }
    if (declaration.initializer().kind() == Expr::Kind::Name) {
      const auto& source_name = static_cast<const NameExpr&>(declaration.initializer());
      const Symbol* source = symbols_.lookup(source_name.name());
      if (source && (source->type.is_pointer() || source->type.is_owned_array() ||
                     contains_owned_array(source->type))) {
        symbol.allocation_id = source->allocation_id;
        if (source->borrowed) {
          symbol.borrowed = true;
          symbol.mutable_borrow = source->mutable_borrow;
        }
        symbol.shared_allocation = source->shared_allocation;
        symbol.cleanup_callee = source->cleanup_callee;
        if (source->owns_allocation && !source->moved) {
          if (symbols_.has_live_borrow(source->allocation_id)) {
            diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                               "cannot move an owner while an array view is live");
          } else {
            symbol.owns_allocation = true;
            transferred_owner = source;
            const_cast<Expr&>(declaration.initializer()).set_transfers_ownership();
          }
        }
      }
    }
    if (symbol.allocation_id == 0) symbol.allocation_id = next_allocation_id_++;
  } else if (binding_type.is_array_view()) {
    symbol.borrowed = true;
    if (declaration.initializer().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(declaration.initializer());
      if (call.callee() == "memory.array_view" && !call.arguments().empty()) {
        if (const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_))
          symbol.allocation_id = source->allocation_id;
      }
    } else {
      const Symbol* source = find_root_symbol(declaration.initializer(), symbols_);
      if (source && source->type.is_array_view() && source->borrowed && !source->moved)
        symbol.allocation_id = source->allocation_id;
    }
    if (symbol.allocation_id == 0) {
      diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                         "array views require a tracked owner or borrowed source");
    }
  }
  const uint64_t allocation_id = symbol.allocation_id;
  const string new_name = declaration.name();
  if (!symbols_.declare(move(symbol))) {
    diagnostics_.error(diag::kDuplicateName, declaration.location(),
                       "'" + declaration.name() + "' is already declared in this scope");
  } else if (transferred_owner) {
    symbols_.mark_allocation_moved(allocation_id);
    symbols_.rebind_pointer(new_name, allocation_id);
  }
  if (const Symbol* declared = symbols_.lookup(new_name))
    declaration.set_cleanup_callee(declared->cleanup_callee);
}

void TypeChecker::check_destructuring(DestructuringStmt& statement) {
  const Type value_type = check_expression(const_cast<Expr&>(statement.value()));
  const vector<string>& names = statement.names();

  if (!value_type.is_valid()) {
    // Still declare all names as Error to avoid cascading diagnostics.
    for (const string& name : names) {
      symbols_.declare(Symbol{name, Type::error(), statement.binding(), statement.location()});
    }
    return;
  }

  if (contains_owned_array(value_type)) {
    diagnostics_.error(diag::kTypeMismatch, statement.value().location(),
                       "resource-bearing aggregates cannot be destructured; move the whole value");
    for (const string& name : names) {
      symbols_.declare(Symbol{name, Type::error(), statement.binding(), statement.location()});
    }
    return;
  }
  if (contains_array_view(value_type)) {
    diagnostics_.error(diag::kTypeMismatch, statement.value().location(),
                       "borrowed array views cannot escape their tracked binding through destructuring");
    for (const string& name : names)
      symbols_.declare(Symbol{name, Type::error(), statement.binding(), statement.location()});
    return;
  }

  // Tuple destructuring: let (a, b) = tup;
  if (value_type.is_tuple()) {
    const vector<Type>& elements = value_type.tuple_elements();
    if (names.size() != elements.size()) {
      diagnostics_.error(diag::kDestructuringMismatch, statement.location(),
                         "tuple destructuring expects " + to_string(elements.size()) +
                             " variable(s), but " + to_string(names.size()) +
                             " were given");
      // Declare with element types up to available count.
      for (size_t i = 0; i < names.size(); ++i) {
        Type t = i < elements.size() ? elements[i] : Type::error();
        symbols_.declare(Symbol{names[i], t, statement.binding(), statement.location()});
      }
      return;
    }
    for (size_t i = 0; i < names.size(); ++i) {
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
    const size_t arr_len = value_type.array_length();
    if (names.size() != arr_len) {
      diagnostics_.error(diag::kDestructuringMismatch, statement.location(),
                         "array destructuring expects " + to_string(arr_len) +
                             " variable(s), but " + to_string(names.size()) +
                             " were given");
    }
    const Type elem_type = value_type.element_type();
    for (size_t i = 0; i < names.size(); ++i) {
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
  for (const string& name : names) {
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
  const Type target_type = assignment.is_simple_name()
      ? (symbols_.lookup(assignment.name())
             ? symbols_.lookup(assignment.name())->type : Type::error())
      : check_expression(const_cast<Expr&>(assignment.target()));

  if ((assignment.target().kind() == Expr::Kind::MemberAccess ||
       assignment.target().kind() == Expr::Kind::Index) &&
      (target_type.is_owned_array() || contains_owned_array(target_type))) {
    diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                       "owned fields cannot be replaced individually; move the whole aggregate");
    return;
  }
  if ((assignment.target().kind() == Expr::Kind::MemberAccess ||
       assignment.target().kind() == Expr::Kind::Index) &&
      has_untracked_pointer_ownership(target_type, assignment.value().ownership())) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "owned, shared, and borrowed pointers cannot be stored in aggregate fields");
    return;
  }
  if ((assignment.target().kind() == Expr::Kind::MemberAccess ||
       assignment.target().kind() == Expr::Kind::Index) &&
      contains_array_view(target_type)) {
    diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                       "borrowed array views cannot be stored in aggregate or allocated memory");
    return;
  }

  const Symbol* symbol = find_root_symbol(assignment.target(), symbols_);
  if (assignment.target().kind() == Expr::Kind::Index &&
      static_cast<const IndexExpr&>(assignment.target()).base().type().is_array_view()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "array views are read-only; mutate through the owning array");
    return;
  }
  if (symbol && symbol->owns_allocation &&
      symbols_.has_live_borrow(symbol->allocation_id)) {
    diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                       "cannot mutate an owner while an array view is live");
    return;
  }
  if (symbol == nullptr && assignment.is_simple_name()) {
    diagnostics_.error(diag::kUndefinedName, assignment.name_location(),
                       "assignment to undeclared variable '" + assignment.name() + "'");
    return;
  }
  if (symbol != nullptr && !symbol->is_mutable()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "'" + symbol->name + "' was declared as " +
                           string(binding_kind_spelling(symbol->binding)) +
                           " and cannot be mutated; declare it with 'var' to mutate it");
    return;
  }
  if (symbol != nullptr && symbol->borrowed && assignment.is_simple_name()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "a borrowed binding cannot be rebound");
    return;
  }
  if (target_type.is_valid() && value_type.is_valid() && target_type != value_type) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot assign a value of type '" + value_type.to_string() +
                           "' to target of type '" + target_type.to_string() + "'");
  }
  if (assignment.is_simple_name() && symbol && symbol->is_mutable() &&
      (target_type.is_pointer() || target_type.is_owned_array() ||
       contains_owned_array(target_type))) {
    uint64_t allocation_id = 0;
    const Symbol* source = nullptr;
    if (assignment.value().kind() == Expr::Kind::Name) {
      const auto& source_name = static_cast<const NameExpr&>(assignment.value());
      source = symbols_.lookup(source_name.name());
      if (source && !source->moved &&
          (source->type.is_pointer() || source->type.is_owned_array() ||
           contains_owned_array(source->type)))
        allocation_id = source->allocation_id;
    }
    if (symbol->owns_allocation && !symbol->moved &&
        allocation_id != 0 && allocation_id != symbol->allocation_id) {
      diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                         "cannot overwrite a live owned pointer; release it first");
      return;
    }
    if (allocation_id == 0) allocation_id = next_allocation_id_++;
    const bool borrowed = source && source->borrowed;
    const bool mutable_borrow = source && source->mutable_borrow;
    bool shared_allocation = source && source->shared_allocation;
    std::string cleanup_callee = source ? source->cleanup_callee : std::string{};
    bool owns_allocation = source && source->owns_allocation && !source->moved;
    if (assignment.value().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(assignment.value());
      const FunctionSignature* signature = find_function(call.callee());
      owns_allocation = expression_creates_owned_pointer(assignment.value()) ||
                        (signature && (signature->return_type.is_pointer() ||
                                       signature->return_type.is_owned_array() ||
                                       contains_owned_array(signature->return_type)));
      shared_allocation = call.callee() == "memory.shared_allocate" ||
                          call.callee() == "memory.shared_retain" ||
                          (signature && signature->returns_shared);
      if (owns_allocation)
        cleanup_callee = cleanup_for_owner_producer(
            call.callee(), signature && signature->returns_shared);
      if (owns_allocation && cleanup_callee.empty())
        cleanup_callee = signature
                             ? cleanup_for_type(signature->return_type)
                             : std::string{};
      if (owns_allocation && cleanup_callee.empty())
        cleanup_callee = contains_owned_array(target_type)
                             ? "aggregate.owned_drop"
                             : target_type.is_owned_array() ? "array.owned_release"
                                                            : "memory.deallocate";
    } else if (contains_owned_array(target_type) &&
               assignment.value().ownership() == ValueOwnership::Owned) {
      owns_allocation = true;
      cleanup_callee = cleanup_for_type(target_type);
    } else if (source && source->owns_allocation && !source->moved) {
      if (symbols_.has_live_borrow(source->allocation_id)) {
        diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                           "cannot move an owner while an array view is live");
      } else {
        const_cast<Expr&>(assignment.value()).set_transfers_ownership();
      }
    }
    if (!symbol->cleanup_callee.empty() &&
        (cleanup_callee.empty() || cleanup_callee != symbol->cleanup_callee)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                         "an owned binding cannot be rebound to a pointer with a different ownership domain");
      return;
    }
    if (owns_allocation) symbols_.mark_allocation_moved(allocation_id);
    symbols_.rebind_pointer(assignment.name(), allocation_id);
    symbols_.set_pointer_ownership(assignment.name(), owns_allocation, borrowed,
                                   mutable_borrow, shared_allocation, cleanup_callee);
    assignment.set_cleanup_callee(cleanup_callee);
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
                           string(binding_kind_spelling(symbol->binding)) +
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
                           "operator '" + string(binary_op_spelling(assignment.op())) +
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
                           "operator '" + string(binary_op_spelling(assignment.op())) +
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
  const SymbolTable incoming = symbols_;
  check_block(const_cast<BlockStmt&>(statement.then_branch()));
  const SymbolTable then_state = symbols_;
  symbols_ = incoming;
  if (statement.has_else()) {
    check_statement(const_cast<Stmt&>(*statement.else_branch()));
  }
  const SymbolTable else_state = symbols_;
  const bool then_falls_through = block_falls_through(statement.then_branch());
  const bool else_falls_through = statement.has_else()
      ? statement_can_fall_through(*statement.else_branch()) : true;
  if (then_falls_through && else_falls_through) {
    symbols_ = incoming;
    symbols_.merge_conditional_ownership(then_state, else_state);
  } else if (then_falls_through) {
    symbols_ = then_state;
  } else if (else_falls_through) {
    symbols_ = else_state;
  } else {
    symbols_ = incoming;
  }
}

void TypeChecker::check_while(WhileStmt& statement) {
  const SymbolTable before_condition = symbols_;
  const Type cond_type = check_expression(const_cast<Expr&>(statement.condition()));
  if (cond_type.is_valid() && !cond_type.is_bool()) {
    diagnostics_.error(diag::kTypeMismatch, statement.condition().location(),
                       "'while' condition must be 'bool', not '" + cond_type.to_string() + "'");
  }
  const SymbolTable condition_state = symbols_;
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
  const SymbolTable body_state = symbols_;
  const bool body_falls_through = block_falls_through(statement.body());
  if (body_falls_through) {
    for (const string& name : condition_state.owners_moved_since(before_condition))
      diagnostics_.error(diag::kTypeMismatch, statement.condition().location(),
                         "owner '" + name +
                             "' cannot be consumed by a loop condition that can repeat");
    for (const string& name : body_state.owners_moved_since(condition_state))
      diagnostics_.error(diag::kTypeMismatch, statement.body().location(),
                       "owner '" + name +
                           "' cannot remain moved at a loop back-edge; reinitialize it before the next iteration");
  }
  symbols_ = condition_state;
  if (body_falls_through)
    symbols_.merge_conditional_ownership(body_state, condition_state);
}

void TypeChecker::check_for(ForStmt& statement) {
  symbols_.push_scope();
  if (statement.has_initializer()) {
    check_statement(const_cast<Stmt&>(*statement.initializer()));
  }
  const SymbolTable before_condition = symbols_;
  if (statement.has_condition()) {
    const Type cond_type = check_expression(const_cast<Expr&>(*statement.condition()));
    if (cond_type.is_valid() && !cond_type.is_bool()) {
      diagnostics_.error(diag::kTypeMismatch, statement.condition()->location(),
                         "'for' condition must be 'bool', not '" + cond_type.to_string() + "'");
    }
  }
  const SymbolTable condition_state = symbols_;
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
  const SymbolTable body_state = symbols_;
  const bool body_falls_through = block_falls_through(statement.body());
  if (statement.has_step()) {
    if (!body_falls_through) symbols_ = condition_state;
    check_statement(const_cast<Stmt&>(*statement.step()));
  }
  const SymbolTable loop_back_state = symbols_;
  const bool step_falls_through = !statement.has_step() ||
      statement_can_fall_through(*statement.step());
  if (body_falls_through && step_falls_through) {
    for (const string& name : condition_state.owners_moved_since(before_condition))
      diagnostics_.error(diag::kTypeMismatch, statement.condition()->location(),
                         "owner '" + name +
                             "' cannot be consumed by a loop condition that can repeat");
    for (const string& name : loop_back_state.owners_moved_since(condition_state))
      diagnostics_.error(diag::kTypeMismatch, statement.location(),
                         "owner '" + name +
                             "' cannot remain moved at a loop back-edge; reinitialize it before the next iteration");
    symbols_ = condition_state;
    symbols_.merge_conditional_ownership(loop_back_state, condition_state);
  } else {
    symbols_ = condition_state;
  }
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
  if (contains_array_view(value_type)) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "borrowed array views cannot escape through a return value");
  }
  if (current_return_type_.is_pointer() && !current_return_shared_) {
    std::string cleanup_callee;
    if (statement.value()->kind() == Expr::Kind::Name) {
      const Symbol* source = find_root_symbol(*statement.value(), symbols_);
      if (source) cleanup_callee = source->cleanup_callee;
    } else if (statement.value()->kind() == Expr::Kind::Call) {
      cleanup_callee = cleanup_for_owner_producer(
          static_cast<const CallExpr&>(*statement.value()).callee());
    }
    if (!cleanup_callee.empty() && cleanup_callee != "memory.deallocate") {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "specialized owned resources cannot be returned until "
                         "the function signature preserves their cleanup type");
    }
  }
  bool value_is_shared = statement.value()->ownership() == ValueOwnership::Shared;
  if (statement.value()->kind() == Expr::Kind::Name) {
    const Symbol* source = find_root_symbol(*statement.value(), symbols_);
    value_is_shared = value_is_shared || (source && source->shared_allocation);
  }
  if (current_return_shared_ && !value_is_shared) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "shared return requires a shared reference");
  } else if (!current_return_shared_ && value_is_shared) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "shared references require a shared return type");
  } else if ((current_return_type_.is_pointer() || current_return_type_.is_owned_array() ||
              contains_owned_array(current_return_type_)) &&
             !current_return_shared_ &&
             statement.value()->ownership() != ValueOwnership::Owned) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "owned pointer return requires an owned pointer; borrowed or raw pointers cannot escape");
  }
  if ((current_return_type_.is_pointer() || current_return_type_.is_owned_array() ||
       contains_owned_array(current_return_type_)) &&
      statement.value()->kind() == Expr::Kind::Name) {
    const Symbol* source = find_root_symbol(*statement.value(), symbols_);
    if (source && source->type != current_return_type_) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "an owned field cannot escape an aggregate individually");
    } else if (source && source->borrowed) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "a borrowed pointer cannot escape through a return value");
    } else if (source && source->owns_allocation && !source->moved &&
               symbols_.has_live_borrow(source->allocation_id)) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "cannot return an owner while an array view is live");
    } else if (source && source->owns_allocation && !source->moved) {
      const_cast<Expr&>(*statement.value()).set_transfers_ownership();
      symbols_.mark_allocation_moved(source->allocation_id);
    }
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
        if (symbol->borrowed)
          name.set_ownership(symbol->mutable_borrow
              ? ValueOwnership::MutableBorrowed : ValueOwnership::Borrowed);
        else if (symbol->shared_allocation)
          name.set_ownership(ValueOwnership::Shared);
        else if (symbol->owns_allocation)
          name.set_ownership(ValueOwnership::Owned);
        else if (symbol->type.is_managed_pointer())
          name.set_ownership(ValueOwnership::GcManaged);
        else if (symbol->type.is_pointer())
          name.set_ownership(ValueOwnership::Raw);
        if (symbol->moved) {
          diagnostics_.error(diag::kUseAfterFree, name.location(),
                             "use of '" + name.name() +
                                 "' after its allocation was deallocated");
        }
      }
      break;
    }
    case Expr::Kind::Call:
      type = check_call(static_cast<CallExpr&>(expr));
      if (type.is_pointer()) {
        auto& call = static_cast<CallExpr&>(expr);
        const FunctionSignature* signature = find_function(call.callee());
        if (call.callee() == "memory.shared_allocate" ||
            call.callee() == "memory.shared_retain")
          expr.set_ownership(ValueOwnership::Shared);
        else if (signature && signature->returns_shared)
          expr.set_ownership(ValueOwnership::Shared);
        else
          expr.set_ownership(expression_creates_owned_pointer(expr) ||
                                     (signature && signature->return_type.is_pointer())
                                 ? ValueOwnership::Owned : ValueOwnership::Raw);
      } else if (type.is_managed_pointer()) {
        expr.set_ownership(ValueOwnership::GcManaged);
      } else if (type.is_owned_array()) {
        expr.set_ownership(ValueOwnership::Owned);
      } else if (contains_owned_array(type)) {
        if (expr.kind() == Expr::Kind::Call || expr.kind() == Expr::Kind::StructInit ||
            expr.kind() == Expr::Kind::TupleLiteral || expr.kind() == Expr::Kind::Name)
          expr.set_ownership(ValueOwnership::Owned);
      } else if (type.is_array_view()) {
        expr.set_ownership(ValueOwnership::Borrowed);
      }
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
                               "operator '" + string(unary_op_spelling(unary.op())) +
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

      const string spelling(binary_op_spelling(binary.op()));

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
          // `char` is an ordered code unit, so range checks such as
          // `c >= '0' && c <= '9'` are ordinary comparisons.
          if ((!lhs_type.is_numeric() && !lhs_type.is_char()) ||
              (!rhs_type.is_numeric() && !rhs_type.is_char())) {
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
  if (contains_owned_array(type) &&
      (expr.kind() == Expr::Kind::StructInit || expr.kind() == Expr::Kind::TupleLiteral ||
       expr.kind() == Expr::Kind::Call || expr.kind() == Expr::Kind::Name))
    expr.set_ownership(ValueOwnership::Owned);
  expr.set_type(type);
  return type;
}

const StructInfo* TypeChecker::find_struct(const string& name) const {
  // Qualified names are looked up verbatim.
  if (name.find('.') != string::npos) {
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

Type TypeChecker::resolve_aggregate_type(const Type& type) {
  unordered_set<string> active;
  return resolve_aggregate_type(type, active);
}

Type TypeChecker::resolve_aggregate_type(const Type& type,
                                        unordered_set<string>& active) {
  if (!type.is_valid()) return type;
  if (type.is_array()) {
    // Rebuild unconditionally: struct types compare equal by name alone, so an
    // equality test cannot tell a resolved struct from an opaque one.
    return Type::array_type(resolve_aggregate_type(type.element_type(), active),
                            type.array_length());
  }
  if (type.is_tuple()) {
    vector<Type> elements;
    elements.reserve(type.tuple_elements().size());
    for (const Type& element : type.tuple_elements()) {
      elements.push_back(resolve_aggregate_type(element, active));
    }
    return Type::tuple_type(move(elements));
  }
  if (type.is_owned_array())
    return Type::owned_array_type(resolve_aggregate_type(type.element_type(), active));
  if (type.is_array_view())
    return Type::array_view_type(resolve_aggregate_type(type.element_type(), active));
  // Pointer elements are deliberately left shallow: layout of a pointee is
  // resolved where the pointee is allocated, and expanding through pointers
  // would recurse forever on a self-referential struct.
  if (!type.is_struct() || !type.struct_fields().empty()) return type;

  const string& name = type.struct_name();
  if (!active.insert(name).second) return type;
  const StructInfo* info = find_struct(name);
  if (info == nullptr) {
    active.erase(name);
    return type;
  }
  vector<pair<string, Type>> fields;
  fields.reserve(info->fields.size());
  for (const Parameter& field : info->fields) {
    fields.emplace_back(field.name, resolve_aggregate_type(field.type, active));
  }
  active.erase(name);
  return Type::struct_type(name, move(fields));
}

void TypeChecker::resolve_declaration_types(FunctionDecl& function) {
  function.set_return_type(resolve_aggregate_type(function.return_type()));
  for (Parameter& parameter : function.parameters()) {
    parameter.type = resolve_aggregate_type(parameter.type);
  }
}

const FunctionSignature* TypeChecker::find_function(const string& name) const {
  auto it = functions_.find(name);
  if (it != functions_.end()) return &it->second;
  if (name.find('.') == string::npos && !current_module_.empty()) {
    auto local = functions_.find(current_module_ + "." + name);
    if (local != functions_.end()) return &local->second;
  }
  return nullptr;
}

Type TypeChecker::check_array_literal(ArrayLiteralExpr& expr) {
  const vector<ExprPtr>& elements = expr.elements();
  if (elements.empty()) {
    // Empty array literal: type is inferred from context; return error for now.
    diagnostics_.error(diag::kTypeMismatch, expr.location(),
                       "empty array literal requires an explicit type annotation");
    return Type::error();
  }

  Type elem_type = check_expression(const_cast<Expr&>(*elements[0]));
  bool ok = elem_type.is_valid();
  for (size_t i = 1; i < elements.size(); ++i) {
    Type t = check_expression(const_cast<Expr&>(*elements[i]));
    if (ok && t.is_valid() && t != elem_type) {
      diagnostics_.error(diag::kTypeMismatch, elements[i]->location(),
                         "array literal element " + to_string(i) +
                             " has type '" + t.to_string() +
                             "', expected '" + elem_type.to_string() + "'");
      ok = false;
    }
  }

  if (elem_type.is_pointer()) {
    for (const ExprPtr& element : elements) {
      if (has_untracked_pointer_ownership(element->type(), element->ownership()))
        diagnostics_.error(diag::kTypeMismatch, element->location(),
                           "owned, shared, and borrowed pointers cannot be embedded in arrays");
    }
  }

  if (!ok) return Type::error();
  if (contains_array_view(elem_type)) {
    diagnostics_.error(diag::kTypeMismatch, expr.location(),
                       "borrowed array views cannot be embedded in arrays");
    return Type::error();
  }
  if (contains_owned_array(elem_type)) {
    for (const ExprPtr& element : elements) {
      Expr& value = const_cast<Expr&>(*element);
      const Symbol* source = value.kind() == Expr::Kind::Name
                                 ? find_root_symbol(value, symbols_) : nullptr;
      if (source && source->owns_allocation && !source->moved) {
        if (symbols_.has_live_borrow(source->allocation_id))
          diagnostics_.error(diag::kTypeMismatch, value.location(),
                             "cannot move an owner while an array view is live");
        else {
          value.set_transfers_ownership();
          symbols_.mark_allocation_moved(source->allocation_id);
        }
      } else if (value.ownership() != ValueOwnership::Owned) {
        diagnostics_.error(diag::kTypeMismatch, value.location(),
                           "resource-bearing array elements require an owned value");
      }
    }
  }
  return Type::array_type(elem_type, elements.size());
}

Type TypeChecker::check_tuple_literal(TupleLiteralExpr& expr) {
  vector<Type> element_types;
  element_types.reserve(expr.elements().size());
  bool ok = true;
  for (const ExprPtr& elem : expr.elements()) {
    Type t = check_expression(const_cast<Expr&>(*elem));
    element_types.push_back(t);
    if (!t.is_valid()) ok = false;
    if (contains_array_view(t))
      diagnostics_.error(diag::kTypeMismatch, elem->location(),
                         "borrowed array views cannot be embedded in tuples");
    if (has_untracked_pointer_ownership(t, elem->ownership()))
      diagnostics_.error(diag::kTypeMismatch, elem->location(),
                         "owned, shared, and borrowed pointers cannot be embedded in tuples");
    if (contains_owned_array(t)) {
      Expr& value = const_cast<Expr&>(*elem);
      const Symbol* source = value.kind() == Expr::Kind::Name
                                 ? find_root_symbol(value, symbols_) : nullptr;
      if (source && source->owns_allocation && !source->moved) {
        if (symbols_.has_live_borrow(source->allocation_id))
          diagnostics_.error(diag::kTypeMismatch, value.location(),
                             "cannot move an owner while an array view is live");
        else {
          value.set_transfers_ownership();
          symbols_.mark_allocation_moved(source->allocation_id);
        }
      } else if (value.ownership() != ValueOwnership::Owned) {
        diagnostics_.error(diag::kTypeMismatch, value.location(),
                           "resource-bearing tuple elements require an owned value");
      }
    }
  }
  if (!ok) return Type::error();
  return Type::tuple_type(move(element_types));
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

  if (base_type.is_array() || base_type.is_dynamic_array() ||
      base_type.is_owned_array() || base_type.is_array_view() ||
      base_type.is_pointer() || base_type.is_managed_pointer()) {
    // Constant-index bounds check for fixed arrays.
    if (base_type.is_array() && expr.index().kind() == Expr::Kind::IntegerLiteral) {
      const auto& lit = static_cast<const IntegerLiteralExpr&>(expr.index());
      const long long idx = lit.value();
      if (idx < 0 || static_cast<size_t>(idx) >= base_type.array_length()) {
        diagnostics_.error(diag::kIndexOutOfBounds, expr.index().location(),
                           "index " + to_string(idx) + " is out of bounds for array of " +
                               "length " + to_string(base_type.array_length()));
        return Type::error();
      }
    }
    Type element_type = base_type.element_type();
    if (element_type.is_struct() && element_type.struct_fields().empty()) {
      if (const StructInfo* info = find_struct(element_type.struct_name())) {
        vector<pair<string, Type>> fields;
        fields.reserve(info->fields.size());
        for (const Parameter& field : info->fields)
          fields.emplace_back(field.name, field.type);
        element_type = Type::struct_type(element_type.struct_name(), move(fields));
      }
    }
    return element_type;
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
    const vector<Type>& elems = base_type.tuple_elements();
    if (idx < 0 || static_cast<size_t>(idx) >= elems.size()) {
      diagnostics_.error(diag::kIndexOutOfBounds, expr.location(),
                         "tuple index " + to_string(idx) +
                             " is out of range for a " + to_string(elems.size()) +
                             "-element tuple");
      return Type::error();
    }
    return elems[static_cast<size_t>(idx)];
  }

  // Struct field access: obj.field
  if (base_type.is_struct()) {
    if (base_type.struct_name() == "GcStatistics" ||
        base_type.struct_name() == "MemoryProfile") {
      for (const auto& field : base_type.struct_fields()) {
        if (field.first == expr.member()) return field.second;
      }
      diagnostics_.error(diag::kUnknownField, expr.location(),
                         "'" + base_type.struct_name() + "' has no field '" +
                             expr.member() + "'");
      return Type::error();
    }
    const string& sname = base_type.struct_name();
    const StructInfo* info = find_struct(sname);
    if (info == nullptr) {
      diagnostics_.error(diag::kUndefinedName, expr.location(),
                         "struct '" + sname + "' is not defined");
      return Type::error();
    }
    for (const Parameter& field : info->fields) {
      if (field.name == expr.member()) return resolve_aggregate_type(field.type);
    }
    diagnostics_.error(diag::kUnknownField, expr.location(),
                       "struct '" + sname + "' has no field '" + expr.member() + "'");
    return Type::error();
  }

  // String .length property
  if (base_type.is_string() && expr.member() == "length") {
    return Type::int_type();
  }

  // Dynamic array .length property (for example `args.length`).
  if ((base_type.is_dynamic_array() || base_type.is_owned_array() ||
       base_type.is_array_view()) &&
      expr.member() == "length") {
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

  const vector<Parameter>& fields = info->fields;

  if (expr.is_positional()) {
    if (expr.positional_inits().size() != fields.size()) {
      diagnostics_.error(diag::kDestructuringMismatch, expr.location(),
                         "struct '" + expr.struct_name() + "' has " +
                             to_string(fields.size()) + " field(s) but " +
                             to_string(expr.positional_inits().size()) +
                             " value(s) were given");
    }
    const size_t count = min(expr.positional_inits().size(), fields.size());
    for (size_t i = 0; i < count; ++i) {
      const Type init_type = check_expression(const_cast<Expr&>(*expr.positional_inits()[i]));
      const Type field_type = resolve_aggregate_type(fields[i].type);
      if (init_type.is_valid() && init_type != field_type) {
        diagnostics_.error(diag::kTypeMismatch, expr.positional_inits()[i]->location(),
                           "field '" + fields[i].name + "' of struct '" +
                               expr.struct_name() + "' has type '" +
                                field_type.to_string() + "', but was given '" +
                               init_type.to_string() + "'");
      }
      if (has_untracked_pointer_ownership(fields[i].type,
                                          expr.positional_inits()[i]->ownership()))
        diagnostics_.error(diag::kTypeMismatch,
                           expr.positional_inits()[i]->location(),
                           "owned, shared, and borrowed pointers cannot be stored in aggregate fields");
      if (contains_owned_array(field_type)) {
        Expr& value = const_cast<Expr&>(*expr.positional_inits()[i]);
        const Symbol* source = value.kind() == Expr::Kind::Name
                                   ? find_root_symbol(value, symbols_) : nullptr;
        if (source && source->owns_allocation && !source->moved) {
          if (symbols_.has_live_borrow(source->allocation_id))
            diagnostics_.error(diag::kTypeMismatch, value.location(),
                               "cannot move an owner while an array view is live");
          else {
            value.set_transfers_ownership();
            symbols_.mark_allocation_moved(source->allocation_id);
          }
        } else if (value.ownership() != ValueOwnership::Owned) {
          diagnostics_.error(diag::kTypeMismatch, value.location(),
                             "resource-bearing struct fields require an owned value");
        }
      }
    }
    for (size_t i = count; i < expr.positional_inits().size(); ++i)
      check_expression(const_cast<Expr&>(*expr.positional_inits()[i]));
  } else {
    // Named-field init: validate each provided field name and type.
    for (const auto& kv : expr.field_inits()) {
      const string& field_name = kv.first;
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
      } else if (init_type.is_valid() &&
                 init_type != resolve_aggregate_type(field_def->type)) {
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "field '" + field_name + "' of struct '" + expr.struct_name() +
                           "' has type '" + resolve_aggregate_type(field_def->type).to_string() +
                               "', but was given '" + init_type.to_string() + "'");
      }
      if (field_def && has_untracked_pointer_ownership(field_def->type,
                                                       kv.second->ownership()))
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "owned, shared, and borrowed pointers cannot be stored in aggregate fields");
      if (field_def && contains_owned_array(resolve_aggregate_type(field_def->type))) {
        Expr& value = const_cast<Expr&>(*kv.second);
        const Symbol* source = value.kind() == Expr::Kind::Name
                                   ? find_root_symbol(value, symbols_) : nullptr;
        if (source && source->owns_allocation && !source->moved) {
          if (symbols_.has_live_borrow(source->allocation_id))
            diagnostics_.error(diag::kTypeMismatch, value.location(),
                               "cannot move an owner while an array view is live");
          else {
            value.set_transfers_ownership();
            symbols_.mark_allocation_moved(source->allocation_id);
          }
        } else if (value.ownership() != ValueOwnership::Owned) {
          diagnostics_.error(diag::kTypeMismatch, value.location(),
                             "resource-bearing struct fields require an owned value");
        }
      }
    }
  }

  vector<pair<string, Type>> resolved_fields;
  resolved_fields.reserve(fields.size());
  for (const Parameter& field : fields)
    resolved_fields.emplace_back(field.name, resolve_aggregate_type(field.type));
  return Type::struct_type(expr.struct_name(), move(resolved_fields));
}

Type TypeChecker::check_call(CallExpr& call) {
  if (call.callee() == "memory.array_view" && call.has_generic_argument()) {
    if (call.arguments().size() != 3) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.array_view<T>' takes a source, start, and length");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::error();
    }
    const Type source_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Type start_type = check_expression(const_cast<Expr&>(*call.arguments()[1]));
    const Type length_type = check_expression(const_cast<Expr&>(*call.arguments()[2]));
    const Type element_type = resolve_aggregate_type(call.generic_argument());
    if (contains_array_view(element_type)) {
      diagnostics_.error(diag::kTypeMismatch, call.location(),
                         "borrowed array views cannot be stored in owned arrays");
      return Type::error();
    }
    if (!source_type.is_owned_array() && !source_type.is_array_view()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "array views require an owned array or an existing array view");
    } else if (source_type.element_type() != element_type) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "array view element type must match its source");
    }
    if (start_type.is_valid() && !start_type.is_integer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                         "array view start must be an integer");
    if (length_type.is_valid() && !length_type.is_integer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[2]->location(),
                         "array view length must be an integer");
    const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_);
    if (!source || source->allocation_id == 0 || source->moved ||
        (!source->owns_allocation && !source->borrowed)) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "array views require a live tracked owner");
    }
    return Type::array_view_type(element_type);
  }
  if ((call.callee() == "memory.owned_array" ||
       call.callee() == "memory.owned_array_aligned") &&
      call.has_generic_argument()) {
    const bool aligned = call.callee() == "memory.owned_array_aligned";
    const size_t expected_arguments = aligned ? 2 : 1;
    if (call.arguments().size() != expected_arguments) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         aligned
                             ? "'memory.owned_array_aligned<T>' takes an element count and alignment"
                             : "'memory.owned_array<T>' takes one element count");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    } else {
      const Type count = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (count.is_valid() && !count.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "owned array element count must be an integer");
      if (aligned) {
        const Type alignment = check_expression(const_cast<Expr&>(*call.arguments()[1]));
        if (alignment.is_valid() && !alignment.is_integer())
          diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                             "owned array alignment must be an integer");
      }
    }
    const Type element_type = resolve_aggregate_type(call.generic_argument());
    if (contains_owned_array(element_type)) {
      diagnostics_.error(diag::kTypeMismatch, call.location(),
                         "owned arrays cannot contain elements that own nested allocations yet");
      return Type::error();
    }
    return Type::owned_array_type(element_type);
  }
  if (call.callee() == "array.owned_release") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'array.owned_release' takes one owned array");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::void_type();
    }
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Symbol* owner = find_root_symbol(*call.arguments()[0], symbols_);
    if (type.is_valid() && !type.is_owned_array())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'array.owned_release' requires an owned array");
    if (!owner || owner->cleanup_callee != "array.owned_release")
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'array.owned_release' requires an owned array binding");
    else if (symbols_.has_live_borrow(owner->allocation_id)) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "cannot release an array while an array view is live");
    } else if (!checking_deferred_cleanup_) {
      const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
      symbols_.mark_allocation_moved(owner->allocation_id);
    }
    return Type::void_type();
  }
  if (call.callee() == "mutex.create") {
    if (!call.arguments().empty()) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'mutex.create' takes no arguments");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    }
    return Type::pointer_type(Type::byte_type());
  }
  if (call.callee() == "mutex.lock" || call.callee() == "mutex.unlock" ||
      call.callee() == "mutex.destroy") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes one mutex handle");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::void_type();
    }
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Symbol* mutex = find_root_symbol(*call.arguments()[0], symbols_);
    if (type.is_valid() && !type.is_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "mutex operation requires a mutex handle");
    if (call.callee() == "mutex.destroy") {
      if (!mutex || mutex->cleanup_callee != "mutex.destroy") {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "mutex.destroy requires an owned mutex handle");
      } else if (mutex->borrowed) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed mutex handles cannot be destroyed");
      } else if (!checking_deferred_cleanup_) {
        const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
        symbols_.mark_allocation_moved(mutex->allocation_id);
      }
    }
    return Type::void_type();
  }

  unordered_set<string> active_structs;
  function<Type(Type)> resolve_generic_type = [&](Type type) -> Type {
    if (type.is_array())
      return Type::array_type(resolve_generic_type(type.element_type()), type.array_length());
    if (type.is_dynamic_array())
      return Type::dynamic_array_type(resolve_generic_type(type.element_type()));
    if (type.is_owned_array())
      return Type::owned_array_type(resolve_generic_type(type.element_type()));
    if (type.is_pointer())
      return Type::pointer_type(type.element_type());
    if (type.is_managed_pointer())
      return Type::managed_pointer_type(type.element_type());
    if (!type.is_struct() || !type.struct_fields().empty() ||
        active_structs.contains(type.struct_name())) return type;
    const StructInfo* info = find_struct(type.struct_name());
    if (!info) {
      diagnostics_.error(diag::kUndefinedName, call.location(),
                         "undefined allocated type '" + type.struct_name() + "'");
      return Type::error();
    }
    active_structs.insert(type.struct_name());
    vector<pair<string, Type>> fields;
    fields.reserve(info->fields.size());
    for (const Parameter& field : info->fields)
      fields.emplace_back(field.name, resolve_generic_type(field.type));
    active_structs.erase(type.struct_name());
    return Type::struct_type(type.struct_name(), move(fields));
  };

  // Builtin 'print'
  if (call.callee() == kPrintBuiltin) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'print' takes exactly one argument, but " +
                             to_string(call.arguments().size()) + " were given");
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
                             to_string(call.arguments().size()) + " were given");
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
                             to_string(call.arguments().size()) + " were given");
      return Type::int_type();
    }
    const Type arg_type = check_expression(const_cast<Expr&>(*call.arguments().front()));
    if (arg_type.is_valid() && !arg_type.is_string() && !arg_type.is_array() &&
        !arg_type.is_dynamic_array() && !arg_type.is_owned_array()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments().front()->location(),
                         "'length' requires a string or array argument, not '" + arg_type.to_string() + "'");
    }
    return Type::int_type();
  }

  // Builtin 'nrq_gc_init'
  if (call.callee() == "nrq_gc_init") {
    if (call.arguments().size() != 0) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'nrq_gc_init' takes no arguments, but " +
                             to_string(call.arguments().size()) + " were given");
      return Type::void_type();
    }
    return Type::void_type();
  }

  // Builtin 'nrq_gc_shutdown'
  if (call.callee() == "nrq_gc_shutdown") {
    if (call.arguments().size() != 0) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'nrq_gc_shutdown' takes no arguments, but " +
                             to_string(call.arguments().size()) + " were given");
      return Type::void_type();
    }
    return Type::void_type();
  }

  // Builtin 'nrq_gc_collect'
  if (call.callee() == "nrq_gc_collect") {
    if (call.arguments().size() != 0) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'nrq_gc_collect' takes no arguments, but " +
                             to_string(call.arguments().size()) + " were given");
      return Type::void_type();
    }
    return Type::void_type();
  }

  // Namespaced GC API from Phase 6.5. Statistics are a value with named
  // fields so callers can inspect individual counters without runtime casts.
  if (call.callee() == "gc.enable" || call.callee() == "gc.disable" ||
      call.callee() == "gc.collect") {
    if (!call.arguments().empty()) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes no arguments, but " +
                             to_string(call.arguments().size()) + " were given");
    }
    for (const ExprPtr& argument : call.arguments())
      check_expression(const_cast<Expr&>(*argument));
    return Type::void_type();
  }
  if (call.callee() == "gc.set_threshold") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'gc.set_threshold' takes exactly one integer argument");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::void_type();
    }
    const Type threshold = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (threshold.is_valid() && !threshold.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'gc.set_threshold' requires an integer byte count");
    }
    return Type::void_type();
  }
  if (call.callee() == "gc.get_statistics") {
    if (!call.arguments().empty()) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'gc.get_statistics' takes no arguments");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    }
    return Type::struct_type("GcStatistics", {
        {"allocated_bytes", Type::u64_type()},
        {"collected_bytes", Type::u64_type()},
        {"collections", Type::u64_type()},
        {"live_objects", Type::u64_type()}});
  }
  if (call.callee() == "gc.is_enabled") {
    if (!call.arguments().empty()) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'gc.is_enabled' takes no arguments");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    }
    return Type::bool_type();
  }
  if (call.callee() == "memory.secure_allocate" && call.has_generic_argument()) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.secure_allocate<T>' takes one byte-count argument");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    } else {
      const Type size = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (size.is_valid() && !size.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "secure allocation size must be an integer");
    }
    return Type::pointer_type(resolve_generic_type(call.generic_argument()));
  }
  if ((call.callee() == "memory.mapped_read" ||
       call.callee() == "memory.mapped_write") && call.has_generic_argument()) {
    const bool write = call.callee() == "memory.mapped_write";
    const size_t expected = write ? 3 : 2;
    if (call.arguments().size() != expected)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "<T>' takes a mapping, byte offset" +
                             (write ? ", and value" : ""));
    for (size_t index = 0; index < call.arguments().size(); ++index) {
      const Type type = check_expression(const_cast<Expr&>(*call.arguments()[index]));
      if (index == 0 && type.is_valid() && !type.is_pointer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "mapped access requires a mapping pointer");
      if (index == 1 && type.is_valid() && !type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "mapped access offset must be an integer");
      if (write && index == 2 && type.is_valid() &&
          type != resolve_generic_type(call.generic_argument()))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "mapped write value must match its type argument");
    }
    return write ? Type::void_type() : resolve_generic_type(call.generic_argument());
  }

  if (call.callee() == "memory.profiler.start" ||
      call.callee() == "memory.profiler.stop") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes no arguments");
    return Type::void_type();
  }
  if (call.callee() == "memory.profiler.capture") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.profiler.capture' takes no arguments");
    // Field order matches `NrqMemoryProfile` in the runtime allocator header;
    // the generated code loads the whole report as one aggregate.
    return Type::struct_type("MemoryProfile", {
        {"current_bytes", Type::u64_type()}, {"total_bytes", Type::u64_type()},
        {"peak_bytes", Type::u64_type()}, {"allocations", Type::u64_type()},
        {"deallocations", Type::u64_type()},
        {"gc_allocated_bytes", Type::u64_type()},
        {"gc_collected_bytes", Type::u64_type()},
        {"gc_collections", Type::u64_type()},
        {"gc_live_objects", Type::u64_type()},
        {"free_bytes", Type::u64_type()},
        {"fragmentation_bytes", Type::u64_type()},
        {"largest_free_block", Type::u64_type()},
        {"largest_allocation", Type::u64_type()},
        {"allocations_le_64", Type::u64_type()},
        {"allocations_le_256", Type::u64_type()},
        {"allocations_le_1024", Type::u64_type()},
        {"allocations_le_4096", Type::u64_type()},
        {"allocations_le_65536", Type::u64_type()},
        {"allocations_gt_65536", Type::u64_type()},
        {"general_bytes", Type::u64_type()},
        {"pool_bytes", Type::u64_type()},
        {"arena_bytes", Type::u64_type()},
        {"stack_bytes", Type::u64_type()},
        {"slab_bytes", Type::u64_type()},
        {"buddy_bytes", Type::u64_type()},
        {"free_list_bytes", Type::u64_type()},
        {"tlsf_bytes", Type::u64_type()},
        {"custom_bytes", Type::u64_type()}});
  }
  if (call.callee() == "memory.used_bytes" ||
      call.callee() == "memory.total_bytes") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.pin_count" ||
      call.callee() == "memory.pin_violations") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.available_bytes") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.available_bytes' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.runtime_version") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.runtime_version' takes no arguments");
    return Type::string_type();
  }
  if (call.callee() == "memory.cpu_count") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.cpu_count' takes no arguments");
    return Type::i32_type();
  }
  if (call.callee() == "memory.cpu_features") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.cpu_features' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.thread_count") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.thread_count' takes no arguments");
    return Type::u32_type();
  }
  if (call.callee() == "memory.allocator_kind") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.allocator_kind' takes one allocator handle");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    } else {
      const Type handle = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (handle.is_valid() && !handle.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "allocator handle must be an integer");
    }
    return Type::i32_type();
  }
  if (call.callee() == "memory.start_ref_count" ||
      call.callee() == "memory.stop_ref_count") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes no arguments");
    return Type::void_type();
  }
  if (call.callee() == "memory.get_ref_count") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.get_ref_count' takes one managed pointer");
    if (call.arguments().empty()) return Type::i64_type();
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (type.is_valid() && !type.is_managed_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "reference diagnostics require a managed pointer");
    return Type::i64_type();
  }

  if (call.callee() == "memory.secure_zero") {
    if (call.arguments().size() != 2)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.secure_zero' takes a pointer and byte count");
    for (size_t index = 0; index < call.arguments().size(); ++index) {
      const Type type = check_expression(const_cast<Expr&>(*call.arguments()[index]));
      if (index == 0 && type.is_valid() && !type.is_pointer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "secure zero requires a pointer");
      if (index == 1 && type.is_valid() && !type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "secure zero byte count must be an integer");
    }
    return Type::void_type();
  }
  if (call.callee() == "memory.secure_release" ||
      call.callee() == "memory.unmap_file" ||
      call.callee() == "memory.unpin") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes one pointer");
    for (const ExprPtr& argument : call.arguments()) {
      const Type type = check_expression(const_cast<Expr&>(*argument));
      if (type.is_valid() && !type.is_pointer() && !type.is_managed_pointer())
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "argument must be a pointer");
    }
    if (!checking_deferred_cleanup_ &&
        (call.callee() == "memory.secure_release" ||
         call.callee() == "memory.unmap_file") && !call.arguments().empty()) {
      const Symbol* pointer_symbol = find_root_symbol(*call.arguments()[0], symbols_);
      if (pointer_symbol && pointer_symbol->borrowed)
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed pointers cannot be released or unmapped");
      else if (pointer_symbol && pointer_symbol->type.is_pointer()) {
        const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
        symbols_.mark_allocation_moved(pointer_symbol->allocation_id);
      }
    }
    return call.callee() == "memory.unmap_file" ? Type::i32_type()
                                                  : Type::void_type();
  }
  if (call.callee() == "memory.map_file") {
    if (call.arguments().size() != 2)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.map_file' takes a path and writable flag");
    if (!call.arguments().empty()) {
      const Type path = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (path.is_valid() && !path.is_string())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "mapping path must be a string");
    }
    if (call.arguments().size() > 1) {
      const Type writable = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (writable.is_valid() && !writable.is_bool())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                           "mapping writable flag must be bool");
    }
    return Type::pointer_type(Type::u8_type());
  }
  if (call.callee() == "memory.mapping_size") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.mapping_size' takes one mapped pointer");
    for (const ExprPtr& argument : call.arguments())
      check_expression(const_cast<Expr&>(*argument));
    return Type::u64_type();
  }
  if (call.callee() == "memory.pin") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.pin' takes one managed pointer");
    if (call.arguments().empty()) return Type::pointer_type(Type::void_type());
    const Type pointer = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (pointer.is_valid() && !pointer.is_managed_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "memory.pin requires a managed pointer");
    return pointer;
  }

  // Explicit allocator handles are represented as opaque u64 values in the
  // source language; the runtime validates each handle before using it.
  const unordered_map<string, bool> allocator_constructors = {
      {"memory.general_allocator", false}, {"memory.pool_allocator", true},
      {"memory.arena_allocator", true}, {"memory.region", true},
      {"memory.stack_allocator", true},
      {"memory.slab_allocator", true}, {"memory.buddy_allocator", true},
      {"memory.free_list_allocator", true}, {"memory.tlsf_allocator", true}};
  if (auto allocator = allocator_constructors.find(call.callee());
      allocator != allocator_constructors.end()) {
    const size_t expected = allocator->second ? 1 : 0;
    if (call.arguments().size() != expected)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes " +
                             (expected ? "one capacity argument" : "no arguments"));
    for (const ExprPtr& argument : call.arguments()) {
      const Type type = check_expression(const_cast<Expr&>(*argument));
      if (!type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "allocator capacity must be an integer");
    }
    return Type::u64_type();
  }
  if (call.callee() == "memory.get_allocator") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.get_allocator' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.set_default_allocator") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.set_default_allocator' takes one allocator handle");
    for (const ExprPtr& argument : call.arguments()) {
      const Type type = check_expression(const_cast<Expr&>(*argument));
      if (type.is_valid() && !type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "allocator handle must be an integer");
    }
    return Type::u64_type();
  }
  if (call.callee() == "memory.get_default_allocator") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.get_default_allocator' takes no arguments");
    return Type::u64_type();
  }
  if (call.callee() == "memory.use_allocator" ||
      call.callee() == "memory.reset_allocator" ||
      call.callee() == "memory.destroy_allocator") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes one allocator handle");
    for (const ExprPtr& argument : call.arguments()) {
      const Type type = check_expression(const_cast<Expr&>(*argument));
      if (!type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "allocator handle must be an integer");
    }
    return call.callee() == "memory.use_allocator" ? Type::u64_type()
                                                       : Type::i32_type();
  }

  // Builtin 'nrq_memory_allocate'
  if (call.callee() == "memory.reallocate" && call.has_generic_argument()) {
    if (call.arguments().size() != 2) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.reallocate<T>' takes a pointer and a new byte count");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::pointer_type(resolve_generic_type(call.generic_argument()));
    }
    const Type pointer_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (pointer_type.is_valid() && !pointer_type.is_pointer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'memory.reallocate<T>' requires an explicitly allocated pointer");
    }
    const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_);
    if (source && source->borrowed) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "borrowed pointers cannot be resized");
    } else if (source && source->shared_allocation) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "shared allocations cannot be resized");
    } else if (source && source->owns_allocation && !source->moved) {
      const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
      symbols_.mark_allocation_moved(source->allocation_id);
    }
    const Type size_type = check_expression(const_cast<Expr&>(*call.arguments()[1]));
    if (size_type.is_valid() && !size_type.is_integer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                         "'memory.reallocate<T>' requires an integer byte count");
    return Type::pointer_type(resolve_generic_type(call.generic_argument()));
  }

  if (call.callee() == "memory.shared_allocate" && call.has_generic_argument()) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.shared_allocate<T>' takes one byte count");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
    } else {
      const Type size_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (size_type.is_valid() && !size_type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "shared allocation size must be an integer");
    }
    return Type::pointer_type(resolve_generic_type(call.generic_argument()));
  }
  if (call.callee() == "memory.shared_retain") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.shared_retain' takes one shared pointer");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::error();
    }
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_);
    if (type.is_valid() && !type.is_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "shared retain requires a pointer");
    if (!source || !source->shared_allocation)
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "shared retain requires a shared allocation");
    return type;
  }
  if (call.callee() == "memory.shared_release" || call.callee() == "memory.shared_count") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'" + call.callee() + "' takes one shared pointer");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return call.callee() == "memory.shared_count" ? Type::u64_type()
                                                      : Type::void_type();
    }
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_);
    if (type.is_valid() && !type.is_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "shared operation requires a pointer");
    if (!source || !source->shared_allocation)
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "operation requires a shared allocation");
    if (call.callee() == "memory.shared_release" && !checking_deferred_cleanup_ &&
        source && source->shared_allocation) {
      const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
      symbols_.mark_allocation_moved(source->allocation_id);
    }
    return call.callee() == "memory.shared_count" ? Type::u64_type()
                                                    : Type::void_type();
  }

  if (call.callee() == "memory.allocate" && call.has_generic_argument()) {
    if (call.arguments().size() != 1 && call.arguments().size() != 2) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.allocate<T>' takes a byte count and optional allocator handle");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::pointer_type(resolve_generic_type(call.generic_argument()));
    }
    const Type size_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (size_type.is_valid() && !size_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'memory.allocate<T>' requires an integer byte count");
    }
    if (call.arguments().size() == 2) {
      const Type allocator_type = check_expression(
          const_cast<Expr&>(*call.arguments()[1]));
      if (allocator_type.is_valid() && !allocator_type.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                           "allocator handle must be an integer");
    }
    return Type::pointer_type(resolve_generic_type(call.generic_argument()));
  }

  if (call.callee() == "array.allocate" && call.has_generic_argument()) {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'array.allocate<T>' takes exactly one element-count argument");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::managed_pointer_type(resolve_generic_type(call.generic_argument()));
    }
    const Type count_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (count_type.is_valid() && !count_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'array.allocate<T>' requires an integer element count");
    }
    const Type element_type = resolve_generic_type(call.generic_argument());
    if (contains_array_view(element_type))
      diagnostics_.error(diag::kTypeMismatch, call.location(),
                         "borrowed array views cannot be stored in GC-managed arrays");
    return Type::managed_pointer_type(element_type);
  }

  if (call.callee() == "memory.deallocate") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'memory.deallocate' takes exactly one pointer argument");
      for (const ExprPtr& argument : call.arguments())
        check_expression(const_cast<Expr&>(*argument));
      return Type::void_type();
    }
    const Type pointer_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (pointer_type.is_valid() && !pointer_type.is_pointer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'memory.deallocate' requires an explicitly allocated pointer");
    }
    const Symbol* pointer_symbol = find_root_symbol(*call.arguments()[0], symbols_);
    if (pointer_symbol && pointer_symbol->shared_allocation) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "shared allocations must be released with memory.shared_release");
    } else if (pointer_symbol && pointer_symbol->borrowed) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "borrowed pointers cannot be deallocated");
    } else if (!checking_deferred_cleanup_ && pointer_symbol && pointer_symbol->type.is_pointer()) {
      const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
      symbols_.mark_allocation_moved(pointer_symbol->allocation_id);
    }
    return Type::void_type();
  }

  if (call.callee() == "nrq_memory_allocate" || call.callee() == "memory.allocate") {
    if (call.arguments().size() != 2) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'nrq_memory_allocate' takes exactly two arguments (bytes, alignment), but " +
                             to_string(call.arguments().size()) + " were given");
      return Type::u64_type();
    }
    const Type arg1_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Type arg2_type = check_expression(const_cast<Expr&>(*call.arguments()[1]));
    if (arg1_type.is_valid() && !arg1_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'nrq_memory_allocate' first argument must be an integer (bytes), not '" + arg1_type.to_string() + "'");
    }
    if (arg2_type.is_valid() && !arg2_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                         "'nrq_memory_allocate' second argument must be an integer (alignment), not '" + arg2_type.to_string() + "'");
    }
    return Type::u64_type();
  }

  // Builtin 'nrq_memory_deallocate'
  if (call.callee() == "nrq_memory_deallocate" || call.callee() == "memory.deallocate") {
    if (call.arguments().size() != 3) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'nrq_memory_deallocate' takes exactly three arguments (ptr, bytes, alignment), but " +
                             to_string(call.arguments().size()) + " were given");
      return Type::void_type();
    }
    const Type arg1_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Type arg2_type = check_expression(const_cast<Expr&>(*call.arguments()[1]));
    const Type arg3_type = check_expression(const_cast<Expr&>(*call.arguments()[2]));
    if (arg1_type.is_valid()) {
      // Pointer type check - accept any non-error type
    }
    if (arg2_type.is_valid() && !arg2_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                         "'nrq_memory_deallocate' second argument must be an integer (bytes), not '" + arg2_type.to_string() + "'");
    }
    if (arg3_type.is_valid() && !arg3_type.is_integer()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[2]->location(),
                         "'nrq_memory_deallocate' third argument must be an integer (alignment), not '" + arg3_type.to_string() + "'");
    }
    return Type::void_type();
  }

  // Math builtins: sqrt, sin, cos, abs, floor, ceil (and math.<name>)
  string fn_name = call.callee();
  if (fn_name.rfind("math.", 0) == 0) fn_name = fn_name.substr(5);
  if (fn_name == "sqrt" || fn_name == "sin" || fn_name == "cos" ||
      fn_name == "abs" || fn_name == "floor" || fn_name == "ceil") {
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "math function '" + call.callee() + "' takes exactly one argument, but " +
                             to_string(call.arguments().size()) + " were given");
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
                             to_string(call.arguments().size()) + " were given");
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
                             to_string(call.arguments().size()) + " were given");
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
                             to_string(call.arguments().size()) + " were given");
      return Type::bool_type();
    }
    const Type path_type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    if (path_type.is_valid() && !path_type.is_string()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "'file.exists' path must be a 'string', not '" + path_type.to_string() + "'");
    }
    return Type::bool_type();
  }

  if (call.callee() == "file.open") {
    if (call.arguments().size() != 2)
      diagnostics_.error(diag::kPrintArity, call.location(), "'file.open' expects path and writable flag");
    if (!call.arguments().empty()) {
      const Type path = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (path.is_valid() && !path.is_string()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(), "file.open path must be a string");
    }
    if (call.arguments().size() > 1) {
      const Type writable = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (writable.is_valid() && !writable.is_bool()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(), "file.open writable flag must be bool");
    }
    return Type::pointer_type(Type::u8_type());
  }
  if (call.callee() == "file.close") {
    if (call.arguments().size() != 1) diagnostics_.error(diag::kPrintArity, call.location(), "'file.close' expects one resource handle");
    for (const ExprPtr& argument : call.arguments()) {
      const Type type = check_expression(const_cast<Expr&>(*argument));
      if (type.is_valid() && !type.is_pointer()) diagnostics_.error(diag::kTypeMismatch, argument->location(), "file.close requires a file resource handle");
    }
    if (!checking_deferred_cleanup_ && !call.arguments().empty()) {
      const Symbol* resource = find_root_symbol(*call.arguments()[0], symbols_);
      if (resource && resource->borrowed)
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed file resources cannot be closed");
      else if (resource && resource->type.is_pointer())
        symbols_.mark_allocation_moved(resource->allocation_id);
    }
    return Type::i32_type();
  }
  if (call.callee() == "file.write") {
    if (call.arguments().size() != 2) diagnostics_.error(diag::kPrintArity, call.location(), "'file.write' expects a resource handle and string");
    if (!call.arguments().empty()) {
      const Type handle = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (handle.is_valid() && !handle.is_pointer()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(), "file.write requires a file resource handle");
    }
    if (call.arguments().size() > 1) {
      const Type text = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (text.is_valid() && !text.is_string()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(), "file.write content must be a string");
    }
    return Type::i32_type();
  }
  if (call.callee() == "file.read") {
    if (call.arguments().size() != 3)
      diagnostics_.error(diag::kPrintArity, call.location(), "'file.read' expects a resource, destination pointer, and capacity");
    if (!call.arguments().empty()) {
      const Type handle = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (handle.is_valid() && !handle.is_pointer()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(), "file.read requires a file resource handle");
    }
    if (call.arguments().size() > 1) {
      const Type destination = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (destination.is_valid() && !destination.is_pointer()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(), "file.read destination must be a pointer");
    }
    if (call.arguments().size() > 2) {
      const Type capacity = check_expression(const_cast<Expr&>(*call.arguments()[2]));
      if (capacity.is_valid() && !capacity.is_integer()) diagnostics_.error(diag::kTypeMismatch, call.arguments()[2]->location(), "file.read capacity must be an integer");
    }
    return Type::i64_type();
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
                             to_string(call.arguments().size()) + " were given");
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
                           to_string(sig.parameter_types.size()) +
                           " argument" + (sig.parameter_types.size() == 1 ? "" : "s") +
                           ", but " + to_string(call.arguments().size()) +
                           " were given");
  }

  const size_t count = min(call.arguments().size(), sig.parameter_types.size());
  vector<uint64_t> ownership_transfers;
  unordered_map<uint64_t, bool> active_borrows;
  for (size_t i = 0; i < count; ++i) {
    const Type arg_type = check_expression(const_cast<Expr&>(*call.arguments()[i]));
    if (arg_type.is_valid() && arg_type != sig.parameter_types[i]) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "argument " + to_string(i + 1) + " of '" + call.callee() +
                             "' must be of type '" + sig.parameter_types[i].to_string() +
                             "', not '" + arg_type.to_string() + "'");
    }
    if (sig.parameter_types[i].is_array_view()) {
      const Expr& argument = *call.arguments()[i];
      const Symbol* source = nullptr;
      if (argument.kind() == Expr::Kind::Call) {
        const auto& view_call = static_cast<const CallExpr&>(argument);
        if (view_call.callee() == "memory.array_view" &&
            !view_call.arguments().empty())
          source = find_root_symbol(*view_call.arguments()[0], symbols_);
      } else {
        source = find_root_symbol(argument, symbols_);
      }
      if (!source || source->allocation_id == 0 || source->moved) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "array view arguments must refer to a live tracked owner");
      } else if (std::find(ownership_transfers.begin(), ownership_transfers.end(),
                           source->allocation_id) != ownership_transfers.end()) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "cannot borrow an owner in a call that also transfers it");
      } else {
        auto found = active_borrows.find(source->allocation_id);
        if (found != active_borrows.end() && found->second)
          diagnostics_.error(diag::kTypeMismatch, argument.location(),
                             "conflicting borrows of the same allocation in one call");
        active_borrows[source->allocation_id] = false;
      }
      continue;
    }
    if (sig.parameter_types[i].is_owned_array() ||
        contains_owned_array(sig.parameter_types[i])) {
      const Symbol* source = call.arguments()[i]->kind() == Expr::Kind::Name
                                 ? find_root_symbol(*call.arguments()[i], symbols_) : nullptr;
      if (source && source->type != sig.parameter_types[i]) source = nullptr;
      if (source && source->borrowed) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "borrowed values cannot be transferred as owned resources");
      } else if (source && active_borrows.find(source->allocation_id) != active_borrows.end()) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "cannot transfer an owner while another argument borrows it");
      } else if (source && std::find(ownership_transfers.begin(), ownership_transfers.end(),
                                     source->allocation_id) != ownership_transfers.end()) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "the same owner cannot be transferred to multiple parameters in one call");
      } else if (!(source && source->owns_allocation && !source->moved) &&
                 call.arguments()[i]->ownership() != ValueOwnership::Owned) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "owning parameter requires an owned resource value");
      } else if (source && source->owns_allocation && !source->moved) {
        if (symbols_.has_live_borrow(source->allocation_id)) {
          diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                             "cannot transfer an owner while an array view is live");
        } else {
          const_cast<Expr&>(*call.arguments()[i]).set_transfers_ownership();
          ownership_transfers.push_back(source->allocation_id);
        }
      }
      continue;
    }
    if (!sig.parameter_types[i].is_pointer()) continue;
    const Symbol* source = call.arguments()[i]->kind() == Expr::Kind::Name
                               ? find_root_symbol(*call.arguments()[i], symbols_) : nullptr;
    if (source && source->type != sig.parameter_types[i]) source = nullptr;
    const bool expects_shared = i < sig.parameter_shared.size() && sig.parameter_shared[i];
    const bool argument_is_shared =
        (source && source->shared_allocation) ||
        call.arguments()[i]->ownership() == ValueOwnership::Shared;
    if (expects_shared && !argument_is_shared) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "shared parameter requires a shared reference");
      continue;
    }
    if (!expects_shared && argument_is_shared) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "shared references can only be transferred to a shared parameter");
      continue;
    }
    if (!expects_shared &&
        !(i < sig.parameter_borrowed.size() && sig.parameter_borrowed[i]) &&
        source && !source->cleanup_callee.empty() &&
        source->cleanup_callee != "memory.deallocate") {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "owned resource requires cleanup with '" +
                             source->cleanup_callee + "'; pass it through a borrow parameter");
      continue;
    }
    if (i < sig.parameter_borrowed.size() && sig.parameter_borrowed[i]) {
      if (!source) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "borrow arguments must refer to a named pointer with a tracked lifetime");
        continue;
      }
      const bool mutable_borrow = i < sig.parameter_mutable_borrow.size() &&
                                  sig.parameter_mutable_borrow[i];
      if (std::find(ownership_transfers.begin(), ownership_transfers.end(),
                    source->allocation_id) != ownership_transfers.end()) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "cannot borrow an owner in a call that also transfers it");
        continue;
      }
      if (mutable_borrow && !source->is_mutable()) {
        diagnostics_.error(diag::kImmutableAssignment, call.arguments()[i]->location(),
                           "mutable borrowing requires a mutable binding");
      }
      auto found = active_borrows.find(source->allocation_id);
      if (found != active_borrows.end() && (found->second || mutable_borrow)) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "conflicting borrows of the same allocation in one call");
      }
      active_borrows[source->allocation_id] = mutable_borrow;
    } else if (source && source->borrowed) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "a borrowed pointer cannot be transferred to an owning parameter");
    } else if (source &&
               active_borrows.find(source->allocation_id) != active_borrows.end()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "cannot transfer an owner while another argument borrows it");
    } else if (source && source->owns_allocation && !source->moved &&
               std::find(ownership_transfers.begin(), ownership_transfers.end(),
                         source->allocation_id) != ownership_transfers.end()) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "the same owner cannot be transferred to multiple parameters in one call");
    } else if (!expects_shared &&
               !(source && source->owns_allocation && !source->moved) &&
               call.arguments()[i]->ownership() != ValueOwnership::Owned) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "owning parameter requires an owned pointer; use a borrow parameter for non-owning access");
    } else if (source && source->owns_allocation && !source->moved) {
      const_cast<Expr&>(*call.arguments()[i]).set_transfers_ownership();
      ownership_transfers.push_back(source->allocation_id);
    }
  }

  for (size_t i = count; i < call.arguments().size(); ++i) {
    check_expression(const_cast<Expr&>(*call.arguments()[i]));
  }

  for (uint64_t allocation_id : ownership_transfers)
    symbols_.mark_allocation_moved(allocation_id);

  return sig.return_type;
}

void TypeChecker::require_type(const Expr& expr, Type expected, const char* context) {
  const Type actual = expr.type();
  if (!actual.is_valid() || !expected.is_valid()) return;
  if (actual != expected) {
    diagnostics_.error(diag::kTypeMismatch, expr.location(),
                       string(context) + " requires '" + expected.to_string() +
                           "', not '" + actual.to_string() + "'");
  }
}

bool TypeChecker::block_can_fall_through(const BlockStmt& block) const {
  return block_falls_through(block);
}

}  // namespace nirq
