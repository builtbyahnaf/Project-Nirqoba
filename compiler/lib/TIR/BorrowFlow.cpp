#include "nirqoba/TIR/BorrowFlow.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nirq {
using namespace std;
namespace {

constexpr const char* kBorrow = "NRQ4007";

bool is_borrowed(ValueOwnership ownership) {
  return ownership == ValueOwnership::Borrowed ||
         ownership == ValueOwnership::MutableBorrowed;
}

// Dotted TIR borrow paths are absolute source spellings such as `p.inner`.
// An empty path means the whole binding. A path overlaps another when it is the
// same path or an ancestor of it.
bool path_is_ancestor(const string& ancestor, const string& path) {
  if (ancestor == path) return true;
  if (ancestor.empty()) return true;
  if (path.size() <= ancestor.size()) return false;
  if (path.compare(0, ancestor.size(), ancestor) != 0) return false;
  const char boundary = path[ancestor.size()];
  return boundary == '.' || boundary == '[' || boundary == ']';
}

bool paths_overlap(const string& left, const string& right) {
  return path_is_ancestor(left, right) || path_is_ancestor(right, left);
}

struct ActiveBorrow {
  int value = -1;
  int owner_root = -1;
  string path;
  bool mutable_access = false;
  SourceLocation location;
};

// The abstract state at a block boundary. `active` is keyed by the borrowed
// pointer value; `invalid` holds owner storage roots that were destroyed or
// moved on at least one path reaching the block.
struct State {
  map<int, ActiveBorrow> active;
  set<int> invalid;
};

bool same_state(const State& left, const State& right) {
  if (left.invalid != right.invalid ||
      left.active.size() != right.active.size())
    return false;
  for (const auto& entry : left.active) {
    const auto found = right.active.find(entry.first);
    if (found == right.active.end()) return false;
    if (found->second.owner_root != entry.second.owner_root ||
        found->second.path != entry.second.path ||
        found->second.mutable_access != entry.second.mutable_access)
      return false;
  }
  return true;
}

void merge_state(State& destination, const State& source) {
  destination.invalid.insert(source.invalid.begin(), source.invalid.end());
  for (const auto& entry : source.active)
    destination.active.emplace(entry.first, entry.second);
}

struct Block {
  int id = -1;
  size_t begin = 0;
  size_t end = 0;
  vector<int> successors;
};

struct FunctionMaps {
  // Storage root of every value that denotes a location. A field GEP and the
  // aggregate slot it projects share the same root, which is what lets a drop
  // of the aggregate invalidate a borrow of one of its fields.
  unordered_map<int, int> root;
  // Root storage a loaded value was read from.
  unordered_map<int, int> loaded_from;
  // Parameter name for the value and for the alloca slot that holds it.
  unordered_map<int, string> parameter_value;
  unordered_map<int, string> parameter_slot;
  // Dotted storage path of a slot or projection, used to distinguish a write
  // to a borrowed field from a write to a disjoint field of the same owner.
  unordered_map<int, string> storage_path;
  // Values that carry a borrowed reference, directly or inside an aggregate.
  unordered_set<int> contains_borrow;
  // Pointer values derived from each borrow: the address-of result, copies,
  // loads from bindings that hold it, and borrowed call results forwarded from
  // it. A borrow is only a restriction while one of these aliases is live.
  unordered_map<int, unordered_set<int>> aliases;
};

int root_of(const FunctionMaps& maps, int value) {
  if (value < 0) return -1;
  const auto found = maps.root.find(value);
  if (found != maps.root.end()) return found->second;
  const auto loaded = maps.loaded_from.find(value);
  if (loaded != maps.loaded_from.end()) return loaded->second;
  return value;
}

void collect_aliases(const tir::Function& function, const tir::Module& module,
                     FunctionMaps& maps) {
  unordered_map<int, unordered_set<int>> value_borrows;
  unordered_map<int, unordered_set<int>> slot_borrows;
  for (const tir::Op& op : function.operations())
    if (op.kind == tir::OpKind::Borrow && op.operand.is_valid())
      value_borrows[op.operand.id].insert(op.operand.id);

  bool changed = true;
  while (changed) {
    changed = false;
    const auto add = [&](unordered_map<int, unordered_set<int>>& target,
                         int key,
                         const unordered_set<int>& source) {
      if (source.empty()) return;
      auto& destination = target[key];
      for (const int borrow : source)
        if (destination.insert(borrow).second) changed = true;
    };
    for (const tir::Op& op : function.operations()) {
      switch (op.kind) {
        case tir::OpKind::Load:
          if (op.result.is_valid()) {
            const auto found = slot_borrows.find(op.lhs.id);
            if (found != slot_borrows.end())
              add(value_borrows, op.result.id, found->second);
          }
          break;
        case tir::OpKind::Store:
          if (op.lhs.is_valid()) {
            const auto found = value_borrows.find(op.operand.id);
            if (found != value_borrows.end())
              add(slot_borrows, op.lhs.id, found->second);
          }
          break;
        case tir::OpKind::Move:
        case tir::OpKind::Cast:
          if (op.result.is_valid()) {
            const auto found = value_borrows.find(op.operand.id);
            if (found != value_borrows.end())
              add(value_borrows, op.result.id, found->second);
          }
          break;
        case tir::OpKind::Call:
          if (!op.result.is_valid()) break;
          for (const auto& callee : module.functions()) {
            if (callee->name() != op.callee ||
                callee->return_borrow_parameter().empty())
              continue;
            for (size_t parameter = 0; parameter < callee->parameters().size();
                 ++parameter) {
              if (callee->parameters()[parameter].name !=
                      callee->return_borrow_parameter() ||
                  parameter >= op.arguments.size())
                continue;
              const auto found = value_borrows.find(op.arguments[parameter].id);
              if (found != value_borrows.end())
                add(value_borrows, op.result.id, found->second);
            }
          }
          break;
        default:
          break;
      }
    }
  }

  for (const auto& entry : value_borrows)
    for (const int borrow : entry.second) maps.aliases[borrow].insert(entry.first);
}

void collect_maps(const tir::Function& function, FunctionMaps& maps) {
  for (const tir::Function::Parameter& parameter : function.parameters())
    maps.parameter_value.emplace(parameter.value.id, parameter.name);
  for (const tir::Op& op : function.operations()) {
    if (op.kind == tir::OpKind::Alloca) {
      if (op.result.is_valid()) {
        maps.root[op.result.id] = op.result.id;
        maps.storage_path[op.result.id] = op.slot_name;
        for (const tir::Function::Parameter& parameter : function.parameters())
          if (parameter.name == op.slot_name)
            maps.parameter_slot[op.result.id] = parameter.name;
      }
    } else if (op.kind == tir::OpKind::ArrayAlloca) {
      if (op.result.is_valid()) {
        maps.root[op.result.id] = op.result.id;
        maps.storage_path[op.result.id] = op.slot_name;
      }
    } else if (op.kind == tir::OpKind::GetElementPtr ||
               op.kind == tir::OpKind::SoaColumnPtr) {
      if (op.result.is_valid()) {
        maps.root[op.result.id] = root_of(maps, op.lhs.id);
        const auto parent = maps.storage_path.find(op.lhs.id);
        if (parent != maps.storage_path.end()) {
          string path = parent->second;
          if (!op.string_value.empty())
            path += (path.empty() ? "" : ".") + op.string_value;
          else
            path += "[]";
          maps.storage_path[op.result.id] = std::move(path);
        }
      }
    } else if (op.kind == tir::OpKind::AddressOf) {
      if (op.result.is_valid()) {
        maps.root[op.result.id] = root_of(maps, op.operand.id);
        const auto source = maps.storage_path.find(op.operand.id);
        if (source != maps.storage_path.end())
          maps.storage_path[op.result.id] = source->second;
      }
    } else if (op.kind == tir::OpKind::Load) {
      if (op.result.is_valid())
        maps.loaded_from[op.result.id] = root_of(maps, op.lhs.id);
    } else if (op.kind == tir::OpKind::Store) {
      if (op.result.is_valid())
        maps.root[op.result.id] = root_of(maps, op.lhs.id);
    } else if (op.kind == tir::OpKind::AggregateExtract) {
      if (op.result.is_valid())
        maps.root[op.result.id] = root_of(maps, op.operand.id);
    }
  }

  // A borrowed value is one whose ownership says so directly, or an aggregate
  // that packs one. The second form makes a borrowed-field escape visible at a
  // return even though the aggregate itself is not borrowed.
  for (const tir::Op& op : function.operations()) {
    if (!op.result.is_valid()) continue;
    if (is_borrowed(op.result.ownership)) {
      maps.contains_borrow.insert(op.result.id);
      continue;
    }
    if (op.kind == tir::OpKind::AggregatePack) {
      for (const tir::Value element : op.aggregate_elements)
        if (maps.contains_borrow.count(element.id) != 0) {
          maps.contains_borrow.insert(op.result.id);
          break;
        }
    } else if (op.kind == tir::OpKind::Move ||
               op.kind == tir::OpKind::Cast) {
      if (maps.contains_borrow.count(op.operand.id) != 0)
        maps.contains_borrow.insert(op.result.id);
    }
  }
}

vector<Block> build_cfg(const tir::Function& function,
                        unordered_map<int, int>& label_block) {
  vector<Block> blocks;
  blocks.push_back(Block{0, 0, 0, {}});
  const auto& operations = function.operations();
  for (size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].kind != tir::OpKind::Label) continue;
    if (index > blocks.back().begin)
      blocks.push_back(Block{static_cast<int>(blocks.size()), index, 0, {}});
    label_block[operations[index].label_id] = blocks.back().id;
  }
  for (size_t index = 0; index < blocks.size(); ++index)
    blocks[index].end =
        index + 1 < blocks.size() ? blocks[index + 1].begin : operations.size();
  for (Block& block : blocks) {
    if (block.end <= block.begin) continue;
    const tir::Op& terminator = operations[block.end - 1];
    if (terminator.kind == tir::OpKind::Branch) {
      const auto target = label_block.find(terminator.label_id);
      if (target != label_block.end())
        block.successors.push_back(target->second);
    } else if (terminator.kind == tir::OpKind::BranchCond) {
      // Both edges are real: a restriction that survives on either path must
      // survive the merge.
      const auto true_target = label_block.find(terminator.true_label);
      if (true_target != label_block.end())
        block.successors.push_back(true_target->second);
      const auto false_target = label_block.find(terminator.false_label);
      if (false_target != label_block.end())
        block.successors.push_back(false_target->second);
    }
  }
  return blocks;
}

void collect_uses(const tir::Op& op, unordered_set<int>& uses) {
  if (op.kind == tir::OpKind::Borrow) return;
  if (op.kind == tir::OpKind::EndBorrow) return;
  const auto add = [&](const tir::Value& value) {
    if (value.is_valid()) uses.insert(value.id);
  };
  add(op.operand);
  add(op.lhs);
  add(op.rhs);
  for (const tir::Value argument : op.arguments) add(argument);
  for (const tir::Value element : op.aggregate_elements) add(element);
}

struct Liveness {
  vector<unordered_set<int>> before;
  vector<unordered_set<int>> after;
};

Liveness compute_liveness(const tir::Function& function,
                          const vector<Block>& blocks) {
  const auto& operations = function.operations();
  vector<unordered_set<int>> live_in(blocks.size());
  vector<unordered_set<int>> live_out(blocks.size());
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t index = blocks.size(); index > 0; --index) {
      const Block& block = blocks[index - 1];
      unordered_set<int> out;
      for (const int successor : block.successors)
        out.insert(live_in[successor].begin(), live_in[successor].end());
      unordered_set<int> in = out;
      for (size_t op_index = block.end; op_index > block.begin; --op_index) {
        const tir::Op& op = operations[op_index - 1];
        if (op.result.is_valid()) in.erase(op.result.id);
        collect_uses(op, in);
      }
      if (in != live_in[index - 1] || out != live_out[index - 1]) {
        live_in[index - 1] = std::move(in);
        live_out[index - 1] = std::move(out);
        changed = true;
      }
    }
  }

  Liveness liveness;
  liveness.before.resize(operations.size());
  liveness.after.resize(operations.size());
  for (const Block& block : blocks) {
    unordered_set<int> live = live_out[block.id];
    for (size_t op_index = block.end; op_index > block.begin; --op_index) {
      const size_t index = op_index - 1;
      liveness.after[index] = live;
      const tir::Op& op = operations[index];
      if (op.result.is_valid()) live.erase(op.result.id);
      collect_uses(op, live);
      liveness.before[index] = live;
    }
  }
  return liveness;
}

bool borrow_is_live(const FunctionMaps& maps, const unordered_set<int>& live,
                    int borrow) {
  const auto found = maps.aliases.find(borrow);
  if (found == maps.aliases.end()) return false;
  for (const int alias : found->second)
    if (live.count(alias) != 0) return true;
  return false;
}

void transfer_block(const tir::Function& function, const FunctionMaps& maps,
                    const Block& block, State& state) {
  const auto& operations = function.operations();
  for (size_t index = block.begin; index < block.end; ++index) {
    const tir::Op& op = operations[index];
    switch (op.kind) {
      case tir::OpKind::Borrow:
        if (!op.operand.is_valid()) break;
        {
          ActiveBorrow borrow;
          borrow.value = op.operand.id;
          borrow.owner_root = root_of(maps, op.lhs.id);
          borrow.path = op.borrow_path;
          borrow.mutable_access = op.mutable_borrow;
          borrow.location = op.location;
          state.active[borrow.value] = std::move(borrow);
        }
        break;
      case tir::OpKind::EndBorrow:
        state.active.erase(op.operand.id);
        break;
      case tir::OpKind::Move: {
        const int root = root_of(maps, op.operand.id);
        if (root < 0) break;
        state.invalid.insert(root);
        for (auto it = state.active.begin(); it != state.active.end();) {
          if (it->second.owner_root == root)
            it = state.active.erase(it);
          else
            ++it;
        }
        break;
      }
      case tir::OpKind::Drop: {
        int root = -1;
        if (const tir::DestructionObligation* obligation =
                function.obligation(op.obligation_id))
          root = root_of(maps, obligation->owner.id);
        else
          root = root_of(maps, op.operand.id);
        if (root < 0) break;
        state.invalid.insert(root);
        for (auto it = state.active.begin(); it != state.active.end();) {
          if (it->second.owner_root == root)
            it = state.active.erase(it);
          else
            ++it;
        }
        break;
      }
      default:
        break;
    }
  }
}

const tir::Function* find_callee(const tir::Module& module,
                                 const string& name) {
  for (const auto& function : module.functions())
    if (function->name() == name) return function.get();
  return nullptr;
}

string function_value(const tir::Function& function, int id) {
  return "internal: function '" + function.name() + "', value %" +
         to_string(id) + ": ";
}

void verify_return(const tir::Function& function, const FunctionMaps& maps,
                   const tir::Op& op, DiagnosticEngine& diagnostics,
                   bool& ok) {
  if (!op.operand.is_valid()) return;
  const bool operand_borrowed = is_borrowed(op.operand.ownership);
  const bool declares_borrow = !function.return_borrow_parameter().empty();
  if (operand_borrowed && !declares_borrow) {
    diagnostics.error(kBorrow, op.location,
                      function_value(function, op.operand.id) +
                          "borrowed value escapes a return that declares no "
                          "source parameter");
    ok = false;
  } else if (!operand_borrowed && declares_borrow) {
    diagnostics.error(kBorrow, op.location,
                      "internal: function '" + function.name() +
                          "' declares a borrowed return but returns a "
                          "non-borrowed value");
    ok = false;
  }
  if (operand_borrowed && declares_borrow) {
    if (function.return_ownership() == ValueOwnership::MutableBorrowed &&
        op.operand.ownership != ValueOwnership::MutableBorrowed) {
      diagnostics.error(kBorrow, op.location,
                        "internal: mutable borrowed return requires a mutable "
                        "borrow operand");
      ok = false;
    } else if (function.return_ownership() == ValueOwnership::Borrowed &&
               op.operand.ownership == ValueOwnership::MutableBorrowed) {
      diagnostics.error(kBorrow, op.location,
                        "internal: immutable borrowed return cannot alias a "
                        "mutable borrow");
      ok = false;
    }
  }
  if (operand_borrowed && declares_borrow) {
    const int root = root_of(maps, op.operand.id);
    const auto parameter = maps.parameter_slot.find(root);
    if (parameter == maps.parameter_slot.end()) {
      const auto value_parameter = maps.parameter_value.find(root);
      if (value_parameter != maps.parameter_value.end() &&
          value_parameter->second != function.return_borrow_parameter()) {
        diagnostics.error(kBorrow, op.location,
                          function_value(function, op.operand.id) +
                              "returns a borrow of parameter '" +
                              value_parameter->second +
                              "' but declares '" +
                              function.return_borrow_parameter() + "'");
        ok = false;
      }
    } else if (parameter->second != function.return_borrow_parameter()) {
      diagnostics.error(kBorrow, op.location,
                        function_value(function, op.operand.id) +
                            "returns a borrow of parameter '" +
                            parameter->second + "' but declares '" +
                            function.return_borrow_parameter() + "'");
      ok = false;
    }
  }
  if (!operand_borrowed &&
      maps.contains_borrow.count(op.operand.id) != 0) {
    diagnostics.error(kBorrow, op.location,
                      function_value(function, op.operand.id) +
                          "aggregate containing a borrow escapes through a "
                          "return");
    ok = false;
  }
}

void verify_call(const tir::Function& function, const tir::Module& module,
                 const FunctionMaps& maps, const State& state,
                 const unordered_set<int>& live, const tir::Op& op,
                 DiagnosticEngine& diagnostics, bool& ok) {
  const tir::Function* callee = find_callee(module, op.callee);
  if (callee == nullptr) return;
  const auto& parameters = callee->parameters();
  for (size_t index = 0;
       index < op.arguments.size() && index < parameters.size(); ++index) {
    const tir::Value argument = op.arguments[index];
    if (!argument.is_valid()) continue;
    if (!is_borrowed(argument.ownership) &&
        maps.contains_borrow.count(argument.id) != 0) {
      diagnostics.error(kBorrow, op.location,
                        function_value(function, argument.id) +
                            "aggregate holding a borrow escapes through call '" +
                            op.callee + "'");
      ok = false;
      continue;
    }
    if (is_borrowed(argument.ownership)) continue;
    const int root = root_of(maps, argument.id);
    ActiveBorrow conflicting;
    bool found = false;
    for (const auto& entry : state.active) {
      if (entry.second.owner_root != root) continue;
      if (!borrow_is_live(maps, live, entry.second.value)) continue;
      conflicting = entry.second;
      found = true;
      break;
    }
    if (!found) continue;
    const ValueOwnership parameter_ownership = parameters[index].ownership;
    const bool transfers =
        parameter_ownership == ValueOwnership::Owned ||
        parameter_ownership == ValueOwnership::Shared;
    const bool exclusive =
        parameter_ownership == ValueOwnership::MutableBorrowed;
    if (!transfers && !exclusive) continue;
    diagnostics.error(
        kBorrow, op.location,
        function_value(function, argument.id) +
            (transfers ? "owner is moved into call '" + op.callee +
                             "' while a borrow is active"
                       : "owner is mutably borrowed by call '" + op.callee +
                             "' while another borrow is active"));
    diagnostics.note(kBorrow, conflicting.location,
                     "conflicting borrow is active here");
    ok = false;
  }
}

}  // namespace

bool verify_borrow_flow(const tir::Function& function, const tir::Module& module,
                        DiagnosticEngine& diagnostics) {
  bool ok = true;
  FunctionMaps maps;
  collect_maps(function, maps);
  collect_aliases(function, module, maps);

  unordered_map<int, int> label_block;
  const vector<Block> blocks = build_cfg(function, label_block);
  if (blocks.empty()) return true;

  const Liveness liveness = compute_liveness(function, blocks);
  vector<State> entry(blocks.size());
  vector<State> exit(blocks.size());
  vector<char> reached(blocks.size(), 0);
  vector<char> queued(blocks.size(), 0);
  vector<int> worklist;
  reached[0] = 1;
  queued[0] = 1;
  worklist.push_back(0);

  auto enqueue = [&](int block, const State& source) {
    if (!reached[block]) {
      entry[block] = source;
      reached[block] = 1;
      if (!queued[block]) {
        queued[block] = 1;
        worklist.push_back(block);
      }
      return;
    }
    State merged = entry[block];
    merge_state(merged, source);
    if (same_state(merged, entry[block])) return;
    entry[block] = std::move(merged);
    if (!queued[block]) {
      queued[block] = 1;
      worklist.push_back(block);
    }
  };

  while (!worklist.empty()) {
    const int block = worklist.back();
    worklist.pop_back();
    queued[block] = 0;
    State state = entry[block];
    transfer_block(function, maps, blocks[block], state);
    exit[block] = state;
    for (const int successor : blocks[block].successors)
      enqueue(successor, exit[block]);
  }

  // Verify each reachable block with its fixed-point entry state. Diagnostics
  // are emitted in this second pass, so a block that is revisited while the
  // worklist converges does not report the same conflict repeatedly.
  set<int> borrow_operations;
  const auto& operations = function.operations();
  for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
    if (!reached[block_index]) continue;
    const Block& block = blocks[block_index];
    State state = entry[block_index];
    for (size_t index = block.begin; index < block.end; ++index) {
      const tir::Op& op = operations[index];
      switch (op.kind) {
        case tir::OpKind::Borrow: {
          if (!op.operand.is_valid()) break;
          borrow_operations.insert(op.operand.id);
          const int root = root_of(maps, op.lhs.id);
          if (state.invalid.count(root) != 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(function, op.operand.id) +
                                  "borrows storage after its owner was moved "
                                  "or destroyed");
            ok = false;
          }
          if (state.active.count(op.operand.id) != 0) {
            diagnostics.error(kBorrow, op.location,
                              function_value(function, op.operand.id) +
                                  "is borrowed again while its previous borrow "
                                  "is still active");
            ok = false;
          }
          for (const auto& existing : state.active) {
            if (existing.second.owner_root != root) continue;
            if (!borrow_is_live(maps, liveness.before[index],
                                existing.second.value))
              continue;
            if (!paths_overlap(existing.second.path, op.borrow_path)) continue;
            if (!existing.second.mutable_access && !op.mutable_borrow) continue;
            diagnostics.error(
                kBorrow, op.location,
                function_value(function, op.operand.id) +
                    "conflicting borrow of the same owner is already active");
            diagnostics.note(kBorrow, existing.second.location,
                             "conflicting borrow is active here");
            ok = false;
            break;
          }
          bool matching_lifetime = false;
          bool any_lifetime = false;
          for (const tir::LifetimeRecord& lifetime : function.lifetimes()) {
            if (lifetime.kind != tir::LifetimeKind::Lexical ||
                lifetime.borrow.id != op.operand.id)
              continue;
            any_lifetime = true;
            if (lifetime.source.id == op.lhs.id &&
                lifetime.field_path == op.borrow_path &&
                lifetime.mutable_access == op.mutable_borrow)
              matching_lifetime = true;
          }
          if (any_lifetime && !matching_lifetime) {
            diagnostics.error(kBorrow, op.location,
                              function_value(function, op.operand.id) +
                                  "borrow lifetime record does not describe "
                                  "the borrow source, path and mutability");
            ok = false;
          }
          ActiveBorrow borrow;
          borrow.value = op.operand.id;
          borrow.owner_root = root;
          borrow.path = op.borrow_path;
          borrow.mutable_access = op.mutable_borrow;
          borrow.location = op.location;
          state.active[borrow.value] = std::move(borrow);
          break;
        }
        case tir::OpKind::EndBorrow: {
          const auto found = state.active.find(op.operand.id);
          if (found == state.active.end()) {
            diagnostics.error(kBorrow, op.location,
                              function_value(function, op.operand.id) +
                                  "ends a borrow that is not active on every "
                                  "path reaching this block");
            ok = false;
            break;
          }
          state.active.erase(found);
          break;
        }
        case tir::OpKind::Store: {
          if (!op.lhs.is_valid()) break;
          const int root = root_of(maps, op.lhs.id);
          const auto target = maps.storage_path.find(op.lhs.id);
          if (target == maps.storage_path.end()) break;
          for (const auto& active : state.active) {
            if (active.second.owner_root != root) continue;
            if (!borrow_is_live(maps, liveness.before[index],
                                active.second.value))
              continue;
            if (!paths_overlap(active.second.path, target->second)) continue;
            diagnostics.error(kBorrow, op.location,
                              "storage is written while a borrow of it is "
                              "active");
            diagnostics.note(kBorrow, active.second.location,
                             "borrow requiring read-only access is active "
                             "here");
            ok = false;
            break;
          }
          break;
        }
        case tir::OpKind::Move: {
          const int root = root_of(maps, op.operand.id);
          for (const auto& active : state.active) {
            if (active.second.owner_root != root) continue;
            if (!borrow_is_live(maps, liveness.before[index],
                                active.second.value))
              continue;
            diagnostics.error(kBorrow, op.location,
                              function_value(function, op.operand.id) +
                                  "owner is moved while a borrow is active");
            diagnostics.note(kBorrow, active.second.location,
                             "borrow requiring the owner is active here");
            ok = false;
            break;
          }
          if (root >= 0) {
            state.invalid.insert(root);
            for (auto it = state.active.begin(); it != state.active.end();) {
              if (it->second.owner_root == root)
                it = state.active.erase(it);
              else
                ++it;
            }
          }
          break;
        }
        case tir::OpKind::Drop: {
          int root = -1;
          if (const tir::DestructionObligation* obligation =
                  function.obligation(op.obligation_id))
            root = root_of(maps, obligation->owner.id);
          else
            root = root_of(maps, op.operand.id);
          for (const auto& active : state.active) {
            if (active.second.owner_root != root) continue;
            if (!borrow_is_live(maps, liveness.before[index],
                                active.second.value))
              continue;
            diagnostics.error(kBorrow, op.location,
                              "internal: owner is destroyed while a borrow is "
                              "active");
            diagnostics.note(kBorrow, active.second.location,
                             "borrow requiring the owner is active here");
            ok = false;
            break;
          }
          if (root >= 0) {
            state.invalid.insert(root);
            for (auto it = state.active.begin(); it != state.active.end();) {
              if (it->second.owner_root == root)
                it = state.active.erase(it);
              else
                ++it;
            }
          }
          break;
        }
        case tir::OpKind::Return: {
          const int returned_root =
              op.operand.is_valid() ? root_of(maps, op.operand.id) : -1;
          for (const auto& active : state.active) {
            if (active.second.owner_root == returned_root) continue;
            if (!borrow_is_live(maps, liveness.before[index],
                                active.second.value))
              continue;
            diagnostics.error(kBorrow, op.location,
                              "function returns while a local owner is still "
                              "borrowed");
            diagnostics.note(kBorrow, active.second.location,
                             "borrow is active here");
            ok = false;
            break;
          }
          verify_return(function, maps, op, diagnostics, ok);
          break;
        }
        case tir::OpKind::Call:
          verify_call(function, module, maps, state, liveness.before[index],
                      op, diagnostics, ok);
          break;
        default:
          break;
      }
    }
  }

  // The lifetime table is the persistent form of the borrow relationship. Every
  // lexical record must be backed by an actual Borrow operation, otherwise a
  // transformation could leave a stale lifetime that still looks valid. A
  // borrow also has exactly one lexical record; duplicates make later
  // transformations choose by accident, not by contract.
  map<int, int> lexical_records;
  for (const tir::LifetimeRecord& lifetime : function.lifetimes()) {
    if (lifetime.kind != tir::LifetimeKind::Lexical) continue;
    if (borrow_operations.count(lifetime.borrow.id) == 0) {
      diagnostics.error(kBorrow, lifetime.location,
                        "lexical lifetime record has no matching borrow "
                        "operation");
      ok = false;
    }
    if (++lexical_records[lifetime.borrow.id] > 1) {
      diagnostics.error(kBorrow, lifetime.location,
                        "borrow has more than one lexical lifetime record");
      ok = false;
    }
  }

  return ok;
}

}  // namespace nirq
