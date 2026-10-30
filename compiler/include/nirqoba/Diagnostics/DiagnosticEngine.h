// Collects diagnostics produced by any compiler stage.
//
// The engine owns no source text; rendering needs a SourceFile, which the
// renderer receives explicitly. Thread-safety: not thread-safe by design, one
// engine per compilation, which is stated in the phase report.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "nirqoba/Diagnostics/Diagnostic.h"

namespace nirq {

class DiagnosticEngine {
 public:
  void report(Severity severity, std::string code, SourceLocation location, std::string message) {
    diagnostics_.push_back(Diagnostic{severity, std::move(code), std::move(location), std::move(message)});
  }

  void error(std::string code, SourceLocation location, std::string message) {
    report(Severity::Error, std::move(code), std::move(location), std::move(message));
  }

  void note(std::string code, SourceLocation location, std::string message) {
    report(Severity::Note, std::move(code), std::move(location), std::move(message));
  }

  // True when at least one error or fatal diagnostic was recorded. Warnings and
  // notes never stop compilation in Phase 1.
  bool has_errors() const {
    for (const Diagnostic& diagnostic : diagnostics_) {
      if (diagnostic.is_error()) return true;
    }
    return false;
  }

  std::size_t error_count() const {
    std::size_t count = 0;
    for (const Diagnostic& diagnostic : diagnostics_) {
      if (diagnostic.is_error()) ++count;
    }
    return count;
  }

  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

 private:
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace nirq
