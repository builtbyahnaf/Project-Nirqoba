// DataLayout.h — the single description of how Nirqoba values are laid out in
// memory. Both the lowering and the `--emit=layout` report read it, so the
// offsets the compiler generates and the offsets it reports cannot disagree.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nirqoba/Common/Type.h"

namespace nirq {

namespace tir {
class Module;
}

// Byte size and alignment of a value of this type.
uint64_t type_size(const Type& type);
uint64_t type_alignment(const Type& type);

// Nirqoba aggregates are layout-private: the language has no FFI, `sizeof`,
// unions or type punning, so no code the compiler does not lower itself can
// observe a struct's byte layout. Fields are therefore stored widest-alignment
// first (declaration order breaking ties), which removes interior padding and
// keeps wide fields on naturally aligned offsets.
std::vector<std::size_t> aggregate_physical_order(const Type& type);

// Declaration index -> index of the same field in the physical layout.
int aggregate_physical_index(const Type& type, int declaration_index);

// Physical index -> declaration index, i.e. the inverse of the above.
int aggregate_declaration_index(const Type& type, std::size_t physical_index);

// Visits every field of the aggregate in physical layout order.
void for_each_field_in_layout_order(const Type& type,
                                    const std::function<void(const Type&)>& visit);

struct FieldLayout {
  std::string name;
  Type type;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint64_t alignment = 1;
};

struct AggregateLayout {
  std::string name;
  uint64_t size = 0;
  uint64_t alignment = 1;
  uint64_t padding = 0;
  // What the same fields would have cost in declaration order, so the padding
  // eliminated by the physical order is visible in the report.
  uint64_t declaration_size = 0;
  std::vector<FieldLayout> fields;
};

// Layout of a struct or tuple: field offsets and sizes in physical order plus
// the bytes wasted on padding.
AggregateLayout describe_aggregate(const Type& type);

// Human-readable layout report for every aggregate the module mentions,
// including how many elements of each type fit in a 64-byte x86-64 cache line.
std::string print_data_layout(const tir::Module& module);

}  // namespace nirq
