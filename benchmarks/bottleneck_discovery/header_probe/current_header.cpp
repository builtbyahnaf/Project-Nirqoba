// C1 probe: the header as the compiler actually lays it out, and what it costs
// per page for representative payloads. It links the runtime so the reproduced
// struct is checked against the size the runtime reports, which is what keeps this
// from drifting away from runtime/src/gc.cpp.
//
// Build and run (from the repository root, with the runtime library built):
//   cl /nologo /std:c++20 /O2 /EHsc /MD /I runtime/include current_header.cpp ^
//      <build>/lib/nirqoba_runtime.lib /link /SUBSYSTEM:CONSOLE psapi.lib
//
#include <cstddef>
#include <cstdio>

struct ReproducedHeader {
  union { void* next; void* tlab_next; };
  void* mark_next;
  void* page;
  unsigned long long birth_epoch;
  unsigned int element_count;
  unsigned int state;
  const nrq_object_layout* layout;
};

int main() {
  std::printf("sizeof=%zu alignof=%zu reported=%zu instance=%zu shared=%zu\n",
              sizeof(ReproducedHeader), alignof(ReproducedHeader),
              nrq_gc_get_allocation_facts().object_header_bytes,
              nrq_gc_get_allocation_facts().object_instance_bytes,
              nrq_gc_get_allocation_facts().object_shared_bytes);
  std::printf("offset next=%zu mark_next=%zu page=%zu epoch=%zu length=%zu state=%zu layout=%zu\n",
              offsetof(ReproducedHeader, next), offsetof(ReproducedHeader, mark_next),
              offsetof(ReproducedHeader, page), offsetof(ReproducedHeader, birth_epoch),
              offsetof(ReproducedHeader, element_count), offsetof(ReproducedHeader, state),
              offsetof(ReproducedHeader, layout));
  const nrq_gc_allocation_facts facts = nrq_gc_get_allocation_facts();
  std::printf("page=%zu tlab=%zu min_slab=%zu payload_align=%zu\n", facts.page_bytes,
              facts.tlab_bytes, facts.tlab_min_slab_bytes, facts.payload_alignment);
  std::printf("%8s %8s %8s %8s %10s %12s\n", "payload", "span", "slots", "slack",
              "per_refill", "MB/100k");
  const size_t payloads[] = {8, 16, 24, 32, 48, 64, 96, 128, 256, 512};
  for (size_t payload : payloads) {
    nrq_gc_allocation_profile profile{};
    if (!nrq_gc_get_allocation_profile(payload, &profile)) continue;
    const double footprint =
        static_cast<double>(profile.span_bytes) * 100000.0 / (1024.0 * 1024.0);
    std::printf("%8zu %8zu %8zu %8zu %10zu %12.2f\n", payload, profile.span_bytes,
                profile.slots_per_page, profile.page_slack_bytes,
                profile.objects_per_refill, footprint);
  }
  return 0;
}
