// TirToMlir.cpp — translates a TIR module into MLIR.

#include "nirqoba/Lowering/TirToMlir.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Location.h"

#include "nirqoba/Diagnostics/Diagnostic.h"
#include "nirqoba/Lowering/NrqDialect.h"
#include "nirqoba/Lowering/NrqOps.h"

namespace nirq {
using namespace std;
namespace {

static mlir::Type tir_type_to_mlir(const Type& type, mlir::MLIRContext* ctx,
                                   mlir::Builder& builder) {
  if (type.is_bool()) return builder.getI1Type();
  if (type.is_char()) return builder.getI32Type();
  if (type.is_f32()) return builder.getF32Type();
  if (type.is_f64()) return builder.getF64Type();
  if (type.is_integer()) {
    switch (type.bit_width()) {
      case 8: return builder.getI8Type();
      case 16: return builder.getI16Type();
      case 32: return builder.getI32Type();
      case 64: return builder.getI64Type();
      default: return builder.getI64Type();
    }
  }
  if (type.is_string()) return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_pointer() || type.is_managed_pointer())
    return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_owned_array()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        ctx, {mlir::LLVM::LLVMPointerType::get(ctx), builder.getI64Type()});
  }
  if (type.is_array_view()) {
    return mlir::LLVM::LLVMStructType::getLiteral(
        ctx, {mlir::LLVM::LLVMPointerType::get(ctx), builder.getI64Type()});
  }
  // A dynamic array is a pointer to a runtime-owned descriptor; the runtime
  // owns the element storage, so the compiler only ever handles the handle.
  if (type.is_dynamic_array()) return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_void()) return {};
  if (type.is_array()) {
    mlir::Type elem = tir_type_to_mlir(type.element_type(), ctx, builder);
    if (!elem) return {};
    return mlir::LLVM::LLVMArrayType::get(elem, static_cast<unsigned>(type.array_length()));
  }
  if (type.is_tuple()) {
    std::vector<mlir::Type> fields;
    fields.reserve(type.tuple_elements().size());
    for (const Type& t : type.tuple_elements()) {
      mlir::Type ft = tir_type_to_mlir(t, ctx, builder);
      if (!ft) return {};
      fields.push_back(ft);
    }
    return mlir::LLVM::LLVMStructType::getLiteral(ctx, fields);
  }
  if (type.is_struct()) {
    if (!type.struct_fields().empty()) {
      std::vector<mlir::Type> fields;
      fields.reserve(type.struct_fields().size());
      for (const auto& field : type.struct_fields()) {
        mlir::Type field_type = tir_type_to_mlir(field.second, ctx, builder);
        if (!field_type) return {};
        fields.push_back(field_type);
      }
      return mlir::LLVM::LLVMStructType::getLiteral(ctx, fields);
    }
    // Opaque struct type by name — fields will be resolved during a full lowering pass.
    // We map it to an identified struct with the struct name.
    auto stype = mlir::LLVM::LLVMStructType::getIdentified(ctx, type.struct_name());
    return stype;
  }
  return {};
}

static mlir::Type llvm_ptr_type(mlir::MLIRContext* ctx) {
  return mlir::LLVM::LLVMPointerType::get(ctx);
}

static uint64_t type_alignment(const Type& type) {
  if (type.is_pointer() || type.is_managed_pointer() || type.is_string() ||
      type.is_dynamic_array() || type.is_owned_array() || type.is_array_view()) return 8;
  if (type.is_array()) return type_alignment(type.element_type());
  if (type.is_struct() || type.is_tuple()) {
    uint64_t alignment = 1;
    if (type.is_struct()) {
      for (const auto& field : type.struct_fields())
        alignment = std::max(alignment, type_alignment(field.second));
    } else {
      for (const Type& field : type.tuple_elements())
        alignment = std::max(alignment, type_alignment(field));
    }
    return alignment;
  }
  if (type.is_float()) return type.is_f32() ? 4 : 8;
  if (type.is_integer() || type.is_char())
    return std::max<uint64_t>(1, std::min<uint64_t>(8, type.bit_width() / 8));
  return 1;
}

static uint64_t type_size(const Type& type) {
  if (type.is_pointer() || type.is_managed_pointer() || type.is_string() ||
      type.is_dynamic_array()) return 8;
  if (type.is_owned_array() || type.is_array_view()) return 16;
  if (type.is_array()) return type.array_length() * type_size(type.element_type());
  if (type.is_struct() || type.is_tuple()) {
    uint64_t size = 0;
    uint64_t aggregate_alignment = 1;
    auto append_field = [&](const Type& field_type) {
      const uint64_t alignment = type_alignment(field_type);
      aggregate_alignment = std::max(aggregate_alignment, alignment);
      size = (size + alignment - 1) & ~(alignment - 1);
      size += type_size(field_type);
    };
    if (type.is_struct()) {
      for (const auto& field : type.struct_fields()) append_field(field.second);
    } else {
      for (const Type& field : type.tuple_elements()) append_field(field);
    }
    return (size + aggregate_alignment - 1) & ~(aggregate_alignment - 1);
  }
  if (type.is_float()) return type.is_f32() ? 4 : 8;
  if (type.is_bool()) return 1;
  if (type.is_integer() || type.is_char())
    return std::max<uint64_t>(1, type.bit_width() / 8);
  return 0;
}

static void collect_managed_reference_offsets(const Type& type, uint64_t base,
                                              std::vector<uint64_t>& offsets) {
  if (type.is_managed_pointer()) {
    offsets.push_back(base);
    return;
  }
  if (type.is_array()) {
    const uint64_t stride = type_size(type.element_type());
    for (std::size_t index = 0; index < type.array_length(); ++index)
      collect_managed_reference_offsets(type.element_type(),
                                        base + index * stride, offsets);
    return;
  }
  if (type.is_struct() || type.is_tuple()) {
    uint64_t field_offset = 0;
    auto append_field = [&](const Type& field_type) {
      const uint64_t alignment = type_alignment(field_type);
      field_offset = (field_offset + alignment - 1) & ~(alignment - 1);
      collect_managed_reference_offsets(field_type, base + field_offset, offsets);
      field_offset += type_size(field_type);
    };
    if (type.is_struct()) {
      for (const auto& field : type.struct_fields()) append_field(field.second);
    } else {
      for (const Type& field : type.tuple_elements()) append_field(field);
    }
  }
}

static bool collect_has_managed_reference(const Type& type) {
  std::vector<uint64_t> offsets;
  collect_managed_reference_offsets(type, 0, offsets);
  return !offsets.empty();
}

static mlir::Location to_mlir_loc(const SourceLocation& loc, mlir::MLIRContext* ctx) {
  if (!loc.is_valid()) return mlir::UnknownLoc::get(ctx);
  return mlir::FileLineColLoc::get(ctx,
    llvm::StringRef(loc.file),
    static_cast<unsigned>(loc.line),
    static_cast<unsigned>(loc.column));
}

static llvm::StringRef ownership_spelling(ValueOwnership ownership) {
  switch (ownership) {
    case ValueOwnership::Owned: return "owned";
    case ValueOwnership::Borrowed: return "borrowed";
    case ValueOwnership::MutableBorrowed: return "mutable_borrowed";
    case ValueOwnership::Shared: return "shared";
    case ValueOwnership::GcManaged: return "gc_managed";
    case ValueOwnership::Raw: return "raw";
    case ValueOwnership::Trivial: return "trivial";
  }
  return "trivial";
}

static mlir::LLVM::LLVMFuncOp ensure_runtime_fn(
    mlir::ModuleOp module, mlir::OpBuilder& builder,
    llvm::StringRef symbol, mlir::Type retTy, llvm::ArrayRef<mlir::Type> argTys) {
  if (auto fn = module.lookupSymbol<mlir::LLVM::LLVMFuncOp>(symbol)) {
    return fn;
  }
  mlir::OpBuilder::InsertionGuard guard(builder);
  builder.setInsertionPointToStart(module.getBody());
  auto fnTy = mlir::LLVM::LLVMFunctionType::get(retTy, argTys);
  return builder.create<mlir::LLVM::LLVMFuncOp>(
      module.getLoc(), symbol, fnTy, mlir::LLVM::Linkage::External);
}

class FunctionTranslator {
 public:
  FunctionTranslator(mlir::ModuleOp module,
                     const tir::Function& tir_fn,
                     mlir::func::FuncOp func_op,
                     mlir::OpBuilder& builder,
                     mlir::MLIRContext* ctx,
                     DiagnosticEngine& diag)
      : module_(module), tir_fn_(tir_fn), func_op_(func_op),
        builder_(builder), ctx_(ctx), diag_(diag) {
    entry_block_ = func_op.addEntryBlock();
    builder_.setInsertionPointToStart(entry_block_);
    values_.resize(tir_fn.value_count());

    for (std::size_t i = 0; i < tir_fn.parameters().size(); ++i) {
      set(tir_fn.parameters()[i].value, entry_block_->getArgument(i));
    }

    auto begin_frame = ensure_runtime_fn(module_, builder_, "nrq_gc_root_frame_begin",
                                         builder_.getI64Type(), {});
    gc_root_frame_marker_ = builder_.create<mlir::LLVM::CallOp>(
        mlir::UnknownLoc::get(ctx_), begin_frame, mlir::ValueRange{}).getResult();
    for (std::size_t i = 0; i < tir_fn.parameters().size(); ++i) {
      std::vector<uint64_t> offsets;
      collect_managed_reference_offsets(tir_fn.parameters()[i].type, 0, offsets);
      if (offsets.empty()) continue;
      mlir::Type value_type = tir_type_to_mlir(tir_fn.parameters()[i].type, ctx_, builder_);
      if (!value_type) continue;
      mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
          mlir::UnknownLoc::get(ctx_), builder_.getI64Type(),
          builder_.getI64IntegerAttr(1)).getResult();
      mlir::Value slot = builder_.create<mlir::LLVM::AllocaOp>(
          mlir::UnknownLoc::get(ctx_), llvm_ptr_type(ctx_), value_type, one).getResult();
      builder_.create<mlir::LLVM::StoreOp>(mlir::UnknownLoc::get(ctx_),
                                           entry_block_->getArgument(i), slot);
      register_gc_root(slot, tir_fn.parameters()[i].type,
                       mlir::UnknownLoc::get(ctx_));
    }
  }

  bool translate() {
    for (const tir::Op& op : tir_fn_.operations()) {
      if (!translate_op(op)) return false;
    }
    flush_gc_root_registrations();
    return true;
  }

 private:
  mlir::Value get(tir::Value v) const {
    if (!v.is_valid()) return {};
    return values_[static_cast<std::size_t>(v.id)];
  }

  void set(tir::Value v, mlir::Value mlir_val) {
    if (!v.is_valid()) return;
    if (v.ownership != ValueOwnership::Trivial && mlir_val.getDefiningOp())
      mlir_val.getDefiningOp()->setAttr(
          "nirqoba.ownership",
          builder_.getStringAttr(ownership_spelling(v.ownership)));
    values_[static_cast<std::size_t>(v.id)] = mlir_val;
  }

  // Every stack slot is allocated in the function's entry block, never at the
  // point of the `tir.alloca`. A loop body re-executes its instructions, and an
  // alloca executed per iteration grows the stack without bound until the
  // process dies; hoisting also lets the backend fold the frame into one
  // prologue adjustment.
  mlir::Value alloca_at_entry(mlir::Type element_type, mlir::Location loc) {
    if (entry_block_ == nullptr) return {};
    mlir::OpBuilder::InsertionGuard guard(builder_);
    builder_.setInsertionPointToStart(entry_block_);
    mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
    return builder_.create<mlir::LLVM::AllocaOp>(
        loc, llvm_ptr_type(ctx_), element_type, one).getResult();
  }

  // Root registration is likewise a per-activation action: the same slot must
  // be registered once, not once per loop iteration. Registrations are queued
  // and emitted into the entry block after the body is translated.
  void register_gc_root(mlir::Value slot, const Type& value_type,
                        mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(value_type, 0, offsets);
    if (offsets.empty()) return;
    pending_gc_roots_.push_back(PendingGcRoot{slot, value_type, loc});
  }

  void flush_gc_root_registrations() {
    if (pending_gc_roots_.empty() || entry_block_ == nullptr) return;
    mlir::OpBuilder::InsertionGuard guard(builder_);
    if (mlir::Operation* terminator = entry_block_->getTerminator()) {
      builder_.setInsertionPoint(terminator);
    } else {
      builder_.setInsertionPointToEnd(entry_block_);
    }
    for (const PendingGcRoot& root : pending_gc_roots_) {
      emit_gc_root_registration(root.slot, root.type, root.loc);
    }
    pending_gc_roots_.clear();
  }

  void emit_gc_root_registration(mlir::Value slot, const Type& value_type,
                                 mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(value_type, 0, offsets);
    if (offsets.empty()) return;
    const std::string symbol = "nrq_gc_root_register_typed";
    auto register_fn = ensure_runtime_fn(module_, builder_, symbol,
                                         mlir::LLVM::LLVMVoidType::get(ctx_),
                                         {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                          llvm_ptr_type(ctx_), builder_.getI64Type()});
    auto offsets_type = mlir::LLVM::LLVMArrayType::get(
        builder_.getI64Type(), static_cast<unsigned>(offsets.size()));
    mlir::Value offsets_slot = alloca_at_entry(offsets_type, loc);
    if (!offsets_slot) return;
    mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
    for (std::size_t index = 0; index < offsets.size(); ++index) {
      mlir::Value index_value = builder_.create<mlir::LLVM::ConstantOp>(
          loc, builder_.getI64Type(),
          builder_.getI64IntegerAttr(static_cast<int64_t>(index))).getResult();
      mlir::Value field_slot = builder_.create<mlir::LLVM::GEPOp>(
          loc, llvm_ptr_type(ctx_), offsets_type, offsets_slot,
          mlir::ValueRange{zero, index_value}).getResult();
      mlir::Value offset_value = builder_.create<mlir::LLVM::ConstantOp>(
          loc, builder_.getI64Type(),
          builder_.getI64IntegerAttr(static_cast<int64_t>(offsets[index]))).getResult();
      builder_.create<mlir::LLVM::StoreOp>(loc, offset_value, field_slot);
    }
    mlir::Value object_size = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(type_size(value_type))).getResult();
    mlir::Value reference_count = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI64Type(),
        builder_.getI64IntegerAttr(static_cast<int64_t>(offsets.size()))).getResult();
    builder_.create<mlir::LLVM::CallOp>(loc, register_fn,
        mlir::ValueRange{slot, object_size, offsets_slot, reference_count});
  }

  void emit_gc_array_root_registration(mlir::Value data, mlir::Value count,
                                       const Type& element_type,
                                       mlir::Location loc) {
    std::vector<uint64_t> offsets;
    collect_managed_reference_offsets(element_type, 0, offsets);
    if (offsets.empty()) return;
    const mlir::Type i64 = builder_.getI64Type();
    auto offsets_type = mlir::LLVM::LLVMArrayType::get(
        i64, static_cast<unsigned>(offsets.size()));
    mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder_.getI64IntegerAttr(1)).getResult();
    mlir::Value offsets_slot = builder_.create<mlir::LLVM::AllocaOp>(
        loc, llvm_ptr_type(ctx_), offsets_type, one).getResult();
    mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
        loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
    for (std::size_t index = 0; index < offsets.size(); ++index) {
      mlir::Value index_value = builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(static_cast<int64_t>(index))).getResult();
      mlir::Value field_slot = builder_.create<mlir::LLVM::GEPOp>(
          loc, llvm_ptr_type(ctx_), offsets_type, offsets_slot,
          mlir::ValueRange{zero, index_value}).getResult();
      mlir::Value offset = builder_.create<mlir::LLVM::ConstantOp>(
          loc, i64, builder_.getI64IntegerAttr(static_cast<int64_t>(offsets[index]))).getResult();
      builder_.create<mlir::LLVM::StoreOp>(loc, offset, field_slot);
    }
    auto register_fn = ensure_runtime_fn(
        module_, builder_, "nrq_gc_root_register_array",
        mlir::LLVM::LLVMVoidType::get(ctx_),
        {llvm_ptr_type(ctx_), i64, i64, llvm_ptr_type(ctx_), i64});
    mlir::Value element_size = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64, builder_.getI64IntegerAttr(type_size(element_type))).getResult();
    mlir::Value offset_count = builder_.create<mlir::LLVM::ConstantOp>(
        loc, i64,
        builder_.getI64IntegerAttr(static_cast<int64_t>(offsets.size()))).getResult();
    builder_.create<mlir::LLVM::CallOp>(
        loc, register_fn,
        mlir::ValueRange{data, count, element_size, offsets_slot, offset_count});
  }

  // Converts a runtime call's ABI result to the type the language assigned to
  // the call, e.g. an i32 status code to a 64-bit `int` or a 0/1 code to
  // `bool`. A no-op when the types already match.
  mlir::Value adapt_runtime_result(mlir::Value result, const Type& wanted_type,
                                   mlir::Location loc) {
    if (!result) return result;
    mlir::Type wanted = tir_type_to_mlir(wanted_type, ctx_, builder_);
    if (!wanted || wanted == result.getType()) return result;

    if (!mlir::isa<mlir::IntegerType>(result.getType()) ||
        !mlir::isa<mlir::IntegerType>(wanted)) {
      return result;
    }

    const unsigned result_bits = result.getType().getIntOrFloatBitWidth();
    const unsigned wanted_bits = wanted.getIntOrFloatBitWidth();

    // A 1-bit language type is `bool`; the runtime reports it as 0/1.
    if (wanted_bits == 1 && result_bits > 1) {
      mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
          loc, result.getType(), builder_.getIntegerAttr(result.getType(), 0)).getResult();
      return builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, result, zero)
          .getResult();
    }
    if (wanted_bits == result_bits) return result;
    if (wanted_bits < result_bits) {
      return builder_.create<mlir::LLVM::TruncOp>(loc, wanted, result).getResult();
    }
    return wanted_type.is_unsigned_integer()
               ? builder_.create<mlir::LLVM::ZExtOp>(loc, wanted, result).getResult()
               : builder_.create<mlir::LLVM::SExtOp>(loc, wanted, result).getResult();
  }

  // Emits (once per distinct literal) a private global holding `text` plus a
  // terminating NUL and returns a pointer to its first byte.
  mlir::Value materialize_string(const std::string& text, mlir::Location loc) {
    auto cached = string_literals_.find(text);
    std::string symbol;
    if (cached != string_literals_.end()) {
      symbol = cached->second;
    } else {
      symbol = "__nrq_strlit_" + std::to_string(string_literal_counter_++);
      const std::string stored = text + '\0';
      auto array_type = mlir::LLVM::LLVMArrayType::get(
          builder_.getI8Type(), static_cast<unsigned>(stored.size()));

      // Globals live at module scope, not at the current insertion point.
      mlir::OpBuilder::InsertionGuard guard(builder_);
      builder_.setInsertionPointToStart(module_.getBody());
      builder_.create<mlir::LLVM::GlobalOp>(loc, array_type, /*isConstant=*/true,
                                            mlir::LLVM::Linkage::Private, symbol,
                                            builder_.getStringAttr(stored));
      string_literals_.emplace(text, symbol);
    }
    return builder_.create<mlir::LLVM::AddressOfOp>(loc, llvm_ptr_type(ctx_), symbol)
        .getResult();
  }

  mlir::Block* get_or_create_block(int label_id) {
    auto it = label_blocks_.find(label_id);
    if (it != label_blocks_.end()) return it->second;
    mlir::Block* block = func_op_.addBlock();
    label_blocks_[label_id] = block;
    return block;
  }

  bool translate_op(const tir::Op& op) {
    mlir::Location loc = to_mlir_loc(op.location, ctx_);

    switch (op.kind) {
      case tir::OpKind::Label: {
        mlir::Block* target_block = get_or_create_block(op.label_id);
        mlir::Block* current = builder_.getBlock();
        if (current != nullptr && (current->empty() || !current->back().hasTrait<mlir::OpTrait::IsTerminator>())) {
          builder_.create<mlir::LLVM::BrOp>(loc, target_block);
        }
        builder_.setInsertionPointToStart(target_block);
        return true;
      }

      case tir::OpKind::Branch: {
        mlir::Block* target_block = get_or_create_block(op.label_id);
        builder_.create<mlir::LLVM::BrOp>(loc, target_block);
        return true;
      }

      case tir::OpKind::BranchCond: {
        mlir::Block* true_block = get_or_create_block(op.true_label);
        mlir::Block* false_block = get_or_create_block(op.false_label);
        mlir::Value cond = get(op.operand);
        builder_.create<mlir::LLVM::CondBrOp>(loc, cond, true_block, false_block);
        return true;
      }

      case tir::OpKind::ConstantInt: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) ty = builder_.getI64Type();
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty, builder_.getIntegerAttr(ty, op.integer_value)).getResult();
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantNull: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) {
          diag_.error("NRQ4005", op.location,
                      "internal: null constant has an unsupported type");
          return false;
        }
        set(op.result, builder_.create<mlir::LLVM::ZeroOp>(loc, ty).getResult());
        return true;
      }

      case tir::OpKind::ConstantFloat: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) ty = builder_.getF64Type();
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty, builder_.getFloatAttr(ty, op.float_value)).getResult();
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantString: {
        // A string literal becomes the address of a private, NUL-terminated
        // byte array, which is what the runtime ABI expects for `const char*`.
        set(op.result, materialize_string(op.string_value, loc));
        return true;
      }

      case tir::OpKind::Alloca: {
        mlir::Type elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!elem_type) {
          diag_.error("NRQ4005", op.location, "internal: alloca of unsupported type");
          return false;
        }
        mlir::Value slot = alloca_at_entry(elem_type, loc);
        if (!slot) {
          diag_.error("NRQ4005", op.location, "internal: no entry block for a stack slot");
          return false;
        }
        std::vector<uint64_t> root_offsets;
        collect_managed_reference_offsets(op.result.type, 0, root_offsets);
        if (!root_offsets.empty()) {
          mlir::Value zero_value = builder_.create<mlir::LLVM::ZeroOp>(
              loc, elem_type).getResult();
          builder_.create<mlir::LLVM::StoreOp>(loc, zero_value, slot);
          register_gc_root(slot, op.result.type, loc);
        }
        set(op.result, slot);
        return true;
      }

      case tir::OpKind::Store: {
        mlir::Value ptr = get(op.lhs);
        mlir::Value val = get(op.operand);
        if (!ptr || !val) return true;
        if (op.operand.type.is_managed_pointer()) {
          auto store_fn = ensure_runtime_fn(module_, builder_, "nrq_gc_root_store",
                                            mlir::LLVM::LLVMVoidType::get(ctx_),
                                            {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, store_fn,
                                              mlir::ValueRange{ptr, val});
        } else if (!collect_has_managed_reference(op.operand.type)) {
          builder_.create<mlir::LLVM::StoreOp>(loc, val, ptr);
        } else {
          mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
          mlir::Value temporary = builder_.create<mlir::LLVM::AllocaOp>(
              loc, llvm_ptr_type(ctx_), val.getType(), one).getResult();
          builder_.create<mlir::LLVM::StoreOp>(loc, val, temporary);
          auto copy_fn = ensure_runtime_fn(module_, builder_, "nrq_gc_root_copy",
                                           mlir::LLVM::LLVMVoidType::get(ctx_),
                                           {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_),
                                            builder_.getI64Type()});
          mlir::Value bytes = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_size(op.operand.type))).getResult();
          builder_.create<mlir::LLVM::CallOp>(loc, copy_fn,
                                              mlir::ValueRange{ptr, temporary, bytes});
        }
        return true;
      }

      case tir::OpKind::Load: {
        mlir::Value ptr = get(op.lhs);
        if (!ptr) return true;
        mlir::Type elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!elem_type) {
          diag_.error("NRQ4005", op.location, "internal: load of unsupported type");
          return false;
        }
        mlir::Value loaded = builder_.create<mlir::LLVM::LoadOp>(loc, elem_type, ptr).getResult();
        set(op.result, loaded);
        return true;
      }

      case tir::OpKind::Unary: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        mlir::Value result;
        const Type& ty = op.operand.type;

        switch (op.unary_op) {
          case tir::UnaryOp::Negate:
            if (ty.is_float()) {
              result = builder_.create<mlir::LLVM::FNegOp>(loc, val).getResult();
            } else {
              mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0)).getResult();
              result = builder_.create<mlir::LLVM::SubOp>(loc, zero, val).getResult();
            }
            break;
          case tir::UnaryOp::Plus:
            result = val;
            break;
          case tir::UnaryOp::LogicalNot: {
            mlir::Value true_val = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI1Type(), builder_.getBoolAttr(true)).getResult();
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, true_val).getResult();
            break;
          }
          case tir::UnaryOp::BitwiseNot: {
            mlir::Value all_ones = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), -1)).getResult();
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, all_ones).getResult();
            break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Binary: {
        mlir::Value lhs = get(op.lhs);
        mlir::Value rhs = get(op.rhs);
        if (!lhs || !rhs) return true;
        mlir::Value result;
        const Type& lhs_type = op.lhs.type;

        if (op.binary_op == tir::BinaryOp::Exponentiate) {
          if (lhs_type.is_float()) {
            auto pow_fn = ensure_runtime_fn(module_, builder_, "nrq_pow_f64",
                                            builder_.getF64Type(),
                                            {builder_.getF64Type(), builder_.getF64Type()});
            mlir::Value l_f64 = lhs_type.is_f32()
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), lhs).getResult()
                : lhs;
            mlir::Value r_f64 = op.rhs.type.is_f32()
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), rhs).getResult()
                : rhs;
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getF64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_f64, r_f64}).getResult();
            if (lhs_type.is_f32()) {
              result = builder_.create<mlir::LLVM::FPTruncOp>(loc, builder_.getF32Type(), result).getResult();
            }
          } else {
            auto pow_fn = ensure_runtime_fn(module_, builder_, "nrq_pow_i64",
                                            builder_.getI64Type(),
                                            {builder_.getI64Type(), builder_.getI64Type()});
            mlir::Value l_i64 = lhs;
            if (lhs_type.bit_width() < 64) {
              l_i64 = lhs_type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), lhs).getResult()
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), lhs).getResult();
            }
            mlir::Value r_i64 = rhs;
            if (op.rhs.type.bit_width() < 64) {
              r_i64 = op.rhs.type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), rhs).getResult()
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), rhs).getResult();
            }
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getI64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_i64, r_i64}).getResult();
            if (lhs_type.bit_width() < 64) {
              mlir::Type orig_ty = tir_type_to_mlir(lhs_type, ctx_, builder_);
              result = builder_.create<mlir::LLVM::TruncOp>(loc, orig_ty, result).getResult();
            }
          }
          set(op.result, result);
          return true;
        }

        if (lhs_type.is_float()) {
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = builder_.create<mlir::LLVM::FAddOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Subtract: result = builder_.create<mlir::LLVM::FSubOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Multiply: result = builder_.create<mlir::LLVM::FMulOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Divide: result = builder_.create<mlir::LLVM::FDivOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Remainder: result = builder_.create<mlir::LLVM::FRemOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oeq, lhs, rhs).getResult(); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::one, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::olt, lhs, rhs).getResult(); break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ole, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ogt, lhs, rhs).getResult(); break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oge, lhs, rhs).getResult(); break;
            default: break;
          }
        } else {
          const bool is_signed = lhs_type.is_signed_integer();
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = builder_.create<mlir::LLVM::AddOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Subtract: result = builder_.create<mlir::LLVM::SubOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Multiply: result = builder_.create<mlir::LLVM::MulOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Divide:
              result = is_signed ? builder_.create<mlir::LLVM::SDivOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::UDivOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Remainder:
              result = is_signed ? builder_.create<mlir::LLVM::SRemOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::URemOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::BitwiseAnd: result = builder_.create<mlir::LLVM::AndOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::BitwiseOr: result = builder_.create<mlir::LLVM::OrOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::BitwiseXor: result = builder_.create<mlir::LLVM::XOrOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::ShiftLeft: result = builder_.create<mlir::LLVM::ShlOp>(loc, lhs, rhs).getResult(); break;
            case tir::BinaryOp::ShiftRight:
              result = is_signed ? builder_.create<mlir::LLVM::AShrOp>(loc, lhs, rhs).getResult()
                                 : builder_.create<mlir::LLVM::LShrOp>(loc, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::eq, lhs, rhs).getResult(); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, lhs, rhs).getResult(); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::slt : mlir::LLVM::ICmpPredicate::ult, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sle : mlir::LLVM::ICmpPredicate::ule, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sgt : mlir::LLVM::ICmpPredicate::ugt, lhs, rhs).getResult();
              break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sge : mlir::LLVM::ICmpPredicate::uge, lhs, rhs).getResult();
              break;
            default: break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Cast: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        mlir::Type target_ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        mlir::Value result;

        switch (op.cast_kind) {
          case tir::CastKind::Trunc:
            result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::SExt:
            result = builder_.create<mlir::LLVM::SExtOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::ZExt:
          case tir::CastKind::BoolToInt:
          case tir::CastKind::CharToInt:
            if (target_ty.getIntOrFloatBitWidth() < val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            } else if (target_ty.getIntOrFloatBitWidth() > val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val).getResult();
            } else {
              result = val;
            }
            break;
          case tir::CastKind::IntToChar:
            if (val.getType().getIntOrFloatBitWidth() > 32) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val).getResult();
            } else if (val.getType().getIntOrFloatBitWidth() < 32) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val).getResult();
            } else {
              result = val;
            }
            break;
          case tir::CastKind::SIToFP:
            result = builder_.create<mlir::LLVM::SIToFPOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::UIToFP:
            result = builder_.create<mlir::LLVM::UIToFPOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPToSI:
            result = builder_.create<mlir::LLVM::FPToSIOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPToUI:
            result = builder_.create<mlir::LLVM::FPToUIOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPExt:
            result = builder_.create<mlir::LLVM::FPExtOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::FPTrunc:
            result = builder_.create<mlir::LLVM::FPTruncOp>(loc, target_ty, val).getResult();
            break;
          case tir::CastKind::IntToBool: {
            mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0)).getResult();
            result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, val, zero).getResult();
            break;
          }
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::Call: {
        std::vector<mlir::Value> args;
        args.reserve(op.arguments.size());
        for (const tir::Value arg : op.arguments) {
          mlir::Value arg_val = get(arg);
          if (arg_val) args.push_back(arg_val);
        }

        if ((op.callee == "memory.owned_array" ||
             op.callee == "memory.owned_array_aligned") && op.result.is_valid() &&
            op.result.type.is_owned_array()) {
          const uint64_t element_size = type_size(op.result.type.element_type());
          const uint64_t element_alignment =
              type_alignment(op.result.type.element_type());
          const bool custom_alignment = op.callee == "memory.owned_array_aligned";
          if (element_size == 0 || args.empty() ||
              (custom_alignment && args.size() < 2)) {
            diag_.error("NRQ4005", op.location,
                        "internal: invalid element type or missing count for memory.owned_array<T>");
            return false;
          }
          mlir::Value count = args[0];
          const mlir::Type i64 = builder_.getI64Type();
          if (count.getType() != i64) {
            const unsigned bits = count.getType().getIntOrFloatBitWidth();
            count = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, count).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, count).getResult();
          }
          auto allocate = ensure_runtime_fn(
              module_, builder_, "nrq_memory_allocate_array", llvm_ptr_type(ctx_),
              {i64, i64, i64});
          mlir::Value element_bytes = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(element_size)).getResult();
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(element_alignment)).getResult();
          if (custom_alignment) {
            mlir::Value requested = args[1];
            if (requested.getType() != i64) {
              const unsigned bits = requested.getType().getIntOrFloatBitWidth();
              requested = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, requested).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, i64, requested).getResult();
            }
            mlir::Value minimum = builder_.create<mlir::LLVM::ConstantOp>(
                loc, i64, builder_.getI64IntegerAttr(element_alignment)).getResult();
            mlir::Value below_natural = builder_.create<mlir::LLVM::ICmpOp>(
                loc, mlir::LLVM::ICmpPredicate::ult, requested, minimum).getResult();
            alignment = builder_.create<mlir::LLVM::SelectOp>(
                loc, below_natural, minimum, requested).getResult();
          }
          mlir::Value data = builder_.create<mlir::LLVM::CallOp>(
              loc, allocate, mlir::ValueRange{count, element_bytes, alignment}).getResult();
          emit_gc_array_root_registration(
              data, count, op.result.type.element_type(), loc);
          mlir::Value null_pointer = builder_.create<mlir::LLVM::ZeroOp>(
              loc, llvm_ptr_type(ctx_)).getResult();
          mlir::Value succeeded = builder_.create<mlir::LLVM::ICmpOp>(
              loc, mlir::LLVM::ICmpPredicate::ne, data, null_pointer).getResult();
          mlir::Value empty_length = builder_.create<mlir::LLVM::ConstantOp>(
              loc, i64, builder_.getI64IntegerAttr(0)).getResult();
          mlir::Value actual_length = builder_.create<mlir::LLVM::SelectOp>(
              loc, succeeded, count, empty_length).getResult();
          mlir::Type descriptor_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value descriptor = builder_.create<mlir::LLVM::UndefOp>(
              loc, descriptor_type).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, data,
              builder_.getDenseI64ArrayAttr({0})).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, actual_length,
              builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, descriptor);
          return true;
        }

        if (op.callee == "memory.array_view" && op.result.is_valid() &&
            op.result.type.is_array_view() && args.size() == 3) {
          const mlir::Type i64 = builder_.getI64Type();
          auto as_i64 = [&](mlir::Value value) {
            if (value.getType() == i64) return value;
            const unsigned bits = value.getType().getIntOrFloatBitWidth();
            return bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, i64, value).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, value).getResult();
          };
          mlir::Value source_data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          mlir::Value source_length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({1})).getResult();
          mlir::Value start = as_i64(args[1]);
          mlir::Value count = as_i64(args[2]);
          auto range_check = ensure_runtime_fn(
              module_, builder_, "nrq_memory_array_range_check",
              mlir::LLVM::LLVMVoidType::get(ctx_), {i64, i64, i64});
          builder_.create<mlir::LLVM::CallOp>(
              loc, range_check, mlir::ValueRange{start, count, source_length});
          mlir::Type element_type = tir_type_to_mlir(
              op.result.type.element_type(), ctx_, builder_);
          mlir::Value data = builder_.create<mlir::LLVM::GEPOp>(
              loc, llvm_ptr_type(ctx_), element_type, source_data,
              mlir::ValueRange{start}).getResult();
          mlir::Type descriptor_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value descriptor = builder_.create<mlir::LLVM::UndefOp>(
              loc, descriptor_type).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, data, builder_.getDenseI64ArrayAttr({0})).getResult();
          descriptor = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, descriptor, count, builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, descriptor);
          return true;
        }

        if (op.callee == "array.owned_release" && args.size() == 1) {
          mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, args[0], builder_.getDenseI64ArrayAttr({0})).getResult();
          auto unregister = ensure_runtime_fn(
              module_, builder_, "nrq_gc_root_unregister",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(
              loc, unregister, mlir::ValueRange{data});
          auto release = ensure_runtime_fn(
              module_, builder_, "nrq_memory_deallocate_checked",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, release,
                                               mlir::ValueRange{data});
          return true;
        }

        if (op.callee == "aggregate.owned_drop" && args.size() == 1) {
          std::function<void(const Type&, mlir::Value, std::vector<int64_t>&)> drop_fields;
          drop_fields = [&](const Type& type, mlir::Value aggregate,
                            std::vector<int64_t>& path) {
            if (type.is_owned_array()) {
              mlir::Value descriptor = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, aggregate, builder_.getDenseI64ArrayAttr(path)).getResult();
              mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
                  loc, descriptor, builder_.getDenseI64ArrayAttr({0})).getResult();
              auto unregister = ensure_runtime_fn(
                  module_, builder_, "nrq_gc_root_unregister",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(
                  loc, unregister, mlir::ValueRange{data});
              auto release = ensure_runtime_fn(
                  module_, builder_, "nrq_memory_deallocate_checked",
                  mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
              builder_.create<mlir::LLVM::CallOp>(loc, release,
                                                   mlir::ValueRange{data});
              return;
            }
            auto visit = [&](const Type& field_type, int64_t index) {
              path.push_back(index);
              if (field_type.is_owned_array() || field_type.is_array() ||
                  field_type.is_struct() || field_type.is_tuple())
                drop_fields(field_type, aggregate, path);
              path.pop_back();
            };
            if (type.is_array()) {
              for (std::size_t index = 0; index < type.array_length(); ++index)
                visit(type.element_type(), static_cast<int64_t>(index));
            } else if (type.is_struct()) {
              for (std::size_t index = 0; index < type.struct_fields().size(); ++index)
                visit(type.struct_fields()[index].second, static_cast<int64_t>(index));
            } else if (type.is_tuple()) {
              for (std::size_t index = 0; index < type.tuple_elements().size(); ++index)
                visit(type.tuple_elements()[index], static_cast<int64_t>(index));
            }
          };
          std::vector<int64_t> path;
          drop_fields(op.arguments[0].type, args[0], path);
          return true;
        }

        // Memory allocation is exposed as an integer handle in the language,
        // while the runtime ABI uses void*. Keep that boundary conversion
        // explicit so the call is emitted as an LLVM runtime call (rather
        // than an unresolved func.call to the runtime symbol).
        if (op.callee == "array.allocate" && op.result.is_valid() &&
            op.result.type.is_managed_pointer()) {
          const Type& element_type = op.result.type.element_type();
          const uint64_t element_size = type_size(element_type);
          if (element_size == 0 || args.empty()) {
            diag_.error("NRQ4005", op.location,
                        "internal: invalid element type or missing count for array.allocate<T>");
            return false;
          }
          mlir::Value count = args[0];
          if (count.getType() != builder_.getI64Type()) {
            const unsigned bits = count.getType().getIntOrFloatBitWidth();
            count = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), count).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), count).getResult();
          }
          std::vector<uint64_t> reference_offsets;
          collect_managed_reference_offsets(element_type, 0, reference_offsets);
          mlir::Value offsets_pointer = builder_.create<mlir::LLVM::ZeroOp>(
              loc, llvm_ptr_type(ctx_)).getResult();
          if (!reference_offsets.empty()) {
            auto offsets_type = mlir::LLVM::LLVMArrayType::get(
                builder_.getI64Type(), static_cast<unsigned>(reference_offsets.size()));
            mlir::Value offset_count = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
            offsets_pointer = builder_.create<mlir::LLVM::AllocaOp>(
                loc, llvm_ptr_type(ctx_), offsets_type, offset_count).getResult();
            mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();
            for (std::size_t index = 0; index < reference_offsets.size(); ++index) {
              mlir::Value index_value = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, builder_.getI64Type(),
                  builder_.getI64IntegerAttr(static_cast<int64_t>(index))).getResult();
              mlir::Value slot = builder_.create<mlir::LLVM::GEPOp>(
                  loc, llvm_ptr_type(ctx_), offsets_type, offsets_pointer,
                  mlir::ValueRange{zero, index_value}).getResult();
              mlir::Value offset_value = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, builder_.getI64Type(),
                  builder_.getI64IntegerAttr(static_cast<int64_t>(reference_offsets[index]))).getResult();
              builder_.create<mlir::LLVM::StoreOp>(loc, offset_value, slot);
            }
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_allocate_array_typed",
                                     llvm_ptr_type(ctx_),
                                     {builder_.getI64Type(), builder_.getI64Type(),
                                      llvm_ptr_type(ctx_), builder_.getI64Type()});
          mlir::Value size = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(element_size)).getResult();
          mlir::Value ref_count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(static_cast<int64_t>(reference_offsets.size()))).getResult();
          mlir::Value result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{count, size, offsets_pointer, ref_count}).getResult();
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          mlir::Value bytes = args.empty() ? mlir::Value{} : args[0];
          if (!bytes) {
            diag_.error("NRQ4005", op.location,
                        "internal: memory.allocate<T> has no byte-count operand");
            return false;
          }
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
          mlir::Value result;
          if (args.size() == 2) {
            mlir::Value allocator = args[1];
            if (allocator.getType() != builder_.getI64Type()) {
              const unsigned bits = allocator.getType().getIntOrFloatBitWidth();
              allocator = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), allocator).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), allocator).getResult();
            }
            allocator = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), allocator).getResult();
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate_from",
                llvm_ptr_type(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type()});
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{allocator, bytes, alignment}).getResult();
          } else {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate",
                llvm_ptr_type(ctx_), {builder_.getI64Type(), builder_.getI64Type()});
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{bytes, alignment}).getResult();
          }
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.shared_allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          if (args.empty()) return false;
          mlir::Value bytes = args[0];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_shared_allocate",
              llvm_ptr_type(ctx_), {builder_.getI64Type(), builder_.getI64Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{bytes, alignment}).getResult());
          return true;
        }
        if (op.callee == "memory.shared_retain" ||
            op.callee == "memory.shared_release" ||
            op.callee == "memory.shared_count") {
          if (args.empty()) return false;
          const char* symbol = op.callee == "memory.shared_retain"
              ? "nrq_memory_shared_retain"
              : op.callee == "memory.shared_release"
                  ? "nrq_memory_shared_release" : "nrq_memory_shared_count";
          const bool count = op.callee == "memory.shared_count";
          const bool retain = op.callee == "memory.shared_retain";
          mlir::Type result_type = (count ? builder_.getI64Type()
                                          : retain ? llvm_ptr_type(ctx_)
                                                   : builder_.getI32Type());
          auto fn = ensure_runtime_fn(module_, builder_, symbol, result_type,
                                      {llvm_ptr_type(ctx_)});
          auto result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{args[0]});
          if (op.result.is_valid()) set(op.result, result.getResult());
          return true;
        }
        if (op.callee == "memory.secure_allocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_allocate",
                                     llvm_ptr_type(ctx_),
                                     {builder_.getI64Type(), builder_.getI64Type()});
          if (args.empty()) return false;
          mlir::Value bytes = args[0];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          mlir::Value alignment = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_alignment(op.result.type.element_type()))).getResult();
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{bytes, alignment}).getResult());
          return true;
        }
        if (op.callee == "memory.mapped_read" ||
            op.callee == "memory.mapped_write") {
          const bool write = op.callee == "memory.mapped_write";
          if (args.size() != (write ? 3u : 2u)) return false;
          mlir::Value offset = args[1];
          if (offset.getType() != builder_.getI64Type()) {
            const unsigned bits = offset.getType().getIntOrFloatBitWidth();
            offset = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), offset).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), offset).getResult();
          }
          mlir::Type element_type = tir_type_to_mlir(
              write ? op.arguments[2].type : op.result.type, ctx_, builder_);
          mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
          mlir::Value temporary = builder_.create<mlir::LLVM::AllocaOp>(
              loc, llvm_ptr_type(ctx_), element_type, one).getResult();
          mlir::Value byte_count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(type_size(write ? op.arguments[2].type
                                                         : op.result.type))).getResult();
          auto fn = ensure_runtime_fn(module_, builder_,
              write ? "nrq_memory_mapping_write" : "nrq_memory_mapping_read",
              builder_.getI32Type(),
              {llvm_ptr_type(ctx_), builder_.getI64Type(), builder_.getI64Type(),
               llvm_ptr_type(ctx_)});
          if (write) builder_.create<mlir::LLVM::StoreOp>(loc, args[2], temporary);
          builder_.create<mlir::LLVM::CallOp>(loc, fn,
              mlir::ValueRange{args[0], offset, byte_count, temporary});
          if (!write) set(op.result, builder_.create<mlir::LLVM::LoadOp>(
              loc, element_type, temporary).getResult());
          return true;
        }
        if (op.callee == "memory.profiler.start" ||
            op.callee == "memory.profiler.stop") {
          auto fn = ensure_runtime_fn(module_, builder_,
              op.callee == "memory.profiler.start" ? "nrq_memory_profiler_start"
                                                     : "nrq_memory_profiler_stop",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{});
          return true;
        }
        if (op.callee == "memory.profiler.capture") {
          mlir::Type profile_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value count = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1)).getResult();
          mlir::Value slot = builder_.create<mlir::LLVM::AllocaOp>(
              loc, llvm_ptr_type(ctx_), profile_type, count).getResult();
          auto capture = ensure_runtime_fn(module_, builder_,
              "nrq_memory_profiler_capture_into",
              mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, capture, mlir::ValueRange{slot});
          set(op.result, builder_.create<mlir::LLVM::LoadOp>(
              loc, profile_type, slot).getResult());
          return true;
        }
        if (op.callee == "memory.available_bytes") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_available_bytes",
                                      builder_.getI64Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.runtime_version") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_runtime_version",
                                      llvm_ptr_type(ctx_), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.cpu_count") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_cpu_count",
                                      builder_.getI32Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.cpu_features") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_cpu_features",
                                      builder_.getI64Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.thread_count") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_system_thread_count",
                                      builder_.getI32Type(), {});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{}).getResult());
          return true;
        }
        if (op.callee == "memory.allocator_kind") {
          mlir::Value handle;
          if (args.empty()) {
            handle = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(0)).getResult();
          } else {
            handle = args.front();
          }
          if (handle.getType() != builder_.getI64Type()) {
            const unsigned bits = handle.getType().getIntOrFloatBitWidth();
            handle = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), handle).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), handle).getResult();
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocator_kind",
              builder_.getI32Type(), {builder_.getI64Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{handle}).getResult());
          return true;
        }
        if (op.callee == "memory.start_ref_count" ||
            op.callee == "memory.stop_ref_count") {
          auto fn = ensure_runtime_fn(module_, builder_,
              op.callee == "memory.start_ref_count" ? "nrq_gc_ref_count_start"
                                                      : "nrq_gc_ref_count_stop",
              mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{});
          return true;
        }
        if (op.callee == "memory.get_ref_count") {
          if (args.empty()) return false;
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_get_ref_count",
              builder_.getI64Type(), {llvm_ptr_type(ctx_)});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{pointer}).getResult());
          return true;
        }
        if (op.callee == "memory.map_file") {
          if (args.size() != 2) return false;
          mlir::Value writable = args[1];
          if (writable.getType() != builder_.getI32Type())
            writable = builder_.create<mlir::LLVM::ZExtOp>(
                loc, builder_.getI32Type(), writable).getResult();
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_map_file",
              llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_), builder_.getI32Type()});
          set(op.result, builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{args[0], writable}).getResult());
          return true;
        }
        if (op.callee == "memory.mapping_size" ||
            op.callee == "memory.unmap_file" ||
            op.callee == "memory.secure_release" ||
            op.callee == "memory.secure_zero" ||
            op.callee == "memory.pin" || op.callee == "memory.unpin") {
          if (args.empty()) return false;
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          if (op.callee == "memory.mapping_size") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_mapping_size",
                builder_.getI64Type(), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else if (op.callee == "memory.unmap_file") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_unmap_file",
                builder_.getI32Type(), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else if (op.callee == "memory.secure_release") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_release",
                mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer});
          } else if (op.callee == "memory.secure_zero") {
            if (args.size() != 2) return false;
            mlir::Value bytes = args[1];
            if (bytes.getType() != builder_.getI64Type()) {
              const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
              bytes = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
            }
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_secure_zero",
                mlir::LLVM::LLVMVoidType::get(ctx_),
                {llvm_ptr_type(ctx_), builder_.getI64Type()});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer, bytes});
          } else if (op.callee == "memory.pin") {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_pin",
                llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
            set(op.result, builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{pointer}).getResult());
          } else {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_gc_unpin",
                mlir::LLVM::LLVMVoidType::get(ctx_), {llvm_ptr_type(ctx_)});
            builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer});
          }
          return true;
        }
        {
          static const std::unordered_map<std::string, int> allocator_kinds = {
              {"memory.general_allocator", 0}, {"memory.pool_allocator", 1},
              {"memory.arena_allocator", 2}, {"memory.region", 2},
              {"memory.stack_allocator", 3},
              {"memory.slab_allocator", 4}, {"memory.buddy_allocator", 5},
              {"memory.free_list_allocator", 6}, {"memory.tlsf_allocator", 7}};
          auto constructor = allocator_kinds.find(op.callee);
          if (constructor != allocator_kinds.end()) {
            auto fn = ensure_runtime_fn(module_, builder_, "nrq_allocator_create",
                                       llvm_ptr_type(ctx_),
                                       {builder_.getI32Type(), builder_.getI64Type()});
            mlir::Value capacity = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1024 * 1024)).getResult();
            if (!args.empty()) {
              capacity = args[0];
              if (capacity.getType() != builder_.getI64Type()) {
                const unsigned bits = capacity.getType().getIntOrFloatBitWidth();
                capacity = bits < 64
                    ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), capacity).getResult()
                    : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), capacity).getResult();
              }
            }
            mlir::Value kind = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI32Type(),
                builder_.getI32IntegerAttr(constructor->second)).getResult();
            auto result = builder_.create<mlir::LLVM::CallOp>(
                loc, fn, mlir::ValueRange{kind, capacity}).getResult();
            set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(
                loc, builder_.getI64Type(), result).getResult());
            return true;
          }
        }
        if (op.callee == "memory.get_allocator" ||
            op.callee == "memory.use_allocator" ||
            op.callee == "memory.reset_allocator" ||
            op.callee == "memory.destroy_allocator" ||
            op.callee == "memory.get_default_allocator" ||
            op.callee == "memory.set_default_allocator") {
          if (op.callee == "memory.get_allocator" ||
              op.callee == "memory.get_default_allocator") {
            auto fn = ensure_runtime_fn(module_, builder_,
                op.callee == "memory.get_allocator" ? "nrq_memory_get_allocator"
                                                     : "nrq_memory_get_default_allocator",
                llvm_ptr_type(ctx_), {});
            auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{}).getResult();
            set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(loc, builder_.getI64Type(), result).getResult());
          } else {
            mlir::Value handle = args.empty() ? mlir::Value{} : args[0];
            if (!handle) return false;
            if (handle.getType() != builder_.getI64Type()) {
              const unsigned bits = handle.getType().getIntOrFloatBitWidth();
              handle = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), handle).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), handle).getResult();
            }
            auto pointer = builder_.create<mlir::LLVM::IntToPtrOp>(loc, llvm_ptr_type(ctx_), handle).getResult();
            if (op.callee == "memory.use_allocator" ||
                op.callee == "memory.set_default_allocator") {
              auto fn = ensure_runtime_fn(module_, builder_,
                  op.callee == "memory.use_allocator" ? "nrq_memory_use_allocator"
                                                       : "nrq_memory_set_default_allocator",
                  llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_)});
              auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer}).getResult();
              set(op.result, builder_.create<mlir::LLVM::PtrToIntOp>(loc, builder_.getI64Type(), result).getResult());
            } else {
              const bool reset = op.callee == "memory.reset_allocator";
              auto fn = ensure_runtime_fn(module_, builder_, reset ? "nrq_allocator_reset" : "nrq_allocator_destroy",
                                         builder_.getI32Type(), {llvm_ptr_type(ctx_)});
              auto result = builder_.create<mlir::LLVM::CallOp>(loc, fn, mlir::ValueRange{pointer}).getResult();
              set(op.result, result);
            }
          }
          return true;
        }
        if (op.callee == "memory.reallocate" && op.result.is_valid() &&
            op.result.type.is_pointer()) {
          if (args.size() != 2) {
            diag_.error("NRQ4005", op.location,
                        "internal: memory.reallocate<T> needs a pointer and a byte count");
            return false;
          }
          mlir::Value pointer = args[0];
          if (!mlir::isa<mlir::LLVM::LLVMPointerType>(pointer.getType()))
            pointer = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), pointer).getResult();
          mlir::Value bytes = args[1];
          if (bytes.getType() != builder_.getI64Type()) {
            const unsigned bits = bytes.getType().getIntOrFloatBitWidth();
            bytes = bits < 64
                ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), bytes).getResult()
                : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), bytes).getResult();
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_reallocate",
              llvm_ptr_type(ctx_), {llvm_ptr_type(ctx_), builder_.getI64Type()});
          mlir::Value result = builder_.create<mlir::LLVM::CallOp>(
              loc, fn, mlir::ValueRange{pointer, bytes}).getResult();
          set(op.result, result);
          return true;
        }
        if (op.callee == "memory.deallocate") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_deallocate_checked",
                                     mlir::LLVM::LLVMVoidType::get(ctx_),
                                     {llvm_ptr_type(ctx_)});
          if (!args.empty()) builder_.create<mlir::LLVM::CallOp>(loc, fn, args);
          return true;
        }
        if (op.callee == "nrq_memory_allocate" || op.callee == "memory.allocate") {
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_allocate",
                                     llvm_ptr_type(ctx_),
                                     {builder_.getI64Type(), builder_.getI64Type()});
          auto call_op = builder_.create<mlir::LLVM::CallOp>(loc, fn, args);
          if (op.result.is_valid()) {
            mlir::Value result = builder_.create<mlir::LLVM::PtrToIntOp>(
                loc, builder_.getI64Type(), call_op.getResult()).getResult();
            set(op.result, result);
          }
          return true;
        }
        if (op.callee == "nrq_memory_deallocate") {
          if (!args.empty()) {
            args[0] = builder_.create<mlir::LLVM::IntToPtrOp>(
                loc, llvm_ptr_type(ctx_), args[0]).getResult();
          }
          auto fn = ensure_runtime_fn(module_, builder_, "nrq_memory_deallocate",
                                     mlir::LLVM::LLVMVoidType::get(ctx_),
                                     {llvm_ptr_type(ctx_), builder_.getI64Type(),
                                      builder_.getI64Type()});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, args);
          return true;
        }
        if (op.callee == "gc.get_statistics") {
          static constexpr const char* stat_symbols[] = {
              "nrq_gc_get_allocated_bytes", "nrq_gc_get_collected_bytes",
              "nrq_gc_get_collection_count", "nrq_gc_get_live_objects"};
          mlir::Type stats_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value stats = builder_.create<mlir::LLVM::UndefOp>(loc, stats_type).getResult();
          for (int index = 0; index < 4; ++index) {
            auto getter = ensure_runtime_fn(module_, builder_, stat_symbols[index],
                                            builder_.getI64Type(), {});
            mlir::Value field = builder_.create<mlir::LLVM::CallOp>(
                loc, getter, mlir::ValueRange{}).getResult();
            stats = builder_.create<mlir::LLVM::InsertValueOp>(
                loc, stats, field,
                builder_.getDenseI64ArrayAttr({index})).getResult();
          }
          set(op.result, stats);
          return true;
        }
        if (op.callee == "gc.set_threshold") {
          auto setter = ensure_runtime_fn(module_, builder_, "nrq_gc_set_threshold",
                                         mlir::LLVM::LLVMVoidType::get(ctx_),
                                         {builder_.getI64Type()});
          if (!args.empty()) {
            mlir::Value threshold = args[0];
            if (threshold.getType() != builder_.getI64Type()) {
              const unsigned bits = threshold.getType().getIntOrFloatBitWidth();
              threshold = bits < 64
                  ? builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), threshold).getResult()
                  : builder_.create<mlir::LLVM::TruncOp>(loc, builder_.getI64Type(), threshold).getResult();
            }
            builder_.create<mlir::LLVM::CallOp>(loc, setter, mlir::ValueRange{threshold});
          }
          return true;
        }
        if (op.callee == "nrq_gc_init" || op.callee == "nrq_gc_enable" ||
            op.callee == "gc.enable" || op.callee == "nrq_gc_shutdown" ||
            op.callee == "nrq_gc_disable" || op.callee == "gc.disable" ||
            op.callee == "nrq_gc_collect" || op.callee == "gc.collect") {
          const char* runtime_sym =
              (op.callee == "nrq_gc_init" || op.callee == "nrq_gc_enable" ||
               op.callee == "gc.enable")
                  ? "nrq_gc_enable"
                  : (op.callee == "nrq_gc_shutdown" || op.callee == "nrq_gc_disable" ||
                     op.callee == "gc.disable")
                        ? "nrq_gc_disable"
                        : "nrq_gc_collect";
          auto fn = ensure_runtime_fn(module_, builder_, runtime_sym,
                                     mlir::LLVM::LLVMVoidType::get(ctx_), {});
          builder_.create<mlir::LLVM::CallOp>(loc, fn, args);
          return true;
        }

        // Check if this is a runtime library builtin call:
        llvm::StringRef callee = op.callee;
        std::string runtime_sym;
        mlir::Type runtime_ret_ty;
        std::vector<mlir::Type> runtime_arg_tys;

        if (callee == "memory.array_bounds_check") {
          runtime_sym = "nrq_memory_array_bounds_check";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {builder_.getI64Type(), builder_.getI64Type()};
        } else if (callee == "mutex.create") {
          runtime_sym = "nrq_mutex_create";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {};
        } else if (callee == "mutex.lock") {
          runtime_sym = "nrq_mutex_lock";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "mutex.unlock") {
          runtime_sym = "nrq_mutex_unlock";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "mutex.destroy") {
          runtime_sym = "nrq_mutex_destroy";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.open") {
          runtime_sym = "nrq_file_open_handle";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_), builder_.getI32Type()};
        } else if (callee == "file.close") {
          runtime_sym = "nrq_file_close_handle";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.write") {
          runtime_sym = "nrq_file_write_handle";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.read") {
          runtime_sym = "nrq_file_read_handle";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_), builder_.getI64Type()};
        } else if (callee == "file.read_text") {
          runtime_sym = "nrq_file_read_text";
          runtime_ret_ty = llvm_ptr_type(ctx_);
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "file.write_text") {
          runtime_sym = "nrq_file_write_text";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.append_text") {
          runtime_sym = "nrq_file_append_text";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_), llvm_ptr_type(ctx_)};
        } else if (callee == "file.exists") {
          runtime_sym = "nrq_file_exists";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {llvm_ptr_type(ctx_)};
        } else if (callee == "time.now") {
          runtime_sym = "nrq_time_now_ms";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.used_bytes") {
          runtime_sym = "nrq_system_used_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.total_bytes") {
          runtime_sym = "nrq_system_total_bytes";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.pin_count") {
          runtime_sym = "nrq_gc_pin_count";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "memory.pin_violations") {
          runtime_sym = "nrq_gc_pin_violations";
          runtime_ret_ty = builder_.getI64Type();
          runtime_arg_tys = {};
        } else if (callee == "gc.is_enabled") {
          // The runtime reports a 32-bit flag, while the language type is bool;
          // adapt_runtime_result performs the narrowing.
          runtime_sym = "nrq_gc_is_enabled";
          runtime_ret_ty = builder_.getI32Type();
          runtime_arg_tys = {};
        } else if (callee == "system.exit") {
          runtime_sym = "nrq_system_exit";
          runtime_ret_ty = mlir::LLVM::LLVMVoidType::get(ctx_);
          runtime_arg_tys = {builder_.getI32Type()};
        } else if (callee == "math.sqrt" || callee == "sqrt") {
          runtime_sym = "nrq_sqrt";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.sin" || callee == "sin") {
          runtime_sym = "nrq_sin";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.cos" || callee == "cos") {
          runtime_sym = "nrq_cos";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.abs" || callee == "abs") {
          runtime_sym = "nrq_abs";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.floor" || callee == "floor") {
          runtime_sym = "nrq_floor";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        } else if (callee == "math.ceil" || callee == "ceil") {
          runtime_sym = "nrq_ceil";
          runtime_ret_ty = builder_.getF64Type();
          runtime_arg_tys = {builder_.getF64Type()};
        }

        if (!runtime_sym.empty()) {
          auto fn = ensure_runtime_fn(module_, builder_, runtime_sym, runtime_ret_ty, runtime_arg_tys);
          llvm::SmallVector<mlir::Value> abi_args;
          abi_args.reserve(args.size());
          for (size_t index = 0; index < args.size(); ++index) {
            mlir::Value value = args[index];
            if (index < runtime_arg_tys.size() && value.getType() != runtime_arg_tys[index]) {
              mlir::Type target = runtime_arg_tys[index];
              if (value.getType().isInteger(1) && target.isInteger()) {
                value = builder_.create<mlir::LLVM::ZExtOp>(loc, target, value).getResult();
              } else if (target.isInteger() &&
                         value.getType().getIntOrFloatBitWidth() != 0) {
                const unsigned source_width = value.getType().getIntOrFloatBitWidth();
                const unsigned target_width = target.getIntOrFloatBitWidth();
                if (source_width < target_width)
                  value = builder_.create<mlir::LLVM::ZExtOp>(loc, target, value).getResult();
                else
                  value = builder_.create<mlir::LLVM::TruncOp>(loc, target, value).getResult();
              }
            }
            abi_args.push_back(value);
          }
          auto call_op = builder_.create<mlir::LLVM::CallOp>(loc, fn, abi_args);
          if (op.result.is_valid()) {
            // The runtime ABI is deliberately narrow (i32 status codes, i64
            // timestamps), while the language types the same call as `int`,
            // `bool` or `string`. Bridge the two here so the TIR result type
            // and the produced SSA value always agree.
            set(op.result, adapt_runtime_result(call_op.getResult(), op.result.type, loc));
          }
          return true;
        }

        if (op.result.is_valid()) {
          mlir::Type ret_ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
          auto call_op = builder_.create<mlir::func::CallOp>(
              loc, op.callee, mlir::TypeRange{ret_ty}, args);
          set(op.result, call_op.getResult(0));
        } else {
          builder_.create<mlir::func::CallOp>(
              loc, op.callee, mlir::TypeRange{}, args);
        }
        return true;
      }

      case tir::OpKind::PrintString: {
        builder_.create<nrq::PrintStringOp>(loc, builder_.getStringAttr(op.string_value));
        return true;
      }

      case tir::OpKind::Print: {
        mlir::Value val = get(op.operand);
        if (!val) return true;
        const Type& ty = op.operand.type;

        if (ty.is_bool()) {
          builder_.create<nrq::PrintBoolOp>(loc, val);
        } else if (ty.is_string()) {
          // A string value (for example `file.read_text(...)`) is a NUL-terminated
          // pointer at this point, so it is printed through the runtime.
          auto print_fn = ensure_runtime_fn(module_, builder_, "nrq_print_cstr",
                                            builder_.getI32Type(), {llvm_ptr_type(ctx_)});
          builder_.create<mlir::LLVM::CallOp>(loc, print_fn, mlir::ValueRange{val});
        } else if (ty.is_char()) {
          builder_.create<nrq::PrintCharOp>(loc, val);
        } else if (ty.is_float()) {
          mlir::Value f64_val = ty.is_f32()
              ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), val).getResult()
              : val;
          builder_.create<nrq::PrintF64Op>(loc, f64_val);
        } else if (ty.is_integer()) {
          mlir::Value i64_val = val;
          if (ty.bit_width() < 64) {
            i64_val = ty.is_signed_integer()
                ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), val).getResult()
                : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), val).getResult();
          }
          if (ty.is_signed_integer()) {
            builder_.create<nrq::PrintIntOp>(loc, i64_val);
          } else {
            builder_.create<nrq::PrintUintOp>(loc, i64_val);
          }
        }
        return true;
      }

      case tir::OpKind::Return: {
        auto end_frame = ensure_runtime_fn(module_, builder_, "nrq_gc_root_frame_end",
                                           mlir::LLVM::LLVMVoidType::get(ctx_),
                                           {builder_.getI64Type()});
        builder_.create<mlir::LLVM::CallOp>(loc, end_frame,
                                            mlir::ValueRange{gc_root_frame_marker_});
        if (op.operand.is_valid()) {
          mlir::Value val = get(op.operand);
          if (val) {
            builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{val});
          } else {
            builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{});
          }
        } else {
          builder_.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{});
        }
        return true;
      }

      case tir::OpKind::Unreachable: {
        builder_.create<mlir::LLVM::UnreachableOp>(loc);
        return true;
      }

      case tir::OpKind::ArrayAlloca: {
        // Alloca for a fixed-size array, hoisted into the entry block.
        mlir::Type arr_mlir = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!arr_mlir) return true;
        set(op.result, alloca_at_entry(arr_mlir, loc));
        return true;
      }

      case tir::OpKind::GetElementPtr: {
        // Pointer into an array or struct field.
        mlir::Value base_ptr = get(op.lhs);
        if (!base_ptr) return true;

        mlir::Type result_elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!result_elem_type) return true;

        // Build GEP indices: [0, dynamic_index] for arrays, [0, const_field] for structs.
        mlir::Value idx;
        if (op.operand.is_valid()) {
          idx = get(op.operand);
        } else {
          idx = builder_.create<mlir::LLVM::ConstantOp>(
              loc, builder_.getI64Type(),
              builder_.getI64IntegerAttr(op.field_index >= 0 ? op.field_index : 0)).getResult();
        }

        // The base type for GEP is the array/struct type; we use a zero-based outer index.
        mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI32Type(), builder_.getI32IntegerAttr(0)).getResult();

        // Determine the pointee type from the base value's allocation type.
        const Type& base_type = op.lhs.type;
        mlir::Type pointee_type = (base_type.is_pointer() || base_type.is_managed_pointer())
            ? tir_type_to_mlir(op.result.type, ctx_, builder_)
            : tir_type_to_mlir(base_type, ctx_, builder_);
        if (!pointee_type) {
          // Fall back: opaque pointer arithmetic using i8 GEP.
          set(op.result, base_ptr);
          return true;
        }

        mlir::Value gep = (base_type.is_pointer() || base_type.is_managed_pointer())
            ? builder_.create<mlir::LLVM::GEPOp>(
                  loc, llvm_ptr_type(ctx_), pointee_type, base_ptr,
                  mlir::ValueRange{idx}).getResult()
            : builder_.create<mlir::LLVM::GEPOp>(
                  loc, llvm_ptr_type(ctx_), pointee_type, base_ptr,
                  mlir::ValueRange{zero, idx}).getResult();
        set(op.result, gep);
        return true;
      }

      case tir::OpKind::AggregatePack: {
        // Pack multiple values into a struct/tuple value using llvm.insertvalue.
        mlir::Type agg_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!agg_type) {
          // Unsupported aggregate type — emit undef.
          set(op.result, {});
          return true;
        }

        mlir::Value agg = builder_.create<mlir::LLVM::UndefOp>(loc, agg_type).getResult();
        for (std::size_t i = 0; i < op.aggregate_elements.size(); ++i) {
          mlir::Value elem = get(op.aggregate_elements[i]);
          if (!elem) continue;
          agg = builder_.create<mlir::LLVM::InsertValueOp>(
              loc, agg, elem,
              builder_.getDenseI64ArrayAttr({static_cast<int64_t>(i)})).getResult();
        }
        set(op.result, agg);
        return true;
      }

      case tir::OpKind::AggregateExtract: {
        // Extract a field from a struct/tuple/array value using llvm.extractvalue.
        mlir::Value agg = get(op.operand);
        if (!agg) return true;

        mlir::Type result_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!result_type) return true;

        const int idx = op.field_index >= 0 ? op.field_index : 0;
        mlir::Value extracted = builder_.create<mlir::LLVM::ExtractValueOp>(
            loc, agg,
            builder_.getDenseI64ArrayAttr({static_cast<int64_t>(idx)})).getResult();
        set(op.result, extracted);
        return true;
      }

      case tir::OpKind::Length: {
        mlir::Value value = get(op.operand);
        if (!value) return true;

        std::string runtime_sym;
        if (op.operand.type.is_string()) {
          runtime_sym = "nrq_strlen";
        } else if (op.operand.type.is_dynamic_array()) {
          runtime_sym = "nrq_args_length";
        } else if (op.operand.type.is_owned_array() ||
                   op.operand.type.is_array_view()) {
          mlir::Value length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, value, builder_.getDenseI64ArrayAttr({1})).getResult();
          set(op.result, adapt_runtime_result(length, op.result.type, loc));
          return true;
        } else {
          diag_.error("NRQ4005", op.location,
                      "internal: no length operation for type '" +
                          op.operand.type.to_string() + "'");
          return false;
        }

        auto fn = ensure_runtime_fn(module_, builder_, runtime_sym,
                                    builder_.getI64Type(), {llvm_ptr_type(ctx_)});
        mlir::Value length = builder_.create<mlir::LLVM::CallOp>(
            loc, fn, mlir::ValueRange{value}).getResult();
        set(op.result, adapt_runtime_result(length, op.result.type, loc));
        return true;
      }

      case tir::OpKind::DynamicIndex: {
        mlir::Value array = get(op.lhs);
        mlir::Value index = get(op.operand);
        if (!array || !index) return true;

        if (op.lhs.type.is_owned_array() || op.lhs.type.is_array_view()) {
          const mlir::Type i64 = builder_.getI64Type();
          if (index.getType() != i64) {
            const unsigned bits = index.getType().getIntOrFloatBitWidth();
            index = bits < 64
                ? (op.operand.type.is_signed_integer()
                       ? builder_.create<mlir::LLVM::SExtOp>(loc, i64, index).getResult()
                       : builder_.create<mlir::LLVM::ZExtOp>(loc, i64, index).getResult())
                : builder_.create<mlir::LLVM::TruncOp>(loc, i64, index).getResult();
          }
          mlir::Value data = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, array, builder_.getDenseI64ArrayAttr({0})).getResult();
          mlir::Value length = builder_.create<mlir::LLVM::ExtractValueOp>(
              loc, array, builder_.getDenseI64ArrayAttr({1})).getResult();
          auto bounds = ensure_runtime_fn(
              module_, builder_, "nrq_memory_array_bounds_check",
              mlir::LLVM::LLVMVoidType::get(ctx_), {i64, i64});
          builder_.create<mlir::LLVM::CallOp>(loc, bounds,
              mlir::ValueRange{index, length});
          mlir::Type element_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
          mlir::Value pointer = builder_.create<mlir::LLVM::GEPOp>(
              loc, llvm_ptr_type(ctx_), element_type, data,
              mlir::ValueRange{index}).getResult();
          mlir::Value element = builder_.create<mlir::LLVM::LoadOp>(
              loc, element_type, pointer).getResult();
          set(op.result, element);
          return true;
        }
        if (!op.lhs.type.is_dynamic_array()) {
          diag_.error("NRQ4005", op.location,
                      "internal: dynamic indexing of non-dynamic array type '" +
                          op.lhs.type.to_string() + "'");
          return false;
        }

        // The runtime ABI takes a 64-bit index.
        mlir::Type i64 = builder_.getI64Type();
        if (index.getType() != i64) {
          if (!mlir::isa<mlir::IntegerType>(index.getType())) {
            diag_.error("NRQ4005", op.location,
                        "internal: dynamic array index is not an integer");
            return false;
          }
          const unsigned bits = index.getType().getIntOrFloatBitWidth();
          if (bits < 64) {
            index = op.operand.type.is_signed_integer()
                        ? builder_.create<mlir::LLVM::SExtOp>(loc, i64, index).getResult()
                        : builder_.create<mlir::LLVM::ZExtOp>(loc, i64, index).getResult();
          } else {
            index = builder_.create<mlir::LLVM::TruncOp>(loc, i64, index).getResult();
          }
        }

        auto fn = ensure_runtime_fn(module_, builder_, "nrq_args_get",
                                    llvm_ptr_type(ctx_),
                                    {llvm_ptr_type(ctx_), i64});
        mlir::Value element = builder_.create<mlir::LLVM::CallOp>(
            loc, fn, mlir::ValueRange{array, index}).getResult();
        set(op.result, element);
        return true;
      }
    }
    return true;
  }

  mlir::ModuleOp module_;
  const tir::Function& tir_fn_;
  mlir::func::FuncOp func_op_;
  mlir::OpBuilder& builder_;
  mlir::MLIRContext* ctx_;
  DiagnosticEngine& diag_;
  mlir::Block* entry_block_;
  struct PendingGcRoot {
    mlir::Value slot;
    Type type;
    mlir::Location loc;
  };
  std::vector<PendingGcRoot> pending_gc_roots_;
  std::vector<mlir::Value> values_;
  std::unordered_map<int, mlir::Block*> label_blocks_;
  std::unordered_map<std::string, std::string> string_literals_;
  int string_literal_counter_ = 0;
  mlir::Value gc_root_frame_marker_;
};

// The Windows CRT calls `main(int argc, char** argv)`. A Nirqoba entry point
// that declares `string[] args` needs a different signature, so it is emitted as
// `nrq_entry_args` and this wrapper marshals the native arguments into the
// runtime-managed array before calling it.
static bool emit_entry_wrapper(mlir::ModuleOp module, mlir::OpBuilder& builder,
                               const tir::Function& user_main,
                               llvm::StringRef user_symbol,
                               DiagnosticEngine& diagnostics) {
  mlir::MLIRContext* ctx = module.getContext();
  mlir::Location loc = mlir::UnknownLoc::get(ctx);
  mlir::Type i32 = builder.getI32Type();
  mlir::Type ptr = llvm_ptr_type(ctx);

  mlir::Type user_ret;
  if (!user_main.return_type().is_void()) {
    user_ret = tir_type_to_mlir(user_main.return_type(), ctx, builder);
    if (!user_ret) {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: unsupported 'main' return type in entry wrapper");
      return false;
    }
  }

  builder.setInsertionPointToEnd(module.getBody());
  auto wrapper = builder.create<mlir::func::FuncOp>(
      loc, "main", mlir::FunctionType::get(ctx, {i32, ptr}, {i32}));
  mlir::Block* entry = wrapper.addEntryBlock();
  builder.setInsertionPointToStart(entry);

  auto args_build = ensure_runtime_fn(module, builder, "nrq_args_build", ptr, {i32, ptr});
  mlir::Value array = builder.create<mlir::LLVM::CallOp>(
      loc, args_build,
      mlir::ValueRange{entry->getArgument(0), entry->getArgument(1)}).getResult();

  mlir::Value exit_code;
  if (user_ret) {
    mlir::Value result = builder.create<mlir::func::CallOp>(
        loc, user_symbol, mlir::TypeRange{user_ret}, mlir::ValueRange{array}).getResult(0);

    if (result.getType() == i32) {
      exit_code = result;
    } else if (mlir::isa<mlir::IntegerType>(result.getType())) {
      const unsigned bits = result.getType().getIntOrFloatBitWidth();
      exit_code = bits < 32
          ? builder.create<mlir::LLVM::SExtOp>(loc, i32, result).getResult()
          : builder.create<mlir::LLVM::TruncOp>(loc, i32, result).getResult();
    } else {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: 'main' must return an integer in the entry wrapper");
      return false;
    }
  } else {
    builder.create<mlir::func::CallOp>(
        loc, user_symbol, mlir::TypeRange{}, mlir::ValueRange{array});
    exit_code = builder.create<mlir::LLVM::ConstantOp>(
        loc, i32, builder.getI32IntegerAttr(0)).getResult();
  }

  builder.create<mlir::func::ReturnOp>(loc, mlir::ValueRange{exit_code});
  return true;
}

}  // namespace

mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics) {

  mlir::OpBuilder builder(&context);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(builder.getUnknownLoc());
  builder.setInsertionPointToStart(module->getBody());

  for (const auto& tir_fn : tir_module.functions()) {
    mlir::Type ret_type;
    if (tir_fn->return_type().is_void()) {
      ret_type = {};
    } else {
      ret_type = tir_type_to_mlir(tir_fn->return_type(), &context, builder);
      if (!ret_type) {
        diagnostics.error("NRQ4005", SourceLocation{},
                          "internal: unsupported function return type in TIR translation");
        return nullptr;
      }
    }

    std::vector<mlir::Type> param_types;
    param_types.reserve(tir_fn->parameters().size());
    for (const auto& param : tir_fn->parameters()) {
      mlir::Type pty = tir_type_to_mlir(param.type, &context, builder);
      if (!pty) {
        diagnostics.error("NRQ4005", SourceLocation{},
                          "internal: unsupported parameter type in TIR translation");
        return nullptr;
      }
      param_types.push_back(pty);
    }

    mlir::FunctionType fn_type;
    if (ret_type) {
      fn_type = mlir::FunctionType::get(&context, param_types, {ret_type});
    } else {
      fn_type = mlir::FunctionType::get(&context, param_types, {});
    }

    // An entry point that takes `string[] args` cannot be the C `main`, so it
    // is emitted under an internal name and reached through the wrapper below.
    const bool is_entry_with_args =
        tir_fn->name() == "main" && !tir_fn->parameters().empty();
    const std::string symbol = is_entry_with_args ? "nrq_entry_args" : tir_fn->name();

    auto func_op = builder.create<mlir::func::FuncOp>(
        builder.getUnknownLoc(), symbol, fn_type);
    if (tir_fn->return_ownership() != ValueOwnership::Trivial)
      func_op->setAttr("nirqoba.return_ownership",
                       builder.getStringAttr(
                           ownership_spelling(tir_fn->return_ownership())));
    if (!tir_fn->parameters().empty()) {
      llvm::SmallVector<mlir::Attribute> argument_attrs;
      argument_attrs.reserve(tir_fn->parameters().size());
      for (const auto& param : tir_fn->parameters()) {
        mlir::NamedAttrList attrs;
        if (param.ownership != ValueOwnership::Trivial)
          attrs.set("nirqoba.ownership",
                    builder.getStringAttr(ownership_spelling(param.ownership)));
        argument_attrs.push_back(attrs.getDictionary(&context));
      }
      func_op.setArgAttrsAttr(builder.getArrayAttr(argument_attrs));
    }

    FunctionTranslator translator(*module, *tir_fn, func_op, builder, &context, diagnostics);
    if (!translator.translate()) return nullptr;

    builder.setInsertionPointAfter(func_op);

    if (is_entry_with_args &&
        !emit_entry_wrapper(*module, builder, *tir_fn, symbol, diagnostics)) {
      return nullptr;
    }
  }

  return module;
}

}  // namespace nirq
