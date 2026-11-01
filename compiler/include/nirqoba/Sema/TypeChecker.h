// Name resolution and type checking for Nirqoba Phase 1, 2, 3, 4 & 5.
#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nirqoba/Diagnostics/DiagnosticEngine.h"
#include "nirqoba/Frontend/Ast.h"
#include "nirqoba/Sema/SymbolTable.h"

namespace nirq {

struct ModuleUnit;

struct FunctionSignature {
  std::string name;
  Type return_type;
  std::vector<Type> parameter_types;
  SourceLocation location;
  std::string module_name;
  Visibility visibility = Visibility::Public;
  // Link-time symbol. Functions of the root module keep their bare name so
  // the entry point is emitted as `main`; every other module's functions are
  // qualified as `module.name` to keep modules link-compatible without a
  // separate name-mangling table.
  std::string symbol;
  std::vector<bool> parameter_borrowed;
  std::vector<bool> parameter_mutable_borrow;
  std::vector<bool> parameter_shared;
  bool returns_shared = false;
};

// Lightweight catalog entry for a registered struct type.
struct StructInfo {
  std::string name;
  std::vector<Parameter> fields;  // ordered field list from StructDecl
  SourceLocation location;
  std::string module_name;
  Visibility visibility = Visibility::Public;
};

class TypeChecker {
 public:
  explicit TypeChecker(DiagnosticEngine& diagnostics) : diagnostics_(diagnostics) {}

  bool check(Program& program);
  bool check_modules(const std::vector<std::unique_ptr<ModuleUnit>>& modules);

 private:
  struct DeferredCleanup {
    const CallExpr* call = nullptr;
    uint64_t allocation_id = 0;
  };
  void check_function(FunctionDecl& function);
  void check_statement(Stmt& statement);
  void check_block(BlockStmt& block);
  void check_variable_declaration(VariableDeclarationStmt& declaration);
  void check_destructuring(DestructuringStmt& statement);
  void check_assignment(AssignmentStmt& assignment);
  void check_compound_assignment(CompoundAssignmentStmt& assignment);
  void check_if(IfStmt& statement);
  void check_while(WhileStmt& statement);
  void check_for(ForStmt& statement);
  void check_break(BreakStmt& statement);
  void check_continue(ContinueStmt& statement);
  void check_return(ReturnStmt& statement);
  void check_deferred_cleanup(DeferredCleanupStmt& statement);

  Type check_expression(Expr& expr);
  Type check_call(CallExpr& call);
  Type check_array_literal(ArrayLiteralExpr& expr);
  Type check_tuple_literal(TupleLiteralExpr& expr);
  Type check_index_expr(IndexExpr& expr);
  Type check_member_access(MemberAccessExpr& expr);
  Type check_struct_init(StructInitExpr& expr);

  void require_type(const Expr& expr, Type expected, const char* context);
  bool block_can_fall_through(const BlockStmt& block) const;

  // Struct catalog lookup helpers. `name` may be module-qualified; a bare name
  // first resolves against the module currently being checked, then against the
  // root module's catalog.
  const StructInfo* find_struct(const std::string& name) const;

  // Expands struct names in an annotation into structs carrying their field
  // layout, recursively through arrays and tuples. A struct parsed from source
  // is only a name, and lowering cannot lay out or index a struct without its
  // fields; resolving here keeps the parser free of catalog knowledge.
  Type resolve_aggregate_type(const Type& type);
  Type resolve_aggregate_type(const Type& type,
                              std::unordered_set<std::string>& active);

  // Resolves a declaration's annotation and records it back on the AST node so
  // lowering sees the same type semantic analysis checked.
  void resolve_declaration_types(FunctionDecl& function);

  // Looks a function up by the spelling used at the call site, falling back to
  // the current module's namespace for unqualified names.
  const FunctionSignature* find_function(const std::string& name) const;

  DiagnosticEngine& diagnostics_;
  SymbolTable symbols_;
  Type current_return_type_;
  bool current_return_shared_ = false;
  int loop_depth_ = 0;
  std::string current_module_;
  uint64_t next_allocation_id_ = 1;
  bool checking_deferred_cleanup_ = false;
  std::vector<std::vector<DeferredCleanup>> deferred_calls_;
  std::unordered_map<std::string, FunctionSignature> functions_;
  std::unordered_map<std::string, StructInfo> structs_;
};

}  // namespace nirq
