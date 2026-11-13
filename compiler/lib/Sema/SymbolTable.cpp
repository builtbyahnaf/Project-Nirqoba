#include "nirqoba/Sema/SymbolTable.h"

#include <utility>

namespace nirq {
using namespace std;

bool borrow_paths_overlap(const string& left, const string& right) {
  if (left.empty() || right.empty()) return true;
  if (left == right) return true;
  if (left.size() < right.size())
    return right.compare(0, left.size(), left) == 0 &&
           right[left.size()] == '.';
  return left.compare(0, right.size(), right) == 0 &&
         left[right.size()] == '.';
}

void BorrowPathSet::add(const string& value) {
  // An empty primary path already covers every field of the owner.
  if (path.empty()) return;
  if (value.empty()) {
    path.clear();
    alternatives.clear();
    return;
  }
  if (value == path) return;
  for (const string& alternative : alternatives)
    if (alternative == value) return;
  alternatives.push_back(value);
}

void BorrowPathSet::merge(const BorrowPathSet& other) {
  if (other.path.empty()) {
    path.clear();
    alternatives.clear();
    return;
  }
  add(other.path);
  for (const string& alternative : other.alternatives) add(alternative);
}

bool BorrowPathSet::overlaps(const string& requested) const {
  if (borrow_paths_overlap(path, requested)) return true;
  for (const string& alternative : alternatives)
    if (borrow_paths_overlap(alternative, requested)) return true;
  return false;
}

bool BorrowPathSet::overlaps(const BorrowPathSet& requested) const {
  if (overlaps(requested.path)) return true;
  for (const string& alternative : requested.alternatives)
    if (overlaps(alternative)) return true;
  return false;
}

bool BorrowPathSet::overlaps_field(const string& requested) const {
  if (!path.empty() && borrow_paths_overlap(path, requested)) return true;
  for (const string& alternative : alternatives)
    if (!alternative.empty() && borrow_paths_overlap(alternative, requested))
      return true;
  return false;
}

bool BorrowPathSet::has_field_projection() const {
  if (!path.empty()) return true;
  for (const string& alternative : alternatives)
    if (!alternative.empty()) return true;
  return false;
}

bool SymbolTable::declare(Symbol symbol) {
  if (scopes_.empty()) scopes_.emplace_back();
  auto& scope = scopes_.back();
  const string name = symbol.name;
  symbol.scope_depth = scopes_.size();
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

uint64_t SymbolTable::ensure_allocation_id(const string& name,
                                           uint64_t allocation_id) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      if (found->second.allocation_id == 0)
        found->second.allocation_id = allocation_id;
      return found->second.allocation_id;
    }
  }
  return 0;
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

bool SymbolTable::has_live_field_borrow(uint64_t allocation_id,
                                         const string& path) const {
  if (allocation_id == 0) return false;
  for (const auto& scope : scopes_)
    for (const auto& entry : scope) {
      const Symbol& symbol = entry.second;
      if (symbol.allocation_id != allocation_id || symbol.moved ||
          !symbol.borrowed || !symbol.borrow_paths.has_field_projection())
        continue;
      if (symbol.borrow_paths.overlaps_field(path)) return true;
    }
  return false;
}

const Symbol* SymbolTable::find_conflicting_borrow(
    uint64_t allocation_id, bool requested_mutable,
    const BorrowPathSet& requested_paths, const string& except_name) const {
  if (allocation_id == 0) return nullptr;
  for (const auto& scope : scopes_)
    for (const auto& entry : scope) {
      const Symbol& symbol = entry.second;
      if (symbol.name == except_name || symbol.allocation_id != allocation_id ||
          !symbol.borrowed || symbol.moved)
        continue;
      if (!symbol.borrow_paths.overlaps(requested_paths)) continue;
      if (requested_mutable || symbol.mutable_borrow) return &symbol;
    }
  return nullptr;
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

void SymbolTable::set_definitely_null(const string& name, bool definitely_null) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.definitely_null = definitely_null;
      return;
    }
  }
}

bool SymbolTable::refine_conditional_borrow(const string& name) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found == scope->end()) continue;
    Symbol& symbol = found->second;
    if (!symbol.borrow_conditional || !symbol.borrow_conditional_absent_null)
      return false;
    symbol.borrow_conditional = false;
    symbol.borrow_conditional_absent_null = false;
    symbol.definitely_null = false;
    return true;
  }
  return false;
}

void SymbolTable::set_pointer_ownership(const string& name,
                                        bool owns_allocation, bool borrowed,
                                        bool mutable_borrow,
                                        bool shared_allocation,
                                        std::string cleanup_callee,
                                        std::string borrow_owner,
                                        BorrowPathSet borrow_paths) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.owns_allocation = owns_allocation;
      found->second.borrowed = borrowed;
      found->second.mutable_borrow = mutable_borrow;
      found->second.borrow_conditional = false;
      found->second.borrow_conditional_absent_null = false;
      found->second.definitely_null = false;
      found->second.shared_allocation = shared_allocation;
      found->second.cleanup_callee = std::move(cleanup_callee);
      found->second.borrow_owner = std::move(borrow_owner);
      found->second.borrow_paths = std::move(borrow_paths);
      return;
    }
  }
}

void SymbolTable::set_borrowed_resource_domain(const string& name,
                                                std::string cleanup_callee) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.cleanup_callee = std::move(cleanup_callee);
      return;
    }
  }
}

bool SymbolTable::field_is_moved(const string& name, const string& path) const {
  const Symbol* symbol = lookup(name);
  if (!symbol) return false;
  if (symbol->moved_fields.find(path) != symbol->moved_fields.end()) return true;
  // Moving a whole nested field also invalidates all descendant projections.
  for (const string& moved : symbol->moved_fields)
    if (path.size() > moved.size() && path.compare(0, moved.size(), moved) == 0 &&
        path[moved.size()] == '.') return true;
  return false;
}

bool SymbolTable::field_is_definitely_moved(const string& name,
                                            const string& path) const {
  const Symbol* symbol = lookup(name);
  return symbol && symbol->definitely_moved_fields.find(path) !=
                       symbol->definitely_moved_fields.end();
}

bool SymbolTable::field_has_moved_descendant(const string& name,
                                             const string& path) const {
  const Symbol* symbol = lookup(name);
  if (!symbol) return false;
  for (const string& moved : symbol->moved_fields)
    if (moved.size() > path.size() && moved.compare(0, path.size(), path) == 0 &&
        moved[path.size()] == '.') return true;
  return false;
}

void SymbolTable::mark_field_moved(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.moved_fields.insert(path);
      found->second.definitely_moved_fields.insert(path);
      return;
    }
  }
}

void SymbolTable::clear_field_moved(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      auto erase_subtree = [&](std::unordered_set<std::string>& fields) {
        for (auto it = fields.begin(); it != fields.end();) {
          if (it->size() > path.size() &&
              it->compare(0, path.size(), path) == 0 &&
              (*it)[path.size()] == '.')
            it = fields.erase(it);
          else
            ++it;
        }
        fields.erase(path);
      };
      erase_subtree(found->second.moved_fields);
      erase_subtree(found->second.definitely_moved_fields);
      return;
    }
  }
}

void SymbolTable::mark_field_uninitialized(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.uninitialized_fields.insert(path);
      return;
    }
  }
}

bool SymbolTable::field_is_uninitialized(const string& name, const string& path) const {
  const Symbol* symbol = lookup(name);
  if (!symbol) return false;
  if (symbol->initialized_fields.find(path) != symbol->initialized_fields.end())
    return false;
  if (symbol->uninitialized_fields.find(path) != symbol->uninitialized_fields.end())
    return true;
  for (const string& pending : symbol->uninitialized_fields)
    if (path.size() > pending.size() &&
        path.compare(0, pending.size(), pending) == 0 &&
        path[pending.size()] == '.') return true;
  return false;
}

void SymbolTable::clear_field_uninitialized(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found == scope->end()) continue;
    auto& fields = found->second.uninitialized_fields;
    for (auto it = fields.begin(); it != fields.end();) {
      if (it->size() > path.size() && it->compare(0, path.size(), path) == 0 &&
          (*it)[path.size()] == '.')
        it = fields.erase(it);
      else
        ++it;
    }
    fields.erase(path);
    found->second.initialized_fields.insert(path);
    return;
  }
}

void SymbolTable::mark_partially_initialized(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      found->second.partially_initialized_fields.insert(path);
      return;
    }
  }
}

bool SymbolTable::is_partially_initialized(const string& name, const string& path) const {
  const Symbol* symbol = lookup(name);
  return symbol &&
         symbol->partially_initialized_fields.find(path) !=
             symbol->partially_initialized_fields.end();
}

void SymbolTable::clear_partially_initialized(const string& name, const string& path) {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found == scope->end()) continue;
    auto& fields = found->second.partially_initialized_fields;
    for (auto it = fields.begin(); it != fields.end();) {
      if (it->size() >= path.size() && it->compare(0, path.size(), path) == 0)
        it = fields.erase(it);
      else
        ++it;
    }
    return;
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
      const bool one_sided_borrow =
          then_symbol->borrowed != else_symbol->borrowed;
      // The incoming state may still have an untracked allocation id (for
      // example a nullable pointer before either branch assigns a borrow). If
      // both branches agree, their shared id is the merged identity. A
      // one-sided borrow may pair a tracked id with the untracked/null id.
      if (then_symbol->allocation_id != else_symbol->allocation_id) {
        const Symbol& inactive =
            then_symbol->borrowed ? *else_symbol : *then_symbol;
        // A one-sided borrow may pair a tracked owner with an ordinary pointer
        // or null binding on the other path. The borrow path is still a real
        // possibility, so retain it; ownership of an inactive owner cannot be
        // merged away silently.
        if (!(one_sided_borrow && !inactive.borrowed &&
              !inactive.owns_allocation && !inactive.shared_allocation)) {
          // A single symbol cannot represent different owners selected by
          // mutually exclusive branches. Keep it unusable after the merge.
          destination.moved = true;
          continue;
        }
      }
      destination.allocation_id = then_symbol->allocation_id != 0
          ? then_symbol->allocation_id : else_symbol->allocation_id;
      destination.definitely_null = then_symbol->definitely_null &&
                                    else_symbol->definitely_null;
      destination.moved = then_symbol->moved || else_symbol->moved;
      destination.owns_allocation = then_symbol->owns_allocation &&
                                    else_symbol->owns_allocation;
      destination.borrowed = then_symbol->borrowed || else_symbol->borrowed;
      destination.mutable_borrow = then_symbol->mutable_borrow ||
                                   else_symbol->mutable_borrow;
      if (then_symbol->borrowed && else_symbol->borrowed) {
        // Both paths are real possibilities after the merge. Conflict checks
        // test every one, so disjoint fields stay usable while any overlap is
        // rejected without falling back to the whole owner.
        destination.borrow_owner = then_symbol->borrow_owner;
        destination.borrow_paths = then_symbol->borrow_paths;
        destination.borrow_paths.merge(else_symbol->borrow_paths);
        destination.borrow_conditional = then_symbol->borrow_conditional ||
                                         else_symbol->borrow_conditional;
        // A definite branch has no absent path; for conditional branches the
        // absent path is null only when every possible absence is null.
        const bool then_absence_null = !then_symbol->borrow_conditional ||
                                       then_symbol->borrow_conditional_absent_null;
        const bool else_absence_null = !else_symbol->borrow_conditional ||
                                       else_symbol->borrow_conditional_absent_null;
        destination.borrow_conditional_absent_null =
            then_absence_null && else_absence_null;
      } else if (one_sided_borrow) {
        // The borrow may be live after the merge, but only on one path. Keep
        // its owner and field projection so overlapping accesses are rejected;
        // using the binding as a definite borrow is rejected at the use site.
        const Symbol& active = then_symbol->borrowed ? *then_symbol
                                                      : *else_symbol;
        const Symbol& absent = then_symbol->borrowed ? *else_symbol
                                                      : *then_symbol;
        destination.borrow_owner = active.borrow_owner;
        destination.borrow_paths = active.borrow_paths;
        destination.borrow_conditional = true;
        destination.borrow_conditional_absent_null = absent.definitely_null;
      } else {
        destination.borrow_owner.clear();
        destination.borrow_paths.set({});
        destination.borrow_conditional = false;
        destination.borrow_conditional_absent_null = false;
      }
      destination.shared_allocation = then_symbol->shared_allocation ||
                                      else_symbol->shared_allocation;
      destination.moved_fields = then_symbol->moved_fields;
      destination.moved_fields.insert(else_symbol->moved_fields.begin(),
                                      else_symbol->moved_fields.end());
      destination.definitely_moved_fields.clear();
      for (const string& path : then_symbol->definitely_moved_fields)
        if (else_symbol->definitely_moved_fields.find(path) !=
            else_symbol->definitely_moved_fields.end())
          destination.definitely_moved_fields.insert(path);
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
