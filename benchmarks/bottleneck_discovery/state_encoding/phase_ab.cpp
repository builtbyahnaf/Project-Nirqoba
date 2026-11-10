// Packed versus separate per-instance state, measured on the collector's own two
// passes rather than on an abstract loop. Nothing here is linked into the
// runtime: it exists so the representation choice in reports/performance/
// ohr-section-b.md can be reproduced.
//
// Build (MSVC, same flags as the runtime's RelWithDebInfo configuration):
//   cl /nologo /std:c++20 /O2 /Ob2 /DNDEBUG /EHsc /MD /Fas phase_ab.cpp ^
//      /link /SUBSYSTEM:CONSOLE
// Run the executable several times: the working set is 48 MB, so run-to-run
// spread is a few percent and a single run does not settle a 20 ms question.
//
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

// Each cell is one real object span: 48 bytes of header, so every state field
// sits in the same cache line whichever encoding is used. The only difference the
// benchmark can then show is the work the access paths do, not density.
struct Separate {
  uint64_t links[4];
  uint32_t length;
  uint8_t marked, young, remembered, age;
};
struct Packed {
  uint64_t links[4];
  uint32_t length;
  uint32_t state;  // bit0 marked, bit1 young, bit2 remembered, bits 8-15 age
};
struct Hybrid {
  uint64_t links[4];
  uint32_t length;
  uint8_t marked;
  uint32_t state;  // bit1 young, bit2 remembered, bits 8-15 age
};

static constexpr uint32_t kMarked = 1u;
static constexpr uint32_t kYoung = 2u;
static constexpr uint32_t kAgeShift = 8u;
static constexpr uint32_t kAgeMask = 0xFFu << kAgeShift;

template <typename Cell>
double pass(std::vector<Cell>& cells, void (*mark)(Cell&), void (*sweep)(Cell&)) {
  double best = 1e9;
  for (int round = 0; round < 7; ++round) {
    auto start = std::chrono::steady_clock::now();
    for (Cell& cell : cells) mark(cell);
    for (Cell& cell : cells) sweep(cell);
    auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    if (ms < best) best = ms;
  }
  return best;
}

int main() {
  const size_t count = 1000000;
  std::vector<Separate> sep(count);
  std::vector<Packed> pack(count);
  std::vector<Hybrid> hyb(count);
  for (size_t i = 0; i < count; ++i) {
    sep[i] = Separate{0, 1, 0, 0};
    pack[i].state = kYoung;
    hyb[i].marked = 0;
    hyb[i].state = kYoung;
  }

  const double sep_ms = pass<Separate>(
      sep,
      [](Separate& c) { if (!c.marked) c.marked = 1; },
      [](Separate& c) { c.marked = 0; if (c.young) ++c.age; });
  const double pack_ms = pass<Packed>(
      pack,
      [](Packed& c) { if (!(c.state & kMarked)) c.state |= kMarked; },
      [](Packed& c) {
        uint32_t s = c.state & ~kMarked;
        if (s & kYoung) {
          uint32_t age = (s & kAgeMask) >> kAgeShift;
          if (age < 255) ++age;
          s = (s & ~kAgeMask) | (age << kAgeShift);
        }
        c.state = s;
      });
  const double hyb_ms = pass<Hybrid>(
      hyb,
      [](Hybrid& c) { if (!c.marked) c.marked = 1; },
      [](Hybrid& c) {
        c.marked = 0;
        uint32_t s = c.state;
        if (s & kYoung) {
          uint32_t age = (s & kAgeMask) >> kAgeShift;
          if (age < 255) ++age;
          s = (s & ~kAgeMask) | (age << kAgeShift);
        }
        c.state = s;
      });
  std::printf("separate %.2f ms\npacked   %.2f ms\nhybrid   %.2f ms\n", sep_ms,
              pack_ms, hyb_ms);
  return 0;
}
