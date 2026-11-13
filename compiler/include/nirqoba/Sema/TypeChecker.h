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

// Compact per-parameter effect bits inferred for a function body. Effects are
// intentionally independent of any caller allocation: they describe what the
// callee may do to a borrowed parameter and are merged through nested calls.
enum BorrowEffectFlag : uint8_t {
  BorrowEffectRead = 1u << 0,
  BorrowEffectWrite = 1u << 1,
  BorrowEffectEscape = 1u << 2,
  BorrowEffectAlias = 1u << 3,
};

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
  std::vector<std::string> parameter_resource_domains;
  std::vector<std::string> parameter_names;
  bool returns_shared = false;
  bool returns_borrowed = false;
  std::string return_resource_domain;
  std::string return_borrow_parameter;
  // Resolved once when the signature is built; callers reuse this compact
  // effect summary instead of searching the parameter-name vector per call.
  int return_borrow_parameter_index = -1;
  bool returns_mutable_borrow = false;
  // One compact effect byte per parameter. Declared effects are available
  // before the body is checked; `effects_known` records that inferred nested
  // effects have replaced the conservative declaration-only summary.
  std::vector<uint8_t> parameter_effects;
  bool effects_known = false;
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
  struct ActiveBorrow {
    uint64_t allocation_id = 0;
    BorrowPathSet paths;
    bool mutable_borrow = false;
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
  SymbolTable loop_exit_state_after_defer() const;
  bool field_path(const Expr& expression, std::string& root,
                  std::string& path) const;
  // Resolves the struct type at a dotted field path (leading dot allowed).
  Type type_at_field_path(const Type& type, const std::string& path);
  // Records a field assignment and propagates completion to enclosing aggregates
  // once every child of an aggregate is initialized.
  void mark_field_initialized(const std::string& root, const std::string& path);
  const Symbol* find_borrow_origin(const Expr& expression) const;
  BorrowPathSet find_borrow_paths(const Expr& expression) const;
  // A borrow's originating binding must still exist and refer to the same
  // allocation; borrowed parameters are their own origin inside a function.
  bool borrow_owner_is_live(const Symbol& borrow) const;
  std::string borrow_owner_name(const Symbol& origin) const;
  bool active_borrow_conflicts(uint64_t allocation_id,
                               const BorrowPathSet& paths,
                               bool requested_mutable) const;
  bool has_live_mutable_access(uint64_t allocation_id,
                               const BorrowPathSet& paths) const;
  void consume_field_move(Expr& expression, const Type& value_type);
  // Records a read/write/escape/alias of a current borrowed parameter.
  void record_parameter_use(const Expr& expression, uint8_t effect);
  // Merges the callee's summary for one argument into the current function's
  // parameter summary. Unknown/external callees contribute all effects.
  void merge_callee_effects(const FunctionSignature& signature, size_t index,
                            const Expr& argument);
  void publish_function_effects(const FunctionDecl& function);
  // True when a struct type contains a borrowed field at any nesting depth.
  bool contains_borrowed_field(const Type& type) const;
  bool contains_borrowed_field(const Type& type,
                               std::unordered_set<std::string>& active) const;
  std::vector<BorrowTarget> aggregate_borrows_for(const Expr& expr) const;
  void validate_aggregate_borrow_lifetime(const std::vector<BorrowTarget>& borrows,
                                          size_t target_depth,
                                          const SourceLocation& location);

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
  std::string current_return_resource_domain_;
  std::string current_return_borrow_parameter_;
  uint64_t current_return_borrow_allocation_id_ = 0;
  bool current_return_mutable_borrow_ = false;
  int loop_depth_ = 0;
  std::vector<std::vector<SymbolTable>> loop_break_states_;
  std::vector<std::vector<SymbolTable>> loop_continue_states_;
  std::vector<std::size_t> loop_deferred_scope_depths_;
  std::string current_module_;
  uint64_t next_allocation_id_ = 1;
  bool checking_deferred_cleanup_ = false;
  bool checking_lvalue_ = false;
  // Set while checking the operand of `&`, so creating a borrow does not also
  // report an ordinary overlapping read.
  bool checking_borrow_operand_ = false;
  // Borrows already claimed by earlier arguments of the call currently being
  // checked. This makes argument ordering obey the same exclusivity rule as
  // live lexical borrows.
  std::vector<ActiveBorrow> active_call_borrows_;
  // Effect summary for the function body currently being checked.
  bool tracking_function_effects_ = false;
  std::vector<uint8_t> current_parameter_effects_;
  std::unordered_map<std::string, size_t> current_parameter_indices_;
  // Borrowed-field origins recovered from struct initializers, keyed by the
  // initializer expression so nested aggregates can contribute transitively.
  std::unordered_map<const Expr*, std::vector<BorrowTarget>> aggregate_borrows_;
  // Set while checking the base of a member access. A container element has no
  // value of its own, so `ps[i]` is only a legal expression when it is claimed
  // as the base of `ps[i].field`.
  bool checking_soa_column_base_ = false;
  // True while checking the base of a member access, which is not a whole-value
  // use of a partially initialized aggregate.
  bool checking_aggregate_base_ = false;
  // Names proven null/non-null by a branch condition. Only facts that hold on
  // every execution path reaching that branch are retained.
  struct NullFacts {
    std::unordered_set<std::string> nonnull;
    std::unordered_set<std::string> null;
  };
  NullFacts null_facts_for(const Expr& expr, bool truth) const;
  void apply_null_facts(const NullFacts& facts);
  // Names proven non-null in the current branch; branch-scoped, cleared on assignment.
  std::unordered_set<std::string> nonnull_proven_;
  std::vector<std::vector<DeferredCleanup>> deferred_calls_;
  std::unordered_map<std::string, FunctionSignature> functions_;
  std::unordered_map<std::string, StructInfo> structs_;
};

}  // namespace nirq
