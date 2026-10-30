#include "nirqoba/Common/Type.h"

namespace nirq {

std::string Type::to_string() const {
  switch (kind_) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Int: return "int";
    case TypeKind::String: return "string";
    case TypeKind::Error: return "<error>";
  }
  return "<error>";
}

std::string Type::mangle() const {
  switch (kind_) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "i1";
    case TypeKind::Int: return "i64";
    case TypeKind::String: return "ptr";
    case TypeKind::Error: return "error";
  }
  return "error";
}

Type parse_type_name(std::string_view spelling) {
  if (spelling == "void") return Type::void_type();
  if (spelling == "bool") return Type::bool_type();
  if (spelling == "int") return Type::int_type();
  if (spelling == "string") return Type::string_type();
  return Type::error();
}

}  // namespace nirq
