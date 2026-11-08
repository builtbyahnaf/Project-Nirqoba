// Candidate object-header layouts. This file is a feasibility probe, not a
// proposal to change the runtime: it compiles each layout with the same MSVC
// toolset and the same target ABI as the runtime, and prints the sizes and
// offsets the compiler actually chose. Nothing here is linked into the runtime.
//
// Build: build_layouts.ps1

#include <cstddef>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------- current
// Exact reproduction of runtime/src/gc.cpp ObjectHeader (96 bytes).
struct RefLayout32 {
  size_t inline_offsets[2];
  void* overflow;
  size_t size;
};
struct H96 {
  void* next;              // 0
  void* mark_next;         // 8
  void* page;              // 16
  size_t size;             // 24
  size_t element_size;     // 32
  void* trace;             // 40
  bool marked;             // 48
  bool young;              // 49
  bool remembered;         // 50
  uint8_t age;             // 51
  uint64_t birth_epoch;    // 56
  RefLayout32 layout;      // 64
};

// ---------------------------------------------------------------- 64 bytes
// Offsets move into a 16-byte descriptor: 1-byte count, two 16-bit inline
// offsets (payloads up to 64 KiB), and an overflow pointer for wide layouts.
// `trace`, `size` and `element_size` are narrowed to 32 bits; a single object
// larger than 4 GiB is not a Nirqoba allocation today (the allocation entry
// takes a size_t but the GC page path rejects spans > 64 KiB).
struct RefLayout16 {
  uint16_t count;
  uint16_t flags;
  uint16_t inline_offsets[2];
  void* overflow;
};
struct H64 {
  void* next;
  void* mark_next;
  void* page;
  void* trace;
  uint32_t size;
  uint32_t element_size;
  uint32_t flags;          // marked/young/remembered/age/pointer-root
  uint32_t birth_epoch;
  RefLayout16 layout;
};

// ---------------------------------------------------------------- 48/40 bytes
// `page` becomes a 32-bit slot in a stable page table (the table lookup is one
// load; the sweep already iterates that table). `trace` and the reference
// offsets move to a per-type descriptor selected by `type_id`, so an object
// carries no inline offsets at all. `element_size` is derived from the type.
struct H48 {
  void* next;
  void* mark_next;
  uint32_t page_index;
  uint32_t size;
  uint32_t element_size;
  uint32_t type_id;
  uint32_t birth_epoch;
  uint32_t flags;
  uint32_t aux;            // overflow / finalizer / reserved
};
// 40 drops the 32-bit page index and derives the page from the address the way
// find_object_locked() already does (one directory probe), or from the sweep.
struct H40 {
  void* next;
  void* mark_next;
  uint32_t size;
  uint32_t element_size;
  uint32_t type_id;
  uint32_t birth_epoch;
  uint32_t flags;
};

// ---------------------------------------------------------------- 32/24 bytes
// 32: mark bit and the sweep link share `next` by tagging (8-byte payload
// alignment leaves three low bits), so the header keeps one link. No page
// index: the sweep supplies the page while walking pages, and marking never
// needs it.
struct H32 {
  void* next;              // mark bit + sweep link (tagged)
  void* reserved;          // overflow chain / finalizer slot
  uint32_t type_id;
  uint32_t size;
  uint32_t birth_epoch;
  uint32_t flags;
};
// 24: no epoch and no flags. The birth epoch would have to become a page-level
// value or a side table; both are analysed in the report.
struct H24 {
  void* next;
  void* reserved;
  uint32_t type_id;
  uint32_t size;
};

template <typename T>
void show(const char* name) {
  std::printf("%-6s sizeof=%3zu align=%2zu\n", name, sizeof(T), alignof(T));
}

void footprint(const char* name, size_t header, size_t payload) {
  const size_t align = alignof(std::max_align_t);
  const size_t span = (header + payload + align - 1) & ~(align - 1);
  const size_t per_page = 65536 / span;
  const size_t pages = (2097151 + per_page - 1) / per_page;
  const double heap_mb = 2097151.0 * span / (1024.0 * 1024.0);
  const double zero_bits =
      static_cast<double>(pages) * (8192.0 - static_cast<double>(per_page));
  std::printf(
      "%-4s header=%2zu span=%3zu obj/page=%4zu pages=%5zu heap=%7.1f MB "
      "bitmap-zero-iter=%8.2f M/page-set\n",
      name, header, span, per_page, pages, heap_mb, zero_bits / 1e6);
}

int main() {
  show<RefLayout32>("ref32");
  show<RefLayout16>("ref16");
  show<H96>("H96");
  show<H64>("H64");
  show<H48>("H48");
  show<H40>("H40");
  show<H32>("H32");
  show<H24>("H24");
  std::printf("max_align_t=%zu\n", alignof(std::max_align_t));
  std::printf("offsets H96: next=%zu mark=%zu page=%zu size=%zu elem=%zu trace=%zu epoch=%zu layout=%zu\n",
              offsetof(H96, next), offsetof(H96, mark_next), offsetof(H96, page),
              offsetof(H96, size), offsetof(H96, element_size), offsetof(H96, trace),
              offsetof(H96, birth_epoch), offsetof(H96, layout));
  std::printf("\n2,097,151-node tree, 24-byte payload:\n");
  footprint("96", sizeof(H96), 24);
  footprint("64", sizeof(H64), 24);
  footprint("48", sizeof(H48), 24);
  footprint("40", sizeof(H40), 24);
  footprint("32", sizeof(H32), 24);
  footprint("24", sizeof(H24), 24);
  return 0;
}
