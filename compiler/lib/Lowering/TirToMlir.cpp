// TirToMlir.cpp — translates a TIR module into MLIR.
//
// Translation algorithm:
//
//   For each tir::Function:
//     - Create a func.func with i64 return type (or void) and no arguments
//       (Phase 1: no parameters).
//     - For each tir::Op, translate to the appropriate MLIR op:
//
//       ConstantInt  -> llvm.mlir.constant i64
//       ConstantString -> llvm.mlir.addressof + llvm.gep  (constant pool)
//       Alloca       -> llvm.alloca i64  (one element)
//       Store        -> llvm.store
//       Load         -> llvm.load
//       Binary       -> llvm.add / llvm.sub / llvm.mul / llvm.sdiv / llvm.srem
//       PrintString  -> nrq.print_string (attr: the literal text)
//       Return       -> func.return / llvm.return
//
//   The nrq.print_string op carries the literal as an attribute; the
//   NrqToLLVMPass later materialises the global constant and the call.
//   This two-step approach keeps the translation readable and ensures that
//   `--emit=mlir` (before lowering) shows the high-level nrq op.
//
// Value numbering:
//   tir::Value IDs map 1-to-1 to MLIR mlir::Values, stored in a flat vector
//   indexed by id. An invalid tir::Value (id < 0) maps to a null mlir::Value.

#include "nirqoba/Lowering/TirToMlir.h"

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

// ---------------------------------------------------------------------------
// Type mapping
// ---------------------------------------------------------------------------

// Maps a TIR Type to the MLIR type used in func.func bodies.
// Note: string type becomes !llvm.ptr (opaque pointer). The nrq.print_string
// op does not need a pointer value — it carries the literal as an attribute —
// so the string LLVM type is only needed when a string variable is loaded.
static mlir::Type tir_type_to_mlir(const Type& type, mlir::MLIRContext* ctx,
                                    mlir::Builder& builder) {
  if (type.is_int())    return builder.getI64Type();
  if (type.is_bool())   return builder.getI1Type();
  if (type.is_string()) return mlir::LLVM::LLVMPointerType::get(ctx);
  if (type.is_void())   return {};  // caller checks for void specially
  return {};
}

// Returns the LLVM pointer type (for alloca results).
static mlir::Type llvm_ptr_type(mlir::MLIRContext* ctx) {
  return mlir::LLVM::LLVMPointerType::get(ctx);
}

// ---------------------------------------------------------------------------
// Unknown location sentinel
// ---------------------------------------------------------------------------
static mlir::Location to_mlir_loc(const SourceLocation& loc, mlir::MLIRContext* ctx) {
  if (!loc.is_valid()) return mlir::UnknownLoc::get(ctx);
  return mlir::FileLineColLoc::get(ctx,
    llvm::StringRef(loc.file),
    static_cast<unsigned>(loc.line),
    static_cast<unsigned>(loc.column));
}

// ---------------------------------------------------------------------------
// Function translator
// ---------------------------------------------------------------------------

class FunctionTranslator {
 public:
  FunctionTranslator(const tir::Function& tir_fn,
                     mlir::func::FuncOp func_op,
                     mlir::OpBuilder& builder,
                     mlir::MLIRContext* ctx,
                     DiagnosticEngine& diag)
      : tir_fn_(tir_fn), builder_(builder), ctx_(ctx), diag_(diag) {
    mlir::Block* entry = func_op.addEntryBlock();
    builder_.setInsertionPointToStart(entry);
    values_.resize(tir_fn.value_count());
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

  bool translate_op(const tir::Op& op) {
    mlir::Location loc = to_mlir_loc(op.location, ctx_);

    switch (op.kind) {
      case tir::OpKind::ConstantInt: {
        mlir::Type ty = op.result.type.is_bool()
            ? static_cast<mlir::Type>(builder_.getI1Type())
            : static_cast<mlir::Type>(builder_.getI64Type());
        mlir::Value v = builder_.create<mlir::LLVM::ConstantOp>(
            loc, ty,
            builder_.getIntegerAttr(ty, op.integer_value));
        set(op.result, v);
        return true;
      }

      case tir::OpKind::ConstantString: {
        // Phase 1: string literals used as PrintString arguments are carried
        // as attributes in nrq.print_string and never need a pointer value in
        // the function body. If a string is stored into a variable and later
        // loaded, we would need a global + GEP here. Phase 1 rejects that
        // path in the type checker (print of non-literal → error), so this
        // branch produces a null placeholder value. The verifier will catch
        // any incorrect use before we reach code generation.
        //
        // When Phase 2 generalises string handling this branch must be
        // replaced with a global constant + GEP, matching NrqToLLVMPass.
        mlir::Value placeholder = builder_.create<mlir::LLVM::ConstantOp>(
            loc, builder_.getI64Type(),
            builder_.getI64IntegerAttr(0));
        set(op.result, placeholder);
        return true;
      }

      case tir::OpKind::Alloca: {
        // Allocate one element of the slot type on the stack.
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
        if (!ptr || !val) return true;  // earlier error; skip silently
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
        mlir::Value loaded = builder_.create<mlir::LLVM::LoadOp>(
            loc, elem_type, ptr);
        set(op.result, loaded);
        return true;
      }

      case tir::OpKind::Binary: {
        mlir::Value lhs = get(op.lhs);
        mlir::Value rhs = get(op.rhs);
        if (!lhs || !rhs) return true;
        mlir::Value result;
        switch (op.binary_op) {
          case tir::BinaryOp::Add:
            result = builder_.create<mlir::LLVM::AddOp>(loc, lhs, rhs); break;
          case tir::BinaryOp::Subtract:
            result = builder_.create<mlir::LLVM::SubOp>(loc, lhs, rhs); break;
          case tir::BinaryOp::Multiply:
            result = builder_.create<mlir::LLVM::MulOp>(loc, lhs, rhs); break;
          case tir::BinaryOp::Divide:
            result = builder_.create<mlir::LLVM::SDivOp>(loc, lhs, rhs); break;
          case tir::BinaryOp::Remainder:
            result = builder_.create<mlir::LLVM::SRemOp>(loc, lhs, rhs); break;
        }
        set(op.result, result);
        return true;
      }

      case tir::OpKind::PrintString: {
        // Keep the high-level nrq.print_string op; NrqToLLVMPass lowers it.
        builder_.create<nrq::PrintStringOp>(
            loc, builder_.getStringAttr(op.string_value));
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

  const tir::Function& tir_fn_;
  mlir::OpBuilder& builder_;
  mlir::MLIRContext* ctx_;
  DiagnosticEngine& diag_;
  std::vector<mlir::Value> values_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Module translation
// ---------------------------------------------------------------------------

mlir::OwningOpRef<mlir::ModuleOp> translate_tir_to_mlir(
    const tir::Module& tir_module,
    mlir::MLIRContext& context,
    DiagnosticEngine& diagnostics) {

  mlir::OpBuilder builder(&context);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(builder.getUnknownLoc());
  builder.setInsertionPointToStart(module->getBody());

  for (const auto& tir_fn : tir_module.functions()) {
    // Build the func.func type.
    mlir::Type ret_type;
    if (tir_fn->return_type().is_int()) {
      ret_type = builder.getI64Type();
    } else if (tir_fn->return_type().is_void()) {
      ret_type = {};
    } else {
      diagnostics.error("NRQ4005", SourceLocation{},
                        "internal: unsupported function return type in TIR translation");
      return nullptr;
    }

    mlir::FunctionType fn_type;
    if (ret_type) {
      fn_type = mlir::FunctionType::get(&context, {}, {ret_type});
    } else {
      fn_type = mlir::FunctionType::get(&context, {}, {});
    }

    auto func_op = builder.create<mlir::func::FuncOp>(
        builder.getUnknownLoc(), tir_fn->name(), fn_type);

    FunctionTranslator translator(*tir_fn, func_op, builder, &context, diagnostics);
    if (!translator.translate()) return nullptr;

    // Reset insertion point after function for the next function.
    builder.setInsertionPointAfter(func_op);
  }

  return module;
}

}  // namespace nirq
