#include "nirqoba/Common/Type.h"

#include <algorithm>
#include <unordered_set>

namespace nirq {

bool is_owning_ownership(ValueOwnership ownership) {
  // ArenaBound storage is owned by its arena, not by the individual binding.
  // It therefore creates no per-value drop obligation; reset/destroy releases
  // the whole region in bulk.
  return ownership == ValueOwnership::Owned ||
         ownership == ValueOwnership::Manual ||
         ownership == ValueOwnership::Shared;
}

bool is_borrowing_ownership(ValueOwnership ownership) {
  return ownership == ValueOwnership::Borrowed ||
         ownership == ValueOwnership::MutableBorrowed ||
         ownership == ValueOwnership::Weak;
}

bool is_transferable_ownership(ValueOwnership ownership) {
  return ownership == ValueOwnership::Owned ||
         ownership == ValueOwnership::Manual ||
         ownership == ValueOwnership::Shared;
}

bool requires_destruction(ValueOwnership ownership) {
  return ownership == ValueOwnership::Owned ||
         ownership == ValueOwnership::Manual ||
         ownership == ValueOwnership::Shared;
}

std::string_view ownership_name(ValueOwnership ownership) {
  switch (ownership) {
    case ValueOwnership::Trivial: return "trivial";
    case ValueOwnership::Owned: return "owned";
    case ValueOwnership::Manual: return "manual";
    case ValueOwnership::Borrowed: return "borrowed";
    case ValueOwnership::MutableBorrowed: return "mutable_borrowed";
    case ValueOwnership::Shared: return "shared";
    case ValueOwnership::GcManaged: return "gc_managed";
    case ValueOwnership::Raw: return "raw";
    case ValueOwnership::ArenaBound: return "arena_bound";
    case ValueOwnership::Weak: return "weak";
  }
  return "unknown";
}
using namespace std;

namespace {

bool arena_eligible_payload(const Type& type,
                            std::unordered_set<std::string>& active) {
  if (!type.is_valid()) return false;
  // Scalar payloads have no ownership: nothing to trace, nothing to destroy.
  if (type.is_integer() || type.is_float() || type.is_bool() || type.is_char())
    return true;
  if (type.is_array())
    return type.array_length() > 0 &&
           arena_eligible_payload(type.element_type(), active);
  if (type.is_tuple()) {
    const vector<Type>& elements = type.tuple_elements();
    if (elements.empty()) return false;
    for (const Type& element : elements)
      if (!arena_eligible_payload(element, active)) return false;
    return true;
  }
  if (type.is_struct()) {
    // A struct that names itself through a pointer is the node in progress, and it
    // is eligible exactly when the rest of it is: assume it while its own fields
    // are visited instead of recursing forever. A named reference reached this way
    // carries no field list of its own, so this check has to come first.
    if (!active.insert(type.struct_name()).second) return true;
    const vector<pair<string, Type>>& fields = type.struct_fields();
    if (fields.empty()) {
      active.erase(type.struct_name());
      return false;
    }
    bool eligible = true;
    for (const pair<string, Type>& field : fields)
      if (!arena_eligible_payload(field.second, active)) {
        eligible = false;
        break;
      }
    active.erase(type.struct_name());
    return eligible;
  }
  if (type.is_managed_pointer()) return false;
  if (type.is_pointer()) {
    // Only storage the compiler frees. A shared token is reference counted, and a
    // resource handle names a runtime destructor: a file handle is the one domain
    // the teardown registry can name, so it may travel through a chunk and the
    // rest stay out until the registry grows a kind for them.
    if (type.is_shared_pointer()) return false;
    if (type.is_resource_pointer()) return type.resource_domain() == "file";
    return arena_eligible_payload(type.element_type(), active);
  }
  // Owned text, a dynamic container and an owned array own storage, but a release
  // can be named for them: the arena records a field teardown when it allocates,
  // so the chunk releases each field's storage before it releases the chunk.
  // Array views and SoA containers still cannot be described that way and stay
  // out of the arena.
  if (type.is_owned_string() || type.is_dynamic_array() ||
      type.is_owned_array())
    return true;
  return false;
}

}  // namespace

bool arena_eligible_payload(const Type& type) {
  std::unordered_set<std::string> active;
  return arena_eligible_payload(type, active);
}

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

Type Type::soa_type(Type element_type) {
  Type t(TypeKind::Soa);
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

Type Type::owned_pointer_type(Type element_type) {
  Type t(TypeKind::OwnedPointer);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::owned_pointer_type(Type element_type, std::string resource_domain) {
  Type t = owned_pointer_type(element_type);
  t.data_->resource_domain = std::move(resource_domain);
  return t;
}

bool Type::is_resource_pointer() const {
  return kind_ == TypeKind::OwnedPointer && data_ && !data_->resource_domain.empty();
}

const std::string& Type::resource_domain() const {
  static const std::string empty;
  return data_ ? data_->resource_domain : empty;
}

Type Type::shared_pointer_type(Type element_type) {
  Type t(TypeKind::SharedPointer);
  t.data_ = make_shared<TypeData>();
  t.data_->element_type = element_type;
  return t;
}

Type Type::array_view_type(Type element_type) {
  Type t(TypeKind::ArrayView);
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
    case TypeKind::OwnedString: return "string_owned";
    case TypeKind::Array:
      return element_type().to_string() + "[" + std::to_string(array_length()) + "]";
    case TypeKind::DynamicArray:
      return element_type().to_string() + "[]";
    case TypeKind::OwnedArray:
      return "owned_array<" + element_type().to_string() + ">";
    case TypeKind::Soa:
      return "soa<" + element_type().to_string() + ">";
    case TypeKind::OwnedPointer:
      if (!resource_domain().empty())
        return "owned " + resource_domain() + " ptr " +
               element_type().to_string();
      return "owned_ptr<" + element_type().to_string() + ">";
    case TypeKind::SharedPointer:
      return "shared_ptr<" + element_type().to_string() + ">";
    case TypeKind::ArrayView:
      return "array_view<" + element_type().to_string() + ">";
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
    case TypeKind::String:
    case TypeKind::OwnedString: return "ptr";
    case TypeKind::Array:
      return "A" + std::to_string(array_length()) + "_" + element_type().mangle();
    case TypeKind::DynamicArray:
      return "DA_" + element_type().mangle();
    case TypeKind::OwnedArray:
      return "OA_" + element_type().mangle();
    case TypeKind::Soa:
      return "SOA_" + element_type().mangle();
    case TypeKind::ArrayView:
      return "AV_" + element_type().mangle();
    case TypeKind::Pointer:
      return "P_" + element_type().mangle();
    case TypeKind::OwnedPointer:
      if (!resource_domain().empty())
        return "OPD" + resource_domain() + "_" + element_type().mangle();
      return "OP_" + element_type().mangle();
    case TypeKind::SharedPointer:
      return "SP_" + element_type().mangle();
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
    case TypeKind::SharedPointer:
    case TypeKind::ArrayView:
      return lhs.element_type() == rhs.element_type();
    case TypeKind::OwnedPointer:
      return lhs.element_type() == rhs.element_type() &&
             lhs.resource_domain() == rhs.resource_domain();
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
  if (spelling == "string_owned") return Type::owned_string_type();
  return Type::error();
}



namespace {

// An edge is owning when the field keeps its target alive. A deterministic owner
// releases its target once; a shared pointer keeps a count that includes itself,
// so a cycle of shared owners never reaches zero - both are owning edges here.
bool owns_its_element(const Type& field) {
  switch (field.kind()) {
    case TypeKind::OwnedPointer:
    case TypeKind::SharedPointer:
    // A container owns its buffer, so its elements are reached by ownership.
    case TypeKind::OwnedArray:
    case TypeKind::DynamicArray:
      return true;
    // Everything else either does not own (borrow, raw pointer, array view, weak)
    // or hands the cycle to the collector (managed pointer).
    default:
      return false;
  }
}

// The fields of `type`'s struct: the table is authoritative when it knows the
// name, and the type's own copy answers for a struct the table never saw.
const std::vector<std::pair<std::string, Type>>* declared_fields(
    const Type& type, const StructFieldTable& table) {
  const auto found = table.find(type.struct_name());
  if (found != table.end() && !found->second.empty()) return &found->second;
  return type.struct_fields().empty() ? nullptr : &type.struct_fields();
}

bool field_closes_a_cycle(const Type& field, const StructFieldTable& table,
                          std::vector<std::string>& path);

// Walks one aggregate's fields, reporting whether any of them leads back to a
// struct already on the path. The path is what bounds the walk: a struct is
// entered once, so a type graph no program could build (a struct holding itself
// by value) answers instead of recursing forever.
bool aggregate_closes_a_cycle(const Type& type, const StructFieldTable& table,
                              std::vector<std::string>& path) {
  if (!type.is_struct()) return false;
  const std::string& name = type.struct_name();
  if (name.empty()) return false;
  if (std::find(path.begin(), path.end(), name) != path.end()) return true;
  const auto* fields = declared_fields(type, table);
  if (fields == nullptr) return false;
  path.push_back(name);
  for (const auto& field : *fields) {
    if (!field_closes_a_cycle(field.second, table, path)) continue;
    path.pop_back();
    return true;
  }
  path.pop_back();
  return false;
}

bool field_closes_a_cycle(const Type& field, const StructFieldTable& table,
                          std::vector<std::string>& path) {
  // A struct stored inline is part of the same allocation, so walking it is
  // walking the parent's own fields.
  if (field.is_struct()) return aggregate_closes_a_cycle(field, table, path);
  if (!owns_its_element(field)) return false;
  const Type element = field.element_type();
  return element.is_valid() && field_closes_a_cycle(element, table, path);
}

}  // namespace

bool is_structurally_cyclic(const Type& type, const StructFieldTable& table) {
  std::vector<std::string> path;
  return field_closes_a_cycle(type, table, path);
}

}  // namespace nirq
