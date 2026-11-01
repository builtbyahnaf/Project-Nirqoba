#include "nirqoba/Common/Type.h"

namespace nirq {
using namespace std;

namespace {
static const vector<Type> kEmptyTypes;
static const string kEmptyString;
static const vector<pair<string, Type>> kEmptyFields;
}  // namespace

Type Type::array_type(Type element_type, size_t length) {
  Type t(TypeKind::Array);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  t.data_->length = length;
  return t;
}

Type Type::dynamic_array_type(Type element_type) {
  Type t(TypeKind::DynamicArray);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::owned_array_type(Type element_type) {
  Type t(TypeKind::OwnedArray);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::pointer_type(Type element_type) {
  Type t(TypeKind::Pointer);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::managed_pointer_type(Type element_type) {
  Type t(TypeKind::ManagedPointer);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::tuple_type(vector<Type> elements) {
  Type t(TypeKind::Tuple);
  t.data_ = make_shared<TypeData>();
  t.data_->tuple_elements = move(elements);
  return t;
}

Type Type::struct_type(string name, vector<pair<string, Type>> fields) {
  Type t(TypeKind::Struct);
  t.data_ = make_shared<TypeData>();
  t.data_->struct_name = move(name);
  t.data_->struct_fields = move(fields);
  return t;
}

Type Type::element_type() const {
  return data_ ? data_->element_type : Type::error();
}

size_t Type::array_length() const {
  return data_ ? data_->length : 0;
}

const vector<Type>& Type::tuple_elements() const {
  return data_ ? data_->tuple_elements : kEmptyTypes;
}

const string& Type::struct_name() const {
  return data_ ? data_->struct_name : kEmptyString;
}

const vector<pair<string, Type>>& Type::struct_fields() const {
  return data_ ? data_->struct_fields : kEmptyFields;
}

string Type::to_string() const {
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
    case TypeKind::Array:
      return element_type().to_string() + "[" + std::to_string(array_length()) + "]";
    case TypeKind::DynamicArray:
      return element_type().to_string() + "[]";
    case TypeKind::OwnedArray:
      return "owned_array<" + element_type().to_string() + ">";
    case TypeKind::Pointer:
      return element_type().to_string() + "*";
    case TypeKind::ManagedPointer:
      return "managed<" + element_type().to_string() + ">";
    case TypeKind::Tuple: {
      string s = "(";
      const auto& elems = tuple_elements();
      for (size_t i = 0; i < elems.size(); ++i) {
        if (i > 0) s += ", ";
        s += elems[i].to_string();
      }
      s += ")";
      return s;
    }
    case TypeKind::Struct:
      return struct_name();
    case TypeKind::Error: return "<error>";
  }
  return "<error>";
}

string Type::mangle() const {
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
    case TypeKind::Array:
      return "A" + std::to_string(array_length()) + "_" + element_type().mangle();
    case TypeKind::DynamicArray:
      return "DA_" + element_type().mangle();
    case TypeKind::OwnedArray:
      return "OA_" + element_type().mangle();
    case TypeKind::Pointer:
      return "P_" + element_type().mangle();
    case TypeKind::ManagedPointer:
      return "MP_" + element_type().mangle();
    case TypeKind::Tuple: {
      string s = "T" + std::to_string(tuple_elements().size());
      for (const auto& elem : tuple_elements()) {
        s += "_" + elem.mangle();
      }
      return s;
    }
    case TypeKind::Struct:
      return "S" + std::to_string(struct_name().size()) + struct_name();
    case TypeKind::Error: return "error";
  }
  return "error";
}

bool operator==(const Type& lhs, const Type& rhs) {
  if (lhs.kind_ != rhs.kind_) {
    if ((lhs.kind_ == TypeKind::Int && rhs.kind_ == TypeKind::I64) ||
        (lhs.kind_ == TypeKind::I64 && rhs.kind_ == TypeKind::Int)) {
      return true;
    }
    return false;
  }

  switch (lhs.kind_) {
    case TypeKind::Array:
      return lhs.array_length() == rhs.array_length() &&
             lhs.element_type() == rhs.element_type();
    case TypeKind::DynamicArray:
    case TypeKind::OwnedArray:
      return lhs.element_type() == rhs.element_type();
    case TypeKind::Pointer:
      return lhs.element_type() == rhs.element_type();
    case TypeKind::ManagedPointer:
      return lhs.element_type() == rhs.element_type();
    case TypeKind::Tuple: {
      const auto& l = lhs.tuple_elements();
      const auto& r = rhs.tuple_elements();
      if (l.size() != r.size()) return false;
      for (size_t i = 0; i < l.size(); ++i) {
        if (l[i] != r[i]) return false;
      }
      return true;
    }
    case TypeKind::Struct:
      return lhs.struct_name() == rhs.struct_name();
    default:
      return true;
  }
}

Type parse_type_name(string_view spelling) {
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
