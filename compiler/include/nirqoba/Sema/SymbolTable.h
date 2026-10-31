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
#include <vector>

#include "nirqoba/Common/Type.h"
#include "nirqoba/Diagnostics/SourceLocation.h"
#include "nirqoba/Frontend/Ast.h"

namespace nirq {

struct Symbol {
  std::string name;
  Type type;
  BindingKind binding = BindingKind::Let;
  SourceLocation location;
  // Pointer aliases share an allocation id so explicit deallocation invalidates
  // every lexical alias visible to the current type-checking pass.
  uint64_t allocation_id = 0;
  bool moved = false;

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
  void rebind_pointer(const std::string& name, uint64_t allocation_id);

  std::size_t scope_depth() const { return scopes_.size(); }

 private:
  std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};

}  // namespace nirq
