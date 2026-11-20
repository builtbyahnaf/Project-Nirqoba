#include "nirqoba/TIR/LifetimeAnalysis.h"

#include <algorithm>
#include <vector>

namespace nirq {

namespace tir {

namespace {

// Value-numbered union-find over pointer identities. Two values share a set when
// one is the other relocated: a binding slot receives the pointer, a load reads
// it back, a move renames it, a type cast preserves it, and an element index
// offsets it without changing which allocation it addresses.
class ValueSets {
 public:
  void add(int id) {
    if (parent_.find(id) == parent_.end()) parent_.emplace(id, id);
  }

  bool contains(int id) const { return parent_.find(id) != parent_.end(); }

  int find(int id) {
    const auto entry = parent_.find(id);
    if (entry == parent_.end()) return -1;
    if (entry->second == id) return id;
    const int root = find(entry->second);
    entry->second = root;
    return root;
  }

  // Joins two values, admitting either one the numbering has not seen yet. A
  // value only enters the numbering through a relation to a known value, so
  // "absent" means "first seen here", not "unrelated": a move, a load or an
  // element index introduces a new value number that names the same storage.
  //
  // Keeps the smaller value number as the root so the answer does not depend on
  // the order operations happen to be visited in.
  bool unite(int left, int right) {
    const bool has_left = contains(left) || left < 0;
    const bool has_right = contains(right) || right < 0;
    if (!has_left && !has_right) return false;
    bool added = false;
    if (!contains(left)) { add(left); added = true; }
    if (!contains(right)) { add(right); added = true; }
    int first = find(left);
    int second = find(right);
    if (first < 0 || second < 0 || first == second) return added;
    if (first > second) std::swap(first, second);
    parent_[second] = first;
    return true;
  }

 private:
  std::unordered_map<int, int> parent_;
};

// The address of a stack slot, as opposed to the value stored in one.
std::unordered_set<int> collect_slot_addresses(const Function& function) {
  std::unordered_set<int> slots;
  for (const Op& op : function.operations())
    if ((op.kind == OpKind::Alloca || op.kind == OpKind::ArrayAlloca) &&
        op.result.is_valid())
      slots.insert(op.result.id);
  return slots;
}

// Where one allocation's storage goes while the body is scanned.
struct FamilyState {
  EscapeReason reason = EscapeReason::None;
  bool manually_released = false;
  // Families stored inside this one, and the families that contain it.
  std::unordered_set<int> contained;
  std::unordered_set<int> containers;
};

void record_escape(FamilyState& state, EscapeReason reason) {
  if (state.reason == EscapeReason::None) state.reason = reason;
}

}  // namespace

std::string_view lifetime_name(AllocationLifetime lifetime) {
  switch (lifetime) {
    case AllocationLifetime::Bounded: return "bounded";
    case AllocationLifetime::Escaping: return "escaping";
  }
  return "escaping";
}

std::string_view escape_reason_name(EscapeReason reason) {
  switch (reason) {
    case EscapeReason::None: return "none";
    case EscapeReason::Returned: return "returned";
    case EscapeReason::PassedToCallee: return "passed-to-callee";
    case EscapeReason::StoredOutsideActivation: return "stored-outside-activation";
    case EscapeReason::ContainedInEscapingObject: return "contained-in-escaping-object";
    case EscapeReason::CyclicGraph: return "cyclic-graph";
  }
  return "none";
}

bool is_allocation_site(const Op& op) {
  return op.kind == OpKind::Call && op.result.is_valid() &&
         (op.callee == "memory.allocate" || op.callee == "memory.manual_allocate");
}

bool is_arena_eligible_payload(const Type& type) {
  if (!type.is_valid()) return false;
  // Scalar payloads have no ownership: nothing to trace, nothing to destroy.
  if (type.is_integer() || type.is_float() || type.is_bool() || type.is_char())
    return true;
  if (type.is_array())
    return type.array_length() > 0 &&
           is_arena_eligible_payload(type.element_type());
  if (type.is_tuple()) {
    const std::vector<Type>& elements = type.tuple_elements();
    if (elements.empty()) return false;
    for (const Type& element : elements)
      if (!is_arena_eligible_payload(element)) return false;
    return true;
  }
  if (type.is_struct()) {
    const auto& fields = type.struct_fields();
    if (fields.empty()) return false;
    for (const auto& field : fields)
      if (!is_arena_eligible_payload(field.second)) return false;
    return true;
  }
  // Pointers, strings, containers, managed and shared references all carry
  // ownership or collector state that bulk release cannot describe.
  return false;
}

LifetimeAnalysis LifetimeAnalysis::analyze(const Function& function) {
  LifetimeAnalysis analysis;

  ValueSets sets;
  std::vector<int> allocation_results;
  for (const Op& op : function.operations()) {
    if (!is_allocation_site(op)) continue;
    allocation_results.push_back(op.result.id);
    sets.add(op.result.id);
  }
  if (allocation_results.empty()) return analysis;

  // Identity propagation. The body is a flat operation stream over slots, so the
  // four relations below are enough to follow one allocation: a store binds its
  // slot to the stored pointer, a load reads a binding back, a move renames, a
  // cast reinterprets, and an element index offsets into the same object.
  // Stores can bind a slot after a load of it was already seen, so the relations
  // are closed to a fixpoint rather than resolved in one pass.
  for (bool changed = true; changed;) {
    changed = false;
    for (const Op& op : function.operations()) {
      switch (op.kind) {
        case OpKind::GetElementPtr:
        case OpKind::Cast:
        case OpKind::Move:
            if (sets.contains(op.lhs.is_valid() ? op.lhs.id : op.operand.id) &&
                op.result.is_valid())
              changed |= sets.unite(
                  op.lhs.is_valid() ? op.lhs.id : op.operand.id, op.result.id);
            break;
        case OpKind::Load:
          if (sets.contains(op.lhs.id) && op.result.is_valid())
            changed |= sets.unite(op.lhs.id, op.result.id);
          break;
        case OpKind::Store:
          // A store of a pointer into a slot binds that name to the storage; the
          // pointer does not become reachable from anyone else. A store of plain
          // data through an element pointer is not a relation between values, so
          // only a pointer source joins the slot.
          if (op.operand.is_valid() && sets.contains(op.operand.id))
            changed |= sets.unite(op.lhs.id, op.operand.id);
          break;
        default:
          break;
      }
    }
  }

  std::unordered_map<int, FamilyState> families;
  for (int id : allocation_results) sets.add(id);
  // Every value that shares storage with an allocation, grouped by root.
  std::unordered_map<int, std::unordered_set<int>> members;
  {
    // The union-find owns the value numbering, so walk the allocation's own
    // value space rather than asking it for its keys.
    for (std::size_t id = 0; id < function.value_count(); ++id) {
      const int value = static_cast<int>(id);
      if (!sets.contains(value)) continue;
      members[sets.find(value)].insert(value);
    }
  }

  auto root_of = [&](const Value& value) {
    if (!value.is_valid() || !sets.contains(value.id)) return -1;
    return sets.find(value.id);
  };
  const std::unordered_set<int> slot_addresses = collect_slot_addresses(function);
  auto state_for = [&](int root) -> FamilyState& { return families[root]; };

  for (const Op& op : function.operations()) {
    switch (op.kind) {
      case OpKind::Return: {
        const int root = root_of(op.operand);
        if (root >= 0) record_escape(state_for(root), EscapeReason::Returned);
        break;
      }
      case OpKind::Call: {
        // The allocation itself is the site being classified, not a use of one.
        if (is_allocation_site(op)) break;
        // A hand-written release is not a leak and not an escape: the storage is
        // reclaimed where the programmer said. It does rule the site out for
        // bulk release, because the arena owns storage its frame allocated.
        if (op.callee == "memory.deallocate") {
          for (const Value& argument : op.arguments) {
            const int root = root_of(argument);
            if (root >= 0) state_for(root).manually_released = true;
          }
          break;
        }
        for (const Value& argument : op.arguments) {
          const int root = root_of(argument);
          if (root >= 0)
            record_escape(state_for(root), EscapeReason::PassedToCallee);
        }
        break;
      }
      case OpKind::Store: {
        // Writing a value through one of the allocation's own element pointers
        // is the ordinary use of the storage. Writing the pointer into storage
        // the family does not own publishes it.
        //
        // A store into a frame slot is neither: the slot is a name for the
        // pointer, not memory inside the object, so it is already part of the
        // family and says nothing about where the storage goes.
        if (slot_addresses.count(op.lhs.id) != 0) break;
        const int destination = root_of(op.lhs);
        const int source = root_of(op.operand);
        if (destination >= 0) {
          if (source >= 0 && source != destination) {
            state_for(source).containers.insert(destination);
            state_for(destination).contained.insert(source);
          } else if (source >= 0) {
            record_escape(state_for(destination), EscapeReason::CyclicGraph);
          }
          break;
        }
        if (source >= 0)
          record_escape(state_for(source), EscapeReason::StoredOutsideActivation);
        break;
      }
      case OpKind::GetElementPtr:
      case OpKind::SoaColumnPtr:
      case OpKind::Load:
      case OpKind::Move:
      case OpKind::Cast:
      case OpKind::Borrow:
      case OpKind::EndBorrow:
      case OpKind::Drop:
      case OpKind::ConstantInt:
      case OpKind::ConstantNull:
      case OpKind::ConstantFloat:
      case OpKind::ConstantString:
      case OpKind::Alloca:
      case OpKind::ArrayAlloca:
      case OpKind::Label:
      case OpKind::Branch:
      case OpKind::BranchCond:
      case OpKind::Unreachable:
      case OpKind::Length:
        break;
      case OpKind::Print:
      case OpKind::PrintString:
        // Printing reads a value; it never retains it. An address on stdout is
        // not a reference the collector or the caller can hold.
        break;
      default: {
        // Anything this analysis does not model conservatively publishes every
        // pointer it reads: a construct it does not understand must never be
        // able to produce a "bounded" answer.
        for (const Value& value :
             {op.operand, op.lhs, op.rhs, op.result})
          if (root_of(value) >= 0 && value.id != op.result.id)
            record_escape(state_for(root_of(value)),
                          EscapeReason::StoredOutsideActivation);
        for (const Value& argument : op.arguments)
          if (root_of(argument) >= 0)
            record_escape(state_for(root_of(argument)),
                          EscapeReason::StoredOutsideActivation);
        for (const Value& element : op.aggregate_elements)
          if (root_of(element) >= 0)
            record_escape(state_for(root_of(element)),
                          EscapeReason::StoredOutsideActivation);
        break;
      }
    }
  }

  // Containment propagates: a pointer stored inside an object that escapes
  // leaves with it. The relation is monotone, so a fixed point over the edges is
  // both sound and enough - a cycle of containers stays unmarked until one of
  // them escapes on its own.
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& entry : families) {
      if (entry.second.reason != EscapeReason::None) continue;
      for (int container : entry.second.containers) {
        const auto found = families.find(container);
        if (found == families.end()) continue;
        if (found->second.reason == EscapeReason::None) continue;
        entry.second.reason = EscapeReason::ContainedInEscapingObject;
        changed = true;
        break;
      }
    }
  }

  // A self-referential object graph is a cycle. Arena storage would hold it
  // happily, but a cycle means the storage is in the program's object graph
  // rather than being a private scratch object, so it is treated as escaping.
  for (auto& entry : families) {
    std::unordered_set<int> visited;
    std::vector<int> pending(entry.second.contained.begin(),
                             entry.second.contained.end());
    bool cyclic = false;
    while (!pending.empty()) {
      const int current = pending.back();
      pending.pop_back();
      if (current == entry.first) {
        cyclic = true;
        break;
      }
      if (!visited.insert(current).second) continue;
      const auto found = families.find(current);
      if (found == families.end()) continue;
      for (int next : found->second.contained) pending.push_back(next);
    }
    if (cyclic) record_escape(entry.second, EscapeReason::CyclicGraph);
  }

  for (int id : allocation_results) {
    const int root = sets.find(id);
    AllocationSite site;
    const auto found = families.find(root);
    if (found != families.end()) {
      site.reason = found->second.reason;
      site.manually_released = found->second.manually_released;
    }
    site.lifetime = site.reason == EscapeReason::None
                        ? AllocationLifetime::Bounded
                        : AllocationLifetime::Escaping;
    const auto shared = members.find(root);
    if (shared != members.end()) site.aliases = shared->second;
    site.aliases.insert(id);
    analysis.sites_.emplace(id, std::move(site));
  }
  return analysis;
}

const AllocationSite* LifetimeAnalysis::site(int allocation_result) const {
  const auto found = sites_.find(allocation_result);
  return found == sites_.end() ? nullptr : &found->second;
}

}  // namespace tir

}  // namespace nirq
