// The Nirqoba type model (Phase 1, 2, 3 & 4).
//
// Extensible value type covering all language scalar and aggregate types:
// - Signed integers: i8, i16, i32, i64, int (= i64)
// - Unsigned integers: u8, u16, u32, u64, byte (= u8)
// - Floating point: f32, f64
// - Boolean: bool
// - Character: char (32-bit Unicode scalar)
// - String: string (UTF-8)
// - Void: void
// - Fixed-size arrays: T[N]
// - Dynamic arrays: T[]
// - Tuples: (T1, T2, ...)
// - Structs: struct Name
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nirq {

enum class TypeKind {
  Error,
  Void,
  Bool,
  Int,
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
  Array,
  DynamicArray,
  Tuple,
  Struct,
};

class Type;

// Payload for aggregate types. Declared here and defined *after* `Type`, because
// it stores a `Type` by value and `Type` is only complete once its own body ends.
// `Type` holds it behind a shared_ptr, so an incomplete type is sufficient there.
struct TypeData;

class Type {
 public:
  Type() = default;
  explicit Type(TypeKind kind) : kind_(kind) {}

  static Type error() { return Type(TypeKind::Error); }
  static Type void_type() { return Type(TypeKind::Void); }
  static Type bool_type() { return Type(TypeKind::Bool); }
  static Type int_type() { return Type(TypeKind::Int); }
  static Type i8_type() { return Type(TypeKind::I8); }
  static Type i16_type() { return Type(TypeKind::I16); }
  static Type i32_type() { return Type(TypeKind::I32); }
  static Type i64_type() { return Type(TypeKind::I64); }
  static Type u8_type() { return Type(TypeKind::U8); }
  static Type u16_type() { return Type(TypeKind::U16); }
  static Type u32_type() { return Type(TypeKind::U32); }
  static Type u64_type() { return Type(TypeKind::U64); }
  static Type byte_type() { return Type(TypeKind::U8); }
  static Type f32_type() { return Type(TypeKind::F32); }
  static Type f64_type() { return Type(TypeKind::F64); }
  static Type char_type() { return Type(TypeKind::Char); }
  static Type string_type() { return Type(TypeKind::String); }

  static Type array_type(Type element_type, std::size_t length);
  static Type dynamic_array_type(Type element_type);
  static Type tuple_type(std::vector<Type> elements);
  static Type struct_type(std::string name, std::vector<std::pair<std::string, Type>> fields = {});

  TypeKind kind() const { return kind_; }

  bool is_valid() const { return kind_ != TypeKind::Error; }
  bool is_void() const { return kind_ == TypeKind::Void; }
  bool is_bool() const { return kind_ == TypeKind::Bool; }
  bool is_string() const { return kind_ == TypeKind::String; }
  bool is_char() const { return kind_ == TypeKind::Char; }
  bool is_f32() const { return kind_ == TypeKind::F32; }
  bool is_f64() const { return kind_ == TypeKind::F64; }
  bool is_array() const { return kind_ == TypeKind::Array; }
  bool is_dynamic_array() const { return kind_ == TypeKind::DynamicArray; }
  bool is_tuple() const { return kind_ == TypeKind::Tuple; }
  bool is_struct() const { return kind_ == TypeKind::Struct; }

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

  Type element_type() const;
  std::size_t array_length() const;
  const std::vector<Type>& tuple_elements() const;
  const std::string& struct_name() const;
  const std::vector<std::pair<std::string, Type>>& struct_fields() const;

  std::string to_string() const;
  std::string mangle() const;

  friend bool operator==(const Type& lhs, const Type& rhs);
  friend bool operator!=(const Type& lhs, const Type& rhs) { return !(lhs == rhs); }

 private:
  TypeKind kind_ = TypeKind::Error;
  std::shared_ptr<TypeData> data_;
};

struct TypeData {
  Type element_type;
  std::size_t length = 0;
  std::vector<Type> tuple_elements;
  std::string struct_name;
  std::vector<std::pair<std::string, Type>> struct_fields;
};

Type parse_type_name(std::string_view spelling);

}  // namespace nirq
