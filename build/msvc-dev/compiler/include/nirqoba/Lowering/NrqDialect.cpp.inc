/*===- TableGen'erated file -------------------------------------*- C++ -*-===*\
|*                                                                            *|
|* Dialect Definitions                                                        *|
|*                                                                            *|
|* Automatically generated file, do not edit!                                 *|
|* From: NrqOps.td                                                            *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

MLIR_DEFINE_EXPLICIT_TYPE_ID(::nirq::nrq::NrqDialect)
namespace nirq::nrq {


NrqDialect::NrqDialect(::mlir::MLIRContext *context)
    : ::mlir::Dialect(getDialectNamespace(), context, ::mlir::TypeID::get<NrqDialect>())
    
     {
  
  initialize();
}

NrqDialect::~NrqDialect() = default;


} // namespace nirq::nrq
