#include "nirqoba/Common/Type.h"

namespace nirq {

std::string Type::to_string() const {
  switch (kind_) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Int: return "int";
    case TypeKind::I8: return "i8";
    case TypeKind::I16: return "i16";
    case TypeKind::I32: return "i32";
    case TypeKind::I64: return "i64";
    case TypeKind::U8: return "u8";
    case TypeKind::U16: return "u16";
    case TypeKind::U32: return "u32";
    case TypeKind::U64: return "u64";
    case TypeKind::F32: return "f32";
    case TypeKind::F64: return "f64";
    case TypeKind::Char: return "char";
    case TypeKind::String: return "string";
    case TypeKind::Error: return "<error>";
  }
  return "<error>";
}

std::string Type::mangle() const {
  switch (kind_) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "i1";
    case TypeKind::Int:
    case TypeKind::I64: return "i64";
    case TypeKind::I8:
    case TypeKind::U8: return "i8";
    case TypeKind::I16:
    case TypeKind::U16: return "i16";
    case TypeKind::I32:
    case TypeKind::U32:
    case TypeKind::Char: return "i32";
    case TypeKind::U64: return "i64";
    case TypeKind::F32: return "f32";
    case TypeKind::F64: return "f64";
    case TypeKind::String: return "ptr";
    case TypeKind::Error: return "error";
  }
  return "error";
}

Type parse_type_name(std::string_view spelling) {
  if (spelling == "void") return Type::void_type();
  if (spelling == "bool") return Type::bool_type();
  if (spelling == "int") return Type::int_type();
  if (spelling == "i8") return Type::i8_type();
  if (spelling == "i16") return Type::i16_type();
  if (spelling == "i32") return Type::i32_type();
  if (spelling == "i64") return Type::i64_type();
  if (spelling == "u8") return Type::u8_type();
  if (spelling == "u16") return Type::u16_type();
  if (spelling == "u32") return Type::u32_type();
  if (spelling == "u64") return Type::u64_type();
  if (spelling == "byte") return Type::byte_type();
  if (spelling == "f32") return Type::f32_type();
  if (spelling == "f64") return Type::f64_type();
  if (spelling == "char") return Type::char_type();
  if (spelling == "string") return Type::string_type();
  return Type::error();
}

}  // namespace nirq
