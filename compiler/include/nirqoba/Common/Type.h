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
#include <unordered_map>
#include <utility>
#include <vector>

namespace nirq {

// Ownership category of a value, parameter or return.
//
// The category is part of the semantic model, not a Sema-only annotation: TIR
// carries it on every value record so lowering and verification can agree on
// who is responsible for a value's lifetime. `Trivial` is used for values with
// no ownership semantics (integers, booleans, ordinary struct fields of such
// types); the remaining categories are the ones the migration plan requires.
enum class ValueOwnership {
  Trivial,
  Owned,             // deterministic owner: this value must be destroyed once
  Manual,            // explicit manual owner released through manual_release
  Borrowed,          // immutable non-owning access, lifetime-bound to its source
  MutableBorrowed,   // exclusive non-owning access, lifetime-bound to its source
  Shared,            // explicit reference-counted owner
  GcManaged,         // collector-managed reference; the collector owns the lifetime
  Raw,               // raw/manual pointer with no compiler-managed lifetime
  ArenaBound,        // lifetime owned by an enclosing arena (Phase 5)
  Weak,              // non-owning weak reference (Phase 6)
};

// True for categories that carry a destruction or release obligation.
// `Manual` is owning: its release is explicit, with a compiler-scheduled
// fallback, and it never converts to `Owned`. `GcManaged` is excluded
// deliberately: the collector, not lexical cleanup, is responsible for
// reclaiming managed objects.
bool is_owning_ownership(ValueOwnership ownership);

// True for non-owning access categories, including weak references.
bool is_borrowing_ownership(ValueOwnership ownership);

// True when the category may be moved to a new owner by an ordinary transfer.
// Borrowed, raw and GC-managed values cannot be transferred as owned values.
bool is_transferable_ownership(ValueOwnership ownership);

// True when the category requires an explicit release/destruction operation to
// discharge its obligation. Shared values are released through their control
// block; arena-bound values are released with their arena.
bool requires_destruction(ValueOwnership ownership);

// Stable lowercase spelling used by TIR printing, MLIR attributes and tests.
std::string_view ownership_name(ValueOwnership ownership);

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
  // A string whose buffer the language owns and frees. It has the same runtime
  // representation as `String` (one pointer, mangled `ptr`), which is why it is a
  // distinct kind rather than a distinct ABI: the kind is what lets the checker
  // attach a destructor and the verifier accept Owned ownership without granting
  // either to a borrowed string.
  OwnedString,
  Array,
  DynamicArray,
  OwnedArray,
  OwnedPointer,
  SharedPointer,
  ArrayView,
  Pointer,
  ManagedPointer,
  Tuple,
  Struct,
  // Structure of arrays over a struct type: the field set is the struct's, and
  // the payload is one contiguous column per field. Distinct from OwnedArray so
  // no existing path can mistake one for the other.
  Soa,
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
  static Type owned_string_type() { return Type(TypeKind::OwnedString); }

  static Type array_type(Type element_type, std::size_t length);
  static Type dynamic_array_type(Type element_type);
  static Type owned_array_type(Type element_type);
  static Type owned_pointer_type(Type element_type);
  static Type owned_pointer_type(Type element_type, std::string resource_domain);
  static Type shared_pointer_type(Type element_type);
  static Type array_view_type(Type element_type);
  static Type pointer_type(Type element_type);
  static Type managed_pointer_type(Type element_type);
  static Type tuple_type(std::vector<Type> elements);
  static Type struct_type(std::string name, std::vector<std::pair<std::string, Type>> fields = {});
  static Type soa_type(Type element_type);

  TypeKind kind() const { return kind_; }

  bool is_valid() const { return kind_ != TypeKind::Error; }

  // Physical footprint of a value of this type, and the alignment it needs.
  // Both answer 0 for a type whose layout is not fixed (aggregates with fields,
  // tuples), so a caller that needs a size cannot silently act on a guess: the
  // dynamic array initializer, for instance, must refuse to emit
  // nrq_array_create with a zero element size rather than allocate nothing.
  //
  // dynamic_array<T> is {ptr buffer, i64 length, i64 capacity}, so its size is
  // three words regardless of T; T only decides the element size passed to the
  // allocator, which is why element_type() is queried separately.
  std::uint32_t byte_size() const {
    switch (kind_) {
      case TypeKind::Bool:
      case TypeKind::I8:
      case TypeKind::U8:
      case TypeKind::Char:
        return 1;
      case TypeKind::I16:
      case TypeKind::U16:
        return 2;
      case TypeKind::I32:
      case TypeKind::U32:
      case TypeKind::F32:
        return 4;
      // The integer default is 64-bit, which is what the type mangler already
      // records (`int` prints as DA_i64 inside an array element).
      case TypeKind::Int:
      case TypeKind::I64:
      case TypeKind::U64:
      case TypeKind::F64:
      case TypeKind::String:
      case TypeKind::OwnedString:
      case TypeKind::Pointer:
      case TypeKind::OwnedPointer:
      case TypeKind::SharedPointer:
      case TypeKind::ManagedPointer:
        return 8;
      case TypeKind::DynamicArray:
        return 24;
      default:
        return 0;
    }
  }

  std::uint8_t alignment() const {
    const std::uint32_t size = byte_size();
    if (size == 0) return 0;
    return static_cast<std::uint8_t>(size < 8 ? size : 8);
  }
  bool is_void() const { return kind_ == TypeKind::Void; }
  bool is_bool() const { return kind_ == TypeKind::Bool; }
  // An owned string is a string at every use site: it can be read, indexed,
  // compared and passed wherever text is expected. Only ownership and cleanup
  // ask the more specific question below.
  bool is_string() const {
    return kind_ == TypeKind::String || kind_ == TypeKind::OwnedString;
  }
  bool is_owned_string() const { return kind_ == TypeKind::OwnedString; }
  bool is_char() const { return kind_ == TypeKind::Char; }
  bool is_f32() const { return kind_ == TypeKind::F32; }
  bool is_f64() const { return kind_ == TypeKind::F64; }
  bool is_u64() const { return kind_ == TypeKind::U64; }
  bool is_array() const { return kind_ == TypeKind::Array; }
  bool is_dynamic_array() const { return kind_ == TypeKind::DynamicArray; }
  bool is_owned_array() const { return kind_ == TypeKind::OwnedArray; }
  bool is_owned_pointer() const { return kind_ == TypeKind::OwnedPointer; }
  bool is_shared_pointer() const { return kind_ == TypeKind::SharedPointer; }
  bool is_array_view() const { return kind_ == TypeKind::ArrayView; }
  bool is_pointer() const {
    return kind_ == TypeKind::Pointer || kind_ == TypeKind::OwnedPointer ||
           kind_ == TypeKind::SharedPointer;
  }
  bool is_managed_pointer() const { return kind_ == TypeKind::ManagedPointer; }
  // An owned pointer that names the runtime resource domain responsible for
  // releasing it (file, socket, mutex, mapping or secure). The domain travels
  // with the type so construction, moves and destruction all see it.
  bool is_resource_pointer() const;
  const std::string& resource_domain() const;
  bool is_tuple() const { return kind_ == TypeKind::Tuple; }
  bool is_struct() const { return kind_ == TypeKind::Struct; }
  bool is_soa() const { return kind_ == TypeKind::Soa; }
  // The struct whose field set this SoA presents.
  Type soa_element() const { return is_soa() ? element_type() : Type::error(); }

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
  std::string resource_domain;
};

Type parse_type_name(std::string_view spelling);

// True when storage of this type can live in an arena: every leaf is plain data
// and every pointer names another payload of the same kind, so a bulk release
// needs no destructor and no collector root. A recursive struct is eligible -
// its own pointer fields are the reason a linked list or a tree can be built in
// one chunk - while owned text, containers, shared and managed references and
// resource handles all need a release an arena cannot describe.
bool arena_eligible_payload(const Type& type);

// The declared fields of every struct a program names, keyed by the struct's
// name. A struct value carries its own fields, but a *field* that names another
// struct keeps nothing but that name, so an analysis that walks a type graph by
// name needs this catalog to descend through it.
using StructFieldTable =
    std::unordered_map<std::string, std::vector<std::pair<std::string, Type>>>;

// True when `type`'s aggregate can close a cycle that deterministic release
// cannot break: a loop of owning edges with a reference count somewhere on it.
// A loop of unique owners is not a cycle - the closing edge would give the object
// a second owner, which no well-formed program can write - so a tree of
// `owned ptr` fields stays a tree. A borrow, a weak reference, a raw pointer and
// an array view do not keep their target alive at all, and a collector-managed
// pointer leaves the cycle to the collector.
//
// This is a property of the type, not of a program's runtime graph: it says the
// shape *allows* a cycle, which is the strongest statement the type alone can
// make before the program runs. Detecting that a particular graph does close one
// is a flow-sensitive question this predicate deliberately does not answer.
bool is_structurally_cyclic(const Type& type, const StructFieldTable& table);

}  // namespace nirq
