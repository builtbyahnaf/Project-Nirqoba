#include "nirqoba/Sema/TypeChecker.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

#include "nirqoba/Driver/ModuleResolver.h"

namespace nirq {
using namespace std;

static const Symbol* find_root_symbol(const Expr& expr,
                                      const SymbolTable& symbols);

namespace {

constexpr const char* kPrintBuiltin = "print";

static bool contains_owned_array(const Type& type,
                                 unordered_set<string>& active_structs) {
  if (type.is_owned_array() || type.is_owned_pointer() || type.is_soa()) return true;
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
                                        unordered_set<string>& active_structs);

// True when an aggregate owns reference-counted pointer fields at any depth.
// Shared fields retain on copy and release on destruction, so aggregates holding
// them are copyable rather than move-only.
static bool contains_shared_pointer(const Type& type) {
  if (type.is_shared_pointer()) return true;
  if (type.is_array()) return contains_shared_pointer(type.element_type());
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_shared_pointer(element)) return true;
  }
  if (type.is_struct()) {
    for (const auto& field : type.struct_fields())
      if (contains_shared_pointer(field.second)) return true;
  }
  return false;
}

// The nested-allocation restriction applies to owned arrays (and aggregates holding
// them), not to aggregates that merely own pointers: recursive destruction already
// drops those.
static bool contains_owned_array_with_owning_elements(const Type& type,
                                                      unordered_set<string>& active) {
  if (type.is_owned_array())
    return contains_nested_owned_array(type.element_type(), active);
  if (type.is_array())
    return contains_owned_array_with_owning_elements(type.element_type(), active);
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_owned_array_with_owning_elements(element, active)) return true;
  }
  if (type.is_struct() && !type.struct_fields().empty() &&
      active.insert(type.struct_name()).second) {
    bool found = false;
    for (const auto& field : type.struct_fields())
      if (contains_owned_array_with_owning_elements(field.second, active)) {
        found = true;
        break;
      }
    active.erase(type.struct_name());
    return found;
  }
  return false;
}

static bool contains_owned_array_with_owning_elements(const Type& type) {
  unordered_set<string> active;
  return contains_owned_array_with_owning_elements(type, active);
}

static bool contains_nested_owned_array(const Type& type,
                                        unordered_set<string>& active_structs) {
  if (type.is_owned_pointer()) return true;
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
  if (type.is_owned_array() || type.is_soa())
    return contains_array_view(type.element_type(), active_structs);
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
  if (type.is_soa()) return "soa.owned_release";
  if (type.is_owned_array()) return "array.owned_release";
  if (type.is_owned_pointer()) return "memory.deallocate";
  return contains_owned_array(type) ? "aggregate.owned_drop" : string{};
}

// `nullptr` takes its pointer type from the declaration that consumes it, so it is
// recognised by marker rather than by the integer value it is represented with.
static bool is_null_pointer_literal(const Expr& expr) {
  return expr.kind() == Expr::Kind::IntegerLiteral &&
         static_cast<const IntegerLiteralExpr&>(expr).is_null_pointer();
}

static bool has_untracked_pointer_ownership(const Type& type,
                                            ValueOwnership ownership) {
  if (type.is_shared_pointer())
    return ownership != ValueOwnership::Shared;
  if (!type.is_pointer()) return false;
  if (type.is_owned_pointer()) return ownership != ValueOwnership::Owned;
  return ownership == ValueOwnership::Owned ||
         ownership == ValueOwnership::Shared ||
         ownership == ValueOwnership::Borrowed ||
         ownership == ValueOwnership::MutableBorrowed;
}

static bool statement_can_fall_through(const Stmt& stmt);

// 17.1/17.5: validate each field against its declared ownership contract. Owned and
// shared pointer fields still lack generated field destruction, so they are rejected
// with a diagnostic that names the actual missing capability instead of the old
// nested-allocation message.
static void validate_struct_field_contracts(const std::vector<Parameter>& fields,
                                            DiagnosticEngine& diagnostics) {
  for (const Parameter& field : fields) {
    if (field.borrowed && (!field.type.is_pointer() ||
                           field.type.is_owned_pointer() ||
                           field.type.is_managed_pointer())) {
      diagnostics.error(diag::kTypeMismatch, field.location,
                        "borrowed field '" + field.name +
                            "' must be a raw pointer type");
    }
    if (field.shared && !field.type.is_shared_pointer()) {
      diagnostics.error(diag::kTypeMismatch, field.location,
                        "shared field '" + field.name +
                            "' must be a shared pointer type");
    }
    if (!field.borrowed && !field.shared && field.type.is_pointer() &&
        !field.type.is_owned_pointer() && !field.type.is_shared_pointer()) {
      // An unqualified raw pointer field has no tracked ownership, so it still cannot
      // be stored in an aggregate; the contract must be stated explicitly.
      diagnostics.error(diag::kTypeMismatch, field.location,
                        "pointer field '" + field.name +
                            "' needs an ownership qualifier: declare it 'owned', "
                            "'borrow' or 'borrow mut'");
    }
    if (field.type.is_owned_pointer() && !field.resource_domain.empty()) {
      diagnostics.error(diag::kTypeMismatch, field.location,
                        "owned resource-domain fields need per-domain field "
                        "destruction, which structs do not support yet");
    }
    if (!field.resource_domain.empty() &&
        (!field.type.is_pointer() || field.borrowed))
      diagnostics.error(diag::kTypeMismatch, field.location,
                        "resource domain '" + field.resource_domain +
                            "' requires an owned pointer field");
  }
}

// A borrowed field holds a non-owning reference, so it may only be initialized from a
// borrow. A mutable borrow may receive a mutable borrow; an immutable one may not.
static bool borrowed_field_value_violation(const Parameter& field, const Expr& value) {
  if (!field.borrowed) return false;
  // `&x` is the act of borrowing, whatever ownership the checker assigns to the
  // enclosing expression.
  if (value.kind() == Expr::Kind::Unary &&
      static_cast<const UnaryExpr&>(value).op() == UnaryOp::AddressOf)
    return false;
  const ValueOwnership ownership = value.ownership();
  if (ownership == ValueOwnership::Borrowed) return false;
  if (field.mutable_borrow && ownership == ValueOwnership::MutableBorrowed) return false;
  return true;
}

static bool expression_creates_owned_pointer(const Expr& expr) {
  if (expr.kind() != Expr::Kind::Call) return false;
  const std::string& callee = static_cast<const CallExpr&>(expr).callee();
  return callee == "file.open" || callee == "memory.allocate" ||
         callee == "memory.secure_allocate" || callee == "memory.reallocate" ||
         callee == "memory.map_file" || callee == "memory.shared_allocate" ||
         callee == "memory.shared_retain" || callee == "mutex.create" ||
         callee == "memory.owned_array" ||
         callee == "memory.owned_soa" ||
         callee == "memory.owned_array_aligned" || callee == "socket.connect";
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
  if (callee == "socket.connect") return "socket.close";
  if (callee == "memory.owned_array" ||
      callee == "memory.owned_array_aligned") return "array.owned_release";
  return {};
}

static std::string cleanup_for_owned_expression(const Expr& expression,
                                               const SymbolTable& symbols) {
  if (expression.kind() == Expr::Kind::Call) {
    const auto& call = static_cast<const CallExpr&>(expression);
    return cleanup_for_owner_producer(call.callee());
  }
  const Symbol* source = find_root_symbol(expression, symbols);
  if (!source || source->borrowed || source->shared_allocation) return {};
  return source->cleanup_callee.empty() && source->owns_allocation
             ? "memory.deallocate" : source->cleanup_callee;
}

static std::string cleanup_for_borrow_domain(const std::string& domain) {
  if (domain == "file") return "file.close";
  if (domain == "socket") return "socket.close";
  if (domain == "mutex") return "mutex.destroy";
  if (domain == "mapping") return "memory.unmap_file";
  if (domain == "secure") return "memory.secure_release";
  return {};
}

static std::string cleanup_for_return_domain(const std::string& domain) {
  return cleanup_for_borrow_domain(domain);
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
    validate_struct_field_contracts(decl->fields(), diagnostics_);
    for (const Parameter& field : decl->fields()) {
      const Type field_type = resolve_aggregate_type(field.type);
      if (contains_owned_array_with_owning_elements(field_type))
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
    if (contains_array_view(function->return_type()) &&
        !(function->return_type().is_array_view() &&
          !function->return_borrow_parameter().empty()))
      diagnostics_.error(diag::kTypeMismatch, function->location(),
                         "borrowed array views can only escape through a source-bound borrow return");
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
    sig.returns_borrowed = !function->return_borrow_parameter().empty();
    sig.return_resource_domain = function->return_resource_domain();
    sig.return_borrow_parameter = function->return_borrow_parameter();
    sig.returns_mutable_borrow = function->returns_mutable_borrow();
    for (const Parameter& param : function->parameters()) {
      sig.parameter_borrowed.push_back(param.borrowed);
      sig.parameter_mutable_borrow.push_back(param.mutable_borrow);
      sig.parameter_shared.push_back(param.shared);
      sig.parameter_resource_domains.push_back(param.resource_domain);
      sig.parameter_names.push_back(param.name);
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
      validate_struct_field_contracts(decl->fields(), diagnostics_);
      for (const Parameter& field : decl->fields()) {
        const Type field_type = resolve_aggregate_type(field.type);
        if (contains_owned_array_with_owning_elements(field_type))
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
      if (contains_array_view(function->return_type()) &&
          !(function->return_type().is_array_view() &&
            !function->return_borrow_parameter().empty()))
        diagnostics_.error(diag::kTypeMismatch, function->location(),
                           "borrowed array views can only escape through a source-bound borrow return");
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
      sig.returns_borrowed = !function->return_borrow_parameter().empty();
      sig.return_resource_domain = function->return_resource_domain();
      sig.return_borrow_parameter = function->return_borrow_parameter();
      sig.returns_mutable_borrow = function->returns_mutable_borrow();
      for (const Parameter& param : function->parameters()) {
        sig.parameter_borrowed.push_back(param.borrowed);
        sig.parameter_mutable_borrow.push_back(param.mutable_borrow);
        sig.parameter_shared.push_back(param.shared);
        sig.parameter_resource_domains.push_back(param.resource_domain);
        sig.parameter_names.push_back(param.name);
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
  current_return_resource_domain_ = function.return_resource_domain();
  current_return_borrow_parameter_ = function.return_borrow_parameter();
  current_return_mutable_borrow_ = function.returns_mutable_borrow();
  current_return_borrow_allocation_id_ = 0;
  if (current_return_shared_ && !current_return_type_.is_pointer()) {
    diagnostics_.error(diag::kTypeMismatch, function.location(),
                       "shared return types must be pointers");
  }
  if (!current_return_resource_domain_.empty() &&
      (!current_return_type_.is_pointer() || current_return_shared_)) {
    diagnostics_.error(diag::kTypeMismatch, function.location(),
                       "owned resource return types must be non-shared pointers");
  }
  if (!current_return_borrow_parameter_.empty() &&
      ((!current_return_type_.is_pointer() && !current_return_type_.is_array_view()) ||
       current_return_shared_ || !current_return_resource_domain_.empty())) {
    diagnostics_.error(diag::kTypeMismatch, function.location(),
                       "borrow returns require an unqualified pointer or array view");
  }
  loop_depth_ = 0;
  loop_break_states_.clear();
  loop_continue_states_.clear();
  loop_deferred_scope_depths_.clear();

  for (const Parameter& parameter : function.parameters()) {
    // Parameters are assignable inside the body. Nothing in the specification
    // makes them `let`, and there is no syntax to opt into mutability; treating
    // them as immutable would make it impossible to modify an array or struct
    // passed to a helper function at all.
    if (parameter.borrowed && !parameter.type.is_pointer() &&
        !parameter.type.is_array_view()) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "borrow parameters must have pointer or array view type");
    }
    if (parameter.mutable_borrow && !parameter.borrowed) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "'mut' requires a borrowed parameter");
    }
    if (!parameter.resource_domain.empty() && parameter.shared) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "resource-qualified parameters cannot be shared");
    }
    if (!parameter.resource_domain.empty() && !parameter.type.is_pointer()) {
      diagnostics_.error(diag::kTypeMismatch, parameter.location,
                         "resource-qualified parameters must have pointer type");
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
    symbol.cleanup_callee = cleanup_for_borrow_domain(parameter.resource_domain);
      symbol.owns_allocation = !symbol.borrowed;
      symbol.shared_allocation = parameter.shared;
    }
    if (!symbols_.declare(move(symbol))) {
      diagnostics_.error(diag::kDuplicateName, parameter.location,
                         "parameter '" + parameter.name + "' is already declared");
    } else if (parameter.name == current_return_borrow_parameter_) {
      const Symbol* source = symbols_.lookup(parameter.name);
      if (source && source->borrowed)
        current_return_borrow_allocation_id_ = source->allocation_id;
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
  current_return_resource_domain_.clear();
  current_return_borrow_parameter_.clear();
  current_return_borrow_allocation_id_ = 0;
  current_return_mutable_borrow_ = false;
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
         callee == "memory.unmap_file" || callee == "socket.close") &&
        cleanup->allocation_id != 0) {
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
       call->callee() == "memory.unmap_file" ||
       call->callee() == "socket.close") && !call->arguments().empty()) {
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
    if (is_null_pointer_literal(declaration.initializer()) && annotation.is_pointer()) {
      // Pointers are non-nullable by default, so holding nullptr requires '@Nullable'.
      if (!declaration.is_nullable()) {
        diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                           "a non-nullable pointer cannot be initialized with nullptr; "
                           "declare it '@Nullable'");
      }
      const_cast<Expr&>(declaration.initializer()).set_type(annotation);
    } else if (annotation.is_valid() && initializer_type.is_valid() &&
        annotation != initializer_type) {
      diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                         "cannot initialize '" + declaration.name() + "' of type '" +
                             annotation.to_string() + "' with a value of type '" +
                             initializer_type.to_string() + "'");
    }
    binding_type = annotation;
  }

  consume_field_move(const_cast<Expr&>(declaration.initializer()), binding_type);

  Symbol symbol{declaration.name(), binding_type, declaration.binding(),
                declaration.location()};
  symbol.nullable = declaration.is_nullable();
  if ((binding_type.is_owned_array() || contains_owned_array(binding_type)) &&
      declaration.initializer().kind() == Expr::Kind::Index) {
    diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                       "owned array elements cannot be moved out individually");
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
        if (!signature->returns_borrowed &&
            (signature->return_type.is_pointer() || signature->return_type.is_owned_array() ||
             contains_owned_array(signature->return_type))) {
          symbol.cleanup_callee = !signature->return_resource_domain.empty()
              ? cleanup_for_return_domain(signature->return_resource_domain)
              : cleanup_for_owner_producer(call.callee(), signature->returns_shared);
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
      symbol.owns_allocation = signature && !signature->returns_borrowed &&
          (signature->return_type.is_pointer() || signature->return_type.is_owned_array() ||
           contains_owned_array(signature->return_type));
      if (symbol.owns_allocation && symbol.cleanup_callee.empty() &&
          !signature->returns_shared)
        symbol.cleanup_callee = cleanup_for_type(signature->return_type);
        if (symbol.cleanup_callee.empty()) symbol.cleanup_callee = "memory.deallocate";
      if (signature && signature->returns_borrowed) {
        const auto source_parameter = std::find(
            signature->parameter_names.begin(), signature->parameter_names.end(),
            signature->return_borrow_parameter);
        if (source_parameter == signature->parameter_names.end()) {
          diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                             "borrow return refers to an unknown source parameter");
        } else {
          const size_t source_index = static_cast<size_t>(
              source_parameter - signature->parameter_names.begin());
          const Symbol* source = source_index < call.arguments().size()
              ? find_borrow_origin(*call.arguments()[source_index]) : nullptr;
          if (!source || source->moved || !source->allocation_id) {
            diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                               "borrow return requires a named live source argument");
          } else {
            symbol.borrowed = true;
            symbol.mutable_borrow = signature->returns_mutable_borrow;
            symbol.owns_allocation = false;
            symbol.allocation_id = source->allocation_id;
            symbol.cleanup_callee = source->cleanup_callee;
            if (symbols_.has_conflicting_borrow(
                    source->allocation_id, signature->returns_mutable_borrow,
                    source->name))
              diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                                 "returned borrow conflicts with an existing live borrow");
            if (signature->returns_mutable_borrow &&
                declaration.binding() != BindingKind::Var)
              diagnostics_.error(diag::kImmutableAssignment, declaration.location(),
                                 "a mutable returned borrow must be bound with 'var'");
          }
        }
      }
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
    symbol.mutable_borrow =
        declaration.initializer().ownership() == ValueOwnership::MutableBorrowed;
    if (symbol.mutable_borrow && declaration.binding() != BindingKind::Var)
      diagnostics_.error(diag::kImmutableAssignment, declaration.location(),
                         "a mutable array view must be bound with 'var'");
    if (declaration.initializer().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(declaration.initializer());
      if ((call.callee() == "memory.array_view" ||
           call.callee() == "memory.array_view_mut") &&
          !call.arguments().empty()) {
        if (const Symbol* source = find_root_symbol(*call.arguments()[0], symbols_))
          symbol.allocation_id = source->allocation_id;
      } else if (const FunctionSignature* signature = find_function(call.callee());
                 signature && signature->returns_borrowed) {
        const Symbol* source = find_borrow_origin(declaration.initializer());
        if (source && source->allocation_id != 0 && !source->moved) {
          symbol.allocation_id = source->allocation_id;
          if (symbols_.has_conflicting_borrow(source->allocation_id,
                                              signature->returns_mutable_borrow,
                                              source->name))
            diagnostics_.error(diag::kTypeMismatch, declaration.initializer().location(),
                               "returned array view conflicts with a live mutable borrow");
        }
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
  if (contains_shared_pointer(binding_type)) {
    symbol.shared_allocation = true;
    if (symbol.cleanup_callee.empty()) symbol.cleanup_callee = "aggregate.owned_drop";
  }
  const bool uninitialized_declaration =
      declaration.initializer().kind() == Expr::Kind::StructInit &&
      static_cast<const StructInitExpr&>(declaration.initializer()).is_uninitialized();
  if (uninitialized_declaration) {
    if (declaration.binding() != BindingKind::Var)
      diagnostics_.error(diag::kImmutableAssignment, declaration.location(),
                         "'uninit' requires a mutable 'var' binding");
    if (binding_type.is_struct() &&
        (contains_owned_array(binding_type) || contains_shared_pointer(binding_type))) {
      symbol.cleanup_callee = "aggregate.owned_drop";
      symbol.owns_allocation = true;
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
  // Every field starts uninitialized: it reads as a moved field until assigned,
  // and the zeroed slot makes destruction of the untouched fields a no-op.
  if (uninitialized_declaration && binding_type.is_struct()) {
    if (const StructInfo* info = find_struct(binding_type.struct_name())) {
      for (const Parameter& field : info->fields)
        symbols_.mark_field_uninitialized(declaration.name(), "." + field.name);
    } else {
      for (const auto& field : binding_type.struct_fields())
        symbols_.mark_field_uninitialized(declaration.name(), "." + field.first);
    }
  }
  if (const Symbol* declared = symbols_.lookup(new_name))
    declaration.set_cleanup_callee(declared->borrowed ? string{}
                                                      : declared->cleanup_callee);
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

bool TypeChecker::field_path(const Expr& expression, string& root,
                             string& path) const {
  if (expression.kind() == Expr::Kind::Name) {
    root = static_cast<const NameExpr&>(expression).name();
    return true;
  }
  if (expression.kind() != Expr::Kind::MemberAccess) return false;
  const auto& member = static_cast<const MemberAccessExpr&>(expression);
  if (!field_path(member.base(), root, path)) return false;
  path += ".";
  path += member.is_tuple_access() ? to_string(member.tuple_index())
                                   : member.member();
  return true;
}

const Symbol* TypeChecker::find_borrow_origin(const Expr& expression) const {
  if (expression.kind() != Expr::Kind::Call)
    return find_root_symbol(expression, symbols_);
  const auto& call = static_cast<const CallExpr&>(expression);
  // A view constructor does not create a new lifetime. Preserve the origin of
  // the source through nested slices so a function can safely return a view
  // derived from its declared borrowed parameter.
  if ((call.callee() == "memory.array_view" ||
       call.callee() == "memory.array_view_mut") && !call.arguments().empty())
    return find_borrow_origin(*call.arguments().front());
  const FunctionSignature* signature = find_function(call.callee());
  if (!signature || !signature->returns_borrowed) return nullptr;
  const auto source_parameter = std::find(
      signature->parameter_names.begin(), signature->parameter_names.end(),
      signature->return_borrow_parameter);
  if (source_parameter == signature->parameter_names.end()) return nullptr;
  const size_t source_index = static_cast<size_t>(
      source_parameter - signature->parameter_names.begin());
  if (source_index >= call.arguments().size()) return nullptr;
  return find_borrow_origin(*call.arguments()[source_index]);
}

void TypeChecker::consume_field_move(Expr& expression, const Type& value_type) {
  if (!contains_owned_array(value_type) ||
      expression.kind() != Expr::Kind::MemberAccess) return;
  string root;
  string path;
  if (!field_path(expression, root, path)) return;
  const Symbol* owner = symbols_.lookup(root);
  if (!owner || !owner->owns_allocation || owner->moved) {
    diagnostics_.error(diag::kTypeMismatch, expression.location(),
                       "owned field moves require a live aggregate owner");
    return;
  }
  if (symbols_.field_is_moved(root, path)) {
    diagnostics_.error(diag::kUseAfterFree, expression.location(),
                       "use of an aggregate field after it was moved");
    return;
  }
  if (symbols_.field_has_moved_descendant(root, path)) {
    diagnostics_.error(diag::kTypeMismatch, expression.location(),
                       "cannot move an aggregate field containing a moved field");
    return;
  }
  if (symbols_.has_live_borrow(owner->allocation_id)) {
    diagnostics_.error(diag::kTypeMismatch, expression.location(),
                       "cannot move an owned field while an array view is live");
    return;
  }
  expression.set_ownership(ValueOwnership::Owned);
  expression.set_transfers_ownership();
  symbols_.mark_field_moved(root, path);
}

static bool has_cleanup_domain(const Expr& expr, const SymbolTable& symbols,
                              const std::string& expected,
                              bool allow_borrow = false) {
  if (expr.kind() == Expr::Kind::Call) {
    const auto& call = static_cast<const CallExpr&>(expr);
    if (cleanup_for_owner_producer(call.callee()) == expected) return true;
  }
  const Symbol* source = find_root_symbol(expr, symbols);
  if (!source) return false;
  if (source->borrowed)
    return allow_borrow && source->cleanup_callee == expected;
  if (source->shared_allocation) return false;
  const std::string actual = source->cleanup_callee.empty() &&
                                     source->owns_allocation
                                 ? "memory.deallocate"
                                 : source->cleanup_callee;
  return actual == expected;
}

void TypeChecker::check_assignment(AssignmentStmt& assignment) {
  const Type value_type = check_expression(const_cast<Expr&>(assignment.value()));
  const bool was_checking_lvalue = checking_lvalue_;
  checking_lvalue_ = true;
  const Type target_type = assignment.is_simple_name()
      ? (symbols_.lookup(assignment.name())
             ? symbols_.lookup(assignment.name())->type : Type::error())
      : check_expression(const_cast<Expr&>(assignment.target()));
  checking_lvalue_ = was_checking_lvalue;

  // 17.1: a write through an immutable borrowed field is a mutation of the borrowed
  // owner, so only `borrow mut` fields allow it.
  if (assignment.target().kind() == Expr::Kind::Index) {
    const Expr& index_base =
        static_cast<const IndexExpr&>(assignment.target()).base();
    if (index_base.kind() == Expr::Kind::MemberAccess) {
      const auto& member = static_cast<const MemberAccessExpr&>(index_base);
      if (member.base().type().is_struct()) {
        const StructInfo* info =
            find_struct(member.base().type().struct_name());
        if (info != nullptr) {
          for (const Parameter& candidate : info->fields) {
            if (candidate.name != member.member()) continue;
            if (candidate.borrowed && !candidate.mutable_borrow) {
              diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                                 "cannot write through borrowed field '" +
                                     candidate.name + "'; declare it 'borrow mut'");
              return;
            }
          }
        }
      }
    }
  }

  string field_owner;
  string field_name;
  const bool assigning_owned_field =
      assignment.target().kind() == Expr::Kind::MemberAccess &&
      contains_owned_array(target_type) &&
      field_path(assignment.target(), field_owner, field_name);
  if ((assignment.target().kind() == Expr::Kind::MemberAccess ||
       assignment.target().kind() == Expr::Kind::Index) &&
      (target_type.is_owned_array() || contains_owned_array(target_type)) &&
      !assigning_owned_field) {
    diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                       "owned fields can only be reinitialized by assigning to a moved field path");
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

  // Assigning a non-owned field initializes it, clearing that field's subtree so
  // later reads are allowed. Owned fields clear this on their reinit path, after
  // the moved/not-yet-initialized precondition has been checked.
  if (assignment.target().kind() == Expr::Kind::MemberAccess &&
      !contains_owned_array(target_type)) {
    string field_root;
    string field_target;
    if (field_path(assignment.target(), field_root, field_target))
      symbols_.clear_field_uninitialized(field_root, field_target);
  }
  const Symbol* symbol = find_root_symbol(assignment.target(), symbols_);
  if (assignment.target().kind() == Expr::Kind::Index &&
      static_cast<const IndexExpr&>(assignment.target()).base().type().is_array_view()) {
    const Expr& view_expr =
        static_cast<const IndexExpr&>(assignment.target()).base();
    const Symbol* view = find_root_symbol(view_expr, symbols_);
    if (!view || !view->mutable_borrow) {
      diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                         "array views are read-only; use a mutable array view for indexed writes");
      return;
    }
  }
  if (assignment.is_simple_name() && symbol && symbol->type.is_array_view()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "borrowed array view bindings cannot be reassigned");
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
  if (assignment.target().kind() == Expr::Kind::MemberAccess &&
      contains_owned_array(target_type)) {
    const Symbol* owner = symbols_.lookup(field_owner);
    if (!owner || !owner->owns_allocation || owner->moved ||
        !owner->is_mutable()) {
      diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                         "owned field reinitialization requires a live mutable aggregate owner");
      return;
    }
    if (!symbols_.field_is_definitely_moved(field_owner, field_name) &&
        !symbols_.field_is_uninitialized(field_owner, field_name)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                         "owned field reinitialization requires the field to be moved on every control-flow path");
      return;
    }
    if (symbols_.field_has_moved_descendant(field_owner, field_name)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                         "cannot reinitialize an aggregate field containing a moved field");
      return;
    }
    if (symbols_.has_live_borrow(owner->allocation_id)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.target().location(),
                         "cannot reinitialize an owned field while an array view is live");
      return;
    }
    // Reinitializing an owned pointer field accepts a fresh owner, the same coercion
    // struct construction performs, not only an already-owned pointer value.
    const bool owned_pointer_coercion =
        target_type.is_owned_pointer() && value_type.is_pointer() &&
        !value_type.is_owned_pointer() &&
        value_type.element_type() == target_type.element_type();
    if ((value_type != target_type && !owned_pointer_coercion) ||
        (assignment.value().ownership() != ValueOwnership::Owned &&
         assignment.value().kind() != Expr::Kind::MemberAccess &&
         assignment.value().kind() != Expr::Kind::Name)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                         "owned field reinitialization requires an owned value of the same type");
      return;
    }
    const Symbol* source = find_root_symbol(assignment.value(), symbols_);
    if (assignment.value().kind() == Expr::Kind::Name &&
        (!source || !source->owns_allocation || source->moved ||
         source->borrowed || source->shared_allocation)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                         "owned field reinitialization requires a live owned source value");
      return;
    }
    if (source && source->owns_allocation &&
        symbols_.has_live_borrow(source->allocation_id)) {
      diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                         "cannot move an owner while an array view is live");
      return;
    }
    consume_field_move(const_cast<Expr&>(assignment.value()), target_type);
    if (assignment.value().kind() == Expr::Kind::Name && source &&
        source->owns_allocation && !source->moved) {
      const_cast<Expr&>(assignment.value()).set_transfers_ownership();
      symbols_.mark_allocation_moved(source->allocation_id);
    }
    symbols_.clear_field_moved(field_owner, field_name);
    symbols_.clear_field_uninitialized(field_owner, field_name);
    return;
  }
  if (symbol != nullptr && symbol->borrowed && assignment.is_simple_name()) {
    diagnostics_.error(diag::kImmutableAssignment, assignment.target().location(),
                       "a borrowed binding cannot be rebound");
    return;
  }
  // Any assignment can change what a name refers to, so no earlier non-null proof
  // survives it.
  nonnull_proven_.clear();
  if (is_null_pointer_literal(assignment.value()) && target_type.is_pointer()) {
    // `p = nullptr;` retypes the literal to the destination pointer type.
    const_cast<Expr&>(assignment.value()).set_type(target_type);
  } else if (target_type.is_valid() && value_type.is_valid() &&
             target_type != value_type &&
             !(target_type.is_shared_pointer() && value_type.is_pointer() &&
               !value_type.is_shared_pointer() &&
               value_type.element_type() == target_type.element_type())) {
    diagnostics_.error(diag::kTypeMismatch, assignment.value().location(),
                       "cannot assign a value of type '" + value_type.to_string() +
                           "' to target of type '" + target_type.to_string() + "'");
  }
  consume_field_move(const_cast<Expr&>(assignment.value()), target_type);
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
        cleanup_callee = signature && !signature->return_resource_domain.empty()
            ? cleanup_for_return_domain(signature->return_resource_domain)
            : cleanup_for_owner_producer(
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

  // A target such as `*p` has no single named owner, so find_root_symbol can
  // legitimately return null; the operator rules then apply to the target's own
  // type. Reading symbol->type unconditionally here crashed the compiler on
  // compound assignment through a dereference.
  const Type operator_type = symbol ? symbol->type : target_type;

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
      if (!operator_type.is_integer()) {
        diagnostics_.error(diag::kTypeMismatch, assignment.location(),
                           "operator '" + string(binary_op_spelling(assignment.op())) +
                               "=' requires integer operands, found '" + operator_type.to_string() + "'");
      }
      break;
    case BinaryOp::Add:
    case BinaryOp::Subtract:
    case BinaryOp::Multiply:
    case BinaryOp::Divide:
    case BinaryOp::Exponentiate:
      if (!operator_type.is_numeric()) {
        diagnostics_.error(diag::kTypeMismatch, assignment.location(),
                           "operator '" + string(binary_op_spelling(assignment.op())) +
                               "=' requires numeric operands, found '" + operator_type.to_string() + "'");
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
  // Flow-sensitive narrowing: `p is not nullptr` proves non-null in the
  // then-branch; `p is nullptr` proves it in the else-branch and, when the
  // then-branch cannot fall through, after the statement.
  std::string narrowed_name;
  bool then_nonnull = false;
  bool else_nonnull = false;
  {
    const Expr& condition = statement.condition();
    if (condition.kind() == Expr::Kind::Binary) {
      const auto& binary = static_cast<const BinaryExpr&>(condition);
      if (binary.op() == BinaryOp::Equal || binary.op() == BinaryOp::NotEqual) {
        const Expr* other = nullptr;
        const Expr* named = nullptr;
        if (binary.lhs().kind() == Expr::Kind::Name) {
          named = &binary.lhs();
          other = &binary.rhs();
        } else if (binary.rhs().kind() == Expr::Kind::Name) {
          named = &binary.rhs();
          other = &binary.lhs();
        }
        if (named && other && is_null_pointer_literal(*other)) {
          narrowed_name = static_cast<const NameExpr&>(*named).name();
          if (binary.op() == BinaryOp::NotEqual) then_nonnull = true;
          else else_nonnull = true;
        }
      }
    }
  }
  const std::unordered_set<std::string> incoming_proven = nonnull_proven_;
  const SymbolTable incoming = symbols_;
  if (then_nonnull) nonnull_proven_.insert(narrowed_name);
  check_block(const_cast<BlockStmt&>(statement.then_branch()));
  const std::unordered_set<std::string> then_proven = nonnull_proven_;
  const SymbolTable then_state = symbols_;
  symbols_ = incoming;
  nonnull_proven_ = incoming_proven;
  if (else_nonnull) nonnull_proven_.insert(narrowed_name);
  if (statement.has_else()) {
    check_statement(const_cast<Stmt&>(*statement.else_branch()));
  }
  const std::unordered_set<std::string> else_proven = nonnull_proven_;
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
  nonnull_proven_ = incoming_proven;
  if (!(then_falls_through && else_falls_through)) {
    if (then_falls_through) nonnull_proven_ = then_proven;
    else if (else_falls_through) nonnull_proven_ = else_proven;
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
  loop_break_states_.emplace_back();
  loop_continue_states_.emplace_back();
  loop_deferred_scope_depths_.push_back(deferred_calls_.size());
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
  std::vector<SymbolTable> break_states = std::move(loop_break_states_.back());
  loop_break_states_.pop_back();
  std::vector<SymbolTable> continue_states = std::move(loop_continue_states_.back());
  loop_continue_states_.pop_back();
  loop_deferred_scope_depths_.pop_back();
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
  for (const SymbolTable& continue_state : continue_states) {
    for (const string& name : continue_state.owners_moved_since(condition_state))
      diagnostics_.error(diag::kTypeMismatch, statement.location(),
                         "owner '" + name +
                             "' cannot remain moved at a loop back-edge; reinitialize it before the next iteration");
  }
  symbols_ = condition_state;
  if (body_falls_through)
    symbols_.merge_conditional_ownership(body_state, condition_state);
  for (const SymbolTable& continue_state : continue_states) {
    const SymbolTable merged = symbols_;
    symbols_.merge_conditional_ownership(merged, continue_state);
  }
  for (const SymbolTable& break_state : break_states) {
    const SymbolTable merged = symbols_;
    symbols_.merge_conditional_ownership(merged, break_state);
  }
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
  loop_break_states_.emplace_back();
  loop_continue_states_.emplace_back();
  loop_deferred_scope_depths_.push_back(deferred_calls_.size());
  ++loop_depth_;
  check_block(const_cast<BlockStmt&>(statement.body()));
  --loop_depth_;
  std::vector<SymbolTable> continue_states = std::move(loop_continue_states_.back());
  loop_continue_states_.back().clear();
  const SymbolTable body_state = symbols_;
  const bool body_falls_through = block_falls_through(statement.body());
  const bool has_iteration_path = body_falls_through || !continue_states.empty();
  symbols_ = body_falls_through ? body_state :
      continue_states.empty() ? condition_state : continue_states.front();
  for (size_t index = body_falls_through ? 0 : 1;
       index < continue_states.size(); ++index) {
    const SymbolTable merged = symbols_;
    symbols_.merge_conditional_ownership(merged, continue_states[index]);
  }
  if (statement.has_step()) {
    check_statement(const_cast<Stmt&>(*statement.step()));
  }
  const SymbolTable loop_back_state = symbols_;
  const bool step_falls_through = !statement.has_step() ||
      statement_can_fall_through(*statement.step());
  if (has_iteration_path && step_falls_through) {
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
  std::vector<SymbolTable> break_states = std::move(loop_break_states_.back());
  loop_break_states_.pop_back();
  std::vector<SymbolTable> step_continue_states =
      std::move(loop_continue_states_.back());
  loop_continue_states_.pop_back();
  loop_deferred_scope_depths_.pop_back();
  for (const SymbolTable& continue_state : step_continue_states) {
    for (const string& name : continue_state.owners_moved_since(condition_state))
      diagnostics_.error(diag::kTypeMismatch, statement.location(),
                         "owner '" + name +
                             "' cannot remain moved at a loop back-edge; reinitialize it before the next iteration");
    const SymbolTable merged = symbols_;
    symbols_.merge_conditional_ownership(merged, continue_state);
  }
  for (const SymbolTable& break_state : break_states) {
    const SymbolTable merged = symbols_;
    symbols_.merge_conditional_ownership(merged, break_state);
  }
  symbols_.pop_scope();
}

void TypeChecker::check_break(BreakStmt& statement) {
  if (loop_depth_ <= 0) {
    diagnostics_.error(diag::kBreakOutsideLoop, statement.location(),
                       "'break' is only allowed inside an enclosing loop");
  } else if (!loop_break_states_.empty()) {
    loop_break_states_.back().push_back(loop_exit_state_after_defer());
  }
}

void TypeChecker::check_continue(ContinueStmt& statement) {
  if (loop_depth_ <= 0) {
    diagnostics_.error(diag::kContinueOutsideLoop, statement.location(),
                       "'continue' is only allowed inside an enclosing loop");
  } else if (!loop_continue_states_.empty()) {
    loop_continue_states_.back().push_back(loop_exit_state_after_defer());
  }
}

SymbolTable TypeChecker::loop_exit_state_after_defer() const {
  SymbolTable state = symbols_;
  if (loop_deferred_scope_depths_.empty()) return state;
  const size_t first_scope = loop_deferred_scope_depths_.back();
  for (size_t scope_index = deferred_calls_.size();
       scope_index > first_scope; --scope_index) {
    const auto& cleanups = deferred_calls_[scope_index - 1];
    for (auto cleanup = cleanups.rbegin(); cleanup != cleanups.rend(); ++cleanup) {
      if (cleanup->allocation_id == 0 || !cleanup->call) continue;
      const string& callee = cleanup->call->callee();
      const bool releases_owner = callee == "memory.deallocate" ||
          callee == "memory.shared_release" || callee == "array.owned_release" ||
          callee == "file.close" || callee == "mutex.destroy" ||
          callee == "memory.secure_release" || callee == "memory.unmap_file" ||
          callee == "socket.close";
      if (!releases_owner || state.allocation_is_moved(cleanup->allocation_id))
        continue;
      if (callee == "array.owned_release" &&
          state.has_live_borrow(cleanup->allocation_id))
        continue;
      state.mark_allocation_moved(cleanup->allocation_id);
    }
  }
  return state;
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
  consume_field_move(const_cast<Expr&>(*statement.value()), current_return_type_);
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
  const bool returns_borrowed = !current_return_borrow_parameter_.empty();
  if (returns_borrowed) {
    const Symbol* source = find_borrow_origin(*statement.value());
    if (!current_return_borrow_allocation_id_ || !source || !source->borrowed ||
        source->allocation_id != current_return_borrow_allocation_id_) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "returned borrow must come from the declared source parameter '" +
                             current_return_borrow_parameter_ + "'");
    } else if (!current_return_mutable_borrow_ && source->mutable_borrow) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "an immutable returned borrow cannot alias a mutable borrowed source");
    } else if (current_return_mutable_borrow_ &&
               (statement.value()->ownership() != ValueOwnership::MutableBorrowed ||
                !source->mutable_borrow)) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "mutable borrow return requires the declared mutable source");
    } else {
      const_cast<Expr&>(*statement.value()).set_ownership(
          current_return_mutable_borrow_ ? ValueOwnership::MutableBorrowed
                                         : ValueOwnership::Borrowed);
    }
  }
  if (contains_array_view(value_type) && !returns_borrowed) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "borrowed array views can only escape through a source-bound borrow return");
  }
  if (current_return_type_.is_pointer() && !current_return_shared_ &&
      !returns_borrowed) {
    std::string cleanup_callee;
    if (statement.value()->kind() == Expr::Kind::Name) {
      const Symbol* source = find_root_symbol(*statement.value(), symbols_);
      if (source) cleanup_callee = source->cleanup_callee;
    } else if (statement.value()->kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(*statement.value());
      const FunctionSignature* signature = find_function(call.callee());
      cleanup_callee = signature && !signature->return_resource_domain.empty()
          ? cleanup_for_return_domain(signature->return_resource_domain)
          : cleanup_for_owner_producer(call.callee());
    }
    const std::string expected_cleanup =
        cleanup_for_return_domain(current_return_resource_domain_);
    if (!cleanup_callee.empty() && cleanup_callee != "memory.deallocate" &&
        cleanup_callee != expected_cleanup) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "returned resource does not match the function's owned resource domain");
    } else if (!cleanup_callee.empty() && cleanup_callee != "memory.deallocate" &&
               current_return_resource_domain_.empty()) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "specialized owned resources require an 'owned <resource>' return type");
    } else if (!current_return_resource_domain_.empty() &&
               cleanup_callee != expected_cleanup) {
      diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                         "owned resource return requires a value from the declared resource domain");
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
             !returns_borrowed &&
             statement.value()->ownership() != ValueOwnership::Owned) {
    diagnostics_.error(diag::kTypeMismatch, statement.value()->location(),
                       "owned pointer return requires an owned pointer; borrowed or raw pointers cannot escape");
  }
  if ((current_return_type_.is_pointer() || current_return_type_.is_owned_array() ||
       contains_owned_array(current_return_type_)) &&
      statement.value()->kind() == Expr::Kind::Name && !returns_borrowed) {
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
        else if (signature && signature->returns_borrowed)
          expr.set_ownership(signature->returns_mutable_borrow
                                 ? ValueOwnership::MutableBorrowed
                                 : ValueOwnership::Borrowed);
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
        bool mutable_view = false;
        if (expr.kind() == Expr::Kind::Call) {
          const auto& call = static_cast<const CallExpr&>(expr);
          const FunctionSignature* signature = find_function(call.callee());
          mutable_view = call.callee() == "memory.array_view_mut" ||
                         (signature && signature->returns_mutable_borrow);
        } else if (expr.kind() == Expr::Kind::Name) {
          const Symbol* symbol = symbols_.lookup(
              static_cast<const NameExpr&>(expr).name());
          mutable_view = symbol && symbol->mutable_borrow;
        }
        expr.set_ownership(
            mutable_view ? ValueOwnership::MutableBorrowed
                         : ValueOwnership::Borrowed);
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
        case UnaryOp::AddressOf: {
          // §8.2: only expressions with a stable storage location have an address.
          const Expr::Kind operand_kind = unary.operand().kind();
          const bool addressable =
              operand_kind == Expr::Kind::Name ||
              operand_kind == Expr::Kind::MemberAccess ||
              operand_kind == Expr::Kind::Index ||
              (operand_kind == Expr::Kind::Unary &&
               static_cast<const UnaryExpr&>(unary.operand()).op() ==
                   UnaryOp::Dereference);
          if (!addressable) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "operator '&' requires a variable, field, element or "
                               "dereference with a stable address");
            type = Type::error();
          } else if (!operand_type.is_numeric() && !operand_type.is_bool() &&
                     !operand_type.is_pointer()) {
            // Aggregates, owned containers and managed references are not
            // addressable yet: a raw alias into them could bypass deterministic
            // destruction or the collector's write barrier. Rejecting them keeps
            // the safety claim honest instead of exposing an unchecked pointer.
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "taking the address of '" + operand_type.to_string() +
                                   "' is not supported yet");
            type = Type::error();
          } else {
            type = Type::pointer_type(operand_type);
          }
          break;
        }
        case UnaryOp::Dereference: {
          if (!operand_type.is_pointer() || operand_type.is_managed_pointer()) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "operator '*' requires a raw pointer operand, not '" +
                                   operand_type.to_string() + "'");
            type = Type::error();
            break;
          }
          const Symbol* pointer_symbol = nullptr;
          if (unary.operand().kind() == Expr::Kind::Name)
            pointer_symbol = symbols_.lookup(
                static_cast<const NameExpr&>(unary.operand()).name());
          if (pointer_symbol && pointer_symbol->nullable &&
              nonnull_proven_.count(pointer_symbol->name) == 0) {
            diagnostics_.error(diag::kTypeMismatch, unary.location(),
                               "'*' requires a non-null pointer; check '" +
                                   pointer_symbol->name + "' with 'is nullptr' first");
            type = Type::error();
          } else {
            type = operand_type.element_type();
          }
          break;
        }
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
          if (is_null_pointer_literal(binary.lhs()) ||
              is_null_pointer_literal(binary.rhs())) {
            // `nullptr` adopts the other operand's pointer type, so `p is nullptr`
            // compares two pointers rather than a pointer and an integer zero.
            const Type& other = is_null_pointer_literal(binary.lhs()) ? rhs_type : lhs_type;
            if (!other.is_pointer()) {
              diagnostics_.error(diag::kTypeMismatch, binary.location(),
                                 "nullptr can only be compared with a pointer, not '" +
                                     other.to_string() + "'");
              type = Type::error();
              break;
            }
            if (is_null_pointer_literal(binary.lhs()))
              const_cast<Expr&>(binary.lhs()).set_type(other);
            else
              const_cast<Expr&>(binary.rhs()).set_type(other);
            type = Type::bool_type();
            break;
          }
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
  // A container's element is a struct, and its fields are the container's shape:
  // leaving them unresolved would make describe_soa see a fieldless type and report
  // zero columns.
  if (type.is_soa())
    return Type::soa_type(resolve_aggregate_type(type.element_type(), active));
  if (type.is_owned_array())
    return Type::owned_array_type(resolve_aggregate_type(type.element_type(), active));
  if (type.is_owned_pointer())
    return Type::owned_pointer_type(resolve_aggregate_type(type.element_type(), active));
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
      consume_field_move(value, elem_type);
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
      consume_field_move(value, t);
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

  // A container element has no array-of-structs address: its fields live in separate
  // columns. The type is still the element struct so a member access can name a
  // column, but a bare `ps[i]` stays an error because there is nothing to load and
  // nothing to assign.
  if (base_type.is_soa()) {
    if (!checking_soa_column_base_) {
      diagnostics_.error(diag::kTypeMismatch, expr.location(),
                         "an element of a container has no value; address a column as ps[i].field");
      return Type::error();
    }
    return base_type.soa_element();
  }

  if (!checking_lvalue_ && base_type.is_owned_array()) {
    const Symbol* owner = find_root_symbol(expr.base(), symbols_);
    if (owner && symbols_.has_conflicting_borrow(owner->allocation_id, false)) {
      diagnostics_.error(diag::kTypeMismatch, expr.base().location(),
                         "cannot read an owned array while a mutable view is live");
      return Type::error();
    }
  }

  if (base_type.is_pointer()) {
    std::string cleanup_callee;
    const Symbol* owner = find_root_symbol(expr.base(), symbols_);
    if (owner) {
      cleanup_callee = owner->cleanup_callee;
    } else if (expr.base().kind() == Expr::Kind::Call) {
      const auto& call = static_cast<const CallExpr&>(expr.base());
      cleanup_callee = cleanup_for_owner_producer(call.callee());
      if (cleanup_callee.empty()) {
        const FunctionSignature* signature = find_function(call.callee());
        if (signature && !signature->return_resource_domain.empty())
          cleanup_callee = cleanup_for_return_domain(
              signature->return_resource_domain);
      }
    }
    if (!cleanup_callee.empty() && cleanup_callee != "memory.deallocate" &&
        cleanup_callee != "memory.secure_release") {
      diagnostics_.error(diag::kTypeMismatch, expr.base().location(),
                         "opaque resource handles cannot be indexed; use the "
                         "resource API or checked mapped accessors");
      return Type::error();
    }
  }

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
  // `ps[i].field` is the only way to reach a container element: the element has no
  // address and no value of its own, so the index expression is claimed here rather
  // than being checked as an expression in its own right.
  const bool enclosing_soa_column = checking_soa_column_base_;
  checking_soa_column_base_ = expr.base().kind() == Expr::Kind::Index;
  const Type base_type = check_expression(const_cast<Expr&>(expr.base()));
  checking_soa_column_base_ = enclosing_soa_column;
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
    const Type field_type = elems[static_cast<size_t>(idx)];
    string root;
    string path;
    if (!checking_lvalue_ && field_path(expr, root, path) &&
        symbols_.field_is_moved(root, path))
      diagnostics_.error(diag::kUseAfterFree, expr.location(),
                         "use of an aggregate field after it was moved");
    if (contains_owned_array(field_type))
      expr.set_ownership(ValueOwnership::Owned);
    return field_type;
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
      if (field.name == expr.member()) {
        const Type field_type = resolve_aggregate_type(field.type);
        string root;
        string path;
        if (!checking_lvalue_ && field_path(expr, root, path) &&
            symbols_.field_is_moved(root, path)) {
          diagnostics_.error(diag::kUseAfterFree, expr.location(),
                             "use of an aggregate field after it was moved");
        }
        string uninitialized_root;
        string uninitialized_path;
        if (!checking_lvalue_ &&
            field_path(expr, uninitialized_root, uninitialized_path) &&
            symbols_.field_is_uninitialized(uninitialized_root, uninitialized_path)) {
          diagnostics_.error(diag::kUseAfterFree, expr.location(),
                             "read of uninitialized aggregate field '" +
                                 uninitialized_path + "'");
        }
        if (contains_owned_array(field_type))
          expr.set_ownership(ValueOwnership::Owned);
        return field_type;
      }
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
  if (expr.is_uninitialized()) {
    if (find_struct(expr.struct_name()) == nullptr) {
      diagnostics_.error(diag::kTypeMismatch, expr.location(),
                         "unknown struct type '" + expr.struct_name() + "'");
      return Type::error();
    }
    return resolve_aggregate_type(Type::struct_type(expr.struct_name()));
  }
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
      consume_field_move(const_cast<Expr&>(*expr.positional_inits()[i]), field_type);
      if (init_type.is_valid() && init_type != field_type &&
          !(field_type.is_owned_pointer() && init_type.is_pointer() &&
            !init_type.is_owned_pointer() &&
            init_type.element_type() == field_type.element_type()) &&
          !(field_type.is_shared_pointer() && init_type.is_pointer() &&
            !init_type.is_shared_pointer() &&
            init_type.element_type() == field_type.element_type())) {
        diagnostics_.error(diag::kTypeMismatch, expr.positional_inits()[i]->location(),
                           "field '" + fields[i].name + "' of struct '" +
                               expr.struct_name() + "' has type '" +
                                field_type.to_string() + "', but was given '" +
                               init_type.to_string() + "'");
      }
      const ValueOwnership positional_ownership =
          expr.positional_inits()[i]->ownership();
      if (borrowed_field_value_violation(fields[i], *expr.positional_inits()[i])) {
        diagnostics_.error(diag::kTypeMismatch,
                           expr.positional_inits()[i]->location(),
                           "borrowed field '" + fields[i].name +
                               "' must be initialized with a borrow");
      } else if (!fields[i].borrowed &&
                 has_untracked_pointer_ownership(fields[i].type, positional_ownership)) {
        diagnostics_.error(diag::kTypeMismatch,
                           expr.positional_inits()[i]->location(),
                           "owned, shared, and borrowed pointers cannot be stored in aggregate fields");
      }
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
                 init_type != resolve_aggregate_type(field_def->type) &&
                 !(resolve_aggregate_type(field_def->type).is_owned_pointer() &&
                   init_type.is_pointer() && !init_type.is_owned_pointer() &&
                   init_type.element_type() ==
                       resolve_aggregate_type(field_def->type).element_type()) &&
                 !(resolve_aggregate_type(field_def->type).is_shared_pointer() &&
                   init_type.is_pointer() && !init_type.is_shared_pointer() &&
                   init_type.element_type() ==
                       resolve_aggregate_type(field_def->type).element_type())) {
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "field '" + field_name + "' of struct '" + expr.struct_name() +
                           "' has type '" + resolve_aggregate_type(field_def->type).to_string() +
                               "', but was given '" + init_type.to_string() + "'");
      }
      if (field_def)
        consume_field_move(const_cast<Expr&>(*kv.second),
                           resolve_aggregate_type(field_def->type));
      if (field_def && borrowed_field_value_violation(*field_def, *kv.second)) {
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "borrowed field '" + field_def->name +
                               "' must be initialized with a borrow");
      } else if (field_def &&
                 !field_def->borrowed &&
                 has_untracked_pointer_ownership(field_def->type,
                                                 kv.second->ownership())) {
        diagnostics_.error(diag::kTypeMismatch, kv.second->location(),
                           "owned, shared, and borrowed pointers cannot be stored in aggregate fields");
      }
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
  if ((call.callee() == "memory.array_view" ||
       call.callee() == "memory.array_view_mut") &&
      call.has_generic_argument()) {
    const bool mutable_view = call.callee() == "memory.array_view_mut";
    if (call.arguments().size() != 3) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "array view constructor takes a source, start, and length");
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
    const bool valid_mutable_source =
        source && source->is_mutable() &&
        (source->owns_allocation || (source->borrowed && source->mutable_borrow));
    if (!source || source->allocation_id == 0 || source->moved ||
        (!source->owns_allocation && !source->borrowed)) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "array views require a live tracked owner");
    } else if (mutable_view && !valid_mutable_source) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "mutable array views require a mutable owned array");
    } else if (mutable_view &&
               symbols_.has_conflicting_borrow(source->allocation_id, true,
                                               source->borrowed ? source->name : std::string{})) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "mutable array view conflicts with another live borrow");
    } else if (!mutable_view && source->mutable_borrow && source->borrowed) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "cannot create an immutable alias from a live mutable array view");
    } else if (!mutable_view &&
               symbols_.has_conflicting_borrow(source->allocation_id, false,
                                               source->name)) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "array view conflicts with a live mutable borrow");
    }
    return Type::array_view_type(element_type);
  }
  // `memory.owned_soa<T>(n)` is the only way to create a container, so the
  // restrictions here are what keep the runtime safe: no reference columns until
  // the strided trace callback exists, and a struct element only.
  if (call.callee() == "memory.owned_soa" && call.has_generic_argument()) {
    Type element = resolve_aggregate_type(call.generic_argument());
    if (!element.is_struct()) {
      diagnostics_.error(diag::kTypeMismatch, call.location(),
                         "soa<T> requires a struct type, found " + element.to_string());
      return Type::error();
    }
    if (call.arguments().size() != 1) {
      diagnostics_.error(diag::kTypeMismatch, call.location(),
                         "'memory.owned_soa<T>' takes an element count");
      return Type::error();
    }
    for (const auto& field : element.struct_fields()) {
      if (contains_owned_array(field.second) || contains_array_view(field.second)) {
        diagnostics_.error(
            diag::kTypeMismatch, call.location(),
            "soa<" + element.struct_name() + "> cannot own column '" + field.first +
                "': columns cannot hold owned arrays or array views yet");
        return Type::error();
      }
    }
    return Type::soa_type(element);
  }
  if ((call.callee() == "memory.owned_array" ||
       call.callee() == "memory.owned_array_aligned") &&
      call.has_generic_argument()) {
    const bool aligned = call.callee() == "memory.owned_array_aligned";
    const size_t minimum_arguments = aligned ? 2 : 1;
    const size_t maximum_arguments = minimum_arguments + 1;
    if (call.arguments().size() < minimum_arguments ||
        call.arguments().size() > maximum_arguments) {
      diagnostics_.error(diag::kPrintArity, call.location(),
                         aligned
                             ? "'memory.owned_array_aligned<T>' takes an element count, an alignment, and an optional allocator handle"
                             : "'memory.owned_array<T>' takes an element count and an optional allocator handle");
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
      if (call.arguments().size() == maximum_arguments) {
        const size_t allocator_index = maximum_arguments - 1;
        const Type allocator = check_expression(
            const_cast<Expr&>(*call.arguments()[allocator_index]));
        if (allocator.is_valid() && !allocator.is_integer())
          diagnostics_.error(diag::kTypeMismatch,
                             call.arguments()[allocator_index]->location(),
                             "owned array allocator handle must be an integer");
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
    else if (call.callee() != "mutex.destroy" &&
             !has_cleanup_domain(*call.arguments()[0], symbols_,
                                 "mutex.destroy", true))
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "mutex operation requires a mutex resource or borrowed mutex handle");
    else if (call.callee() != "mutex.destroy" && !mutex)
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "mutex operations require a named owner or borrowed handle");
    if (call.callee() == "mutex.destroy") {
      if (!mutex || mutex->cleanup_callee != "mutex.destroy") {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "mutex.destroy requires an owned mutex handle");
      } else if (mutex->borrowed) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed mutex handles cannot be destroyed");
      } else if (symbols_.has_live_borrow(mutex->allocation_id)) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "cannot destroy a mutex while a borrow is live");
      } else if (!checking_deferred_cleanup_) {
        const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
        symbols_.mark_allocation_moved(mutex->allocation_id);
      }
    }
    return Type::void_type();
  }
  if (call.callee() == "socket.connect") {
    if (call.arguments().size() != 2)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'socket.connect' takes a host and port");
    if (!call.arguments().empty()) {
      const Type host = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (host.is_valid() && !host.is_string())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket.connect host must be a string");
    }
    if (call.arguments().size() > 1) {
      const Type port = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (port.is_valid() && !port.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                           "socket.connect port must be an integer");
    }
    return Type::pointer_type(Type::byte_type());
  }
  if (call.callee() == "socket.close") {
    if (call.arguments().size() != 1)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'socket.close' takes one socket handle");
    if (call.arguments().empty()) return Type::i32_type();
    const Type type = check_expression(const_cast<Expr&>(*call.arguments()[0]));
    const Symbol* socket = find_root_symbol(*call.arguments()[0], symbols_);
    if (type.is_valid() && !type.is_pointer())
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "socket.close requires a socket handle");
    else if (!socket || socket->cleanup_callee != "socket.close")
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "socket.close requires an owned socket resource");
    else if (socket->borrowed)
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "borrowed sockets cannot be closed");
    else if (symbols_.has_live_borrow(socket->allocation_id))
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "cannot close a socket while a borrow is live");
    else if (!checking_deferred_cleanup_) {
      const_cast<Expr&>(*call.arguments()[0]).set_transfers_ownership();
      symbols_.mark_allocation_moved(socket->allocation_id);
    }
    return Type::i32_type();
  }
  if (call.callee() == "socket.send") {
    if (call.arguments().size() != 2)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'socket.send' takes a socket handle and text");
    if (!call.arguments().empty()) {
      const Type handle = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (handle.is_valid() && !handle.is_pointer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket.send requires a socket handle");
      else if (!has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "socket.close", true))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket.send requires a socket or borrowed socket handle");
      else if (!find_root_symbol(*call.arguments()[0], symbols_))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket operations require a named owner or borrowed handle");
    }
    if (call.arguments().size() > 1) {
      const Type text = check_expression(const_cast<Expr&>(*call.arguments()[1]));
      if (text.is_valid() && !text.is_string())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                           "socket.send content must be a string");
    }
    return Type::i64_type();
  }
  if (call.callee() == "socket.receive") {
    if (call.arguments().size() != 3)
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'socket.receive' takes a socket, destination, and capacity");
    if (!call.arguments().empty()) {
      const Type handle = check_expression(const_cast<Expr&>(*call.arguments()[0]));
      if (handle.is_valid() && !handle.is_pointer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket.receive requires a socket handle");
      else if (!has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "socket.close", true))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket.receive requires a socket or borrowed socket handle");
      else if (!find_root_symbol(*call.arguments()[0], symbols_))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "socket operations require a named owner or borrowed handle");
    }
    if (call.arguments().size() > 1) {
      const Expr& destination_expr = *call.arguments()[1];
      const Type destination = check_expression(
          const_cast<Expr&>(destination_expr));
      if (destination.is_valid() && !destination.is_pointer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[1]->location(),
                           "socket.receive destination must be a pointer");
      const Symbol* destination_symbol =
          find_root_symbol(destination_expr, symbols_);
      std::string destination_cleanup;
      if (destination_symbol) {
        destination_cleanup = destination_symbol->cleanup_callee;
        if (destination_symbol->borrowed && !destination_symbol->mutable_borrow)
          diagnostics_.error(diag::kTypeMismatch, destination_expr.location(),
                             "socket.receive destination requires a mutable borrow");
        else if (!destination_symbol->borrowed &&
                 !destination_symbol->is_mutable())
          diagnostics_.error(diag::kImmutableAssignment,
                             destination_expr.location(),
                             "socket.receive destination must be mutable");
      } else if (destination_expr.kind() == Expr::Kind::Call) {
        destination_cleanup = cleanup_for_owner_producer(
            static_cast<const CallExpr&>(destination_expr).callee());
      }
      if (!destination_cleanup.empty() &&
          destination_cleanup != "memory.deallocate" &&
          destination_cleanup != "memory.secure_release")
        diagnostics_.error(diag::kTypeMismatch, destination_expr.location(),
                           "socket.receive destination cannot be an opaque resource handle");
    }
    if (call.arguments().size() > 2) {
      const Type capacity = check_expression(const_cast<Expr&>(*call.arguments()[2]));
      if (capacity.is_valid() && !capacity.is_integer())
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[2]->location(),
                           "socket.receive capacity must be an integer");
    }
    return Type::i64_type();
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
          !argument_type.is_char() && !argument_type.is_string() &&
          !argument_type.is_pointer()) {
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
      if (index == 0 && type.is_valid() && type.is_pointer() &&
          !has_cleanup_domain(*call.arguments()[index], symbols_,
                              "memory.unmap_file", true))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "mapped access requires a mapping handle");
      else if (index == 0 && type.is_valid() && type.is_pointer() &&
               !find_root_symbol(*call.arguments()[index], symbols_))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[index]->location(),
                           "mapped access requires a named owner or borrowed handle");
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
      call.callee() == "memory.total_bytes" ||
      call.callee() == "memory.device_memory_total_bytes" ||
      call.callee() == "memory.device_memory_used_bytes") {
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
      const bool unpin = call.callee() == "memory.unpin";
      if (type.is_valid() &&
          (unpin ? !type.is_managed_pointer()
                 : !type.is_pointer()))
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           unpin ? "memory.unpin requires a managed pointer"
                                 : "argument must be an explicitly allocated pointer");
    }
    if (!checking_deferred_cleanup_ &&
        (call.callee() == "memory.secure_release" ||
         call.callee() == "memory.unmap_file") && !call.arguments().empty()) {
      const Symbol* pointer_symbol = find_root_symbol(*call.arguments()[0], symbols_);
      if (pointer_symbol && pointer_symbol->borrowed)
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed pointers cannot be released or unmapped");
      else if (pointer_symbol &&
               symbols_.has_live_borrow(pointer_symbol->allocation_id))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "cannot release a resource while a borrow is live");
      else if (call.callee() == "memory.unmap_file" &&
               !has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "memory.unmap_file"))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "memory.unmap_file requires a mapped file resource");
      else if (call.callee() == "memory.secure_release" &&
               !has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "memory.secure_release") &&
               !has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "memory.deallocate"))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "secure release cannot destroy a different resource type");
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
    for (const ExprPtr& argument : call.arguments()) {
      check_expression(const_cast<Expr&>(*argument));
      if (!has_cleanup_domain(*argument, symbols_, "memory.unmap_file", true))
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "memory.mapping_size requires a mapping or borrowed mapping handle");
      else if (!find_root_symbol(*argument, symbols_))
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "mapping queries require a named owner or borrowed handle");
    }
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
    } else if (pointer_symbol &&
               symbols_.has_live_borrow(pointer_symbol->allocation_id)) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "cannot deallocate an owner while a borrow is live");
    } else if (!has_cleanup_domain(*call.arguments()[0], symbols_,
                                   "memory.deallocate")) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                         "owned resource requires its specialized cleanup function");
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
      else if (!has_cleanup_domain(*argument, symbols_, "file.close"))
        diagnostics_.error(diag::kTypeMismatch, argument->location(),
                           "file.close requires an owned file resource");
    }
    if (!checking_deferred_cleanup_ && !call.arguments().empty()) {
      const Symbol* resource = find_root_symbol(*call.arguments()[0], symbols_);
      if (resource && resource->borrowed)
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "borrowed file resources cannot be closed");
      else if (resource && symbols_.has_live_borrow(resource->allocation_id))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "cannot close a file while a borrow is live");
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
      else if (!has_cleanup_domain(*call.arguments()[0], symbols_, "file.close", true))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "file.write requires a file resource or borrowed file handle");
      else if (!find_root_symbol(*call.arguments()[0], symbols_))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "file operations require a named owner or borrowed handle");
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
      else if (!has_cleanup_domain(*call.arguments()[0], symbols_, "file.close", true))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "file.read requires a file resource or borrowed file handle");
      else if (!find_root_symbol(*call.arguments()[0], symbols_))
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[0]->location(),
                           "file operations require a named owner or borrowed handle");
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
  if (call.callee() == "time.cycles") {
    if (!call.arguments().empty())
      diagnostics_.error(diag::kPrintArity, call.location(),
                         "'time.cycles' takes no arguments");
    return Type::u64_type();
  }
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
        if ((view_call.callee() == "memory.array_view" ||
             view_call.callee() == "memory.array_view_mut") &&
            !view_call.arguments().empty())
          source = find_root_symbol(*view_call.arguments()[0], symbols_);
      } else {
        source = find_root_symbol(argument, symbols_);
      }
      const bool expects_mutable =
          i < sig.parameter_mutable_borrow.size() &&
          sig.parameter_mutable_borrow[i];
      const bool argument_is_mutable =
          argument.ownership() == ValueOwnership::MutableBorrowed ||
          (source && source->mutable_borrow);
      if (!source || source->allocation_id == 0 || source->moved) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "array view arguments must refer to a live tracked owner");
      } else if (expects_mutable && !argument_is_mutable) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "mutable array view parameter requires an exclusive mutable view");
      } else if (std::find(ownership_transfers.begin(), ownership_transfers.end(),
                           source->allocation_id) != ownership_transfers.end()) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "cannot borrow an owner in a call that also transfers it");
      } else if (symbols_.has_conflicting_borrow(source->allocation_id,
                                                 expects_mutable,
                                                 source->borrowed ? source->name :
                                                                    std::string{})) {
        diagnostics_.error(diag::kTypeMismatch, argument.location(),
                           "array view argument conflicts with another live borrow");
      } else {
        auto found = active_borrows.find(source->allocation_id);
        if (found != active_borrows.end() &&
            (found->second || expects_mutable))
          diagnostics_.error(diag::kTypeMismatch, argument.location(),
                             "conflicting borrows of the same allocation in one call");
        active_borrows[source->allocation_id] = expects_mutable;
      }
      continue;
    }
    if (sig.parameter_types[i].is_owned_array() ||
        contains_owned_array(sig.parameter_types[i])) {
      consume_field_move(const_cast<Expr&>(*call.arguments()[i]),
                         sig.parameter_types[i]);
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
    const Symbol* source = find_borrow_origin(*call.arguments()[i]);
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
    const std::string expected_owned_resource =
        i < sig.parameter_resource_domains.size() &&
                !(i < sig.parameter_borrowed.size() && sig.parameter_borrowed[i])
            ? cleanup_for_borrow_domain(sig.parameter_resource_domains[i])
            : std::string{};
    std::string actual_resource = source
        ? (source->cleanup_callee.empty() && source->owns_allocation
               ? "memory.deallocate" : source->cleanup_callee)
        : call.arguments()[i]->kind() == Expr::Kind::Call
              ? cleanup_for_owner_producer(
                    static_cast<const CallExpr&>(*call.arguments()[i]).callee())
              : std::string{};
    if (actual_resource.empty() &&
        call.arguments()[i]->kind() == Expr::Kind::Call) {
      const auto& argument_call = static_cast<const CallExpr&>(*call.arguments()[i]);
      if (const FunctionSignature* argument_signature =
              find_function(argument_call.callee())) {
        actual_resource = !argument_signature->return_resource_domain.empty()
            ? cleanup_for_return_domain(argument_signature->return_resource_domain)
            : argument_signature->return_type.is_pointer()
                  ? "memory.deallocate" : std::string{};
      }
    }
    if (!expected_owned_resource.empty() &&
        actual_resource != expected_owned_resource) {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "owning parameter requires a '" +
                             sig.parameter_resource_domains[i] +
                             "' resource handle");
      continue;
    }
    if (!expects_shared && expected_owned_resource.empty() &&
        !(i < sig.parameter_borrowed.size() && sig.parameter_borrowed[i]) &&
        !actual_resource.empty() && actual_resource != "memory.deallocate") {
      diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                         "owned resource requires cleanup with '" +
                             actual_resource +
                             "'; use a matching owned parameter or a borrow parameter");
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
      const std::string expected_resource =
          i < sig.parameter_resource_domains.size()
              ? cleanup_for_borrow_domain(sig.parameter_resource_domains[i])
              : std::string{};
      const std::string actual_resource = source->cleanup_callee.empty() &&
                                                  source->owns_allocation
                                              ? "memory.deallocate"
                                              : source->cleanup_callee;
      if (!expected_resource.empty() && actual_resource != expected_resource) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "borrow parameter requires a '" +
                               sig.parameter_resource_domains[i] +
                               "' resource handle");
        continue;
      }
      if (expected_resource.empty() && !actual_resource.empty() &&
          actual_resource != "memory.deallocate") {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "opaque resources require a resource-qualified borrow parameter");
        continue;
      }
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
      string borrowed_source_name = source->name;
      string borrowed_field_path;
      string argument_root;
      if (field_path(*call.arguments()[i], argument_root, borrowed_field_path))
        borrowed_source_name = argument_root;
      if (symbols_.has_conflicting_borrow(source->allocation_id, mutable_borrow,
                                          borrowed_source_name)) {
        diagnostics_.error(diag::kTypeMismatch, call.arguments()[i]->location(),
                           "borrow argument conflicts with an existing live borrow");
        continue;
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
