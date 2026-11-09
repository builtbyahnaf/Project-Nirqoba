"""Injects stage timing into one revision's gc.cpp (diagnostic only).

The collector is copied verbatim from the extracted revision tree; every
replacement below is an exact string taken from that tree, and the script
fails loudly if a replacement does not apply exactly once. Nothing in the
repository's production sources is touched.
"""

import sys

src_path, dst_path = sys.argv[1], sys.argv[2]
text = open(src_path, encoding="utf-8").read()

bitmap = "sweep_cursor" in text


def replace_once(old, new):
    global text
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            "replacement matched %d times, expected 1:\n%s" % (count, old))
    text = text.replace(old, new, 1)


replace_once(
    "#include <cstring>\n",
    "#include <cstring>\n#include <chrono>\n#include <cstdio>\n")

replace_once(
    "namespace {\n",
    """namespace {

struct StageProbeCounters {
  uint64_t mark_ns = 0;
  uint64_t sweep_ns = 0;
  uint64_t begin_ns = 0;
  uint64_t finish_ns = 0;
  uint64_t collections = 0;
  uint64_t mark_calls = 0;
  uint64_t sweep_calls = 0;
  uint64_t zero_bits = 0;
  uint64_t set_bits = 0;
};
StageProbeCounters stage_probe_counters;

inline uint64_t stage_now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct StageProbeDump {
  ~StageProbeDump() {
    const StageProbeCounters& c = stage_probe_counters;
    std::fprintf(
        stdout,
        "PROBE collections %llu\\n"
        "PROBE begin_ms %.3f\\n"
        "PROBE mark_ms %.3f\\n"
        "PROBE sweep_ms %.3f\\n"
        "PROBE finish_ms %.3f\\n"
        "PROBE mark_calls %llu\\n"
        "PROBE sweep_calls %llu\\n"
        "PROBE zero_bits %llu\\n"
        "PROBE set_bits %llu\\n",
        static_cast<unsigned long long>(c.collections), c.begin_ns / 1e6,
        c.mark_ns / 1e6, c.sweep_ns / 1e6, c.finish_ns / 1e6,
        static_cast<unsigned long long>(c.mark_calls),
        static_cast<unsigned long long>(c.sweep_calls),
        static_cast<unsigned long long>(c.zero_bits),
        static_cast<unsigned long long>(c.set_bits));
    std::fflush(stdout);
  }
};
StageProbeDump stage_probe_dump;
""")

replace_once(
    "  begin_cycle(young_only);\n",
    """  {
    const uint64_t begin_t0 = stage_now_ns();
    begin_cycle(young_only);
    stage_probe_counters.begin_ns += stage_now_ns() - begin_t0;
    ++stage_probe_counters.collections;
  }
""")

mark_budget = ("kMarkSliceBytes" if "mark_slice_locked(kMarkSliceBytes)"
               in text else "kMarkBudget")
sweep_budget = ("kSweepSliceBytes" if "sweep_chunk_locked(kSweepSliceBytes)"
                in text else "kSweepBudget")

replace_once(
    "        const bool marked = mark_slice_locked(%s);\n" % mark_budget,
    """        const uint64_t mark_t0 = stage_now_ns();
        const bool marked = mark_slice_locked(%s);
        stage_probe_counters.mark_ns += stage_now_ns() - mark_t0;
        ++stage_probe_counters.mark_calls;
""" % mark_budget)

replace_once(
    "        if (!mark_slice_locked(%s)) {\n" % mark_budget,
    """        const uint64_t drain_t0 = stage_now_ns();
        const bool mark_drained = mark_slice_locked(%s);
        stage_probe_counters.mark_ns += stage_now_ns() - drain_t0;
        ++stage_probe_counters.mark_calls;
        if (!mark_drained) {
""" % mark_budget)

replace_once(
    """          if (sweep_chunk_locked(%s)) {
            finish_cycle_locked(young_only);
            finished = true;
          }
""" % sweep_budget,
    """          const uint64_t sweep_t0 = stage_now_ns();
          const bool sweep_done = sweep_chunk_locked(%s);
          stage_probe_counters.sweep_ns += stage_now_ns() - sweep_t0;
          ++stage_probe_counters.sweep_calls;
          if (sweep_done) {
            const uint64_t finish_t0 = stage_now_ns();
            finish_cycle_locked(young_only);
            stage_probe_counters.finish_ns += stage_now_ns() - finish_t0;
            finished = true;
          }
""" % sweep_budget)

if "sweep_cursor.mask" in text:
    # Set-bit walk: every iteration is one object start, so a zero-bit count is
    # zero by construction and there is no position loop to instrument.
    replace_once(
        "    const size_t slot = word_index * 64 + index;\n",
        """    const size_t slot = word_index * 64 + index;
    ++stage_probe_counters.set_bits;
""")
elif bitmap:
    replace_once(
        """    if (((bits >> sweep_cursor.bit) & uint64_t{1}) == 0) {
      ++sweep_cursor.bit;
""",
        """    if (((bits >> sweep_cursor.bit) & uint64_t{1}) == 0) {
      ++stage_probe_counters.zero_bits;
      ++sweep_cursor.bit;
""")
    replace_once(
        "    const size_t slot = sweep_cursor.word * 64 + sweep_cursor.bit;\n",
        """    const size_t slot = sweep_cursor.word * 64 + sweep_cursor.bit;
    ++stage_probe_counters.set_bits;
""")
else:
    replace_once(
        "    ObjectHeader* header = sweep_list;\n",
        """    ObjectHeader* header = sweep_list;
    ++stage_probe_counters.set_bits;
""")

open(dst_path, "w", encoding="utf-8", newline="\n").write(text)
print("patched %s -> %s (%s sweep)" %
      (src_path, dst_path, "bitmap" if bitmap else "list"))
