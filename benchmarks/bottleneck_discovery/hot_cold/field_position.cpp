// field_position.cpp — E2 baseline: does the position of a hot field inside a
// record change how fast a scan of many records runs?
//
// This measures the question E2 has to answer before any reordering is allowed.
// It does not touch the compiler: the variants are hand-written structs that match
// what the layout rule produces for the same field sets, so the numbers describe
// the hardware, not a proposed transformation.
//
// Variants:
//   A  record 64 B  (one cache line), hot field first
//   B  record 128 B (two lines), hot field first
//   C  record 128 B, hot field on the second line
//   D  hot field alone in its own array (SoA: the locality ceiling)
//   E  record 128 B, hot field first, random object order (hot/cold OBJECTS)
//   F  record 128 B, hot field second line, random object order

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

namespace {

constexpr std::size_t kObjects = 1u << 20;
constexpr int kPasses = 8;
constexpr int kRounds = 5;

struct Obj64 {
  std::int64_t hot;
  std::int64_t a0, a1, a2, a3, a4, a5, a6;
};

struct Obj128HotFirst {
  std::int64_t hot;
  std::int64_t a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14;
};

struct Obj128HotSecond {
  std::int64_t a0, a1, a2, a3, a4, a5, a6, a7;
  std::int64_t hot;
  std::int64_t a8, a9, a10, a11, a12, a13, a14;
};

volatile std::int64_t sink;

template <typename Fn>
double measure(Fn&& one_pass) {
  double best = 1e30;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    std::int64_t sum = 0;
    for (int pass = 0; pass < kPasses; ++pass) sum += one_pass();
    const auto end = std::chrono::steady_clock::now();
    sink = sum;
    const double ns =
        std::chrono::duration<double, std::nano>(end - start).count() /
        (static_cast<double>(kObjects) * kPasses);
    best = std::min(best, ns);
  }
  return best;
}

}  // namespace

int main() {
  std::vector<Obj64> v64(kObjects);
  std::vector<Obj128HotFirst> v128a(kObjects);
  std::vector<Obj128HotSecond> v128b(kObjects);
  std::vector<std::int64_t> soa_hot(kObjects);
  std::vector<std::uint32_t> order(kObjects);
  std::iota(order.begin(), order.end(), 0u);
  std::mt19937 rng(12345);
  std::shuffle(order.begin(), order.end(), rng);

  for (std::size_t i = 0; i < kObjects; ++i) {
    v64[i].hot = static_cast<std::int64_t>(i);
    v128a[i].hot = static_cast<std::int64_t>(i);
    v128b[i].hot = static_cast<std::int64_t>(i);
    soa_hot[i] = static_cast<std::int64_t>(i);
  }

  const double a = measure([&] {
    std::int64_t s = 0;
    for (std::size_t i = 0; i < kObjects; ++i) s += v64[i].hot;
    return s;
  });
  const double b = measure([&] {
    std::int64_t s = 0;
    for (std::size_t i = 0; i < kObjects; ++i) s += v128a[i].hot;
    return s;
  });
  const double c = measure([&] {
    std::int64_t s = 0;
    for (std::size_t i = 0; i < kObjects; ++i) s += v128b[i].hot;
    return s;
  });
  const double d = measure([&] {
    std::int64_t s = 0;
    for (std::size_t i = 0; i < kObjects; ++i) s += soa_hot[i];
    return s;
  });
  const double e = measure([&] {
    std::int64_t s = 0;
    for (std::size_t k = 0; k < kObjects; ++k) s += v128a[order[k]].hot;
    return s;
  });
  const double f = measure([&] {
    std::int64_t s = 0;
    for (std::size_t k = 0; k < kObjects; ++k) s += v128b[order[k]].hot;
    return s;
  });

  std::printf("%-44s %8.3f ns/object  lines/object\n", "A 64B record, hot first", a, 1.0);
  std::printf("%-44s %8.3f ns/object  lines/object\n", "B 128B record, hot first", b, 2.0);
  std::printf("%-44s %8.3f ns/object  lines/object\n", "C 128B record, hot second line", c, 2.0);
  std::printf("%-44s %8.3f ns/object  lines/object\n", "D SoA hot array", d, 0.125);
  std::printf("%-44s %8.3f ns/object  lines/object\n", "E 128B hot first, random order", e, 2.0);
  std::printf("%-44s %8.3f ns/object  lines/object\n", "F 128B hot second, random order", f, 2.0);
  std::printf("\nC/B %.3f   E/F %.3f   sizeof checks %zu %zu %zu\n", c / b, e / f,
              sizeof(Obj64), sizeof(Obj128HotFirst), sizeof(Obj128HotSecond));
  return 0;
}
