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

}  // namespace nirq
