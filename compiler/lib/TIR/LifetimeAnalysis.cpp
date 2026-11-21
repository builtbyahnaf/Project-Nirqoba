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
  bool used_locally = false;
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

  const std::unordered_set<int> slot_addresses = collect_slot_addresses(function);
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
          // A load through a frame slot reads a name back. A load through the
          // storage itself (an element pointer) reads data, which is not a
          // pointer to the same allocation and must never join the family.
          if (sets.contains(op.lhs.id) && op.result.is_valid() &&
              slot_addresses.count(op.lhs.id) != 0)
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
  auto state_for = [&](int root) -> FamilyState& { return families[root]; };
  // Storage is "used" when the body touches it through the pointer rather than
  // merely naming it: an element index, a load, or a write into it.
  auto note_use = [&](const Value& address) {
    if (slot_addresses.count(address.id) != 0) return;
    const int root = root_of(address);
    if (root < 0) return;
    state_for(root).used_locally = true;
  };

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
        note_use(op.lhs);
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
        // Indexing the storage is the ordinary way it is used.
        note_use(op.lhs);
        break;
      case OpKind::SoaColumnPtr:
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
      case OpKind::Load:
        // A load through the storage reads data out of it; a load of the slot
        // reads the pointer back and is not a use of the storage itself.
        note_use(op.lhs);
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

  // Repeated work. A site runs once per call unless a back edge encloses it, and
  // sites that allocate one layout can share a pool. Both facts come from the
  // operation stream: labels plus branches that reach backwards mark loop bodies,
  // and the payload type plus its byte count name a layout.
  std::unordered_map<int, std::size_t> label_index;
  std::unordered_map<int, std::uint64_t> integer_constants;
  for (std::size_t index = 0; index < function.operations().size(); ++index) {
    const Op& op = function.operations()[index];
    if (op.kind == OpKind::Label) label_index.emplace(op.label_id, index);
    if (op.kind == OpKind::ConstantInt && op.result.is_valid())
      integer_constants.emplace(op.result.id,
                                static_cast<std::uint64_t>(op.integer_value));
  }
  std::vector<std::pair<std::size_t, std::size_t>> loop_spans;
  for (std::size_t index = 0; index < function.operations().size(); ++index) {
    const Op& op = function.operations()[index];
    if (op.kind != OpKind::Branch && op.kind != OpKind::BranchCond) continue;
    for (int target : {op.label_id, op.true_label, op.false_label}) {
      const auto found = label_index.find(target);
      if (found == label_index.end() || found->second >= index) continue;
      loop_spans.emplace_back(found->second, index);
    }
  }
  auto under_back_edge = [&](std::size_t index) {
    for (const auto& span : loop_spans)
      if (index >= span.first && index <= span.second) return true;
    return false;
  };
  // Where a loop-scoped region's create can run: on the edge into the header,
  // which is the fall-through from the operation before it. The header therefore
  // needs exactly one way in from outside the loop, and the operation before it
  // must leave its block open for the create.
  auto loop_scope_enterable =
      [&](const std::pair<std::size_t, std::size_t>& span) {
        if (span.first == 0 || span.first >= function.operations().size())
          return false;
        // The edge into the header is the operation before it: either a branch
        // the front end wrote explicitly (that target must be this header) or the
        // fall-through that leaves its block open for the create.
        const Op& before = function.operations()[span.first - 1];
        if (before.kind == OpKind::Branch) {
          const auto target = label_index.find(before.label_id);
          if (target == label_index.end() || target->second != span.first)
            return false;
        } else if (before.kind == OpKind::BranchCond ||
                   before.kind == OpKind::Return ||
                   before.kind == OpKind::Unreachable) {
          return false;
        }
        for (std::size_t index = 0; index < function.operations().size();
             ++index) {
          if (index >= span.first && index <= span.second) continue;
          // The sanctioned entry edge is the create's home, not a second way in.
          if (index + 1 == span.first) continue;
          const Op& op = function.operations()[index];
          if (op.kind != OpKind::Branch && op.kind != OpKind::BranchCond)
            continue;
          for (int target : {op.label_id, op.true_label, op.false_label}) {
            const auto found = label_index.find(target);
            if (found != label_index.end() && found->second == span.first)
              return false;
          }
        }
        return true;
      };

  // Every position a value is read at. A region may only be scoped to a loop when
  // the storage never leaves it, so containment is checked against the uses of
  // the whole family rather than against where the allocation happens to sit.
  std::unordered_map<int, std::vector<std::size_t>> use_positions;
  for (std::size_t index = 0; index < function.operations().size(); ++index) {
    const Op& op = function.operations()[index];
    auto note = [&](const Value& value) {
      if (value.is_valid()) use_positions[value.id].push_back(index);
    };
    note(op.operand);
    note(op.lhs);
    note(op.rhs);
    for (const Value& argument : op.arguments) note(argument);
    for (const Value& element : op.aggregate_elements) note(element);
  }

  std::unordered_map<int, std::size_t> operation_index;
  std::unordered_map<std::string, std::size_t> layout_counts;
  for (std::size_t index = 0; index < function.operations().size(); ++index) {
    const Op& op = function.operations()[index];
    if (!is_allocation_site(op)) continue;
    operation_index.emplace(op.result.id, index);
    const auto bytes = op.arguments.empty()
                           ? integer_constants.end()
                           : integer_constants.find(op.arguments[0].id);
    std::string key = op.result.type.element_type().mangle();
    key += bytes == integer_constants.end()
               ? ":dynamic"
               : ":" + std::to_string(bytes->second);
    ++layout_counts[key];
  }

  for (int id : allocation_results) {
    const int root = sets.find(id);
    AllocationSite site;
    const auto found = families.find(root);
    if (found != families.end()) {
      site.reason = found->second.reason;
      site.manually_released = found->second.manually_released;
      site.used_locally = found->second.used_locally;
    }
    site.lifetime = site.reason == EscapeReason::None
                        ? AllocationLifetime::Bounded
                        : AllocationLifetime::Escaping;
    const auto shared = members.find(root);
    if (shared != members.end()) site.aliases = shared->second;
    site.aliases.insert(id);

    const Op* allocation = nullptr;
    const auto position = operation_index.find(id);
    if (position != operation_index.end()) {
      allocation = &function.operations()[position->second];
      site.in_loop = under_back_edge(position->second);
      // Loop scope: the narrowest loop whose span contains the site and every
      // use of its storage. A wider loop would hold the storage past its last
      // use, and no loop at all means the frame owns the region.
      std::size_t width = 0;
      for (const auto& span : loop_spans) {
        const std::size_t here = position->second;
        if (here < span.first || here > span.second) continue;
        const std::size_t candidate = span.second - span.first;
        if (site.loop_scoped && candidate >= width) continue;
        if (!loop_scope_enterable(span)) continue;
        bool contained = true;
        for (int alias : site.aliases) {
          const auto uses = use_positions.find(alias);
          if (uses == use_positions.end()) continue;
          for (std::size_t use : uses->second) {
            if (use >= span.first && use <= span.second) continue;
            contained = false;
            break;
          }
          if (!contained) break;
        }
        if (!contained) continue;
        site.loop_scoped = true;
        site.scope_begin = span.first;
        site.scope_end = span.second;
        width = candidate;
      }
    }
    if (allocation != nullptr) {
      site.layout_key = allocation->result.type.element_type().mangle();
      if (!allocation->arguments.empty()) {
        const auto bytes = integer_constants.find(allocation->arguments[0].id);
        if (bytes != integer_constants.end()) {
          site.constant_payload = true;
          site.payload_bytes = bytes->second;
          site.layout_key += ":" + std::to_string(bytes->second);
        } else {
          site.layout_key += ":dynamic";
        }
      }
      const auto count = layout_counts.find(site.layout_key);
      site.identical_sites = count == layout_counts.end() ? 1 : count->second;
    }
    // The pooling shape: storage that dies with its activation - the pool is
    // destroyed on the way out - is plain enough to hold, is actually used, has
    // a slot size the compiler knows, and is handed out repeatedly. Anything the
    // stack or the arena descends on first keeps that cheaper answer; this flag
    // only says the site is a candidate.
    site.pool_eligible =
        site.lifetime == AllocationLifetime::Bounded && site.used_locally &&
        !site.manually_released && site.constant_payload &&
        site.payload_bytes > 0 && allocation != nullptr &&
        is_arena_eligible_payload(allocation->result.type.element_type()) &&
        (site.in_loop || site.identical_sites >= kPoolSiteThreshold);

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
