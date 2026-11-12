// Lexically scoped symbol table.
//
// Phase 1 bindings live in one function and shadowing across nested blocks is
// allowed. Lookup walks from innermost to outermost scope; declaration checks
// only the innermost scope, so `{ let x = 1; } { let x = 2; }` is legal while
// `let x = 1; let x = 2;` in the same block is not.
#pragma once

#include <string>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nirqoba/Common/Type.h"
#include "nirqoba/Diagnostics/SourceLocation.h"
#include "nirqoba/Frontend/Ast.h"

namespace nirq {

// True when two dotted field paths refer to the same storage: equal paths, an
// ancestor/descendant pair, or either whole-binding empty path. Independent
// sibling paths do not overlap.
bool borrow_paths_overlap(const std::string& left, const std::string& right);

struct Symbol {
  std::string name;
  Type type;
  BindingKind binding = BindingKind::Let;
  SourceLocation location;
  // Pointer aliases share an allocation id so explicit deallocation invalidates
  // every lexical alias visible to the current type-checking pass.
  uint64_t allocation_id = 0;
  bool moved = false;
  // True when the binding uniquely owns an explicit runtime allocation/resource.
  bool owns_allocation = false;
  bool shared_allocation = false;
  std::string cleanup_callee;
  bool borrowed = false;
  bool mutable_borrow = false;
  // Lexical depth of this binding, and for a borrow the name of the binding
  // whose storage it aliases. A borrow cannot outlive that owner, so the
  // checker validates the origin before every rebind and call.
  std::size_t scope_depth = 0;
  std::string borrow_owner;
  // Dotted field path from borrow_owner when this borrow aliases a field, for
  // example `.inner.data`. Empty means the borrow covers the whole binding.
  std::string borrow_field;
  // Set for '@Nullable' pointer bindings: the value may be nullptr, so dereference
  // requires a non-null proof in the current branch.
  bool nullable = false;
  // A partial aggregate move clears the corresponding runtime field. Keep the
  // path in semantic state so later reads and branch merges cannot resurrect it.
  std::unordered_set<std::string> moved_fields;
  std::unordered_set<std::string> definitely_moved_fields;
  // Fields of an aggregate declared with `uninit` that have not been assigned
  // yet. Reads are rejected; destruction is safe because the slot is zeroed.
  std::unordered_set<std::string> uninitialized_fields;
  // Fields explicitly assigned since `uninit`; a nested projection stays readable
  // even though its enclosing aggregate is not complete yet.
  std::unordered_set<std::string> initialized_fields;
  // Nested aggregates with some, but not all, fields initialized. Assigning the
  // whole field would overwrite the initialized parts without destroying them.
  std::unordered_set<std::string> partially_initialized_fields;

  bool is_mutable() const { return binding == BindingKind::Var; }
};

class SymbolTable {
 public:
  void push_scope() { scopes_.emplace_back(); }
  void pop_scope() {
    if (!scopes_.empty()) scopes_.pop_back();
  }

  // Returns false when `name` already exists in the current scope.
  bool declare(Symbol symbol);

  // Innermost binding of `name`, or nullptr.
  const Symbol* lookup(const std::string& name) const;
  void mark_allocation_moved(uint64_t allocation_id);
  bool allocation_is_moved(uint64_t allocation_id) const;
  // Address-taking gives a scalar a tracked lifetime shared with its aliases.
  uint64_t ensure_allocation_id(const std::string& name, uint64_t allocation_id);
  bool has_live_borrow(uint64_t allocation_id) const;
  bool has_live_field_borrow(uint64_t allocation_id,
                             const std::string& path) const;
  // The first live borrow that conflicts with a request of the given
  // mutability and path, or nullptr. Immutable requests conflict only with
  // mutable borrows; mutable requests conflict with either kind.
  const Symbol* find_conflicting_borrow(
      uint64_t allocation_id, bool requested_mutable,
      const std::string& requested_path = {},
      const std::string& except_name = {}) const;
  void rebind_pointer(const std::string& name, uint64_t allocation_id);
  void set_pointer_ownership(const std::string& name, bool owns_allocation,
                             bool borrowed, bool mutable_borrow,
                             bool shared_allocation = false,
                             std::string cleanup_callee = {},
                             std::string borrow_owner = {},
                             std::string borrow_field = {});
  void set_borrowed_resource_domain(const std::string& name,
                                    std::string cleanup_callee);
  bool field_is_moved(const std::string& name, const std::string& path) const;
  bool field_is_definitely_moved(const std::string& name,
                                 const std::string& path) const;
  bool field_has_moved_descendant(const std::string& name,
                                  const std::string& path) const;
  void mark_field_moved(const std::string& name, const std::string& path);
  void clear_field_moved(const std::string& name, const std::string& path);
  void mark_field_uninitialized(const std::string& name, const std::string& path);
  bool field_is_uninitialized(const std::string& name, const std::string& path) const;
  void clear_field_uninitialized(const std::string& name, const std::string& path);
  void mark_partially_initialized(const std::string& name, const std::string& path);
  bool is_partially_initialized(const std::string& name, const std::string& path) const;
  void clear_partially_initialized(const std::string& name, const std::string& path);
  void merge_conditional_ownership(const SymbolTable& then_state,
                                   const SymbolTable& else_state);
  std::vector<std::string> owners_moved_since(const SymbolTable& previous) const;

  std::size_t scope_depth() const { return scopes_.size(); }

 private:
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};

}  // namespace nirq
