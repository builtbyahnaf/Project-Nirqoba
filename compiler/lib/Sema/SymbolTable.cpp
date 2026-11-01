#include "nirqoba/Sema/SymbolTable.h"

#include <utility>

namespace nirq {
using namespace std;

bool SymbolTable::declare(Symbol symbol) {
  if (scopes_.empty()) scopes_.emplace_back();
  auto& scope = scopes_.back();
  const string name = symbol.name;
  auto [iterator, inserted] = scope.emplace(name, move(symbol));
  (void)iterator;
  return inserted;
}

const Symbol* SymbolTable::lookup(const string& name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto found = scope->find(name);
    if (found != scope->end()) return &found->second;
  }
  return nullptr;
}

void SymbolTable::mark_allocation_moved(uint64_t allocation_id) {
  if (allocation_id == 0) return;
  for (auto& scope : scopes_)
    for (auto& entry : scope)
      if (entry.second.allocation_id == allocation_id)
        entry.second.moved = true;
}

bool SymbolTable::allocation_is_moved(uint64_t allocation_id) const {
  if (allocation_id == 0) return false;
  bool found_allocation = false;
  bool found_live_allocation = false;
  for (const auto& scope : scopes_)
    for (const auto& entry : scope)
      if (entry.second.allocation_id == allocation_id) {
        found_allocation = true;
        if (!entry.second.moved) found_live_allocation = true;
      }
  return found_allocation && !found_live_allocation;
}

bool SymbolTable::has_live_borrow(uint64_t allocation_id) const {
  if (allocation_id == 0) return false;
  for (const auto& scope : scopes_)
    for (const auto& entry : scope)
      if (entry.second.allocation_id == allocation_id &&
          entry.second.borrowed && !entry.second.moved) return true;
  return false;
}

void SymbolTable::rebind_pointer(const string& name, uint64_t allocation_id) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.allocation_id = allocation_id;
      found->second.moved = false;
      return;
    }
  }
}

void SymbolTable::set_pointer_ownership(const string& name,
                                        bool owns_allocation, bool borrowed,
                                        bool mutable_borrow,
                                        bool shared_allocation,
                                        std::string cleanup_callee) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.owns_allocation = owns_allocation;
      found->second.borrowed = borrowed;
      found->second.mutable_borrow = mutable_borrow;
      found->second.shared_allocation = shared_allocation;
      found->second.cleanup_callee = std::move(cleanup_callee);
      return;
    }
  }
}

void SymbolTable::merge_conditional_ownership(const SymbolTable& then_state,
                                               const SymbolTable& else_state) {
  for (auto& scope : scopes_) {
    for (auto& entry : scope) {
      Symbol& destination = entry.second;
      const Symbol* then_symbol = then_state.lookup(destination.name);
      const Symbol* else_symbol = else_state.lookup(destination.name);
      if (!then_symbol || !else_symbol) continue;
      const bool same_allocation =
          then_symbol->allocation_id == else_symbol->allocation_id &&
          then_symbol->allocation_id == destination.allocation_id;
      if (!same_allocation) {
        // A single symbol cannot represent different owners selected by
        // mutually exclusive branches. Keep it unusable after the merge.
        destination.moved = true;
        continue;
      }
      destination.moved = then_symbol->moved || else_symbol->moved;
      destination.owns_allocation = then_symbol->owns_allocation &&
                                    else_symbol->owns_allocation;
      destination.borrowed = then_symbol->borrowed || else_symbol->borrowed;
      destination.mutable_borrow = then_symbol->mutable_borrow ||
                                   else_symbol->mutable_borrow;
      destination.shared_allocation = then_symbol->shared_allocation ||
                                      else_symbol->shared_allocation;
    }
  }
}

std::vector<std::string> SymbolTable::owners_moved_since(
    const SymbolTable& previous) const {
  std::vector<std::string> moved_owners;
  for (const auto& scope : previous.scopes_) {
    for (const auto& entry : scope) {
      const Symbol& before = entry.second;
      if (!before.owns_allocation || before.moved) continue;
      const Symbol* after = lookup(before.name);
      if (after && after->allocation_id == before.allocation_id && after->moved)
        moved_owners.push_back(before.name);
    }
  }
  return moved_owners;
}

}  // namespace nirq
