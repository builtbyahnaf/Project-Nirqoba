// slot_index.h — internal open-addressing index for GC root-slot bookkeeping.
//
// PO-27 regression note. The first implementation (commit 0b2d457) doubled the
// table capacity whenever `size_ + tombstones_` crossed the load threshold and
// never distinguished a table full of live entries from one full of
// tombstones. A program that registers and releases one root slot at a time
// therefore grew the table on every rehash even though only one entry was
// live, reaching gigabyte-sized allocations and making each probe scan
// gigabytes of sparse memory.
//
// The policy below is the one PageDirectory already uses: a rehash is a
// capacity increase only when live occupancy alone justifies it; otherwise it
// is a same-capacity compaction that clears tombstones. Table capacity is
// therefore bounded by the peak number of concurrently live slots, not by the
// total number of insertions the program has ever performed.
//
// This header is not part of the public runtime ABI. The unit tests include it
// directly so they exercise the exact implementation the collector uses. The
// Diagnostics accessors exist for those tests and for the debug configuration's
// tooling. In a production build nothing reads them, so the probe bookkeeping
// they need is compiled out: `record_probe` becomes empty and the compiler drops
// the call, leaving the probe chain itself unchanged.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "runtime_config.h"

namespace nirq::gc::detail {

// Stable hash used by every slot index. Exposed so tests can construct
// deliberately colliding addresses without duplicating the mixing constants.
inline size_t slot_hash(void* slot) {
  uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slot));
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return static_cast<size_t>(value);
}

// `State` is the per-slot payload owned by the caller. It must be default
// constructible and move assignable (entries move during a rehash) and must
// expose a `count` member that tracks duplicate registrations of the slot.
template <typename State>
class SlotIndex {
 public:
  static constexpr size_t kInitialCapacity = 16;

  // Test-only view of the table state. Not used by the collector.
  struct Diagnostics {
    size_t capacity = 0;
    size_t live_entries = 0;
    size_t tombstones = 0;
    size_t rehash_count = 0;
    size_t compaction_count = 0;
    size_t growth_count = 0;
    size_t max_probe_length = 0;
  };

  // Returns the state for `slot`, or null when the slot is not present.
  //
  // The probe loop is bounded by the table capacity even when every entry in
  // the table is live or tombstoned, so an unsuccessful lookup can never spin.
  State* find(void* slot) {
    if (entries_.empty() || !slot) return nullptr;
    const size_t mask = entries_.size() - 1;
    size_t index = slot_hash(slot) & mask;
    for (size_t probes = 1; probes <= entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.slot == nullptr) {
        record_probe(probes);
        return nullptr;
      }
      if (entry.slot == slot) {
        record_probe(probes);
        return &entry.state;
      }
      index = (index + 1) & mask;
    }
    // The table contains no empty entry: the probe wrapped completely.
    record_probe(entries_.size());
    return nullptr;
  }

  // Returns the state for `slot`, default constructed when absent. The caller
  // fills it in; the state is never reallocated afterwards (rehashes move the
  // state, which can invalidate previously returned pointers, so callers must
  // not cache them across an insertion).
  State* insert(void* slot) { return insert_impl(slot, true); }

  // Test-only entry point. `allow_growth == false` lets a test fill the table
  // exactly to capacity so the failure and full-table termination paths can be
  // exercised without provoking an impossible allocation. Production always
  // calls insert() with growth enabled.
  State* insert_for_testing(void* slot, bool allow_growth) {
    return insert_impl(slot, allow_growth);
  }

  // Drops `count` registrations of `slot`, tombstoning the entry only when the
  // last one goes away. One probe chain settles the removal instead of a lookup
  // followed by a separate erase probe. `fully_released`, when supplied, reports
  // whether this call removed the key entirely, which lets the caller retire
  // anything it derived from the slot without a second lookup.
  bool release(void* slot, uint32_t count, bool* fully_released = nullptr) {
    if (fully_released) *fully_released = false;
    if (!slot || entries_.empty()) return false;
    const size_t mask = entries_.size() - 1;
    size_t index = slot_hash(slot) & mask;
    for (size_t probes = 1; probes <= entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.slot == nullptr) {
        record_probe(probes);
        return false;
      }
      if (entry.slot == slot) {
        record_probe(probes);
        if (count >= entry.state.count) {
          entry = Entry{};
          entry.slot = kTombstone();
          --size_;
          ++tombstones_;
          if (fully_released) *fully_released = true;
        } else {
          entry.state.count -= count;
        }
        return true;
      }
      index = (index + 1) & mask;
    }
    record_probe(entries_.size());
    return false;
  }

  size_t size() const { return size_; }
  size_t capacity() const { return entries_.size(); }

  Diagnostics diagnostics() const {
    Diagnostics result;
    result.capacity = entries_.size();
    result.live_entries = size_;
    result.tombstones = tombstones_;
    result.rehash_count = rehash_count_;
    result.compaction_count = compaction_count_;
    result.growth_count = growth_count_;
    result.max_probe_length = max_probe_length_;
    return result;
  }

  // Test-only: clears the event counters (rehash, compaction, growth, maximum
  // probe length) while leaving the table contents and live/tombstone counts
  // untouched. Lets a test isolate the probes performed by one operation.
  void reset_diagnostics() {
    rehash_count_ = 0;
    compaction_count_ = 0;
    growth_count_ = 0;
    max_probe_length_ = 0;
  }

  // Chooses the capacity the next rehash should use. Grow only when live
  // occupancy alone is high; otherwise keep the current capacity so the rehash
  // compacts tombstones in place. Returns false when a required doubling would
  // overflow size_t. Exposed for the capacity-overflow unit test.
  static bool select_next_capacity(size_t capacity, size_t live, size_t* next) {
    if (!next || capacity == 0) return false;
    if (live >= capacity / 2) {
      if (capacity > SIZE_MAX / 2) return false;
      *next = capacity * 2;
    } else {
      *next = capacity;
    }
    return true;
  }

 private:
  struct Entry {
    void* slot = nullptr;
    State state;
  };
  static void* kTombstone() { return reinterpret_cast<void*>(uintptr_t{1}); }

  State* insert_impl(void* slot, bool allow_growth) {
    if (!slot) return nullptr;
    if (entries_.empty()) {
      if (!rehash(kInitialCapacity)) return nullptr;
    } else if (allow_growth &&
               size_ + tombstones_ + 1 >=
                   entries_.size() - entries_.size() / 4) {
      size_t next = 0;
      if (!select_next_capacity(entries_.size(), size_, &next)) return nullptr;
      if (!rehash(next)) return nullptr;
    }
    const size_t mask = entries_.size() - 1;
    size_t index = slot_hash(slot) & mask;
    size_t tombstone = entries_.size();
    for (size_t probes = 1; probes <= entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.slot == slot) {
        record_probe(probes);
        return &entry.state;
      }
      if (entry.slot == kTombstone() && tombstone == entries_.size())
        tombstone = index;
      if (entry.slot == nullptr) {
        if (tombstone != entries_.size()) {
          index = tombstone;
          --tombstones_;
        }
        entries_[index] = Entry{};
        entries_[index].slot = slot;
        ++size_;
        record_probe(probes);
        return &entries_[index].state;
      }
      index = (index + 1) & mask;
    }
    if (tombstone != entries_.size()) {
      entries_[tombstone] = Entry{};
      entries_[tombstone].slot = slot;
      --tombstones_;
      ++size_;
      record_probe(entries_.size());
      return &entries_[tombstone].state;
    }
    record_probe(entries_.size());
    return nullptr;
  }

  // Rebuilds the table at `capacity`. `capacity` is either the current size
  // (compaction) or twice the current size (growth); both keep the power-of-two
  // invariant the mask arithmetic depends on. The caller guarantees capacity is
  // strictly larger than the live entry count, so re-insertion always finds an
  // empty slot.
  bool rehash(size_t capacity) {
    if (capacity == 0 || size_ >= capacity) return false;
    try {
      std::vector<Entry> replacement(capacity);
      const size_t mask = capacity - 1;
      for (Entry& entry : entries_) {
        if (!entry.slot || entry.slot == kTombstone()) continue;
        size_t index = slot_hash(entry.slot) & mask;
        while (replacement[index].slot != nullptr) index = (index + 1) & mask;
        replacement[index] = std::move(entry);
      }
      const size_t previous_capacity = entries_.size();
      entries_.swap(replacement);
      tombstones_ = 0;
      record_rehash(previous_capacity, capacity);
      return true;
    } catch (...) {
      return false;
    }
  }

  // Probe and rehash statistics are observation only: no collector path reads
  // them, so the production build does not record them at all.
  void record_probe(size_t probes) {
    if constexpr (nirq::runtime_debug_enabled) {
      if (probes > max_probe_length_) max_probe_length_ = probes;
    }
  }

  void record_rehash(size_t previous_capacity, size_t capacity) {
    if constexpr (nirq::runtime_debug_enabled) {
      ++rehash_count_;
      // Initial table creation is neither a growth nor a compaction: there is
      // no previous capacity to compare against.
      if (previous_capacity == 0) return;
      if (capacity > previous_capacity) {
        ++growth_count_;
      } else {
        ++compaction_count_;
      }
    }
  }

  std::vector<Entry> entries_;
  size_t size_ = 0;
  size_t tombstones_ = 0;
  size_t rehash_count_ = 0;
  size_t compaction_count_ = 0;
  size_t growth_count_ = 0;
  size_t max_probe_length_ = 0;
};

}  // namespace nirq::gc::detail
