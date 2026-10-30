#include "nirqoba/Sema/SymbolTable.h"

#include <utility>

namespace nirq {

bool SymbolTable::declare(Symbol symbol) {
  if (scopes_.empty()) scopes_.emplace_back();
  auto& scope = scopes_.back();
  const std::string name = symbol.name;
  auto [iterator, inserted] = scope.emplace(name, std::move(symbol));
  (void)iterator;
  return inserted;
}

const Symbol* SymbolTable::lookup(const std::string& name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto found = scope->find(name);
    if (found != scope->end()) return &found->second;
  }
  return nullptr;
}

}  // namespace nirq
