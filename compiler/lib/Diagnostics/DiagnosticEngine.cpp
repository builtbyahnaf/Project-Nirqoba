#include "nirqoba/Diagnostics/Diagnostic.h"

namespace nirq {
using namespace std;

string_view severity_name(Severity severity) {
  switch (severity) {
    case Severity::Note:
      return "note";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
    case Severity::Fatal:
      return "fatal";
  }
  return "error";
}

}  // namespace nirq
