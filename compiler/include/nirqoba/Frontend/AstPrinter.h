// Stable textual dump of the AST, used by `--emit=ast` and golden tests.
//
// The format is not a language: it is one line per node with fixed indentation
// so a test can compare whole files. Changing it is a deliberate, reviewable
// event because integration goldens depend on it.
#pragma once

#include <string>

#include "nirqoba/Frontend/Ast.h"

namespace nirq {

std::string print_program(const Program& program);

}  // namespace nirq
