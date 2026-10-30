// TirToMlir.cpp — translates a TIR module into MLIR.

#include "nirqoba/Lowering/TirToMlir.h"

#include <unordered_map>
#include <vector>

#include "mlir/Dialect/Func/IR/FuncDialect.h"
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
  if (type.is_void()) return {};
  return {};
}

static mlir::Type llvm_ptr_type(mlir::MLIRContext* ctx) {
  return mlir::LLVM::LLVMPointerType::get(ctx);
}

static mlir::Location to_mlir_loc(const SourceLocation& loc, mlir::MLIRContext* ctx) {
  if (!loc.is_valid()) return mlir::UnknownLoc::get(ctx);
  return mlir::FileLineColLoc::get(ctx,
    llvm::StringRef(loc.file),
    static_cast<unsigned>(loc.line),
    static_cast<unsigned>(loc.column));
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
  }

  bool translate() {
    for (const tir::Op& op : tir_fn_.operations()) {
      if (!translate_op(op)) return false;
    }
    return true;
  }

 private:
  mlir::Value get(tir::Value v) const {
    if (!v.is_valid()) return {};
    return values_[static_cast<std::size_t>(v.id)];
  }

  void set(tir::Value v, mlir::Value mlir_val) {
    if (!v.is_valid()) return;
    values_[static_cast<std::size_t>(v.id)] = mlir_val;
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
            loc, ty, builder_.getIntegerAttr(ty, op.integer_value));
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantFloat: {
        mlir::Type ty = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!ty) ty = builder_.getF64Type();
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty, builder_.getFloatAttr(ty, op.float_value));
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantString: {
        mlir::Value placeholder = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI64Type(), builder_.getI64IntegerAttr(0));
        set(op.result, placeholder);
        return true;
      }

      case tir::OpKind::Alloca: {
        mlir::Type elem_type = tir_type_to_mlir(op.result.type, ctx_, builder_);
        if (!elem_type) {
          diag_.error("NRQ4005", op.location, "internal: alloca of unsupported type");
          return false;
        }
        mlir::Value one = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI64Type(), builder_.getI64IntegerAttr(1));
        mlir::Value slot = builder_.create<mlir::LLVM::AllocaOp>(
            loc, llvm_ptr_type(ctx_), elem_type, one);
        set(op.result, slot);
        return true;
      }

      case tir::OpKind::Store: {
        mlir::Value ptr = get(op.lhs);
        mlir::Value val = get(op.operand);
        if (!ptr || !val) return true;
        builder_.create<mlir::LLVM::StoreOp>(loc, val, ptr);
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
        mlir::Value loaded = builder_.create<mlir::LLVM::LoadOp>(loc, elem_type, ptr);
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
              result = builder_.create<mlir::LLVM::FNegOp>(loc, val);
            } else {
              mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                  loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0));
              result = builder_.create<mlir::LLVM::SubOp>(loc, zero, val);
            }
            break;
          case tir::UnaryOp::Plus:
            result = val;
            break;
          case tir::UnaryOp::LogicalNot: {
            mlir::Value true_val = builder_.create<mlir::LLVM::ConstantOp>(
                loc, builder_.getI1Type(), builder_.getBoolAttr(true));
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, true_val);
            break;
          }
          case tir::UnaryOp::BitwiseNot: {
            mlir::Value all_ones = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), -1));
            result = builder_.create<mlir::LLVM::XOrOp>(loc, val, all_ones);
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
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), lhs)
                : lhs;
            mlir::Value r_f64 = op.rhs.type.is_f32()
                ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), rhs)
                : rhs;
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getF64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_f64, r_f64}).getResult();
            if (lhs_type.is_f32()) {
              result = builder_.create<mlir::LLVM::FPTruncOp>(loc, builder_.getF32Type(), result);
            }
          } else {
            auto pow_fn = ensure_runtime_fn(module_, builder_, "nrq_pow_i64",
                                            builder_.getI64Type(),
                                            {builder_.getI64Type(), builder_.getI64Type()});
            mlir::Value l_i64 = lhs;
            if (lhs_type.bit_width() < 64) {
              l_i64 = lhs_type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), lhs)
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), lhs);
            }
            mlir::Value r_i64 = rhs;
            if (op.rhs.type.bit_width() < 64) {
              r_i64 = op.rhs.type.is_signed_integer()
                  ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), rhs)
                  : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), rhs);
            }
            result = builder_.create<mlir::LLVM::CallOp>(
                loc, mlir::TypeRange{builder_.getI64Type()},
                mlir::FlatSymbolRefAttr::get(ctx_, pow_fn.getSymName()),
                mlir::ValueRange{l_i64, r_i64}).getResult();
            if (lhs_type.bit_width() < 64) {
              mlir::Type orig_ty = tir_type_to_mlir(lhs_type, ctx_, builder_);
              result = builder_.create<mlir::LLVM::TruncOp>(loc, orig_ty, result);
            }
          }
          set(op.result, result);
          return true;
        }

        if (lhs_type.is_float()) {
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = builder_.create<mlir::LLVM::FAddOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Subtract: result = builder_.create<mlir::LLVM::FSubOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Multiply: result = builder_.create<mlir::LLVM::FMulOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Divide: result = builder_.create<mlir::LLVM::FDivOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Remainder: result = builder_.create<mlir::LLVM::FRemOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oeq, lhs, rhs); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::one, lhs, rhs); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::olt, lhs, rhs); break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ole, lhs, rhs); break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::ogt, lhs, rhs); break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::FCmpOp>(loc, mlir::LLVM::FCmpPredicate::oge, lhs, rhs); break;
            default: break;
          }
        } else {
          const bool is_signed = lhs_type.is_signed_integer();
          switch (op.binary_op) {
            case tir::BinaryOp::Add: result = builder_.create<mlir::LLVM::AddOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Subtract: result = builder_.create<mlir::LLVM::SubOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Multiply: result = builder_.create<mlir::LLVM::MulOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::Divide:
              result = is_signed ? builder_.create<mlir::LLVM::SDivOp>(loc, lhs, rhs)
                                 : builder_.create<mlir::LLVM::UDivOp>(loc, lhs, rhs);
              break;
            case tir::BinaryOp::Remainder:
              result = is_signed ? builder_.create<mlir::LLVM::SRemOp>(loc, lhs, rhs)
                                 : builder_.create<mlir::LLVM::URemOp>(loc, lhs, rhs);
              break;
            case tir::BinaryOp::BitwiseAnd: result = builder_.create<mlir::LLVM::AndOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::BitwiseOr: result = builder_.create<mlir::LLVM::OrOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::BitwiseXor: result = builder_.create<mlir::LLVM::XOrOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::ShiftLeft: result = builder_.create<mlir::LLVM::ShlOp>(loc, lhs, rhs); break;
            case tir::BinaryOp::ShiftRight:
              result = is_signed ? builder_.create<mlir::LLVM::AShrOp>(loc, lhs, rhs)
                                 : builder_.create<mlir::LLVM::LShrOp>(loc, lhs, rhs);
              break;
            case tir::BinaryOp::Equal:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::eq, lhs, rhs); break;
            case tir::BinaryOp::NotEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, lhs, rhs); break;
            case tir::BinaryOp::Less:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::slt : mlir::LLVM::ICmpPredicate::ult, lhs, rhs);
              break;
            case tir::BinaryOp::LessEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sle : mlir::LLVM::ICmpPredicate::ule, lhs, rhs);
              break;
            case tir::BinaryOp::Greater:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sgt : mlir::LLVM::ICmpPredicate::ugt, lhs, rhs);
              break;
            case tir::BinaryOp::GreaterEqual:
              result = builder_.create<mlir::LLVM::ICmpOp>(
                  loc, is_signed ? mlir::LLVM::ICmpPredicate::sge : mlir::LLVM::ICmpPredicate::uge, lhs, rhs);
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
            result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val);
            break;
          case tir::CastKind::SExt:
            result = builder_.create<mlir::LLVM::SExtOp>(loc, target_ty, val);
            break;
          case tir::CastKind::ZExt:
          case tir::CastKind::BoolToInt:
          case tir::CastKind::CharToInt:
            if (target_ty.getIntOrFloatBitWidth() < val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val);
            } else if (target_ty.getIntOrFloatBitWidth() > val.getType().getIntOrFloatBitWidth()) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val);
            } else {
              result = val;
            }
            break;
          case tir::CastKind::IntToChar:
            if (val.getType().getIntOrFloatBitWidth() > 32) {
              result = builder_.create<mlir::LLVM::TruncOp>(loc, target_ty, val);
            } else if (val.getType().getIntOrFloatBitWidth() < 32) {
              result = builder_.create<mlir::LLVM::ZExtOp>(loc, target_ty, val);
            } else {
              result = val;
            }
            break;
          case tir::CastKind::SIToFP:
            result = builder_.create<mlir::LLVM::SIToFPOp>(loc, target_ty, val);
            break;
          case tir::CastKind::UIToFP:
            result = builder_.create<mlir::LLVM::UIToFPOp>(loc, target_ty, val);
            break;
          case tir::CastKind::FPToSI:
            result = builder_.create<mlir::LLVM::FPToSIOp>(loc, target_ty, val);
            break;
          case tir::CastKind::FPToUI:
            result = builder_.create<mlir::LLVM::FPToUIOp>(loc, target_ty, val);
            break;
          case tir::CastKind::FPExt:
            result = builder_.create<mlir::LLVM::FPExtOp>(loc, target_ty, val);
            break;
          case tir::CastKind::FPTrunc:
            result = builder_.create<mlir::LLVM::FPTruncOp>(loc, target_ty, val);
            break;
          case tir::CastKind::IntToBool: {
            mlir::Value zero = builder_.create<mlir::LLVM::ConstantOp>(
                loc, val.getType(), builder_.getIntegerAttr(val.getType(), 0));
            result = builder_.create<mlir::LLVM::ICmpOp>(loc, mlir::LLVM::ICmpPredicate::ne, val, zero);
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
        } else if (ty.is_char()) {
          builder_.create<nrq::PrintCharOp>(loc, val);
        } else if (ty.is_float()) {
          mlir::Value f64_val = ty.is_f32()
              ? builder_.create<mlir::LLVM::FPExtOp>(loc, builder_.getF64Type(), val)
              : val;
          builder_.create<nrq::PrintF64Op>(loc, f64_val);
        } else if (ty.is_integer()) {
          mlir::Value i64_val = val;
          if (ty.bit_width() < 64) {
            i64_val = ty.is_signed_integer()
                ? builder_.create<mlir::LLVM::SExtOp>(loc, builder_.getI64Type(), val)
                : builder_.create<mlir::LLVM::ZExtOp>(loc, builder_.getI64Type(), val);
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
  std::vector<mlir::Value> values_;
  std::unordered_map<int, mlir::Block*> label_blocks_;
};

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

    auto func_op = builder.create<mlir::func::FuncOp>(
        builder.getUnknownLoc(), tir_fn->name(), fn_type);

    FunctionTranslator translator(*module, *tir_fn, func_op, builder, &context, diagnostics);
    if (!translator.translate()) return nullptr;

    builder.setInsertionPointAfter(func_op);
  }

  return module;
}

}  // namespace nirq
