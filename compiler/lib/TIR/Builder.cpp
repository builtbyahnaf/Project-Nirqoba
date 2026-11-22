#include "nirqoba/TIR/Builder.h"

#include <utility>

namespace nirq {
using namespace std;
namespace {

constexpr const char* kUnresolvedSymbol = "NRQ4001";
constexpr const char* kUnsupportedExpression = "NRQ4002";

bool contains_owned_array(const Type& type) {
  if (type.is_owned_array() || type.is_owned_pointer()) return true;
  if (type.is_array()) return contains_owned_array(type.element_type());
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_owned_array(element)) return true;
  }
  if (type.is_struct()) {
    for (const auto& field : type.struct_fields())
      if (contains_owned_array(field.second)) return true;
  }
  return false;
}

// Ownership category of the value a release call destroys. The TIR value record
// must say what the release means; a bare Trivial load would let a Drop look
// like it discharged nothing.
ValueOwnership ownership_for_release(const std::string& callee) {
  if (callee == "memory.shared_release" || callee == "aggregate.shared_release")
    return ValueOwnership::Shared;
  if (callee == "memory.manual_release") return ValueOwnership::Manual;
  return ValueOwnership::Owned;
}

// Stable spelling of the storage a borrow selects. It mirrors the dotted field
// paths Sema already tracks, so a lifetime record and a Sema diagnostic name the
// same location. Dynamic indices are rendered as `[]` because their value is not
// known at compile time.
std::string borrow_path_spelling(const Expr& expression) {
  switch (expression.kind()) {
    case Expr::Kind::Name:
      return static_cast<const NameExpr&>(expression).name();
    case Expr::Kind::MemberAccess: {
      const auto& member = static_cast<const MemberAccessExpr&>(expression);
      const std::string base = borrow_path_spelling(member.base());
      if (base.empty()) return member.member();
      return base + "." + member.member();
    }
    case Expr::Kind::Index: {
      const auto& index = static_cast<const IndexExpr&>(expression);
      const std::string base = borrow_path_spelling(index.base());
      return base.empty() ? std::string("[]") : base + "[]";
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expression);
      if (unary.op() == UnaryOp::Dereference) {
        const std::string base = borrow_path_spelling(unary.operand());
        return base.empty() ? std::string("[]") : base + "[]";
      }
      return {};
    }
    default:
      return {};
  }
}

static bool is_release_callee(const std::string& callee) {
  return callee == "memory.deallocate" || callee == "memory.manual_release" ||
         callee == "memory.shared_release" ||
         callee == "array.owned_release" || callee == "soa.owned_release" ||
         callee == "aggregate.owned_drop" || callee == "aggregate.shared_release" ||
         callee == "file.close" || callee == "socket.close" ||
         callee == "mutex.destroy" || callee == "memory.secure_release" ||
         callee == "memory.unmap_file";
}

std::string cleanup_for_resource_domain(const std::string& domain) {
  if (domain == "file") return "file.close";
  if (domain == "socket") return "socket.close";
  if (domain == "mutex") return "mutex.destroy";
  if (domain == "mapping") return "memory.unmap_file";
  if (domain == "secure") return "memory.secure_release";
  return {};
}

tir::BinaryOp map_binary_op(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return tir::BinaryOp::Add;
    case BinaryOp::Subtract: return tir::BinaryOp::Subtract;
    case BinaryOp::Multiply: return tir::BinaryOp::Multiply;
    case BinaryOp::Divide: return tir::BinaryOp::Divide;
    case BinaryOp::Remainder: return tir::BinaryOp::Remainder;
    case BinaryOp::Exponentiate: return tir::BinaryOp::Exponentiate;
    case BinaryOp::BitwiseAnd: return tir::BinaryOp::BitwiseAnd;
    case BinaryOp::BitwiseOr: return tir::BinaryOp::BitwiseOr;
    case BinaryOp::BitwiseXor: return tir::BinaryOp::BitwiseXor;
    case BinaryOp::ShiftLeft: return tir::BinaryOp::ShiftLeft;
    case BinaryOp::ShiftRight: return tir::BinaryOp::ShiftRight;
    case BinaryOp::Equal: return tir::BinaryOp::Equal;
    case BinaryOp::NotEqual: return tir::BinaryOp::NotEqual;
    case BinaryOp::Less: return tir::BinaryOp::Less;
    case BinaryOp::LessEqual: return tir::BinaryOp::LessEqual;
    case BinaryOp::Greater: return tir::BinaryOp::Greater;
    case BinaryOp::GreaterEqual: return tir::BinaryOp::GreaterEqual;
    case BinaryOp::LogicalAnd: return tir::BinaryOp::LogicalAnd;
    case BinaryOp::LogicalOr: return tir::BinaryOp::LogicalOr;
  }
  return tir::BinaryOp::Add;
}

tir::CastKind determine_cast_kind(Type src, Type dst) {
  if (src.is_integer() && dst.is_integer()) {
    if (dst.bit_width() < src.bit_width()) return tir::CastKind::Trunc;
    if (src.is_signed_integer()) return tir::CastKind::SExt;
    return tir::CastKind::ZExt;
  }
  if (src.is_integer() && dst.is_float()) {
    return src.is_signed_integer() ? tir::CastKind::SIToFP : tir::CastKind::UIToFP;
  }
  if (src.is_float() && dst.is_integer()) {
    return dst.is_signed_integer() ? tir::CastKind::FPToSI : tir::CastKind::FPToUI;
  }
  if (src.is_float() && dst.is_float()) {
    return dst.bit_width() > src.bit_width() ? tir::CastKind::FPExt : tir::CastKind::FPTrunc;
  }
  if (src.is_bool() && dst.is_integer()) return tir::CastKind::BoolToInt;
  if (src.is_integer() && dst.is_bool()) return tir::CastKind::IntToBool;
  if (src.is_char() && dst.is_integer()) return tir::CastKind::CharToInt;
  if (src.is_integer() && dst.is_char()) return tir::CastKind::IntToChar;
  return tir::CastKind::Trunc;
}

}  // namespace

unique_ptr<tir::Module> TirBuilder::lower(const Program& program) {
  auto module = make_unique<tir::Module>();
  lower_module("", program, *module);
  return module;
}

void TirBuilder::lower_module(const string& symbol_prefix, const Program& program,
                              tir::Module& module) {
  for (const auto& function : program.functions()) {
    // An extern declaration names a C symbol: there is no body to lower, and the
    // ABI it must match is described at each call site.
    if (function->is_external()) continue;
    // The entry point is emitted as `main` and must not be prefixed, otherwise
    // the MSVC CRT would not find it.
    const string symbol =
        symbol_prefix.empty() || function->name() == "main"
            ? function->name()
            : symbol_prefix + "." + function->name();
    lower_function(*function, symbol, module);
  }
}

void TirBuilder::lower_function(const FunctionDecl& function, const string& symbol,
                                tir::Module& module) {
  auto body = make_unique<tir::Function>(symbol, function.return_type());
  if (!function.return_resource_domain().empty()) {
    body->set_return_ownership(ValueOwnership::Owned);
    body->set_return_resource_domain(function.return_resource_domain());
  }
  if (function.returns_shared()) body->set_return_ownership(ValueOwnership::Shared);
  else if (!function.return_borrow_parameter().empty())
    body->set_return_ownership(function.returns_mutable_borrow()
                                   ? ValueOwnership::MutableBorrowed
                                   : ValueOwnership::Borrowed);
  else if (function.return_type().is_owned_array())
    body->set_return_ownership(ValueOwnership::Owned);
  else if (contains_owned_array(function.return_type()))
    body->set_return_ownership(ValueOwnership::Owned);
  if (!function.return_borrow_parameter().empty()) {
    body->set_return_borrow_parameter(function.return_borrow_parameter());
    tir::LifetimeRecord lifetime;
    lifetime.kind = tir::LifetimeKind::Return;
    lifetime.source_parameter = function.return_borrow_parameter();
    lifetime.mutable_access = function.returns_mutable_borrow();
    lifetime.location = function.location();
    body->add_lifetime(std::move(lifetime));
  }
  tir::Function* function_ir = module.add_function(move(body));
  current_function_ = function_ir;

  scopes_.clear();
  loop_stack_.clear();
  deferred_expressions_.clear();
  conditional_depth_ = 0;
  transfer_context_ = tir::TransferKind::Local;
  borrow_scopes_.clear();
  escaping_borrows_.clear();
  push_scope();
  borrow_scopes_.emplace_back();
  deferred_expressions_.emplace_back();

  for (const Parameter& parameter : function.parameters()) {
    const ValueOwnership ownership = parameter.shared
        ? ValueOwnership::Shared
        : parameter.borrowed
        ? (parameter.mutable_borrow ? ValueOwnership::MutableBorrowed
                                    : ValueOwnership::Borrowed)
        : parameter.type.is_array_view()
        ? ValueOwnership::Borrowed
        : ((parameter.type.is_pointer() || parameter.type.is_owned_array() ||
            contains_owned_array(parameter.type) ||
            parameter.type.is_owned_string() ||
            parameter.type.is_dynamic_array())
                                       ? ValueOwnership::Owned
                                       : ValueOwnership::Trivial);
    tir::Value param_val = function_ir->allocate_value(parameter.type, ownership);
    function_ir->add_parameter(parameter.name, parameter.type, param_val, ownership,
                               parameter.resource_domain);
    if (parameter.borrowed) {
      // A borrowed parameter's lifetime is owned by the caller. Keeping the
      // record here preserves that relationship through TIR transformations.
      tir::LifetimeRecord lifetime;
      lifetime.borrow = param_val;
      lifetime.kind = tir::LifetimeKind::Parameter;
      lifetime.source_parameter = parameter.name;
      lifetime.mutable_access = parameter.mutable_borrow;
      lifetime.location = parameter.location;
      function_ir->add_lifetime(std::move(lifetime));
    }

    tir::Value slot = function_ir->allocate_value(parameter.type);
    tir::Op alloca;
    alloca.kind = tir::OpKind::Alloca;
    alloca.location = parameter.location;
    alloca.result = slot;
    alloca.slot_name = parameter.name;
    function_ir->add_operation(move(alloca));

    tir::Op store;
    store.kind = tir::OpKind::Store;
    store.location = parameter.location;
    store.lhs = slot;
    store.operand = param_val;
    store.result = slot;
    function_ir->add_operation(move(store));

    scopes_.back().emplace(parameter.name, slot);
    if ((parameter.type.is_pointer() || parameter.type.is_owned_array() ||
         contains_owned_array(parameter.type) ||
         parameter.type.is_owned_string() ||
         parameter.type.is_dynamic_array()) &&
        !parameter.borrowed) {
      DeferredCall cleanup;
      // A dynamic array parameter owns its buffer, so the callee releases it at
      // its own scope exit. The release takes the container value; the lowering
      // hands the runtime the buffer field, which is the only part it owns.
      cleanup.callee = parameter.shared ? "memory.shared_release"
          : parameter.type.is_owned_array() ? "array.owned_release"
          : contains_owned_array(parameter.type) ? "aggregate.owned_drop"
          : parameter.type.is_owned_string() ? "file.free_text"
          : parameter.type.is_dynamic_array() ? "nrq_array_free"
          : !parameter.resource_domain.empty()
                ? cleanup_for_resource_domain(parameter.resource_domain)
                : "memory.deallocate";
      cleanup.location = parameter.location;
      cleanup.automatic = true;
      cleanup.owner_slot = slot;
      register_obligation(slot, cleanup.callee, parameter.location,
                          *function_ir);
      deferred_expressions_.front().push_back(std::move(cleanup));
    }
  }

  lower_block(function.body(), *function_ir);
  if (!block_terminated(*function_ir)) {
    emit_end_borrows(*function_ir);
    emit_deferred_from(0, *function_ir);
  }
  deferred_expressions_.clear();
  borrow_scopes_.clear();
  current_function_ = nullptr;
  pop_scope();

  // Every block must end in a terminator. A void function may fall off the end
  // of its body, so it gets an implicit `return`. A non-void function can still
  // end on a merge label when every branch already returned (sema proves there
  // is no real fall-through), and that leftover block is simply unreachable.
  if (!block_terminated(*function_ir)) {
    tir::Op terminator;
    terminator.kind = function.return_type().is_void() ? tir::OpKind::Return
                                                       : tir::OpKind::Unreachable;
    terminator.location = function.location();
    function_ir->add_operation(move(terminator));
  }
}

bool TirBuilder::block_terminated(const tir::Function& function) {
  if (function.operations().empty()) return false;
  switch (function.operations().back().kind) {
    case tir::OpKind::Return:
    case tir::OpKind::Branch:
    case tir::OpKind::BranchCond:
      return true;
    default:
      return false;
  }
}

static bool contains_shared_pointer(const Type& type) {
  if (type.is_shared_pointer()) return true;
  if (type.is_array()) return contains_shared_pointer(type.element_type());
  if (type.is_tuple()) {
    for (const Type& element : type.tuple_elements())
      if (contains_shared_pointer(element)) return true;
  }
  if (type.is_struct()) {
    for (const auto& field : type.struct_fields())
      if (contains_shared_pointer(field.second)) return true;
  }
  return false;
}

void TirBuilder::lower_block(const BlockStmt& block, tir::Function& function) {
  push_scope();
  borrow_scopes_.emplace_back();
  deferred_expressions_.emplace_back();
  for (const StmtPtr& statement : block.statements()) {
    // Statements after `return`, `break` or `continue` are unreachable; the
    // backend cannot represent operations behind a terminator, so skip them.
    if (block_terminated(function)) break;
    lower_statement(*statement, function);
  }
  if (!block_terminated(function)) {
    // Ends borrows before the owning scope releases its values: a borrow must
    // never outlive the owner whose cleanup runs at this boundary.
    emit_end_borrows(function);
    emit_deferred_from(deferred_expressions_.size() - 1, function);
  }
  deferred_expressions_.pop_back();
  borrow_scopes_.pop_back();
  pop_scope();
}

void TirBuilder::emit_move(const tir::Value& source, tir::Function& function,
                           const SourceLocation& location, tir::Value& result) {
  if (!source.is_valid()) return;
  // A move produces a distinct value number even though lowering aliases the
  // source. The alias is what lets verification say "the source is no longer
  // usable" without inventing a runtime copy.
  result = function.allocate_value(source.type, source.ownership, source.arena_id,
                                  source.arena_kind);
  tir::Op move_op;
  move_op.kind = tir::OpKind::Move;
  move_op.location = location;
  move_op.operand = source;
  move_op.result = result;
  move_op.transfer_kind = transfer_context_;
  function.add_operation(std::move(move_op));
}

void TirBuilder::emit_end_borrows(tir::Function& function) {
  if (borrow_scopes_.empty()) return;
  for (tir::Value borrow : borrow_scopes_.back()) {
    // A borrow returned to the caller remains valid there and is not ended at
    // this scope boundary.
    if (escaping_borrows_.count(borrow.id) != 0) continue;
    tir::Op end;
    end.kind = tir::OpKind::EndBorrow;
    end.operand = borrow;
    function.add_operation(std::move(end));
  }
  borrow_scopes_.back().clear();
}

void TirBuilder::emit_all_end_borrows(tir::Function& function) {
  // A terminator closes every lexical scope at once, innermost first. The
  // vectors stay in place because the corresponding lower_block calls still pop
  // one entry each on the way out.
  for (size_t depth = borrow_scopes_.size(); depth > 0; --depth) {
    for (tir::Value borrow : borrow_scopes_[depth - 1]) {
      if (escaping_borrows_.count(borrow.id) != 0) continue;
      tir::Op end;
      end.kind = tir::OpKind::EndBorrow;
      end.operand = borrow;
      function.add_operation(std::move(end));
    }
    borrow_scopes_[depth - 1].clear();
  }
}

void TirBuilder::register_obligation(const tir::Value& slot,
                                     const std::string& callee,
                                     const SourceLocation& location,
                                     tir::Function& function) {
  if (!slot.is_valid() || callee.empty()) return;
  // One live obligation per owner slot. A slot that was moved out or already
  // discharged and then receives a new owner needs a fresh obligation: reusing
  // the transferred one would make the new destruction invisible to verification.
  for (const tir::DestructionObligation& obligation : function.obligations())
    if (obligation.owner.id == slot.id && !obligation.transferred &&
        !obligation.discharged)
      return;
  function.add_obligation(slot, slot.type, callee, /*resource_domain=*/{},
                          location);
}

// The operation that produced a value. Values are numbered in emission order, so
// the defining operation is the last one that names this result.
static const tir::Op* defining_operation(const tir::Function& function,
                                         const tir::Value& value) {
  for (auto op = function.operations().rbegin();
       op != function.operations().rend(); ++op)
    if (op->result.is_valid() && op->result.id == value.id) return &*op;
  return nullptr;
}

// The release one managed field needs, or false when the field owns nothing.
// Where the value came from decides the release: storage a C function handed over
// goes back to the foreign allocator, because the runtime's own releases hand a
// pointer to the deterministic allocator that issued it and abort on anything
// else.
static bool managed_field_release(const Type& type,
                                  const tir::Function& function,
                                  const tir::Value& value,
                                  std::string& release) {
  const tir::Op* origin = defining_operation(function, value);
  const bool foreign = origin != nullptr && origin->kind == tir::OpKind::Call &&
                       origin->external_call;
  if (type.is_owned_string()) {
    release = foreign ? "nrq_ffi_free" : "file.free_text";
    return true;
  }
  if (type.is_dynamic_array()) {
    release = foreign ? "nrq_ffi_free" : "nrq_array_free";
    return true;
  }
  // A fixed array owns its buffer the same way, but its release takes the whole
  // aggregate: only field zero is a pointer, and the runtime releases that.
  if (type.is_owned_array()) {
    release = "array.owned_release";
    return true;
  }
  // A resource handle's release is its domain's destructor, not a collector
  // collection: the registry names the domains it can release as teardown kinds.
  if (type.is_resource_pointer()) {
    release = cleanup_for_resource_domain(type.resource_domain());
    return !release.empty();
  }
  return false;
}

void TirBuilder::emit_destruction(const DeferredCall& deferred,
                                  tir::Function& function) {
  if (!deferred.owner_slot.is_valid()) return;
  // An aggregate owns its fields, so its destruction is one release per managed
  // field and never a release of the aggregate itself: the memory belongs to the
  // binding that holds it, and the fields own what is inside.
  if (!deferred.field_releases.empty()) {
    // One operation stands for the whole field walk. Lowering expands it - or
    // drops it, when the storage belongs to an arena or a pool and the registry
    // destroys the fields instead - so the decision is made where the ownership
    // of the storage is known, not here.
    tir::Op teardown;
    teardown.kind = tir::OpKind::AggregateFieldDestroy;
    teardown.location = deferred.location;
    teardown.lhs = deferred.owner_slot;
    teardown.operand = deferred.owner_slot;
    for (const std::pair<int, std::string>& field : deferred.field_releases) {
      teardown.release_fields.push_back(field.first);
      teardown.release_callees.push_back(field.second);
    }
    function.add_operation(std::move(teardown));
    // An aggregate the code still holds is emptied like any other owner. A
    // pointer already names storage that some other owner releases, so clearing it
    // would only detach the program from a value it may still read.
    if (!deferred.owner_slot.type.is_pointer()) {
      tir::Value null_value = function.allocate_value(deferred.owner_slot.type);
      tir::Op null_op;
      null_op.kind = tir::OpKind::ConstantNull;
      null_op.location = deferred.location;
      null_op.result = null_value;
      function.add_operation(std::move(null_op));
      tir::Op clear;
      clear.kind = tir::OpKind::Store;
      clear.location = deferred.location;
      clear.lhs = deferred.owner_slot;
      clear.operand = null_value;
      clear.result = deferred.owner_slot;
      function.add_operation(std::move(clear));
    }
    return;
  }
  tir::Value owner = function.allocate_value(
      deferred.owner_slot.type, ownership_for_release(deferred.callee));
  tir::Op load;
  load.kind = tir::OpKind::Load;
  load.location = deferred.location;
  load.lhs = deferred.owner_slot;
  load.result = owner;
  function.add_operation(std::move(load));

  register_obligation(deferred.owner_slot, deferred.callee, deferred.location,
                      function);
  tir::DestructionObligation* obligation =
      function.pending_obligation_for_owner(deferred.owner_slot);
  if (obligation != nullptr) {
    tir::Op drop;
    drop.kind = tir::OpKind::Drop;
    drop.location = deferred.location;
    drop.operand = owner;
    drop.obligation_id = obligation->id;
    drop.string_value = deferred.callee;
    function.add_operation(std::move(drop));
    obligation->discharged = true;
  } else {
    tir::Op call;
    call.kind = tir::OpKind::Call;
    call.location = deferred.location;
    call.callee = deferred.callee;
    call.arguments = {owner};
    function.add_operation(std::move(call));
  }

  tir::Value null_value = function.allocate_value(deferred.owner_slot.type);
  tir::Op null_op;
  null_op.kind = tir::OpKind::ConstantNull;
  null_op.location = deferred.location;
  null_op.result = null_value;
  function.add_operation(std::move(null_op));
  tir::Op clear;
  clear.kind = tir::OpKind::Store;
  clear.location = deferred.location;
  clear.lhs = deferred.owner_slot;
  clear.operand = null_value;
  clear.result = deferred.owner_slot;
  function.add_operation(std::move(clear));
}

void TirBuilder::emit_deferred_from(size_t scope_depth, tir::Function& function) {
  for (size_t depth = deferred_expressions_.size(); depth > scope_depth; --depth) {
    const auto& expressions = deferred_expressions_[depth - 1];
    for (auto deferred = expressions.rbegin(); deferred != expressions.rend(); ++deferred) {
      if (!deferred->active) continue;
      if (deferred->callee == "print" && deferred->arguments.size() == 1) {
        tir::Op print;
        print.kind = tir::OpKind::Print;
        print.location = deferred->location;
        print.operand = deferred->arguments.front();
        function.add_operation(move(print));
        continue;
      }
      if (deferred->automatic) {
        emit_destruction(*deferred, function);
        continue;
      }
      tir::Op call;
      call.kind = tir::OpKind::Call;
      call.location = deferred->location;
      call.callee = deferred->callee;
      call.arguments = deferred->arguments;
      if (deferred->result_type.is_valid() && !deferred->result_type.is_void())
        call.result = function.allocate_value(deferred->result_type);
      function.add_operation(move(call));
      if (deferred->clear_owner_slot_after_call &&
          deferred->owner_slot.is_valid()) {
        tir::Value null_value = function.allocate_value(deferred->owner_slot.type);
        tir::Op null_op;
        null_op.kind = tir::OpKind::ConstantNull;
        null_op.location = deferred->location;
        null_op.result = null_value;
        function.add_operation(std::move(null_op));
        tir::Op clear;
        clear.kind = tir::OpKind::Store;
        clear.location = deferred->location;
        clear.lhs = deferred->owner_slot;
        clear.operand = null_value;
        clear.result = deferred->owner_slot;
        function.add_operation(std::move(clear));
      }
    }
  }
}

void TirBuilder::suppress_automatic_cleanup(const tir::Value& slot) {
  if (!slot.is_valid()) return;
  for (auto& scope : deferred_expressions_)
    for (auto& pending : scope)
      if (pending.automatic && pending.active &&
          pending.owner_slot.is_valid() && pending.owner_slot.id == slot.id)
        pending.active = false;
  // An explicit release call destroys the value here, while any other transfer
  // passes the obligation to the new owner. Verification can distinguish the
  // two, and completeness no longer reports either as a lost obligation.
  if (current_function_ != nullptr) {
    if (tir::DestructionObligation* obligation =
            current_function_->pending_obligation_for_owner(slot)) {
      if (lowering_release_argument_)
        obligation->discharged = true;
      else
        obligation->transferred = true;
    }
  }
}

void TirBuilder::track_assignment_cleanup(const tir::Value& slot,
                                          const std::string& callee,
                                          const SourceLocation& location) {
  if (!slot.is_valid() || callee.empty()) return;
  std::size_t owner_scope = scopes_.empty() ? 0 : scopes_.size() - 1;
  for (std::size_t depth = scopes_.size(); depth > 0; --depth) {
    bool found_slot = false;
    for (const auto& binding : scopes_[depth - 1])
      if (binding.second.id == slot.id) found_slot = true;
    if (found_slot) {
      owner_scope = depth - 1;
      break;
    }
  }
  if (owner_scope >= deferred_expressions_.size()) return;

  DeferredCall* inactive = nullptr;
  for (auto& pending : deferred_expressions_[owner_scope]) {
    if (!pending.automatic || !pending.owner_slot.is_valid() ||
        pending.owner_slot.id != slot.id || pending.callee != callee)
      continue;
    if (pending.active) return;
    if (!inactive) inactive = &pending;
  }
  if (inactive) {
    inactive->active = true;
    inactive->location = location;
    if (current_function_ != nullptr)
      register_obligation(slot, callee, location, *current_function_);
    return;
  }

  DeferredCall cleanup;
  cleanup.callee = callee;
  cleanup.location = location;
  cleanup.automatic = true;
  cleanup.owner_slot = slot;
  if (current_function_ != nullptr)
    register_obligation(slot, callee, location, *current_function_);
  deferred_expressions_[owner_scope].push_back(std::move(cleanup));
}

void TirBuilder::lower_statement(const Stmt& statement, tir::Function& function) {
  switch (statement.kind()) {
    case Stmt::Kind::Block:
      lower_block(static_cast<const BlockStmt&>(statement), function);
      return;
    case Stmt::Kind::Destructuring: {
      const auto& dest = static_cast<const DestructuringStmt&>(statement);
      const tir::Value agg_val = lower_expression(dest.value(), function);
      const Type& agg_type = agg_val.type;
      const std::vector<std::string>& names = dest.names();
      const std::size_t count = names.size();

      for (std::size_t i = 0; i < count; ++i) {
        Type elem_type;
        if (agg_type.is_tuple() && i < agg_type.tuple_elements().size()) {
          elem_type = agg_type.tuple_elements()[i];
        } else if (agg_type.is_array()) {
          elem_type = agg_type.element_type();
        } else {
          continue;
        }

        // Extract element i from the aggregate.
        tir::Value extracted = function.allocate_value(elem_type);
        {
          tir::Op ext_op;
          ext_op.kind = tir::OpKind::AggregateExtract;
          ext_op.location = dest.location();
          ext_op.operand = agg_val;
          ext_op.field_index = static_cast<int>(i);
          ext_op.result = extracted;
          function.add_operation(std::move(ext_op));
        }

        // Alloca for the bound name.
        tir::Value slot = function.allocate_value(elem_type);
        {
          tir::Op alloca;
          alloca.kind = tir::OpKind::Alloca;
          alloca.location = dest.location();
          alloca.result = slot;
          alloca.slot_name = names[i];
          function.add_operation(std::move(alloca));
        }

        // Store extracted value into slot.
        {
          tir::Op store;
          store.kind = tir::OpKind::Store;
          store.location = dest.location();
          store.lhs = slot;
          store.operand = extracted;
          store.result = slot;
          function.add_operation(std::move(store));
        }

        scopes_.back().emplace(names[i], slot);
      }
      return;
    }
    case Stmt::Kind::VariableDeclaration: {
      const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
      const tir::Value initial = lower_expression(declaration.initializer(), function);
      const Type slot_type = declaration.has_annotation() ? declaration.annotation() : initial.type;

      tir::Value slot = function.allocate_value(slot_type);
      tir::Op alloca;
      alloca.kind = tir::OpKind::Alloca;
      alloca.location = declaration.location();
      alloca.result = slot;
      alloca.slot_name = declaration.name();
      function.add_operation(std::move(alloca));

      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = declaration.location();
      store.operand = initial;
      store.lhs = slot;
      store.result = slot;
      function.add_operation(std::move(store));

      // Copying an aggregate with shared fields acquires one reference per copied
      // field; a transferring initializer moves them instead and must not retain.
      const Expr& copy_source = declaration.initializer();
      const bool copies_shared_aggregate =
          copy_source.kind() == Expr::Kind::MemberAccess ||
          (copy_source.kind() == Expr::Kind::Name &&
           copy_source.ownership() == ValueOwnership::Shared);
      if (contains_shared_pointer(slot_type) && copies_shared_aggregate &&
          !copy_source.transfers_ownership()) {
        tir::Op retain;
        retain.kind = tir::OpKind::Call;
        retain.location = declaration.location();
        retain.callee = "aggregate.shared_retain";
        retain.arguments.push_back(initial);
        function.add_operation(std::move(retain));
      }

      scopes_.back().emplace(declaration.name(), slot);
      // The binding owns the aggregate, and the aggregate owns its managed
      // fields: destruction walks those fields rather than releasing the binding
      // as one value, whose memory is the binding's own frame slot.
      std::vector<std::pair<int, std::string>> field_releases;
      if (const tir::Op* origin = defining_operation(function, initial);
          origin != nullptr && origin->kind == tir::OpKind::AggregatePack &&
          initial.ownership != ValueOwnership::ArenaBound) {
        for (std::size_t i = 0; i < origin->release_fields.size() &&
                                i < origin->release_callees.size();
             ++i)
          field_releases.emplace_back(origin->release_fields[i],
                                      origin->release_callees[i]);
      }
      if (!field_releases.empty()) {
        DeferredCall cleanup;
        cleanup.callee = "aggregate.field_destroy";
        cleanup.location = declaration.location();
        cleanup.automatic = true;
        cleanup.owner_slot = slot;
        cleanup.field_releases = std::move(field_releases);
        deferred_expressions_.back().push_back(std::move(cleanup));
        return;
      }
      // Arena-bound storage belongs to the arena; a per-binding release would
      // free inside the region or bypass its bulk lifetime. Sema suppresses the
      // cleanup, and this guard keeps a stale cleanup annotation from
      // reintroducing it.
      if (!declaration.cleanup_callee().empty() &&
          initial.ownership != ValueOwnership::ArenaBound) {
        DeferredCall cleanup;
        cleanup.callee = declaration.cleanup_callee();
        cleanup.location = declaration.location();
        cleanup.automatic = true;
        cleanup.owner_slot = slot;
        register_obligation(slot, cleanup.callee, declaration.location(),
                            function);
        deferred_expressions_.back().push_back(std::move(cleanup));
      }
      return;
    }
    case Stmt::Kind::Assignment: {
      const auto& assignment = static_cast<const AssignmentStmt&>(statement);
      const tir::Value value = lower_expression(assignment.value(), function);
      const tir::Value slot = resolve_lvalue_ptr(assignment.target(), function);
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.target().location(),
                           "internal: assignment target has no TIR slot");
        return;
      }
      // An assignment to a simple name may overwrite an owner that is still
      // live on some path. The active automatic cleanup is the precise signal:
      // an unconditional move or explicit release deactivates it, while a
      // conditional release leaves it armed and the slot null only on that path.
      // Destroying now (a null release on the moved path) prevents the overwrite
      // from leaking the value that is still owned on the other path.
      bool destroys_before_store = false;
      if (assignment.target().kind() == Expr::Kind::Name &&
          !(assignment.value().kind() == Expr::Kind::Name &&
            static_cast<const NameExpr&>(assignment.value()).name() ==
                static_cast<const NameExpr&>(assignment.target()).name())) {
        for (auto& scope : deferred_expressions_) {
          for (auto& pending : scope) {
            if (!pending.automatic || !pending.active ||
                !pending.owner_slot.is_valid() ||
                pending.owner_slot.id != slot.id)
              continue;
            emit_destruction(pending, function);
            destroys_before_store = true;
            break;
          }
          if (destroys_before_store) break;
        }
      }
      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = assignment.location();
      store.operand = value;
      store.lhs = slot;
      store.result = slot;
      function.add_operation(std::move(store));
      const Expr& copy_source = assignment.value();
      const bool copies_shared_aggregate =
          copy_source.kind() == Expr::Kind::MemberAccess ||
          (copy_source.kind() == Expr::Kind::Name &&
           copy_source.ownership() == ValueOwnership::Shared);
      if (contains_shared_pointer(slot.type) && copies_shared_aggregate &&
          !copy_source.transfers_ownership()) {
        tir::Op retain;
        retain.kind = tir::OpKind::Call;
        retain.location = assignment.location();
        retain.callee = "aggregate.shared_retain";
        retain.arguments.push_back(value);
        function.add_operation(std::move(retain));
      }
      track_assignment_cleanup(slot, assignment.cleanup_callee(),
                               assignment.location());
      return;
    }
    case Stmt::Kind::CompoundAssignment: {
      const auto& assignment = static_cast<const CompoundAssignmentStmt&>(statement);
      const tir::Value slot = resolve_lvalue_ptr(assignment.target(), function);
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, assignment.target().location(),
                           "internal: compound assignment target has no TIR slot");
        return;
      }
      tir::Value current_val = function.allocate_value(slot.type);
      tir::Op load;
      load.kind = tir::OpKind::Load;
      load.location = assignment.target().location();
      load.lhs = slot;
      load.result = current_val;
      function.add_operation(std::move(load));

      tir::Value rhs_val = lower_expression(assignment.value(), function);
      tir::Value res_val = function.allocate_value(slot.type);
      tir::Op bin;
      bin.kind = tir::OpKind::Binary;
      bin.location = assignment.location();
      bin.binary_op = map_binary_op(assignment.op());
      bin.lhs = current_val;
      bin.rhs = rhs_val;
      bin.result = res_val;
      function.add_operation(std::move(bin));

      tir::Op store;
      store.kind = tir::OpKind::Store;
      store.location = assignment.location();
      store.operand = res_val;
      store.lhs = slot;
      store.result = slot;
      function.add_operation(std::move(store));
      return;
    }
    case Stmt::Kind::If: {
      const auto& if_stmt = static_cast<const IfStmt&>(statement);
      tir::Value cond = lower_expression(if_stmt.condition(), function);
      int then_label = function.allocate_label();
      int else_label = if_stmt.has_else() ? function.allocate_label() : -1;
      int merge_label = function.allocate_label();

      tir::Op br_cond;
      br_cond.kind = tir::OpKind::BranchCond;
      br_cond.location = if_stmt.location();
      br_cond.operand = cond;
      br_cond.true_label = then_label;
      br_cond.false_label = if_stmt.has_else() ? else_label : merge_label;
      function.add_operation(std::move(br_cond));

      // Then block
      tir::Op then_lbl;
      then_lbl.kind = tir::OpKind::Label;
      then_lbl.label_id = then_label;
      function.add_operation(std::move(then_lbl));
      {
        ConditionalRegion branch_region(conditional_depth_);
        lower_block(if_stmt.then_branch(), function);
      }
      if (!block_terminated(function)) {
        tir::Op br_then;
        br_then.kind = tir::OpKind::Branch;
        br_then.label_id = merge_label;
        function.add_operation(std::move(br_then));
      }

      // Else block
      if (if_stmt.has_else()) {
        tir::Op else_lbl;
        else_lbl.kind = tir::OpKind::Label;
        else_lbl.label_id = else_label;
        function.add_operation(std::move(else_lbl));
        {
          ConditionalRegion branch_region(conditional_depth_);
          lower_statement(*if_stmt.else_branch(), function);
        }
        if (!block_terminated(function)) {
          tir::Op br_else;
          br_else.kind = tir::OpKind::Branch;
          br_else.label_id = merge_label;
          function.add_operation(std::move(br_else));
        }
      }

      // Merge block
      tir::Op merge_lbl;
      merge_lbl.kind = tir::OpKind::Label;
      merge_lbl.label_id = merge_label;
      function.add_operation(std::move(merge_lbl));
      return;
    }
    case Stmt::Kind::While: {
      const auto& while_stmt = static_cast<const WhileStmt&>(statement);
      int header_label = function.allocate_label();
      int body_label = function.allocate_label();
      int exit_label = function.allocate_label();

      loop_stack_.push_back(LoopContext{exit_label, header_label,
                                        deferred_expressions_.size()});

      tir::Op br_head;
      br_head.kind = tir::OpKind::Branch;
      br_head.label_id = header_label;
      function.add_operation(std::move(br_head));

      // Header block
      tir::Op head_lbl;
      head_lbl.kind = tir::OpKind::Label;
      head_lbl.label_id = header_label;
      function.add_operation(std::move(head_lbl));
      tir::Value cond;
      {
        ConditionalRegion condition_region(conditional_depth_);
        cond = lower_expression(while_stmt.condition(), function);
      }
      tir::Op br_cond;
      br_cond.kind = tir::OpKind::BranchCond;
      br_cond.location = while_stmt.location();
      br_cond.operand = cond;
      br_cond.true_label = body_label;
      br_cond.false_label = exit_label;
      function.add_operation(std::move(br_cond));

      // Body block
      tir::Op body_lbl;
      body_lbl.kind = tir::OpKind::Label;
      body_lbl.label_id = body_label;
      function.add_operation(std::move(body_lbl));
      {
        ConditionalRegion body_region(conditional_depth_);
        lower_block(while_stmt.body(), function);
      }
      if (!block_terminated(function)) {
        tir::Op br_loop;
        br_loop.kind = tir::OpKind::Branch;
        br_loop.label_id = header_label;
        function.add_operation(std::move(br_loop));
      }

      // Exit block
      tir::Op exit_lbl;
      exit_lbl.kind = tir::OpKind::Label;
      exit_lbl.label_id = exit_label;
      function.add_operation(std::move(exit_lbl));

      loop_stack_.pop_back();
      return;
    }
    case Stmt::Kind::For: {
      const auto& for_stmt = static_cast<const ForStmt&>(statement);
      // The initializer has its own scope: its owned values are destroyed when
      // the loop exits, not when the enclosing block exits. Continue does not
      // leave this scope; break, a false condition, and return do.
      push_scope();
      deferred_expressions_.emplace_back();
      const std::size_t for_cleanup_scope = deferred_expressions_.size() - 1;
      if (for_stmt.has_initializer()) {
        lower_statement(*for_stmt.initializer(), function);
      }

      int header_label = function.allocate_label();
      int body_label = function.allocate_label();
      int step_label = function.allocate_label();
      int exit_label = function.allocate_label();

      loop_stack_.push_back(LoopContext{exit_label, step_label,
                                        deferred_expressions_.size()});

      tir::Op br_head;
      br_head.kind = tir::OpKind::Branch;
      br_head.label_id = header_label;
      function.add_operation(std::move(br_head));

      // Header block
      tir::Op head_lbl;
      head_lbl.kind = tir::OpKind::Label;
      head_lbl.label_id = header_label;
      function.add_operation(std::move(head_lbl));
      if (for_stmt.has_condition()) {
        ConditionalRegion condition_region(conditional_depth_);
        tir::Value cond = lower_expression(*for_stmt.condition(), function);
        tir::Op br_cond;
        br_cond.kind = tir::OpKind::BranchCond;
        br_cond.location = for_stmt.location();
        br_cond.operand = cond;
        br_cond.true_label = body_label;
        br_cond.false_label = exit_label;
        function.add_operation(std::move(br_cond));
      } else {
        tir::Op br_body;
        br_body.kind = tir::OpKind::Branch;
        br_body.label_id = body_label;
        function.add_operation(std::move(br_body));
      }

      // Body block
      tir::Op body_lbl;
      body_lbl.kind = tir::OpKind::Label;
      body_lbl.label_id = body_label;
      function.add_operation(std::move(body_lbl));
      {
        ConditionalRegion body_region(conditional_depth_);
        lower_block(for_stmt.body(), function);
      }
      if (!block_terminated(function)) {
        tir::Op br_step;
        br_step.kind = tir::OpKind::Branch;
        br_step.label_id = step_label;
        function.add_operation(std::move(br_step));
      }

      // Step block
      tir::Op step_lbl;
      step_lbl.kind = tir::OpKind::Label;
      step_lbl.label_id = step_label;
      function.add_operation(std::move(step_lbl));
      if (for_stmt.has_step()) {
        ConditionalRegion step_region(conditional_depth_);
        lower_statement(*for_stmt.step(), function);
      }
      if (!block_terminated(function)) {
        tir::Op br_next;
        br_next.kind = tir::OpKind::Branch;
        br_next.label_id = header_label;
        function.add_operation(std::move(br_next));
      }

      // Exit block. Both break and a false condition arrive here, so the
      // initializer scope is released exactly once on the loop's exit edge.
      tir::Op exit_lbl;
      exit_lbl.kind = tir::OpKind::Label;
      exit_lbl.label_id = exit_label;
      function.add_operation(std::move(exit_lbl));
      emit_deferred_from(for_cleanup_scope, function);

      loop_stack_.pop_back();
      deferred_expressions_.pop_back();
      pop_scope();
      return;
    }
    case Stmt::Kind::Break: {
      emit_deferred_from(loop_stack_.back().deferred_scope_depth, function);
      tir::Op br;
      br.kind = tir::OpKind::Branch;
      br.location = statement.location();
      br.label_id = loop_stack_.back().break_label;
      function.add_operation(std::move(br));
      return;
    }
    case Stmt::Kind::Continue: {
      emit_deferred_from(loop_stack_.back().deferred_scope_depth, function);
      tir::Op br;
      br.kind = tir::OpKind::Branch;
      br.location = statement.location();
      br.label_id = loop_stack_.back().continue_label;
      function.add_operation(std::move(br));
      return;
    }
    case Stmt::Kind::Return: {
      const auto& return_statement = static_cast<const ReturnStmt&>(statement);
      tir::Op op;
      op.kind = tir::OpKind::Return;
      op.location = return_statement.location();
      if (return_statement.has_value()) {
        TransferContextGuard guard(transfer_context_, tir::TransferKind::Return);
        op.operand = lower_expression(*return_statement.value(), function);
        if (op.operand.is_valid() &&
            (op.operand.ownership == ValueOwnership::Borrowed ||
             op.operand.ownership == ValueOwnership::MutableBorrowed))
          escaping_borrows_.insert(op.operand.id);
      }
      // A return ends every lexical scope. Borrows owned by those scopes must be
      // marked ended before the terminator; only a borrow being returned is
      // deliberately left open for the caller.
      emit_all_end_borrows(function);
      emit_deferred_from(0, function);
      function.add_operation(std::move(op));
      return;
    }
    case Stmt::Kind::DeferredCleanup: {
      const auto& deferred = static_cast<const DeferredCleanupStmt&>(statement);
      const auto& call = static_cast<const CallExpr&>(deferred.expression());
      DeferredCall lowered;
      lowered.callee = call.has_resolved_symbol() ? call.resolved_symbol() : call.callee();
      lowered.location = call.location();
      lowered.result_type = call.type();
      lowered.arguments.reserve(call.arguments().size());
      for (const ExprPtr& argument : call.arguments()) {
        if (argument->kind() == Expr::Kind::Name) {
          const tir::Value slot = lookup_slot(
              static_cast<const NameExpr&>(*argument).name());
          if (slot.is_valid()) {
            // An explicit defer in the owner's own lexical scope runs exactly
            // when that scope's automatic cleanup would run, so it supersedes
            // it. A defer outside any branch or loop is unconditional and may
            // also supersede an outer scope's declaration cleanup. A defer in
            // a conditional scope keeps outer automatic fallbacks; it still
            // clears the slot when it does run.
            auto suppress_matching = [&](std::vector<DeferredCall>& scope) {
              for (auto& pending : scope) {
                if (pending.automatic && pending.active &&
                    pending.owner_slot.is_valid() &&
                    pending.owner_slot.id == slot.id &&
                    pending.callee == lowered.callee) {
                  lowered.owner_slot = slot;
                  lowered.clear_owner_slot_after_call = true;
                  pending.active = false;
                }
              }
            };
            suppress_matching(deferred_expressions_.back());
            if (conditional_depth_ == 0)
              for (auto& scope : deferred_expressions_) suppress_matching(scope);
            // The defer itself is the destruction of this owner. Without this,
            // a superseded automatic cleanup leaves a resolved obligation behind.
            if (lowered.owner_slot.is_valid() && current_function_ != nullptr)
              if (tir::DestructionObligation* obligation =
                      current_function_->pending_obligation_for_owner(
                          lowered.owner_slot))
                obligation->discharged = true;
            if (!lowered.owner_slot.is_valid()) {
              for (auto& scope : deferred_expressions_)
                for (auto& pending : scope)
                  if (pending.automatic && pending.active &&
                      pending.owner_slot.is_valid() &&
                      pending.owner_slot.id == slot.id &&
                      pending.callee == lowered.callee) {
                    lowered.owner_slot = slot;
                    lowered.clear_owner_slot_after_call = true;
                  }
            }
          }
        }
        lowered.arguments.push_back(lower_expression(*argument, function));
      }
      deferred_expressions_.back().push_back(std::move(lowered));
      return;
    }
    case Stmt::Kind::Expression: {
      const auto& expression_statement = static_cast<const ExprStmt&>(statement);
      lower_expression(expression_statement.expr(), function);
      return;
    }
  }
}

tir::Value TirBuilder::lower_expression(const Expr& expr, tir::Function& function) {
  switch (expr.kind()) {
    case Expr::Kind::IntegerLiteral: {
      const auto& literal = static_cast<const IntegerLiteralExpr&>(expr);
      Type lit_type = literal.type().is_valid() ? literal.type() : Type::int_type();
      tir::Value result = function.allocate_value(lit_type);
      tir::Op op;
      // A null pointer literal is a null pointer value, not the integer zero: the op
      // keeps the pointer type so printing and comparisons see a pointer.
      op.kind = literal.is_null_pointer() ? tir::OpKind::ConstantNull
                                         : tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::FloatLiteral: {
      const auto& literal = static_cast<const FloatLiteralExpr&>(expr);
      Type lit_type = literal.type().is_valid() ? literal.type() : Type::f64_type();
      tir::Value result = function.allocate_value(lit_type);
      tir::Op op;
      op.kind = tir::OpKind::ConstantFloat;
      op.location = literal.location();
      op.result = result;
      op.float_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::CharLiteral: {
      const auto& literal = static_cast<const CharLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::char_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::BoolLiteral: {
      const auto& literal = static_cast<const BoolLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::bool_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantInt;
      op.location = literal.location();
      op.result = result;
      op.integer_value = literal.value() ? 1 : 0;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::StringLiteral: {
      const auto& literal = static_cast<const StringLiteralExpr&>(expr);
      tir::Value result = function.allocate_value(Type::string_type());
      tir::Op op;
      op.kind = tir::OpKind::ConstantString;
      op.location = literal.location();
      op.result = result;
      op.string_value = literal.value();
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Name: {
      const auto& name = static_cast<const NameExpr&>(expr);
      const tir::Value slot = lookup_slot(name.name());
      if (!slot.is_valid()) {
        diagnostics_.error(kUnresolvedSymbol, name.location(),
                           "internal: variable '" + name.name() + "' has no TIR slot");
        return tir::Value{};
      }
      tir::Value result = function.allocate_value(slot.type, name.ownership(),
                                                  name.arena_id(),
                                                  name.arena_kind());
      tir::Op op;
      op.kind = tir::OpKind::Load;
      op.location = name.location();
      op.result = result;
      op.lhs = slot;
      function.add_operation(std::move(op));
      if (expr.transfers_ownership()) {
        // First make the ownership transfer explicit, then perform the slot
        // invalidation that already existed. Lowering treats the Move as an
        // alias; verification uses it to reject use of the moved value.
        tir::Value moved;
        emit_move(result, function, name.location(), moved);
        result = moved;
        tir::Value null_value = function.allocate_value(slot.type);
        tir::Op null_op;
        null_op.kind = tir::OpKind::ConstantNull;
        null_op.location = name.location();
        null_op.result = null_value;
        function.add_operation(std::move(null_op));
        tir::Op clear;
        clear.kind = tir::OpKind::Store;
        clear.location = name.location();
        clear.lhs = slot;
        clear.operand = null_value;
        clear.result = slot;
        function.add_operation(std::move(clear));
        // A transfer outside any branch, loop body, or conditional evaluation
        // executes on every path that reaches the owner's scope exit, so the
        // automatic cleanup can never observe a live value there.
        if (conditional_depth_ == 0) suppress_automatic_cleanup(slot);
      }
      return result;
    }
    case Expr::Kind::Cast: {
      const auto& cast = static_cast<const CastExpr&>(expr);
      const tir::Value operand = lower_expression(cast.operand(), function);
      tir::CastKind cast_kind = determine_cast_kind(cast.operand().type(), cast.target_type());
      tir::Value result = function.allocate_value(cast.target_type());
      tir::Op op;
      op.kind = tir::OpKind::Cast;
      op.location = cast.location();
      op.result = result;
      op.operand = operand;
      op.cast_kind = cast_kind;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Unary: {
      const auto& unary = static_cast<const UnaryExpr&>(expr);
      if (unary.op() == UnaryOp::AddressOf) {
        // `&x` yields the address of the existing stack slot. Sema has already
        // restricted the operand to an addressable lvalue, so the slot lookup can
        // only fail if the two stages disagree; that is reported as internal.
        const tir::Value slot = resolve_lvalue_ptr(unary.operand(), function);
        if (!slot.is_valid()) {
          diagnostics_.error(kUnresolvedSymbol, unary.location(),
                             "internal: address-of target has no TIR slot");
          return tir::Value{};
        }
        const bool mutable_borrow =
            unary.ownership() != ValueOwnership::Borrowed;
        tir::Value address = function.allocate_value(
            unary.type().is_valid() ? unary.type() : Type::pointer_type(slot.type),
            mutable_borrow ? ValueOwnership::MutableBorrowed
                           : ValueOwnership::Borrowed,
            slot.arena_id, slot.arena_kind);
        tir::Op op;
        op.kind = tir::OpKind::AddressOf;
        op.location = unary.location();
        op.result = address;
        op.operand = slot;
        function.add_operation(std::move(op));

        // Record the borrow relationship explicitly. `slot` is the owner's
        // storage, so the borrow's lifetime is bound to that storage's scope.
        tir::Op borrow;
        borrow.kind = tir::OpKind::Borrow;
        borrow.location = unary.location();
        borrow.operand = address;
        borrow.lhs = slot;
        borrow.mutable_borrow = mutable_borrow;
        borrow.borrow_path = borrow_path_spelling(unary.operand());
        function.add_operation(std::move(borrow));
        tir::LifetimeRecord lifetime;
        lifetime.borrow = address;
        lifetime.source = slot;
        lifetime.kind = tir::LifetimeKind::Lexical;
        lifetime.field_path = borrow_path_spelling(unary.operand());
        lifetime.mutable_access = mutable_borrow;
        lifetime.location = unary.location();
        function.add_lifetime(std::move(lifetime));
        if (!borrow_scopes_.empty()) borrow_scopes_.back().push_back(address);
        return address;
      }
      if (unary.op() == UnaryOp::Dereference) {
        // `*p` is `p[0]`: one GEP at offset zero and a load, reusing the pointer
        // element addressing the index path already uses.
        const tir::Value pointer = lower_expression(unary.operand(), function);
        if (!pointer.is_valid()) return tir::Value{};
        const Type elem_type =
            unary.type().is_valid() ? unary.type() : pointer.type.element_type();
        tir::Value zero = function.allocate_value(Type::u64_type());
        tir::Op zero_op;
        zero_op.kind = tir::OpKind::ConstantInt;
        zero_op.location = unary.location();
        zero_op.integer_value = 0;
        zero_op.result = zero;
        function.add_operation(std::move(zero_op));
        tir::Value elem_ptr = function.allocate_value(
            elem_type, ValueOwnership::Trivial, pointer.arena_id,
            pointer.arena_kind);
        tir::Op gep;
        gep.kind = tir::OpKind::GetElementPtr;
        gep.location = unary.location();
        gep.lhs = pointer;
        gep.operand = zero;
        gep.field_index = -1;
        gep.result = elem_ptr;
        function.add_operation(std::move(gep));
        tir::Value loaded = function.allocate_value(elem_type);
        tir::Op load;
        load.kind = tir::OpKind::Load;
        load.location = unary.location();
        load.lhs = elem_ptr;
        load.result = loaded;
        function.add_operation(std::move(load));
        return loaded;
      }
      const tir::Value operand = lower_expression(unary.operand(), function);
      if (unary.op() == UnaryOp::Plus) {
        return operand;
      }
      tir::UnaryOp unary_op = tir::UnaryOp::Negate;
      switch (unary.op()) {
        case UnaryOp::Negate: unary_op = tir::UnaryOp::Negate; break;
        case UnaryOp::LogicalNot: unary_op = tir::UnaryOp::LogicalNot; break;
        case UnaryOp::BitwiseNot: unary_op = tir::UnaryOp::BitwiseNot; break;
        default: break;
      }
      tir::Value result = function.allocate_value(unary.type().is_valid() ? unary.type() : operand.type);
      tir::Op op;
      op.kind = tir::OpKind::Unary;
      op.location = unary.location();
      op.result = result;
      op.operand = operand;
      op.unary_op = unary_op;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Binary: {
      const auto& binary = static_cast<const BinaryExpr&>(expr);

      if (binary.op() == BinaryOp::LogicalAnd || binary.op() == BinaryOp::LogicalOr) {
        tir::Value slot = function.allocate_value(Type::bool_type());
        tir::Op alloca;
        alloca.kind = tir::OpKind::Alloca;
        alloca.location = binary.location();
        alloca.result = slot;
        alloca.slot_name = "$short_circuit";
        function.add_operation(std::move(alloca));

        tir::Value lhs = lower_expression(binary.lhs(), function);
        tir::Op store_lhs;
        store_lhs.kind = tir::OpKind::Store;
        store_lhs.location = binary.location();
        store_lhs.lhs = slot;
        store_lhs.operand = lhs;
        store_lhs.result = slot;
        function.add_operation(std::move(store_lhs));

        int rhs_label = function.allocate_label();
        int merge_label = function.allocate_label();

        tir::Op br_cond;
        br_cond.kind = tir::OpKind::BranchCond;
        br_cond.location = binary.location();
        br_cond.operand = lhs;
        if (binary.op() == BinaryOp::LogicalAnd) {
          br_cond.true_label = rhs_label;
          br_cond.false_label = merge_label;
        } else {
          br_cond.true_label = merge_label;
          br_cond.false_label = rhs_label;
        }
        function.add_operation(std::move(br_cond));

        tir::Op rhs_lbl;
        rhs_lbl.kind = tir::OpKind::Label;
        rhs_lbl.label_id = rhs_label;
        function.add_operation(std::move(rhs_lbl));

        tir::Value rhs;
        {
          ConditionalRegion rhs_region(conditional_depth_);
          rhs = lower_expression(binary.rhs(), function);
        }
        tir::Op store_rhs;
        store_rhs.kind = tir::OpKind::Store;
        store_rhs.location = binary.location();
        store_rhs.lhs = slot;
        store_rhs.operand = rhs;
        store_rhs.result = slot;
        function.add_operation(std::move(store_rhs));

        tir::Op br_merge;
        br_merge.kind = tir::OpKind::Branch;
        br_merge.label_id = merge_label;
        function.add_operation(std::move(br_merge));

        tir::Op merge_lbl;
        merge_lbl.kind = tir::OpKind::Label;
        merge_lbl.label_id = merge_label;
        function.add_operation(std::move(merge_lbl));

        tir::Value final_val = function.allocate_value(Type::bool_type());
        tir::Op load_res;
        load_res.kind = tir::OpKind::Load;
        load_res.location = binary.location();
        load_res.lhs = slot;
        load_res.result = final_val;
        function.add_operation(std::move(load_res));
        return final_val;
      }

      const tir::Value lhs = lower_expression(binary.lhs(), function);
      const tir::Value rhs = lower_expression(binary.rhs(), function);
      tir::BinaryOp op_kind = map_binary_op(binary.op());

      Type res_type = binary.type().is_valid() ? binary.type() : lhs.type;
      tir::Value result = function.allocate_value(res_type);
      tir::Op op;
      op.kind = tir::OpKind::Binary;
      op.location = binary.location();
      op.result = result;
      op.lhs = lhs;
      op.rhs = rhs;
      op.binary_op = op_kind;
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::Call: {
      const auto& call = static_cast<const CallExpr&>(expr);
      if (call.callee() == "print") {
        if (call.arguments().size() != 1) return tir::Value{};
        const Expr& argument = *call.arguments().front();
        if (argument.kind() == Expr::Kind::StringLiteral) {
          tir::Op op;
          op.kind = tir::OpKind::PrintString;
          op.location = call.location();
          op.string_value = static_cast<const StringLiteralExpr&>(argument).value();
          function.add_operation(std::move(op));
          return tir::Value{};
        }

        const tir::Value arg_val = lower_expression(argument, function);
        tir::Op op;
        op.kind = tir::OpKind::Print;
        op.location = call.location();
        op.operand = arg_val;
        function.add_operation(std::move(op));
        return tir::Value{};
      }

      // `memory.array_append(values, element)` grows the array held in the
      // binding's slot. The first argument names the binding, so it is not
      // lowered as a value: the operation writes the grown header into that
      // slot, which is the storage the destructor reads at scope exit.
      if (call.callee() == "memory.array_append" &&
          call.arguments().size() == 2 &&
          call.arguments()[0]->kind() == Expr::Kind::Name) {
        const tir::Value slot = lookup_slot(
            static_cast<const NameExpr&>(*call.arguments()[0]).name());
        if (!slot.is_valid() || !slot.type.is_dynamic_array()) {
          diagnostics_.error(kUnresolvedSymbol, call.location(),
                             "internal: append target has no dynamic array slot");
          return tir::Value{};
        }
        const tir::Value element =
            lower_expression(*call.arguments()[1], function);
        // An owned element belongs to the array now, so this scope's automatic
        // cleanup for the source stands down: the array's destructor is what
        // releases the element, element by element.
        if (call.arguments()[1]->transfers_ownership() &&
            call.arguments()[1]->kind() == Expr::Kind::Name)
          suppress_automatic_cleanup(
              lookup_slot(static_cast<const NameExpr&>(*call.arguments()[1])
                              .name()));
        tir::Value result = function.allocate_value(
            call.type().is_valid() ? call.type() : Type::int_type());
        tir::Op append;
        append.kind = tir::OpKind::ArrayAppend;
        append.location = call.location();
        append.lhs = slot;
        append.operand = element;
        append.result = result;
        function.add_operation(std::move(append));
        return result;
      }

      std::vector<tir::Value> arg_vals;
      const bool release_call = is_release_callee(
          call.has_resolved_symbol() ? call.resolved_symbol() : call.callee());
      const bool previous_release_argument = lowering_release_argument_;
      lowering_release_argument_ = release_call;
      for (const auto& argument : call.arguments()) {
        TransferContextGuard guard(transfer_context_, tir::TransferKind::Argument);
        const tir::Value value = lower_expression(*argument, function);
        // A transferred owner belongs to the callee now: its cleanup runs there,
        // so this scope's automatic cleanup for that slot must stand down or the
        // buffer is released twice.
        if (argument->transfers_ownership() &&
            argument->kind() == Expr::Kind::Name) {
          const tir::Value slot = lookup_slot(
              static_cast<const NameExpr&>(*argument).name());
          if (slot.is_valid()) {
            for (auto& scope : deferred_expressions_)
              for (auto& pending : scope)
                if (pending.automatic && pending.active &&
                    pending.owner_slot.is_valid() &&
                    pending.owner_slot.id == slot.id)
                  pending.active = false;
            if (current_function_ != nullptr)
              if (tir::DestructionObligation* obligation =
                      current_function_->pending_obligation_for_owner(slot))
                obligation->discharged = true;
          }
        }
        arg_vals.push_back(value);
      }
      lowering_release_argument_ = previous_release_argument;

      // An `out` aggregate crosses the boundary as the address of the caller's
      // binding: C fills the header in place, so there is no value to pass. The
      // buffer it installs belongs to the foreign allocator, so this binding's
      // release is retargeted to the FFI deallocator - both the pending cleanup
      // and the obligation that verification compares it against.
      if (call.is_external_call()) {
        for (size_t index = 0; index < arg_vals.size(); ++index) {
          if (!call.is_external_out_parameter(index)) continue;
          const Expr& argument = *call.arguments()[index];
          if (argument.kind() != Expr::Kind::Name) continue;
          const tir::Value slot =
              lookup_slot(static_cast<const NameExpr&>(argument).name());
          if (!slot.is_valid()) continue;
          arg_vals[index] = slot;
          for (auto& scope : deferred_expressions_)
            for (auto& pending : scope)
              if (pending.automatic && pending.active &&
                  pending.owner_slot.is_valid() &&
                  pending.owner_slot.id == slot.id)
                pending.callee = "nrq_ffi_free";
          if (current_function_ != nullptr)
            if (tir::DestructionObligation* obligation =
                    current_function_->pending_obligation_for_owner(slot))
              obligation->release = "nrq_ffi_free";
        }
      }

      // `length(value)` is the call spelling of the `.length` property.
      if (call.callee() == "length" && arg_vals.size() == 1) {
        tir::Value result = function.allocate_value(
            call.type().is_valid() ? call.type() : Type::int_type());
        tir::Op length;
        length.kind = tir::OpKind::Length;
        length.location = call.location();
        length.operand = arg_vals.front();
        length.result = result;
        function.add_operation(std::move(length));
        return result;
      }

      tir::Value result;
      if (call.type().is_valid() && !call.type().is_void()) {
        result = function.allocate_value(call.type(), call.ownership(),
                                        call.arena_id(), call.arena_kind());
      }

      tir::Op op;
      op.kind = tir::OpKind::Call;
      op.location = call.location();
      // Prefer the symbol semantic analysis resolved; builtins keep their
      // spelling because they lower to runtime entry points directly.
      op.callee = call.has_resolved_symbol() ? call.resolved_symbol() : call.callee();
      op.arguments = std::move(arg_vals);
      op.result = result;
      if (call.is_external_call()) {
        op.external_call = true;
        op.external_parameter_types = call.external_parameter_types();
      }
      function.add_operation(std::move(op));
      return result;
    }
    case Expr::Kind::ArrayLiteral: {
      const auto& arr = static_cast<const ArrayLiteralExpr&>(expr);
      const Type arr_type = expr.type().is_valid() ? expr.type()
          : (arr.elements().empty() ? Type::error()
             : Type::array_type(arr.elements()[0]->type(), arr.elements().size()));

      // Alloca for the entire array on the stack.
      tir::Value arr_slot = function.allocate_value(arr_type);
      {
        tir::Op alloca;
        alloca.kind = tir::OpKind::ArrayAlloca;
        alloca.location = arr.location();
        alloca.result = arr_slot;
        alloca.slot_name = "$array_lit";
        alloca.integer_value = static_cast<long long>(arr.elements().size());
        function.add_operation(std::move(alloca));
      }

      // For each element, compute a GEP and store.
      for (std::size_t i = 0; i < arr.elements().size(); ++i) {
        const tir::Value elem_val = lower_expression(*arr.elements()[i], function);

        // Index constant for GEP.
        tir::Value idx_val = function.allocate_value(Type::int_type());
        {
          tir::Op idx_op;
          idx_op.kind = tir::OpKind::ConstantInt;
          idx_op.location = arr.location();
          idx_op.result = idx_val;
          idx_op.integer_value = static_cast<long long>(i);
          function.add_operation(std::move(idx_op));
        }

        // GEP to element pointer.
        const Type elem_type = arr_type.is_array() ? arr_type.element_type() : Type::error();
        tir::Value elem_ptr = function.allocate_value(elem_type);
        {
          tir::Op gep;
          gep.kind = tir::OpKind::GetElementPtr;
          gep.location = arr.location();
          gep.lhs = arr_slot;
          gep.operand = idx_val;
          gep.field_index = static_cast<int>(i);
          gep.result = elem_ptr;
          function.add_operation(std::move(gep));
        }

        // Store element.
        {
          tir::Op store;
          store.kind = tir::OpKind::Store;
          store.location = arr.location();
          store.lhs = elem_ptr;
          store.operand = elem_val;
          store.result = elem_ptr;
          function.add_operation(std::move(store));
        }
      }

      // Load the whole array value from the slot.
      tir::Value loaded = function.allocate_value(arr_type);
      {
        tir::Op load;
        load.kind = tir::OpKind::Load;
        load.location = arr.location();
        load.lhs = arr_slot;
        load.result = loaded;
        function.add_operation(std::move(load));
      }
      return loaded;
    }
    case Expr::Kind::TupleLiteral: {
      const auto& tup = static_cast<const TupleLiteralExpr&>(expr);
      const Type tup_type = expr.type().is_valid() ? expr.type() : Type::error();

      // Lower each element and pack into AggregatePack.
      std::vector<tir::Value> elem_vals;
      elem_vals.reserve(tup.elements().size());
      for (const ExprPtr& elem : tup.elements()) {
        elem_vals.push_back(lower_expression(*elem, function));
      }

      tir::Value result = function.allocate_value(tup_type);
      tir::Op pack;
      pack.kind = tir::OpKind::AggregatePack;
      pack.location = tup.location();
      pack.aggregate_elements = std::move(elem_vals);
      pack.result = result;
      function.add_operation(std::move(pack));
      return result;
    }
    case Expr::Kind::Index: {
      const auto& idx_expr = static_cast<const IndexExpr&>(expr);
      const Expr& base_expr = idx_expr.base();
      const Type base_type = base_expr.type();
      const Type result_type = expr.type().is_valid() ? expr.type() : Type::error();

      // A dynamic array owns its storage, so its elements are fetched through
      // the runtime instead of a stack GEP.
      if (base_type.is_dynamic_array() || base_type.is_owned_array() ||
          base_type.is_array_view()) {
        const tir::Value array = lower_expression(base_expr, function);
        const tir::Value index_val = lower_expression(idx_expr.index(), function);

        tir::Value element = function.allocate_value(result_type);
        tir::Op index;
        index.kind = tir::OpKind::DynamicIndex;
        index.location = idx_expr.location();
        index.lhs = array;
        index.operand = index_val;
        index.result = element;
        function.add_operation(std::move(index));
        return element;
      }

      // A fixed array lives in storage, so it is indexed through its address.
      // GEP on the loaded array value itself would be invalid IR.
      tir::Value base_val;
      if (base_type.is_array()) {
        base_val = resolve_lvalue_ptr(base_expr, function);
        if (!base_val.is_valid()) {
          // Not an lvalue (for example an array literal): give it a slot first.
          const tir::Value array_value = lower_expression(base_expr, function);
          base_val = function.allocate_value(array_value.type);

          tir::Op alloca;
          alloca.kind = tir::OpKind::Alloca;
          alloca.location = base_expr.location();
          alloca.result = base_val;
          alloca.slot_name = "$index_base";
          function.add_operation(std::move(alloca));

          tir::Op store;
          store.kind = tir::OpKind::Store;
          store.location = base_expr.location();
          store.lhs = base_val;
          store.operand = array_value;
          store.result = base_val;
          function.add_operation(std::move(store));
        }
      } else {
        base_val = lower_expression(base_expr, function);
      }

      const tir::Value index_val = lower_expression(idx_expr.index(), function);

      // GEP to the element pointer.
      tir::Value elem_ptr = function.allocate_value(
          result_type, ValueOwnership::Trivial, base_val.arena_id,
          base_val.arena_kind);
      {
        tir::Op gep;
        gep.kind = tir::OpKind::GetElementPtr;
        gep.location = idx_expr.location();
        gep.lhs = base_val;
        gep.operand = index_val;
        gep.field_index = -1;  // dynamic index
        gep.result = elem_ptr;
        function.add_operation(std::move(gep));
      }

      // Load value from pointer.
      tir::Value loaded = function.allocate_value(result_type);
      {
        tir::Op load;
        load.kind = tir::OpKind::Load;
        load.location = idx_expr.location();
        load.lhs = elem_ptr;
        load.result = loaded;
        function.add_operation(std::move(load));
      }
      return loaded;
    }
    case Expr::Kind::MemberAccess: {
      const auto& mem = static_cast<const MemberAccessExpr&>(expr);
      // `ps[i].field` addresses one column element: the column base is scaled by the
      // element count, which only the lowering can read, so the pair is one op.
      if (mem.base().kind() == Expr::Kind::Index) {
        const auto& row = static_cast<const IndexExpr&>(mem.base());
        if (row.base().type().is_soa()) {
          const Type column_type = expr.type().is_valid() ? expr.type() : Type::error();
          int column = -1;
          const auto& fields = row.base().type().soa_element().struct_fields();
          for (std::size_t field = 0; field < fields.size(); ++field)
            if (fields[field].first == mem.member()) { column = static_cast<int>(field); break; }
          if (column < 0) return tir::Value{};
          tir::Value container = lower_expression(row.base(), function);
          tir::Value row_index = lower_expression(row.index(), function);
          if (!container.is_valid() || !row_index.is_valid()) return tir::Value{};
          tir::Value column_ptr = function.allocate_value(column_type);
          tir::Op address;
          address.kind = tir::OpKind::SoaColumnPtr;
          address.location = mem.location();
          address.lhs = container;
          address.operand = row_index;
          address.field_index = column;
          address.string_value = mem.member();
          address.result = column_ptr;
          function.add_operation(std::move(address));
          tir::Value loaded = function.allocate_value(column_type);
          tir::Op load;
          load.kind = tir::OpKind::Load;
          load.location = mem.location();
          load.lhs = column_ptr;
          load.result = loaded;
          function.add_operation(std::move(load));
          return loaded;
        }
      }
      const tir::Value base_val = lower_expression(mem.base(), function);
      const Type result_type = expr.type().is_valid() ? expr.type() : Type::error();
      const int field_idx = mem.is_tuple_access() ? mem.tuple_index() : -1;

      // A dynamic array's length and capacity are its second and third words, so
      // the property read is a field extraction of the value the program
      // already holds, never a runtime query.
      if (base_val.type.is_dynamic_array() &&
          (mem.member() == "length" || mem.member() == "capacity")) {
        tir::Value result = function.allocate_value(result_type);
        tir::Op extract;
        extract.kind = tir::OpKind::AggregateExtract;
        extract.location = mem.location();
        extract.operand = base_val;
        extract.field_index = mem.member() == "length" ? 1 : 2;
        extract.string_value = mem.member();
        extract.result = result;
        function.add_operation(std::move(extract));
        return result;
      }

      // `value.length` on a string or an owned array is a length query, not a
      // field of an aggregate.
      if (mem.member() == "length" &&
          (base_val.type.is_string() || base_val.type.is_owned_array() ||
           base_val.type.is_array_view())) {
        tir::Value result = function.allocate_value(result_type);
        tir::Op length;
        length.kind = tir::OpKind::Length;
        length.location = mem.location();
        length.operand = base_val;
        length.result = result;
        function.add_operation(std::move(length));
        return result;
      }

      // For struct member: we need to look up the field index.
      int computed_field_idx = field_idx;
      if (!mem.is_tuple_access() && base_val.type.is_struct()) {
        const auto& fields = base_val.type.struct_fields();
        for (std::size_t index = 0; index < fields.size(); ++index) {
          if (fields[index].first == mem.member()) {
            computed_field_idx = static_cast<int>(index);
            break;
          }
        }
        // We can't look up the struct catalog from Builder, so we embed -1
        // and let MLIR lowering resolve by field name. For now use field_index
        // from the string_value (we'll use slot_name to carry the field name).
      }

      // A transferred field is itself an owner after extraction. Recording the
      // ownership on the extracted value is what makes the following move
      // verifiable; a plain read keeps the trivial category.
      tir::Value result = function.allocate_value(
          result_type, expr.transfers_ownership() ? expr.ownership()
                                                  : ValueOwnership::Trivial);
      tir::Op extract;
      extract.kind = tir::OpKind::AggregateExtract;
      extract.location = mem.location();
      extract.operand = base_val;
      extract.field_index = computed_field_idx;
      extract.string_value = mem.member();  // carry field name for struct lowering
      extract.result = result;
      function.add_operation(std::move(extract));
      if (expr.transfers_ownership()) {
        const tir::Value field_ptr = resolve_lvalue_ptr(mem, function);
        if (field_ptr.is_valid()) {
          tir::Value empty = function.allocate_value(result_type);
          tir::Op clear_value;
          clear_value.kind = tir::OpKind::ConstantNull;
          clear_value.location = mem.location();
          clear_value.result = empty;
          function.add_operation(std::move(clear_value));
          tir::Op clear_field;
          clear_field.kind = tir::OpKind::Store;
          clear_field.location = mem.location();
          clear_field.lhs = field_ptr;
          clear_field.operand = empty;
          clear_field.result = field_ptr;
          function.add_operation(std::move(clear_field));
          tir::Value moved;
          TransferContextGuard guard(transfer_context_, tir::TransferKind::Field);
          emit_move(result, function, mem.location(), moved);
          result = moved;
        }
      }
      return result;
    }
    case Expr::Kind::StructInit: {
      const auto& si = static_cast<const StructInitExpr&>(expr);
      const Type struct_type = expr.type().is_valid() ? expr.type()
                                : Type::struct_type(si.struct_name());

      if (si.is_uninitialized()) {
        // Zero the slot so untouched fields are null and destruction is a no-op.
        tir::Value result = function.allocate_value(struct_type);
        tir::Op zero;
        zero.kind = tir::OpKind::ConstantNull;
        zero.location = si.location();
        zero.result = result;
        function.add_operation(std::move(zero));
        return result;
      }

      std::vector<tir::Value> field_vals;
      if (si.is_positional()) {
        field_vals.reserve(si.positional_inits().size());
        for (const ExprPtr& e : si.positional_inits()) {
          field_vals.push_back(lower_expression(*e, function));
        }
      } else {
        field_vals.reserve(si.field_inits().size());
        for (const auto& kv : si.field_inits()) {
          field_vals.push_back(lower_expression(*kv.second, function));
        }
      }

      tir::Value result = function.allocate_value(struct_type);
      tir::Op pack;
      pack.kind = tir::OpKind::AggregatePack;
      pack.location = si.location();
      pack.aggregate_elements = std::move(field_vals);
      pack.slot_name = si.struct_name();
      pack.result = result;
      // A field that owns storage brings its release with it: the aggregate is
      // now that storage's owner, so whoever destroys the aggregate destroys the
      // field. The release is the one the producing value needed, which is why an
      // extern result keeps the foreign deallocator.
      // The pack position is the field index the lowering reads, so a literal
      // whose type names only the struct still records every managed field in the
      // slot the lowering will address. The element's own type decides whether it
      // owns anything.
      const std::vector<std::pair<std::string, Type>> declared_fields =
          struct_type.struct_fields();
      auto collect_release = [&](std::size_t position, const Expr& element,
                                 const tir::Value& value) {
        if (!value.is_valid()) return;
        // The field's declared type decides what it owns: a move hands over a bare
        // handle whose value type no longer carries the resource domain, and the
        // declared type is what names the destructor.
        const Type field_type =
            position < declared_fields.size() ? declared_fields[position].second
                                              : value.type;
        std::string release;
        if (!managed_field_release(field_type, function, value, release)) return;
        // A named owner moves into the field: the binding it left no longer owns
        // it, so its own cleanup stands down and the field's takes over.
        if (element.kind() == Expr::Kind::Name) {
          const tir::Value source =
              lookup_slot(static_cast<const NameExpr&>(element).name());
          if (source.is_valid()) suppress_automatic_cleanup(source);
        }
        pack.release_fields.push_back(static_cast<int>(position));
        pack.release_callees.push_back(std::move(release));
      };
      if (si.is_positional()) {
        for (std::size_t i = 0;
             i < si.positional_inits().size() && i < pack.aggregate_elements.size();
             ++i)
          collect_release(i, *si.positional_inits()[i],
                          pack.aggregate_elements[i]);
      } else {
        const std::vector<std::pair<std::string, ExprPtr>>& inits =
            si.field_inits();
        for (std::size_t i = 0; i < inits.size() && i < pack.aggregate_elements.size();
             ++i)
          collect_release(i, *inits[i].second, pack.aggregate_elements[i]);
      }
      function.add_operation(std::move(pack));
      return result;
    }
  }
  return tir::Value{};
}

tir::Value TirBuilder::lookup_slot(const std::string& name) const {
  for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
    const auto found = scope->find(name);
    if (found != scope->end()) return found->second;
  }
  return tir::Value{};
}

tir::Value TirBuilder::resolve_lvalue_ptr(const Expr& target, tir::Function& function) {
  if (target.kind() == Expr::Kind::Name) {
    const auto& name_expr = static_cast<const NameExpr&>(target);
    return lookup_slot(name_expr.name());
  }
  if (target.kind() == Expr::Kind::Index) {
    const auto& idx_expr = static_cast<const IndexExpr&>(target);
    tir::Value base_ptr;
    tir::Value owned_array_value;
    if (idx_expr.base().type().is_owned_array() ||
        idx_expr.base().type().is_array_view()) {
      owned_array_value = lower_expression(idx_expr.base(), function);
      base_ptr = function.allocate_value(
          Type::pointer_type(idx_expr.base().type().element_type()));
      tir::Op data;
      data.kind = tir::OpKind::AggregateExtract;
      data.location = idx_expr.base().location();
      data.operand = owned_array_value;
      data.field_index = 0;
      data.result = base_ptr;
      function.add_operation(std::move(data));
    } else {
      base_ptr = (idx_expr.base().type().is_pointer() ||
                  idx_expr.base().type().is_managed_pointer())
          ? lower_expression(idx_expr.base(), function)
          : resolve_lvalue_ptr(idx_expr.base(), function);
    }
    if (!base_ptr.is_valid()) return tir::Value{};
    tir::Value idx_val = lower_expression(idx_expr.index(), function);
    if (idx_expr.base().type().is_owned_array() ||
        idx_expr.base().type().is_array_view()) {
      tir::Value length = function.allocate_value(Type::u64_type());
      tir::Op length_extract;
      length_extract.kind = tir::OpKind::AggregateExtract;
      length_extract.location = idx_expr.base().location();
      length_extract.operand = owned_array_value;
      length_extract.field_index = 1;
      length_extract.result = length;
      function.add_operation(std::move(length_extract));
      tir::Op check;
      check.kind = tir::OpKind::Call;
      check.location = idx_expr.location();
      check.callee = "memory.array_bounds_check";
      check.arguments = {idx_val, length};
      function.add_operation(std::move(check));
    }
    Type elem_type = idx_expr.type().is_valid() ? idx_expr.type() : base_ptr.type.element_type();
    tir::Value elem_ptr = function.allocate_value(
        elem_type, ValueOwnership::Trivial, base_ptr.arena_id,
        base_ptr.arena_kind);
    tir::Op gep;
    gep.kind = tir::OpKind::GetElementPtr;
    gep.location = idx_expr.location();
    gep.lhs = base_ptr;
    gep.operand = idx_val;
    gep.result = elem_ptr;
    function.add_operation(std::move(gep));
    return elem_ptr;
  }
  if (target.kind() == Expr::Kind::Unary) {
    // `*p = v` names the object at p, so the lvalue is p's element pointer.
    const auto& unary = static_cast<const UnaryExpr&>(target);
    if (unary.op() == UnaryOp::Dereference) {
      const tir::Value pointer = lower_expression(unary.operand(), function);
      if (!pointer.is_valid()) return tir::Value{};
      const Type elem_type =
          unary.type().is_valid() ? unary.type() : pointer.type.element_type();
      tir::Value zero = function.allocate_value(Type::u64_type());
      tir::Op zero_op;
      zero_op.kind = tir::OpKind::ConstantInt;
      zero_op.location = unary.location();
      zero_op.integer_value = 0;
      zero_op.result = zero;
      function.add_operation(std::move(zero_op));
      tir::Value elem_ptr = function.allocate_value(
          elem_type, ValueOwnership::Trivial, pointer.arena_id,
          pointer.arena_kind);
      tir::Op gep;
      gep.kind = tir::OpKind::GetElementPtr;
      gep.location = unary.location();
      gep.lhs = pointer;
      gep.operand = zero;
      gep.field_index = -1;
      gep.result = elem_ptr;
      function.add_operation(std::move(gep));
      return elem_ptr;
    }
    return tir::Value{};
  }
  if (target.kind() == Expr::Kind::MemberAccess) {
    const auto& mem_expr = static_cast<const MemberAccessExpr&>(target);
    // A writable column element: the same address the read path computes, returned as
    // an lvalue instead of being loaded.
    if (mem_expr.base().kind() == Expr::Kind::Index) {
      const auto& row = static_cast<const IndexExpr&>(mem_expr.base());
      if (row.base().type().is_soa()) {
        const Type column_type = mem_expr.type().is_valid() ? mem_expr.type() : Type::error();
        int column = -1;
        const auto& fields = row.base().type().soa_element().struct_fields();
        for (std::size_t field = 0; field < fields.size(); ++field)
          if (fields[field].first == mem_expr.member()) { column = static_cast<int>(field); break; }
        if (column < 0) return tir::Value{};
        tir::Value container = lower_expression(row.base(), function);
        tir::Value row_index = lower_expression(row.index(), function);
        if (!container.is_valid() || !row_index.is_valid()) return tir::Value{};
        tir::Value column_ptr = function.allocate_value(column_type);
        tir::Op address;
        address.kind = tir::OpKind::SoaColumnPtr;
        address.location = mem_expr.location();
        address.lhs = container;
        address.operand = row_index;
        address.field_index = column;
        address.string_value = mem_expr.member();
        address.result = column_ptr;
        function.add_operation(std::move(address));
        return column_ptr;
      }
    }
    tir::Value base_ptr = resolve_lvalue_ptr(mem_expr.base(), function);
    if (!base_ptr.is_valid()) return tir::Value{};
    Type field_type = mem_expr.type().is_valid() ? mem_expr.type() : Type::error();
    tir::Value field_ptr = function.allocate_value(
        field_type, ValueOwnership::Trivial, base_ptr.arena_id,
        base_ptr.arena_kind);
    tir::Op gep;
    gep.kind = tir::OpKind::GetElementPtr;
    gep.location = mem_expr.location();
    gep.lhs = base_ptr;
    gep.field_index = mem_expr.is_tuple_access() ? mem_expr.tuple_index() : -1;
    if (!mem_expr.is_tuple_access() && base_ptr.type.is_struct()) {
      const auto& fields = base_ptr.type.struct_fields();
      for (std::size_t index = 0; index < fields.size(); ++index) {
        if (fields[index].first == mem_expr.member()) {
          gep.field_index = static_cast<int>(index);
          break;
        }
      }
    }
    gep.string_value = mem_expr.member();
    gep.result = field_ptr;
    function.add_operation(std::move(gep));
    return field_ptr;
  }
  return tir::Value{};
}

}  // namespace nirq

