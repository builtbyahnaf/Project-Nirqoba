// The Nirqoba scalar type model (Phase 2).
//
// Extensible value type covering all language scalar types:
// - Signed integers: i8, i16, i32, i64, int (= i64)
// - Unsigned integers: u8, u16, u32, u64, byte (= u8)
// - Floating point: f32, f64
// - Boolean: bool
// - Character: char (32-bit Unicode scalar)
// - String: string (UTF-8)
// - Void: void
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nirq {

enum class TypeKind {
  Error,   // result of a failed analysis; suppresses follow-on diagnostics
  Void,
  Bool,
  Int,     // permanently 64-bit signed integer (SYNTAX 2.1)
  I8,
  I16,
  I32,
  I64,
  U8,
  U16,
  U32,
  U64,
  F32,
  F64,
  Char,
  String,
};

class Type {
 public:
  constexpr Type() = default;
  constexpr explicit Type(TypeKind kind) : kind_(kind) {}

  static constexpr Type error() { return Type(TypeKind::Error); }
  static constexpr Type void_type() { return Type(TypeKind::Void); }
  static constexpr Type bool_type() { return Type(TypeKind::Bool); }
  static constexpr Type int_type() { return Type(TypeKind::Int); }
  static constexpr Type i8_type() { return Type(TypeKind::I8); }
  static constexpr Type i16_type() { return Type(TypeKind::I16); }
  static constexpr Type i32_type() { return Type(TypeKind::I32); }
  static constexpr Type i64_type() { return Type(TypeKind::I64); }
  static constexpr Type u8_type() { return Type(TypeKind::U8); }
  static constexpr Type u16_type() { return Type(TypeKind::U16); }
  static constexpr Type u32_type() { return Type(TypeKind::U32); }
  static constexpr Type u64_type() { return Type(TypeKind::U64); }
  static constexpr Type byte_type() { return Type(TypeKind::U8); }
  static constexpr Type f32_type() { return Type(TypeKind::F32); }
  static constexpr Type f64_type() { return Type(TypeKind::F64); }
  static constexpr Type char_type() { return Type(TypeKind::Char); }
  static constexpr Type string_type() { return Type(TypeKind::String); }

  TypeKind kind() const { return kind_; }

  bool is_valid() const { return kind_ != TypeKind::Error; }
  bool is_void() const { return kind_ == TypeKind::Void; }
  bool is_bool() const { return kind_ == TypeKind::Bool; }
  bool is_string() const { return kind_ == TypeKind::String; }
  bool is_char() const { return kind_ == TypeKind::Char; }

  bool is_signed_integer() const {
    return kind_ == TypeKind::Int || kind_ == TypeKind::I8 ||
           kind_ == TypeKind::I16 || kind_ == TypeKind::I32 ||
           kind_ == TypeKind::I64;
  }

  bool is_unsigned_integer() const {
    return kind_ == TypeKind::U8 || kind_ == TypeKind::U16 ||
           kind_ == TypeKind::U32 || kind_ == TypeKind::U64;
  }

  bool is_integer() const {
    return is_signed_integer() || is_unsigned_integer();
  }

  bool is_float() const {
    return kind_ == TypeKind::F32 || kind_ == TypeKind::F64;
  }

  bool is_numeric() const {
    return is_integer() || is_float();
  }

  bool is_int() const {
    return kind_ == TypeKind::Int || kind_ == TypeKind::I64;
  }

  int bit_width() const {
    switch (kind_) {
      case TypeKind::Bool: return 1;
      case TypeKind::I8:
      case TypeKind::U8: return 8;
      case TypeKind::I16:
      case TypeKind::U16: return 16;
      case TypeKind::I32:
      case TypeKind::U32:
      case TypeKind::F32:
      case TypeKind::Char: return 32;
      case TypeKind::Int:
      case TypeKind::I64:
      case TypeKind::U64:
      case TypeKind::F64: return 64;
      default: return 0;
    }
  }

  std::string to_string() const;
  std::string mangle() const;

  friend bool operator==(Type lhs, Type rhs) {
    if (lhs.kind_ == rhs.kind_) return true;
    if ((lhs.kind_ == TypeKind::Int && rhs.kind_ == TypeKind::I64) ||
        (lhs.kind_ == TypeKind::I64 && rhs.kind_ == TypeKind::Int)) {
      return true;
    }
    return false;
  }
  friend bool operator!=(Type lhs, Type rhs) { return !(lhs == rhs); }

 private:
  TypeKind kind_ = TypeKind::Error;
};

Type parse_type_name(std::string_view spelling);

}  // namespace nirq
