// TableGen definition of the Nirqoba `nrq` dialect.
//
// The dialect holds only operations that have no upstream home. Everything that
// already exists in MLIR (constants, allocation, arithmetic, calls) is emitted
// as LLVM-dialect operations, so this file stays small by design and grows one
// op at a time as later phases need one.
#ifndef NIRQ_DIALECT_OPS
#define NIRQ_DIALECT_OPS

include "mlir/IR/OpBase.td"

def Nrq_Dialect : Dialect {
  let name = "nrq";
  let cppNamespace = "::nirq::nrq";
  let summary = "Operations specific to the Nirqoba language";
  let description = [{
    The `nrq` dialect represents language-level operations that are lowered to
    calls into the Nirqoba runtime.
  }];
  let useDefaultAttributePrinterParser = 1;
}

class Nrq_Op<string mnemonic> : Op<Nrq_Dialect, mnemonic>;

def Nrq_PrintStringOp : Nrq_Op<"print_string"> {
  let summary = "Write a string literal followed by a newline to standard output";
  let description = [{
    Lowers to a call to `nrq_print_string` in the Nirqoba runtime.
  }];
  let arguments = (ins StrAttr:$value);
  let assemblyFormat = "$value attr-dict";
}

def Nrq_PrintIntOp : Nrq_Op<"print_int"> {
  let summary = "Print an i64 integer followed by a newline";
  let arguments = (ins I64:$value);
  let assemblyFormat = "$value attr-dict";
}

def Nrq_PrintUintOp : Nrq_Op<"print_uint"> {
  let summary = "Print an u64 unsigned integer followed by a newline";
  let arguments = (ins I64:$value);
  let assemblyFormat = "$value attr-dict";
}

def Nrq_PrintF64Op : Nrq_Op<"print_f64"> {
  let summary = "Print an f64 float followed by a newline";
  let arguments = (ins F64:$value);
  let assemblyFormat = "$value attr-dict";
}

def Nrq_PrintBoolOp : Nrq_Op<"print_bool"> {
  let summary = "Print a bool followed by a newline";
  let arguments = (ins I1:$value);
  let assemblyFormat = "$value attr-dict";
}

def Nrq_PrintCharOp : Nrq_Op<"print_char"> {
  let summary = "Print a char (unicode code point) followed by a newline";
  let arguments = (ins I32:$value);
  let assemblyFormat = "$value attr-dict";
}

#endif  // NIRQ_DIALECT_OPS
