// The Phase 1 type model.
//
// Deliberately closed and tiny: five kinds, no aliases, no subtyping. Phase 2
// adds the remaining scalar kinds (see project_context/SYNTAX.md 2.1) by
// extending TypeKind and the mapping tables, which is why Type is a value type
// with a public kind rather than a class hierarchy.
#pragma once

#include <string>
#include <string_view>

namespace nirq {

enum class TypeKind {
  Error,   // result of a failed analysis; suppresses follow-on diagnostics
  Void,
  Bool,
  Int,     // permanently 64-bit (SYNTAX 2.1)
  String,  // immutable UTF-8 string literal in Phase 1
};

class Type {
 public:
  constexpr Type() = default;
  constexpr explicit Type(TypeKind kind) : kind_(kind) {}

  static constexpr Type error() { return Type(TypeKind::Error); }
  static constexpr Type void_type() { return Type(TypeKind::Void); }
  static constexpr Type bool_type() { return Type(TypeKind::Bool); }
  static constexpr Type int_type() { return Type(TypeKind::Int); }
  static constexpr Type string_type() { return Type(TypeKind::String); }

  TypeKind kind() const { return kind_; }

  // True when this is not the error type. Callers use it to avoid cascading
  // diagnostics after a type already failed to resolve.
  bool is_valid() const { return kind_ != TypeKind::Error; }
  bool is_void() const { return kind_ == TypeKind::Void; }
  bool is_int() const { return kind_ == TypeKind::Int; }
  bool is_string() const { return kind_ == TypeKind::String; }
  bool is_bool() const { return kind_ == TypeKind::Bool; }

  // Source-level spelling: "int", "string", "bool", "void", "<error>".
  std::string to_string() const;
  // Spelling used inside MLIR types: "i64", "!nrq.string"? Phase 1 uses i64
  // and opaque pointers, so the string type maps to i8* at lowering time.
  std::string mangle() const;

  friend bool operator==(Type lhs, Type rhs) { return lhs.kind_ == rhs.kind_; }
  friend bool operator!=(Type lhs, Type rhs) { return lhs.kind_ != rhs.kind_; }

 private:
  TypeKind kind_ = TypeKind::Error;
};

// Parses a source-level type spelling. Returns Type::error() for an unknown
// spelling; the caller reports the diagnostic so it can carry a location.
Type parse_type_name(std::string_view spelling);

}  // namespace nirq
