// Diagnostic value type shared by every compiler stage.
#pragma once

#include <string>
#include <string_view>

#include "nirqoba/Diagnostics/SourceLocation.h"

namespace nirq {

enum class Severity { Note, Warning, Error, Fatal };

std::string_view severity_name(Severity severity);

namespace diag {
inline constexpr const char* kUnexpectedCharacter = "NRQ1008";
inline constexpr const char* kUnterminatedString = "NRQ1007";
inline constexpr const char* kUnknownEscape = "NRQ1006";
inline constexpr const char* kEmptySource = "NRQ1005";
inline constexpr const char* kInvalidFloatLiteral = "NRQ1010";
inline constexpr const char* kUnterminatedChar = "NRQ1011";
inline constexpr const char* kNotImplementedInPhase1 = "NRQ2100";
inline constexpr const char* kUnexpectedToken = "NRQ2001";
inline constexpr const char* kExpectedToken = "NRQ2002";
inline constexpr const char* kMissingMain = "NRQ3001";
inline constexpr const char* kDuplicateMain = "NRQ3002";
inline constexpr const char* kInvalidMainReturn = "NRQ3003";
inline constexpr const char* kMainParameters = "NRQ3004";
inline constexpr const char* kVoidReturnsValue = "NRQ3005";
inline constexpr const char* kMissingReturn = "NRQ3006";
inline constexpr const char* kPrintRequiresString = "NRQ3010";
inline constexpr const char* kUndefinedName = "NRQ3011";
inline constexpr const char* kDuplicateName = "NRQ3012";
inline constexpr const char* kNotCallable = "NRQ3013";
inline constexpr const char* kImmutableAssignment = "NRQ3014";
inline constexpr const char* kTypeMismatch = "NRQ3015";
inline constexpr const char* kDuplicateFunction = "NRQ3016";
inline constexpr const char* kReturnValueRequired = "NRQ3017";
inline constexpr const char* kPrintArity = "NRQ3018";
inline constexpr const char* kConstantInitializer = "NRQ3019";
inline constexpr const char* kUseBeforeInit = "NRQ3020";
inline constexpr const char* kConstDivisionByZero = "NRQ3021";
inline constexpr const char* kBreakOutsideLoop = "NRQ3022";
inline constexpr const char* kContinueOutsideLoop = "NRQ3023";
  inline constexpr const char* kUnknownField = "NRQ3025";
  inline constexpr const char* kIndexOutOfBounds = "NRQ3026";
  inline constexpr const char* kDestructuringMismatch = "NRQ3027";
inline constexpr const char* kModuleNotFound = "NRQ3028";
inline constexpr const char* kCyclicDependency = "NRQ3029";
inline constexpr const char* kPrivateAccessViolation = "NRQ3030";
inline constexpr const char* kDuplicateModule = "NRQ3031";
inline constexpr const char* kImportConflict = "NRQ3032";
inline constexpr const char* kMissingSourceFile = "NRQ5001";
inline constexpr const char* kInvalidCommandLine = "NRQ5002";
inline constexpr const char* kToolchainUnavailable = "NRQ5003";
inline constexpr const char* kInternalError = "NRQ5004";
inline constexpr const char* kLinkFailed = "NRQ5005";
inline constexpr const char* kProgramFailedToStart = "NRQ5006";
inline constexpr const char* kInvalidSourceExtension = "NRQ5007";
inline constexpr const char* kProjectScaffoldFailed = "NRQ5008";
}  // namespace diag

struct Diagnostic {
  Severity severity = Severity::Error;
  std::string code;
  SourceLocation location;
  std::string message;

  bool is_error() const {
    return severity == Severity::Error || severity == Severity::Fatal;
  }
};

}  // namespace nirq

