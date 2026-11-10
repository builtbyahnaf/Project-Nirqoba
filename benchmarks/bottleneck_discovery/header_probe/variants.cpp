// Geometry and occupancy of the four header candidates, compiled with the
// runtime's flags and cross-checked against the runtime's own allocation profile.
// Nothing here is linked into the runtime.
//
// Build and run (from the repository root, runtime already built):
//   cl /nologo /std:c++20 /O2 /EHsc /MD /I runtime/include variants.cpp ^
//      <build>/lib/nirqoba_runtime.lib /link /SUBSYSTEM:CONSOLE psapi.lib
//
#include <cstddef>
#include <cstdio>

using u8 = unsigned char; using u32 = unsigned int; using u64 = unsigned long long;
using ptr = void*;

struct V48 { ptr a; ptr b; ptr page; u64 epoch; u32 len; u32 state; ptr layout; };
struct V40 { ptr a; ptr b; u64 epoch; u32 len; u32 state; ptr layout; };
struct V36 { ptr a; ptr b; u32 epoch; u32 len; u32 state; ptr layout; };
struct V32 { ptr a; u64 epoch; u32 len; u32 state; ptr layout; };

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

static void report(const char* name, size_t used, size_t size, size_t align) {
  std::printf("%s used=%zu sizeof=%zu alignof=%zu overhead=%zu\n", name, used, size,
              align, size - used);
  const size_t aligns[2] = {8, 16};
  const size_t payloads[3] = {8, 64, 512};
  for (size_t a : aligns) {
    const bool legal = (size % a) == 0;
    std::printf("  align %2zu %s:", a, legal ? "ok  " : "FAIL");
    for (size_t p : payloads) {
      const size_t span = align_up(size + p, a);
      std::printf(" p=%zu span=%zu slots=%zu", p, span, 65536 / span);
    }
    std::printf("\n");
  }
}

int main() {
  std::printf("runtime header=%zu payload_align=%zu (max_align_t=%zu)\n",
              nrq_gc_get_allocation_facts().object_header_bytes,
              nrq_gc_get_allocation_facts().payload_alignment,
              alignof(max_align_t));
  report("H48", sizeof(V48), sizeof(V48), alignof(V48));
  report("H40", sizeof(V40), sizeof(V40), alignof(V40));
  report("H36", 36, sizeof(V36), alignof(V36));
  report("H32", sizeof(V32), sizeof(V32), alignof(V32));
  // Field availability per candidate.
  std::printf("offsets H48 next=%zu mk=%zu page=%zu ep=%zu len=%zu st=%zu lay=%zu\n",
              offsetof(V48,a), offsetof(V48,b), offsetof(V48,page),
              offsetof(V48,epoch), offsetof(V48,len), offsetof(V48,state),
              offsetof(V48,layout));
  std::printf("offsets H40 next=%zu mk=%zu ep=%zu len=%zu st=%zu lay=%zu\n",
              offsetof(V40,a), offsetof(V40,b), offsetof(V40,epoch),
              offsetof(V40,len), offsetof(V40,state), offsetof(V40,layout));
  std::printf("offsets H36 next=%zu mk=%zu ep=%zu len=%zu st=%zu lay=%zu\n",
              offsetof(V36,a), offsetof(V36,b), offsetof(V36,epoch),
              offsetof(V36,len), offsetof(V36,state), offsetof(V36,layout));
  std::printf("offsets H32 next=%zu ep=%zu len=%zu st=%zu lay=%zu\n",
              offsetof(V32,a), offsetof(V32,epoch), offsetof(V32,len),
              offsetof(V32,state), offsetof(V32,layout));
  // Formula check against the shipping profile for the shipping header.
  bool formula_ok = true;
  const size_t payloads[] = {8, 16, 24, 32, 48, 64, 96, 128, 256, 512};
  for (size_t p : payloads) {
    nrq_gc_allocation_profile prof{};
    if (!nrq_gc_get_allocation_profile(p, &prof)) { std::printf("profile %zu none\n", p); continue; }
    const size_t mine = align_up(nrq_gc_get_allocation_facts().object_header_bytes + p,
                                 nrq_gc_get_allocation_facts().payload_alignment);
    if (mine != prof.span_bytes) { formula_ok = false;
      std::printf("MISMATCH p=%zu api=%zu formula=%zu\n", p, prof.span_bytes, mine); }
  }
  std::printf("formula matches the runtime profile: %s\n", formula_ok ? "yes" : "no");
  return 0;
}
