// DataLayout.cpp — how Nirqoba values are laid out in memory, shared by the
// lowering and the `--emit=layout` report.

#include "nirqoba/Lowering/DataLayout.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

#include "nirqoba/TIR/IR.h"

namespace nirq {

namespace {

// Collects every struct type reachable from `type`, nested ones included.
void collect_struct_types(const Type& type, std::vector<Type>& found,
                          std::set<std::string>& seen) {
  if (!type.is_valid()) return;
  if (type.is_array()) {
    collect_struct_types(type.element_type(), found, seen);
    return;
  }
  if (type.is_struct()) {
    if (seen.insert(type.struct_name()).second) found.push_back(type);
    for (const auto& field : type.struct_fields()) {
      collect_struct_types(field.second, found, seen);
    }
    return;
  }
  if (type.is_tuple()) {
    for (const Type& field : type.tuple_elements()) {
      collect_struct_types(field, found, seen);
    }
  }
}

}  // namespace

uint64_t type_alignment(const Type& type) {
  if (type.is_pointer() || type.is_managed_pointer() || type.is_string() ||
      type.is_dynamic_array() || type.is_owned_array() || type.is_array_view()) return 8;
  if (type.is_array()) return type_alignment(type.element_type());
  if (type.is_struct() || type.is_tuple()) {
    uint64_t alignment = 1;
    if (type.is_struct()) {
      for (const auto& field : type.struct_fields())
        alignment = std::max(alignment, type_alignment(field.second));
    } else {
      for (const Type& field : type.tuple_elements())
        alignment = std::max(alignment, type_alignment(field));
    }
    return alignment;
  }
  if (type.is_float()) return type.is_f32() ? 4 : 8;
  if (type.is_integer() || type.is_char())
    return std::max<uint64_t>(1, std::min<uint64_t>(8, type.bit_width() / 8));
  return 1;
}

std::vector<std::size_t> aggregate_physical_order(const Type& type) {
  std::vector<std::size_t> order;
  std::vector<uint64_t> alignments;
  if (type.is_struct()) {
    for (const auto& field : type.struct_fields()) {
      alignments.push_back(type_alignment(field.second));
    }
  } else if (type.is_tuple()) {
    for (const Type& field : type.tuple_elements()) {
      alignments.push_back(type_alignment(field));
    }
  } else {
    return order;
  }
  order.resize(alignments.size());
  for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
    return alignments[lhs] > alignments[rhs];
  });
  return order;
}

int aggregate_physical_index(const Type& type, int declaration_index) {
  if (declaration_index < 0) return declaration_index;
  const std::vector<std::size_t> order = aggregate_physical_order(type);
  for (std::size_t physical = 0; physical < order.size(); ++physical) {
    if (order[physical] == static_cast<std::size_t>(declaration_index)) {
      return static_cast<int>(physical);
    }
  }
  return declaration_index;
}

int aggregate_declaration_index(const Type& type, std::size_t physical_index) {
  const std::vector<std::size_t> order = aggregate_physical_order(type);
  if (physical_index >= order.size()) return static_cast<int>(physical_index);
  return static_cast<int>(order[physical_index]);
}

void for_each_field_in_layout_order(const Type& type,
                                    const std::function<void(const Type&)>& visit) {
  const std::vector<std::size_t> order = aggregate_physical_order(type);
  for (std::size_t physical : order) {
    if (type.is_struct()) {
      visit(type.struct_fields()[physical].second);
    } else {
      visit(type.tuple_elements()[physical]);
    }
  }
}

uint64_t type_size(const Type& type) {
  if (type.is_pointer() || type.is_managed_pointer() || type.is_string() ||
      type.is_dynamic_array()) return 8;
  if (type.is_owned_array() || type.is_array_view()) return 16;
  if (type.is_array()) return type.array_length() * type_size(type.element_type());
  if (type.is_struct() || type.is_tuple()) {
    uint64_t size = 0;
    uint64_t aggregate_alignment = 1;
    for_each_field_in_layout_order(type, [&](const Type& field_type) {
      const uint64_t alignment = type_alignment(field_type);
      aggregate_alignment = std::max(aggregate_alignment, alignment);
      size = (size + alignment - 1) & ~(alignment - 1);
      size += type_size(field_type);
    });
    return (size + aggregate_alignment - 1) & ~(aggregate_alignment - 1);
  }
  if (type.is_float()) return type.is_f32() ? 4 : 8;
  if (type.is_bool()) return 1;
  if (type.is_integer() || type.is_char())
    return std::max<uint64_t>(1, type.bit_width() / 8);
  return 0;
}

AggregateLayout describe_aggregate(const Type& type) {
  AggregateLayout layout;
  layout.name = type.is_struct() ? type.struct_name() : "tuple";
  layout.size = type_size(type);
  layout.alignment = type_alignment(type);
  const std::vector<std::size_t> order = aggregate_physical_order(type);
  const bool is_struct = type.is_struct();
  uint64_t offset = 0;
  uint64_t used = 0;
  for (std::size_t physical = 0; physical < order.size(); ++physical) {
    const std::size_t declaration = order[physical];
    FieldLayout field;
    if (is_struct) {
      field.name = type.struct_fields()[declaration].first;
      field.type = type.struct_fields()[declaration].second;
    } else {
      field.name = "." + std::to_string(declaration);
      field.type = type.tuple_elements()[declaration];
    }
    field.alignment = type_alignment(field.type);
    offset = (offset + field.alignment - 1) & ~(field.alignment - 1);
    field.offset = offset;
    field.size = type_size(field.type);
    offset += field.size;
    used += field.size;
    layout.fields.push_back(std::move(field));
  }
  layout.padding = layout.size > used ? layout.size - used : 0;

  // Same fields, declaration order: the difference is the padding the physical
  // order removed.
  uint64_t declaration_size = 0;
  uint64_t declaration_alignment = 1;
  auto append_declared = [&](const Type& field_type) {
    const uint64_t alignment = type_alignment(field_type);
    declaration_alignment = std::max(declaration_alignment, alignment);
    declaration_size = (declaration_size + alignment - 1) & ~(alignment - 1);
    declaration_size += type_size(field_type);
  };
  if (is_struct) {
    for (const auto& field : type.struct_fields()) append_declared(field.second);
  } else {
    for (const Type& field : type.tuple_elements()) append_declared(field);
  }
  layout.declaration_size =
      (declaration_size + declaration_alignment - 1) & ~(declaration_alignment - 1);
  return layout;
}

std::string print_data_layout(const tir::Module& module) {
  std::vector<Type> structs;
  std::set<std::string> seen;
  for (const auto& function : module.functions()) {
    if (!function) continue;
    collect_struct_types(function->return_type(), structs, seen);
    for (const auto& parameter : function->parameters()) {
      collect_struct_types(parameter.type, structs, seen);
    }
    for (const auto& op : function->operations()) {
      collect_struct_types(op.result.type, structs, seen);
      collect_struct_types(op.operand.type, structs, seen);
      collect_struct_types(op.lhs.type, structs, seen);
      collect_struct_types(op.rhs.type, structs, seen);
      for (const auto& argument : op.arguments) {
        collect_struct_types(argument.type, structs, seen);
      }
    }
  }

  std::ostringstream out;
  out << "// data layout: widest-alignment-first fields, natural alignment\n";
  if (structs.empty()) {
    out << "// no aggregate types\n";
    return out.str();
  }
  for (const Type& type : structs) {
    const AggregateLayout layout = describe_aggregate(type);
    const uint64_t per_line = layout.size == 0 ? 0 : (64 + layout.size - 1) / layout.size;
    out << "struct " << layout.name << " {\n";
    out << "  size " << layout.size << " align " << layout.alignment
        << " padding " << layout.padding << " elements_per_cache_line " << per_line
        << " declared " << layout.declaration_size << "\n";
    for (const FieldLayout& field : layout.fields) {
      out << "  " << field.name << " : " << field.type.to_string() << " @ " << field.offset
          << " (" << field.size << " bytes, align " << field.alignment << ")\n";
    }
    out << "}\n";
  }
  return out.str();
}

}  // namespace nirq
