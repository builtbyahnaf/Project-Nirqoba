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
    calls into the Nirqoba runtime. Phase 1 defines a single operation,
    `nrq.print_string`, which carries the literal text as an attribute: the
    string table is materialised only when the operation is lowered to LLVM.
  }];
  let useDefaultAttributePrinterParser = 1;
}

class Nrq_Op<string mnemonic> : Op<Nrq_Dialect, mnemonic>;

def Nrq_PrintStringOp : Nrq_Op<"print_string"> {
  let summary = "Write a string literal followed by a newline to standard output";
  let description = [{
    Lowers to a call to `nrq_print_string` in the Nirqoba runtime, which writes
    the bytes and a trailing newline to the standard output handle. The runtime
    call is the ABI boundary established in Phase 1 and the place where Phase 6
    will attach flush/ownership handling.
  }];
  let arguments = (ins StrAttr:$value);
  let assemblyFormat = "$value attr-dict";
}

#endif  // NIRQ_DIALECT_OPS
