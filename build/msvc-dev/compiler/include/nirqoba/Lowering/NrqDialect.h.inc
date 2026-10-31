/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* Dialect Declarations                                                       *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|* From: NrqOps.td                                                            *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

namespace nirq::nrq {

/// Operations specific to the Nirqoba language
/// The `nrq` dialect represents language-level operations that are lowered to
/// calls into the Nirqoba runtime.
class NrqDialect : public ::mlir::Dialect {
  explicit NrqDialect(::mlir::MLIRContext *context);

  void initialize();
  friend class ::mlir::MLIRContext;
public:
  ~NrqDialect() override;
  static constexpr ::llvm::StringLiteral getDialectNamespace() {
    return ::llvm::StringLiteral("nrq");
  }
};

} // namespace nirq::nrq
MLIR_DECLARE_EXPLICIT_TYPE_ID(::nirq::nrq::NrqDialect)
