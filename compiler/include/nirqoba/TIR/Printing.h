// Stable textual form of TIR, used by `--emit=tir` and golden tests.
//
// One operation per line, `%N = tir.op ...` for value-producing operations and
// `tir.op ...` for the rest. Types are printed in nirq spelling (`i64`, `ptr`).
#pragma once

#include <string>

#include "nirqoba/TIR/IR.h"

namespace nirq {

std::string print_tir(const tir::Module& module);

}  // namespace nirq
