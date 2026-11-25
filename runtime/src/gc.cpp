// Precise non-moving mark-and-sweep collector for explicitly described objects.
// The runtime owns a linked allocation registry, while roots are addresses of
// pointer slots registered by generated code or native callers. Marking follows
// only trace callbacks, so integer data that happens to resemble an address is
// never treated as a reference.
//
// Collection is generational and incremental. A minor cycle traces only young
// objects plus the remembered old-to-young set; survivors are promoted after a
// few cycles. Marking runs in bounded slices while mutators execute, and the
// sweep uses the same pattern after detaching its snapshot list. Mutators are
// parked only briefly at cycle boundaries to publish TLabs and prepare roots.

#include "nirqoba/runtime/gc.h"
#include "nirqoba/runtime/allocator.h"
#include "nirqoba/runtime/alloc_hint.h"
#include "nirqoba/runtime/cache_line.h"
#include "nirqoba/runtime/fatal.h"
#include "nirqoba/runtime/gc_inline.h"

#include "gc_debug_internal.h"
#include "slot_index.h"

#include <algorithm>
#include <array>
#include <bit>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>

#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

using namespace std;

// The slot index lives in a header so the unit tests can exercise the exact
// table implementation instead of approximating it through the public GC API.
using nirq::gc::detail::SlotIndex;
// Emission points below name these directly. Every helper is `if constexpr` on
// the build configuration, so in a production build the calls and their
// arguments disappear from this translation unit entirely.
using nirq::gc::detail::debug_emit;
using nirq::gc::detail::debug_tag;

// Reference counts held by collector storage are registered per object, because a
// layout only describes traced edges, and two payloads of the same geometry can
// differ in which words carry a count.
void nrq_gc_release_shared_fields(void* payload);

namespace {
struct SharedFieldRegistrations {
  std::mutex mutex;
  std::unordered_map<void*, std::vector<uint32_t>> fields;
};

SharedFieldRegistrations& shared_field_registrations() {
  static SharedFieldRegistrations registry;
  return registry;
}
}  // namespace

// Public entry points live outside the file's anonymous namespace: they are part
// of the runtime ABI, so their symbols have to reach the linker.
void nrq_gc_register_shared_fields(void* object, const size_t* offsets,
                                   size_t count) {
  if (object == nullptr || offsets == nullptr || count == 0) return;
  std::vector<uint32_t> narrowed;
  try {
    narrowed.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      if (offsets[index] > UINT32_MAX) return;
      narrowed.push_back(static_cast<uint32_t>(offsets[index]));
    }
  } catch (...) {
    return;
  }
  SharedFieldRegistrations& registry = shared_field_registrations();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.fields[object] = std::move(narrowed);
}
size_t nrq_gc_shared_field_registrations(void) {
  SharedFieldRegistrations& registry = shared_field_registrations();
  std::lock_guard<std::mutex> lock(registry.mutex);
  return registry.fields.size();
}
void nrq_gc_release_shared_fields(void* payload) {
  if (payload == nullptr) return;
  std::vector<uint32_t> offsets;
  {
    SharedFieldRegistrations& registry = shared_field_registrations();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.fields.find(payload);
    if (found == registry.fields.end()) return;
    offsets = std::move(found->second);
    registry.fields.erase(found);
  }
  // The registry lock is released first: a final count drops storage, which takes
  // the shared-control lock, and the sweep already holds the heap lock.
  for (uint32_t offset : offsets) {
    void* value = nullptr;
    std::memcpy(&value, static_cast<unsigned char*>(payload) + offset,
                sizeof(void*));
    if (value != nullptr) nrq_memory_shared_release(value);
  }
}
namespace {

constexpr size_t kGcPageShift = 16;
constexpr size_t kGcPageBytes = size_t{1} << kGcPageShift;
constexpr size_t kGcRegionShift = 21;
constexpr size_t kGcRegionBytes = size_t{1} << kGcRegionShift;
constexpr size_t kPagesPerRegion = kGcRegionBytes / kGcPageBytes;
constexpr size_t kObjectStartAlignment = alignof(std::max_align_t);

static_assert(kGcRegionBytes == (size_t{2} << 20),
              "GC regions must be 2 MiB aligned blocks");
static_assert((kGcRegionBytes & (kGcRegionBytes - 1)) == 0,
              "GC region size must be a power of two");
static_assert(kPagesPerRegion == 32,
              "Each 2 MiB GC region must host 32 64-KiB pages");

struct GcPage;

// Fixed-size 2 MiB aligned region backing ordinary GC pages and TLAB slabs.
// Regions enable O(1) bulk reclamation of dead or evacuated blocks and track
// live/pinned density for survivor evacuation candidate selection.
struct GcRegion {
  unsigned char* base = nullptr;
  size_t capacity = kGcRegionBytes;
  size_t region_index = SIZE_MAX;
  uint8_t next_slot = 0;
  uint8_t active_pages = 0;
  bool is_survivor = false;
  bool recycled_dirty = false;
  bool has_unpublished = false;
  std::array<GcPage*, kPagesPerRegion> pages{};
  size_t pinned_objects = 0;
  size_t live_objects = 0;
  size_t live_payload_bytes = 0;
  size_t marked_objects = 0;
  size_t cycle_pinned_objects = 0;
};

// The representation every object on a page uses. A page is dedicated to one
// representation so an object never has to describe its own header size: the
// page already does, and discovery is one subtraction from the span start.
//
//   Headerless  H0: no per-object header; one layout and one slot size
//   Compact     H8: state word + layout id
//   Extended    H16: element count + state + layout id + page-relative link
//   Large       H16, no walkable start bitmap, exact object index
//   Fallback    H16, directory insertion failed, exact object index
//
// Large and fallback pages are the only ones whose objects use the link word.
enum class PageKind : uint8_t {
  Headerless,
  Compact,
  Extended,
  Large,
  Fallback
};

constexpr size_t page_header_bytes(PageKind kind) {
  switch (kind) {
    case PageKind::Headerless: return 0;
    case PageKind::Compact: return 8;
    case PageKind::Extended:
    case PageKind::Large:
    case PageKind::Fallback: return 16;
  }
  return 16;
}

inline bool page_is_representable(PageKind kind) {
  return kind == PageKind::Headerless || kind == PageKind::Compact ||
         kind == PageKind::Extended;
}

struct GcPage {
  unsigned char* storage = nullptr;
  size_t capacity = 0;
  // Owning 2 MiB region and 64 KiB slot index [0..31] for ordinary pages.
  // Large pages (> kGcPageBytes) keep `region == nullptr`.
  GcRegion* region = nullptr;
  uint8_t region_slot = 0;
  // Representation metadata. A headerless page uses one fixed span
  // (`slot_bytes`) and one layout (`uniform_layout`) for every object, which is
  // what lets the object carry no header at all. Compact and extended pages
  // leave `slot_bytes == 0`; their objects find their layout through the header.
  PageKind kind = PageKind::Extended;
  size_t slot_bytes = 0;
  size_t slot_capacity = 0;
  const nrq_object_layout* uniform_layout = nullptr;
  size_t used = 0;
  size_t live_objects = 0;
  // Objects on this page that are found through ObjectIndex rather than the
  // start bitmap. Large pages use this count to keep their fallback metadata
  // separate from the ordinary page counters.
  size_t indexed_objects = 0;
  // Young objects in this page. A minor collection can only reclaim or promote
  // young objects, so a page where this count is zero is passed straight over:
  // a minor never sets a mark bit on an old object
  // (enqueue_header_locked refuses it), so there is nothing in such a page for
  // the minor to change.
  size_t young_objects = 0;
  // Payload bytes currently represented by `live_objects`. A whole-page
  // reclamation needs the amount without walking the page, the same way the
  // live-object count lets it skip the walk entirely.
  size_t live_payload_bytes = 0;
  // Objects allocated after the current cycle began. They are retained by the
  // sweep rather than marked, so a page's survivor count is
  // `marked_objects + born_this_cycle`.
  //
  // Birth used to be an 8-byte field in every object. It is now one bit per
  // span slot, valid only while `born_epoch` equals the active cycle: the first
  // allocation of a cycle clears the bitmap and stamps the page, so a stale bit
  // from an older cycle is never read and no page walk is needed to reset it.
  size_t born_this_cycle = 0;
  uint64_t* born_bits = nullptr;
  size_t born_words = 0;
  uint64_t born_epoch = 0;
  // Per-page mark metadata. Marks are off-heap: a header write is no longer
  // needed to set or test one, and the counts let sweep decide a page's fate
  // without reading every header in it.
  uint64_t* mark_bits = nullptr;
  size_t mark_words = 0;
  size_t marked_objects = 0;
  size_t marked_young_objects = 0;
  // Remembered-set membership is a property of a young object, but it is
  // maintained as a page bitmap so the store barrier does not rewrite the
  // object's state word. `remembered_objects` is exact while the page is live.
  uint64_t* remembered_bits = nullptr;
  size_t remembered_words = 0;
  size_t remembered_objects = 0;
  // Page-local object-start metadata. One bit per max_align_t-sized slot lets
  // reference resolution validate an address and derive its header without a
  // global hash probe. Pages larger than kGcPageBytes are not indexed; their
  // references keep using ObjectIndex.
  uint64_t* object_starts = nullptr;
  size_t object_start_words = 0;
  // Ordinary pages store all four bitmaps contiguously inside the page struct
  // so acquiring a page performs one heap allocation and one contiguous zero
  // instead of five separate allocations. Large pages fall back to a single
  // dynamically sized buffer for the three bitmaps they use.
  static constexpr size_t kInlineBitmapWords =
      ((kGcPageBytes / kObjectStartAlignment) + 63) / 64;
  std::array<uint64_t, 4 * kInlineBitmapWords> inline_bitmaps;
  std::unique_ptr<uint64_t[]> grown_bitmaps;
  // Per-object state for headerless pages. With no object word to hold young and
  // promotion age, one byte per object carries them: bit 0 young, bits 1-7 age.
  // A headerless page has one slot size, so the table is indexed by the object's
  // ordinal (`offset / slot_bytes`), not by its 8-byte grid position: indexing by
  // grid position would spend eight bytes of page metadata on a 64-byte record,
  // which is exactly the H8 header the representation exists to avoid.
  std::unique_ptr<uint8_t[]> h0_states;
  size_t h0_state_capacity = 0;
  // Pages emptied by a concurrent sweep are retained until the next
  // stop-the-world cycle boundary, when no mutator can still be inside an
  // allocator or holding a TLAB cursor into the page.
  bool retired = false;
  // How many times this page has been emptied and handed storage out again. The
  // incremental sweep walks a captured page set across chunks, and a page it has
  // already passed stays in that set: the space it reclaimed is handed straight
  // back to mutators, and a page emptied before the cursor reached it would be
  // walked with bitmaps that now describe a different generation of objects. The
  // sweep binds this counter when it starts on a page and abandons the page when
  // it no longer matches.
  uint64_t reuse_generation = 0;
  // Set when this page cannot be found through the page directory, either
  // because its start bitmap was not allocated or because indexing it failed.
  // Objects in such a page are tracked by the global object index instead.
  bool use_object_index = false;
  // Runs of storage whose object was reclaimed during a sweep. A bump allocator
  // can only move forward, so without these a page that churned through a mix of
  // short- and long-lived objects had to be abandoned while most of it was dead
  // space. Reuse is first-fit within the page and requires no allocation: a fixed
  // array keeps the bookkeeping inline, and a page that fills it simply behaves
  // like the bump allocator it used to be.
  // Page-relative and 32-bit: a page is at most `kGcPageBytes` for the ordinary
  // case, and the whole representation is bounded by the 4 GiB payload limit the
  // layout descriptor enforces. Eight bytes per run doubles the number of runs a
  // page can remember for the same 1.5 KiB, which matters now that a denser
  // header packs twice as many objects into a page: churn over 128-byte objects
  // leaves one run per two dead objects, and the old 16-byte entry capped a page
  // at 96 of them and silently dropped the rest.
  struct FreeSpan {
    uint32_t offset;
    uint32_t span;
  };
  // A headerless page packs twice as many objects into a 64 KiB page as an H32
  // page did, so a workload that frees every other object produces twice as many
  // runs before they merge. The list is sized for that density: dropping a run
  // does not corrupt anything, but it makes the page unable to answer a request
  // and forces a fresh page, which is the fragmentation drift this bound exists
  // to avoid.
  static constexpr size_t kMaxFreeSpans = 64;
  // The list grows past the inline array only on a page that actually shatters.
  // A page that has lost every other object records one run per dead object, and
  // the old fixed bound dropped everything above 384 of them: at 16-byte slots
  // that is 2 048 holes in one page, so four fifths of the reclaimed space was
  // unreachable and the next allocation wave opened fresh pages it did not need.
  // Growing on demand pays the extra metadata only where it buys back space.
  static constexpr size_t kMaxGrownFreeSpans = 4096;
  static constexpr size_t kNoSpanIndex = SIZE_MAX;
  std::array<FreeSpan, kMaxFreeSpans> inline_free_spans;
  // Points at the inline array until the list outgrows it. Every reader and
  // writer of the run list uses this pointer, so there is one representation of
  // "where the runs live" rather than two paths to keep in step.
  FreeSpan* free_spans = inline_free_spans.data();
  size_t free_span_capacity = kMaxFreeSpans;
  std::unique_ptr<FreeSpan[]> grown_free_spans;
  size_t free_span_count = 0;
  size_t free_span_bytes = 0;
  // Largest run currently recorded in this page. It lets a refill ask whether a
  // page can host a slab without walking the run list, and lets a single-object
  // search reject a page with one comparison. Kept exact incrementally: a
  // release can only raise it, and a take only matters when it consumes the last
  // run of that size, which `max_free_span_count` identifies.
  size_t max_free_span = 0;
  // Number of recorded runs whose span equals `max_free_span`. Consuming one of
  // several equal runs leaves the maximum unchanged, so the O(runs) rescan that
  // recomputes it runs once per page rather than once per refill.
  size_t max_free_span_count = 0;
  // Position in the index of pages that record at least one freed run, or
  // `kNoSpanIndex` while this page records none. Keeping the position here makes
  // joining and leaving that index a constant-time swap-remove instead of a
  // lookup, and lets every reuse path iterate only pages that can answer a
  // request.
  size_t span_index = kNoSpanIndex;
  // Position in the index of pages with unused tail space, or `kNoSpanIndex`
  // while the page has none. Refills used to walk the whole page set looking for
  // a page with room and find none, once per TLAB; the index answers that in
  // constant time and holds only pages that can serve a request.
  size_t tail_index = kNoSpanIndex;
  // The fields below were added after the page layout had been tuned around the
  // cache line the allocation fast path touches, so they live at the end of the
  // struct: inserting them beside `used` would push the bitmaps and counters the
  // hot path reads onto a second line for every page.
  //
  // Offset from which this page's storage is known to hold zeros, up to
  // `capacity`. Every managed payload is promised zero-initialised, and the
  // allocator used to write those zeros itself for every object. Storage the OS
  // has just committed is already zero, so the first pass over a page skips the
  // fill and only moves this cursor past the header it wrote. Reclaimed space and
  // a recycled page shell are dirty and set the cursor to `capacity`, which makes
  // the next allocation pay the fill again rather than expose stale bytes.
  size_t zero_cursor = 0;
  // Storage published as a contiguous run whose object-start bits are not
  // written yet. The counters for those objects are already applied when the
  // run is recorded, so the only thing left is discovery: the bits
  // `find_object_locked` tests to decide whether an address is an object start.
  // A page that nothing reaches - a graph that has been dropped, which is what
  // collecting a dead object graph looks like - therefore never reads its
  // payload at all, and a page that marking does reach pays one walk the first
  // time a reference resolves into it.
  struct PendingStartRun {
    uint32_t begin = 0;
    uint32_t end = 0;
  };
  static constexpr size_t kMaxPendingStartRuns = 8;
  std::array<PendingStartRun, kMaxPendingStartRuns> pending_starts{};
  uint8_t pending_start_count = 0;
  bool starts_pending = false;
  // Reclaimed runs this page could not record because its run list was full.
  // Counted per page so a workload can show whether the bound is costing space
  // rather than leaving that to inference.
  size_t dropped_free_bytes = 0;
  // Objects a mutator has written into this page but not yet published. A
  // thread-local object is invisible to the collector until the range that names
  // it is published, so a page whose counters and marks say it is entirely dead
  // can still hold one that a mutator is about to hand out. The whole-page
  // reclamation path therefore has to see this at zero; a page that carries an
  // uncommitted object falls back to the per-object sweep, which only frees the
  // starts it can actually see.
  std::atomic<uint32_t> uncommitted_objects{0};
};

// Drops `count` unpublished objects from a page's tally, saturating at zero so a
// publication that names more objects than the page is charged with cannot wrap
// the counter into a value that looks committed.
void release_uncommitted_objects(GcPage& page, uint64_t count) {
  if (count == 0) return;
  const uint32_t amount =
      count > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(count);
  uint32_t current = page.uncommitted_objects.load(std::memory_order_relaxed);
  while (current != 0) {
    const uint32_t next = current > amount ? current - amount : 0;
    if (page.uncommitted_objects.compare_exchange_weak(
            current, next, std::memory_order_relaxed)) {
      return;
    }
  }
}


inline size_t page_header_bytes(const GcPage& page) {
  return page_header_bytes(page.kind);
}

// Writes the start bits of a page's recorded runs. Defined next to the
// statistics it does not touch, because a page that nothing reaches must never
// run it: collecting a dropped graph then costs no object traffic at all.
void materialize_page_starts_locked(GcPage& page);

// Small pages are discoverable from their own start bitmap, so their objects
// never touch the global index. Only pages without start metadata (large-object
// pages, or a page whose directory insertion failed) need the exact index.
bool page_uses_index(const GcPage* page) {
  return !page || page->use_object_index || !page->object_starts;
}

// Maps page-aligned storage to its page. Small pages are aligned to
// kGcPageBytes, so every address in a page has the same chunk key and the
// table holds one entry per page. The table is only mutated under gc_mutex.
class PageDirectory {
 public:
  bool insert(GcPage* page) {
    if (!page || !page->storage || !page->object_starts) return false;
    if (entries_.empty()) {
      if (!rehash(64)) return false;
    } else if (size_ + tombstones_ + 1 >=
               entries_.size() - entries_.size() / 3) {
      size_t next = entries_.size();
      if (size_ >= entries_.size() / 2) {
        if (entries_.size() > SIZE_MAX / 2) return false;
        next = entries_.size() * 2;
      }
      if (!rehash(next)) return false;
    }
    const uintptr_t key = key_for(page->storage);
    size_t index = hash(key) & (entries_.size() - 1);
    size_t tombstone = entries_.size();
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.key == key) {
        direct_[key & (kDirectEntries - 1)] = DirectEntry{key, entry.page};
        return entry.page == page;
      }
      if (entry.key == kTombstone && tombstone == entries_.size())
        tombstone = index;
      if (entry.key == kEmpty) {
        if (tombstone != entries_.size()) {
          index = tombstone;
          --tombstones_;
        }
        entries_[index] = Entry{key, page};
        ++size_;
        direct_[key & (kDirectEntries - 1)] = DirectEntry{key, page};
        return true;
      }
      index = (index + 1) & (entries_.size() - 1);
    }
    if (tombstone != entries_.size()) {
      entries_[tombstone] = Entry{key, page};
      --tombstones_;
      ++size_;
      direct_[key & (kDirectEntries - 1)] = DirectEntry{key, page};
      return true;
    }
    return false;
  }

  void erase(GcPage* page) {
    if (!page || !page->storage || entries_.empty()) return;
    const uintptr_t key = key_for(page->storage);
    const size_t mask = entries_.size() - 1;
    size_t index = hash(key) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.key == kEmpty) return;
      if (entry.key == key) {
        if (entry.page != page) return;
        entry = Entry{kTombstone, nullptr};
        DirectEntry& direct = direct_[key & (kDirectEntries - 1)];
        if (direct.key == key) direct = DirectEntry{};
        --size_;
        ++tombstones_;
        return;
      }
      index = (index + 1) & mask;
    }
  }

  GcPage* find(void* object) const {
    if (!object || entries_.empty()) return nullptr;
    const uintptr_t key = key_for(object);
    // Page-local metadata is read far more often than the directory changes.
    // A 1024-slot direct map answers the common case with one compare; the hash
    // table below remains authoritative and handles collisions and large pages.
    const DirectEntry& direct = direct_[key & (kDirectEntries - 1)];
    if (direct.key == key) return direct.page;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(key) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      const Entry& entry = entries_[index];
      if (entry.key == kEmpty) return nullptr;
      if (entry.key == key) {
        direct_[key & (kDirectEntries - 1)] = DirectEntry{key, entry.page};
        return entry.page;
      }
      index = (index + 1) & mask;
    }
    return nullptr;
  }

 private:
  static constexpr uintptr_t kEmpty = UINTPTR_MAX;
  static constexpr uintptr_t kTombstone = UINTPTR_MAX - 1;
  static constexpr size_t kDirectEntries = 1024;
  struct Entry {
    uintptr_t key = kEmpty;
    GcPage* page = nullptr;
  };
  struct DirectEntry {
    uintptr_t key = kEmpty;
    GcPage* page = nullptr;
  };

  static uintptr_t key_for(const void* address) {
    return reinterpret_cast<uintptr_t>(address) >> kGcPageShift;
  }

  static size_t hash(uintptr_t key) {
    uint64_t value = static_cast<uint64_t>(key);
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<size_t>(value);
  }

  bool rehash(size_t capacity) {
    try {
      std::vector<Entry> replacement(capacity);
      for (const Entry& entry : entries_) {
        if (entry.key == kEmpty || entry.key == kTombstone) continue;
        const size_t mask = capacity - 1;
        size_t index = hash(entry.key) & mask;
        while (replacement[index].key != kEmpty)
          index = (index + 1) & mask;
        replacement[index] = entry;
      }
      entries_.swap(replacement);
      tombstones_ = 0;
      return true;
    } catch (...) {
      return false;
    }
  }

  std::vector<Entry> entries_;
  // mutable because a const lookup may fill the cache; every caller holds
  // gc_mutex, so this is not shared mutable state across collectors.
  mutable std::array<DirectEntry, kDirectEntries> direct_{};
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

bool test_object_start(const GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return false;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return false;
  return (page.object_starts[index / 64] >> (index % 64)) & 1;
}


void mark_object_start(GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return;
  page.object_starts[index / 64] |= uint64_t{1} << (index % 64);
}

void clear_object_start(GcPage& page, size_t offset) {
  if (!page.object_starts || offset % kObjectStartAlignment != 0) return;
  const size_t index = offset / kObjectStartAlignment;
  if (index / 64 >= page.object_start_words) return;
  page.object_starts[index / 64] &= ~(uint64_t{1} << (index % 64));
}

// ---------------------------------------------------------------------------
// Adaptive object representations
//
// The page kind names the representation, which fixes the header size and
// therefore where the payload begins:
//
//   H0   headerless   payload at the span start; layout and slot from the page
//   H8   compact      {state, layout_id}; one element by definition
//   H16  extended     {element_count, state, layout_id, link}; arrays and large
//
// The object-start bitmap indexes span starts, which are always 16-aligned. A
// payload is `span_start + header_bytes`, so an H8 payload keeps an 8-byte
// alignment and an H0 or H16 payload keeps the full 16.
// ---------------------------------------------------------------------------

// Mutable state encoding, shared by H8 and H16 and composed for H0. Bit 1 is
// young, bit 2 is remembered (reserved: membership is page-local), bits 8-15 are
// the promotion age. The words are asserted rather than described.
constexpr uint32_t kYoungBit = 1u << 1;
constexpr uint32_t kRememberedBit = 1u << 2;
constexpr uint32_t kAgeShift = 8;
constexpr uint32_t kAgeMask = 0xFFu << kAgeShift;
static_assert((kYoungBit & kRememberedBit) == 0, "state fields overlap");
static_assert((kYoungBit & kAgeMask) == 0, "state fields overlap");
static_assert((kRememberedBit & kAgeMask) == 0, "state fields overlap");
static_assert(kAgeShift + 8 <= 32, "the age does not fit the state word");
static_assert(kAgeMask <= 0xFFFFFFFFu, "the age mask leaves the word");
static_assert((kYoungBit >> 1) == 1u, "H0 young bit must stay at bit 1");

inline uint32_t inline_kind_for(PageKind kind) {
  switch (kind) {
    case PageKind::Compact: return NRQ_GC_INLINE_KIND_COMPACT;
    case PageKind::Extended: return NRQ_GC_INLINE_KIND_EXTENDED;
    default: return NRQ_GC_INLINE_KIND_NONE;
  }
}

// H8. A fixed record that needs its own state word and its layout name, and
// nothing else: the element count is always one and birth lives on the page.
struct CompactHeader {
  uint32_t state = 0;
  uint32_t layout_id = 0;
};

// H16. Arrays need the element count back, and a large or fallback page needs a
// way to chain its objects without an intrusive pointer in every object. The
// count is first so the payload is at offset 16 and the SoA lowering's
// `payload - 16` read keeps working unchanged.
struct ExtendedHeader {
  uint32_t element_count = 0;
  uint32_t state = 0;
  uint32_t layout_id = 0;
  // Representation flags. Zero today; kept as the fourth word so the header is
  // exactly 16 bytes and an array-specific flag has a home in Phase 5 without a
  // header resize.
  uint32_t flags = 0;
};

static_assert(sizeof(CompactHeader) == 8, "the compact header must stay 8 bytes");
static_assert(sizeof(ExtendedHeader) == 16,
              "the extended header must stay 16 bytes");
static_assert(alignof(CompactHeader) <= sizeof(uint32_t),
              "the compact header must not demand more alignment than it needs");

// A resolved object: its span start (the header's address) and its page. The
// collector carries both because the representation, geometry, and bitmaps all
// come from the page; re-probing the directory per field is exactly the
// indirection this phase exists to avoid.
struct ObjectRef {
  unsigned char* header = nullptr;
  GcPage* page = nullptr;
  bool valid() const { return header != nullptr && page != nullptr; }
};

inline size_t span_slot(const ObjectRef& ref) {
  return static_cast<size_t>(ref.header - ref.page->storage) /
         kObjectStartAlignment;
}

inline size_t object_header_bytes(const ObjectRef& ref) {
  return page_header_bytes(ref.page->kind);
}

inline void* object_payload(const ObjectRef& ref) {
  return ref.header + object_header_bytes(ref);
}

inline bool object_young(const ObjectRef& ref);
inline const nrq_object_layout* object_layout(const ObjectRef& ref);

// One word of mutable state, wherever this representation keeps it. A headerless
// object has no word of its own, so the page's per-slot byte carries the same
// young bit and a 7-bit age.
// The headerless state table is indexed by object ordinal. The page's slot size
// divides every span start, so the ordinal is exact and needs no bitmap.
inline size_t h0_state_index(const ObjectRef& ref) {
  const GcPage& page = *ref.page;
  if (page.slot_bytes == 0)
    nrq_runtime_fatal("headerless page has no slot size");
  const size_t offset = static_cast<size_t>(ref.header - page.storage);
  if (offset % page.slot_bytes != 0)
    nrq_runtime_fatal("headerless object is not slot-aligned");
  const size_t index = offset / page.slot_bytes;
  if (!page.h0_states || index >= page.h0_state_capacity)
    nrq_runtime_fatal("headerless state index is outside its page");
  return index;
}

inline uint32_t object_state(const ObjectRef& ref) {
  switch (ref.page->kind) {
    case PageKind::Headerless: {
      const uint8_t packed = ref.page->h0_states
                                 ? ref.page->h0_states[h0_state_index(ref)]
                                 : uint8_t{0};
      uint32_t state = (packed & uint8_t{1}) ? kYoungBit : 0u;
      state |= (static_cast<uint32_t>(packed >> 1) << kAgeShift) & kAgeMask;
      return state;
    }
    case PageKind::Compact:
      return reinterpret_cast<const CompactHeader*>(ref.header)->state;
    default:
      return reinterpret_cast<const ExtendedHeader*>(ref.header)->state;
  }
}

inline void set_object_state(const ObjectRef& ref, uint32_t state) {
  switch (ref.page->kind) {
    case PageKind::Headerless: {
      if (!ref.page->h0_states)
        nrq_runtime_fatal("headerless page has no state table");
      const uint32_t age = (state & kAgeMask) >> kAgeShift;
      ref.page->h0_states[h0_state_index(ref)] = static_cast<uint8_t>(
          ((state & kYoungBit) ? 1u : 0u) | ((age & 0x7Fu) << 1));
      return;
    }
    case PageKind::Compact:
      reinterpret_cast<CompactHeader*>(ref.header)->state = state;
      return;
    default:
      reinterpret_cast<ExtendedHeader*>(ref.header)->state = state;
      return;
  }
}

inline uint32_t object_age(const ObjectRef& ref) {
  return (object_state(ref) & kAgeMask) >> kAgeShift;
}

inline bool object_young(const ObjectRef& ref) {
  return (object_state(ref) & kYoungBit) != 0;
}

// Birth is page-local. A bit is meaningful only while the page's stamp matches
// the cycle being asked about, so the first allocation of a cycle clears the
// bitmap and no page walk is needed at a cycle boundary.
inline bool object_born_in_cycle(const ObjectRef& ref, uint64_t epoch) {
  const GcPage& page = *ref.page;
  if (!page.born_bits || page.born_epoch < epoch) return false;
  const size_t index = span_slot(ref);
  if (index / 64 >= page.born_words) return false;
  return (page.born_bits[index / 64] >> (index % 64)) & 1;
}

inline void mark_object_born(const ObjectRef& ref, uint64_t epoch) {
  GcPage& page = *ref.page;
  if (!page.born_bits) return;
  if (page.born_epoch < epoch) {
    std::memset(page.born_bits, 0, page.born_words * sizeof(uint64_t));
    page.born_epoch = epoch;
    page.born_this_cycle = 0;
  }
  const size_t index = span_slot(ref);
  if (index / 64 >= page.born_words)
    nrq_runtime_fatal("GC object birth bit is outside its page");
  uint64_t& word = page.born_bits[index / 64];
  const uint64_t bit = uint64_t{1} << (index % 64);
  if ((word & bit) == 0) {
    word |= bit;
    if (page.born_this_cycle == SIZE_MAX)
      nrq_runtime_fatal("GC page born-object count overflow");
    ++page.born_this_cycle;
  }
}

inline uint32_t object_element_count(const ObjectRef& ref) {
  if (ref.page->kind == PageKind::Headerless ||
      ref.page->kind == PageKind::Compact)
    return 1;
  return reinterpret_cast<const ExtendedHeader*>(ref.header)->element_count;
}

// Off-heap mark and remembered bit index. Both bitmaps use the same slot grid as
// the object-start bitmap, so this is pure pointer arithmetic.
inline size_t mark_bit_index(const ObjectRef& ref) { return span_slot(ref); }

GcPage* page_for_object(const void* object);

inline bool object_marked(const ObjectRef& ref) {
  const GcPage& page = *ref.page;
  if (!page.mark_bits) return false;
  const size_t index = mark_bit_index(ref);
  if (index / 64 >= page.mark_words) return false;
  return (page.mark_bits[index / 64] >> (index % 64)) & 1;
}

// Sets or clears a mark and keeps the page's survivor counts exact. The counts
// are what let sweep classify a whole page without reading its objects.
inline void set_object_marked(const ObjectRef& ref, bool value) {
  GcPage& page = *ref.page;
  if (!page.mark_bits)
    nrq_runtime_fatal("GC object has no page mark bitmap");
  const size_t index = mark_bit_index(ref);
  if (index / 64 >= page.mark_words)
    nrq_runtime_fatal("GC object mark bit is outside its page");
  uint64_t& word = page.mark_bits[index / 64];
  const uint64_t bit = uint64_t{1} << (index % 64);
  const bool was_marked = (word & bit) != 0;
  if (value == was_marked) return;
  if (value) {
    word |= bit;
    ++page.marked_objects;
    if (object_young(ref)) ++page.marked_young_objects;
  } else {
    word &= ~bit;
    if (page.marked_objects == 0)
      nrq_runtime_fatal("GC page marked-object count underflow");
    --page.marked_objects;
    if (object_young(ref)) {
      if (page.marked_young_objects == 0)
        nrq_runtime_fatal("GC page marked-young count underflow");
      --page.marked_young_objects;
    }
  }
}

// A page whose objects are all live (or all dead) needs no per-object walk. The
// one write that remains is clearing its marks so the next cycle starts clean.
inline void clear_page_marks(GcPage& page) {
  if (page.mark_bits && page.mark_words != 0) {
    std::memset(page.mark_bits, 0, page.mark_words * sizeof(uint64_t));
  }
  page.marked_objects = 0;
  page.marked_young_objects = 0;
}

inline bool object_remembered(const ObjectRef& ref) {
  const GcPage& page = *ref.page;
  if (!page.remembered_bits) return false;
  const size_t index = mark_bit_index(ref);
  if (index / 64 >= page.remembered_words) return false;
  return (page.remembered_bits[index / 64] >> (index % 64)) & 1;
}

inline void set_object_remembered(const ObjectRef& ref, bool value) {
  GcPage& page = *ref.page;
  if (!page.remembered_bits)
    nrq_runtime_fatal("GC object has no page remembered bitmap");
  const size_t index = mark_bit_index(ref);
  if (index / 64 >= page.remembered_words)
    nrq_runtime_fatal("GC object remembered bit is outside its page");
  uint64_t& word = page.remembered_bits[index / 64];
  const uint64_t bit = uint64_t{1} << (index % 64);
  const bool was_remembered = (word & bit) != 0;
  if (value == was_remembered) return;
  if (value) {
    word |= bit;
    ++page.remembered_objects;
  } else {
    word &= ~bit;
    if (page.remembered_objects == 0)
      nrq_runtime_fatal("GC page remembered count underflow");
    --page.remembered_objects;
  }
}

inline void clear_page_remembered(GcPage& page) {
  if (page.remembered_bits && page.remembered_words != 0) {
    std::memset(page.remembered_bits, 0,
                page.remembered_words * sizeof(uint64_t));
  }
  page.remembered_objects = 0;
}

// Pull the next object's header and payload into cache while the current one is
// traced. The worklist already has the pointer; this only changes when the
// cache line arrives.
inline void prefetch_object(const ObjectRef& ref) {
  if (!ref.valid()) return;
#if defined(_MSC_VER) && defined(_M_X64)
  _mm_prefetch(reinterpret_cast<const char*>(ref.header), _MM_HINT_T0);
  _mm_prefetch(reinterpret_cast<const char*>(object_payload(ref)), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(ref.header);
  __builtin_prefetch(object_payload(ref));
#endif
}

// The largest standard representation is 16 bytes. The three sizes are asserted
// here as well as at the structs so a change to a page kind's header cannot
// silently turn an extended object into something larger.
static_assert(page_header_bytes(PageKind::Headerless) == 0,
              "the headerless representation must be zero bytes");
static_assert(page_header_bytes(PageKind::Compact) == 8,
              "the compact representation must be 8 bytes");
static_assert(page_header_bytes(PageKind::Extended) == 16,
              "the extended representation must be 16 bytes");
static_assert(page_header_bytes(PageKind::Extended) <= 16,
              "no standard representation may exceed 16 bytes");

// Storage a managed object occupies, header included, rounded up to the span
// alignment the bump allocator uses. The header size is a parameter instead of a
// constant: that is the whole of the representation change at this level.
inline size_t object_span_bytes(size_t header_bytes, size_t payload_bytes) {
  constexpr size_t alignment = alignof(std::max_align_t);
  const size_t raw = header_bytes + payload_bytes;
  return (raw + alignment - 1) & ~(alignment - 1);
}

// The payload size an object reports: its element count times the element size
// its shared descriptor names. One multiply, no per-instance size field, and it
// cannot disagree with the descriptor.
inline size_t object_payload_bytes(const ObjectRef& ref) {
  return static_cast<size_t>(object_element_count(ref)) *
         object_layout(ref)->payload_size;
}

// Span of one concrete object. A headerless page knows the answer from its slot
// size alone, which is what lets publication walk a uniform range without
// reading a header.

// Layout descriptors are interned by every property that changes how the
// payload is interpreted. Matching sizes alone is not enough: two records of the
// same size with reference words at different offsets, or with a different trace
// callback, must not share a descriptor.
struct LayoutKey {
  size_t payload_bytes;
  size_t element_bytes;
  nrq_gc_trace_fn trace;
  std::vector<uint32_t> offsets;

  bool operator==(const LayoutKey& other) const {
    return payload_bytes == other.payload_bytes &&
           element_bytes == other.element_bytes && trace == other.trace &&
           offsets == other.offsets;
  }
};

struct LayoutKeyHash {
  size_t operator()(const LayoutKey& key) const {
    size_t hash = key.payload_bytes * static_cast<size_t>(0x9E3779B97F4A7C15ull);
    hash ^= key.element_bytes * static_cast<size_t>(0xC2B2AE3D27D4EB4Full);
    hash ^= reinterpret_cast<size_t>(key.trace) >> 4;
    for (const uint32_t offset : key.offsets) hash = (hash * 31u) ^ offset;
    return hash;
  }
};

// Descriptors and their offset tables are never reclaimed, so a live object can
// always read the layout it was born with and pointer equality is a valid
// identity test. The table itself is only touched while a layout is created;
// allocation and tracing read the descriptor through a plain pointer.
std::mutex layout_mutex;
std::unordered_map<LayoutKey, const nrq_object_layout*, LayoutKeyHash> layout_pool;
// Bytes the published descriptors and their offset tables occupy. They are never
// freed, so this is the whole structural cost of sharing.
size_t layout_bytes_total = 0;

// Layout identity for compact and extended headers. A 32-bit id fits both, and
// tracing resolves it with one indexed load; the descriptor itself stays in the
// interning pool. Ids are assigned under layout_mutex and the table entry is
// published with a release store, so tracing never takes that lock.
constexpr uint32_t kMaxLayoutIds = 1u << 14;
std::array<std::atomic<const nrq_object_layout*>, kMaxLayoutIds> layout_by_id{};
std::atomic<uint32_t> layout_id_count{0};
std::unordered_map<const nrq_object_layout*, uint32_t> layout_id_pool;

inline const nrq_object_layout* layout_from_id(uint32_t id) {
  if (id == 0 || id > kMaxLayoutIds) return nullptr;
  return layout_by_id[id - 1].load(std::memory_order_acquire);
}

const nrq_object_layout* object_layout(const ObjectRef& ref) {
  if (ref.page->kind == PageKind::Headerless) {
    if (!ref.page->uniform_layout)
      nrq_runtime_fatal("headerless page has no layout");
    return ref.page->uniform_layout;
  }
  const uint32_t id =
      ref.page->kind == PageKind::Compact
          ? reinterpret_cast<const CompactHeader*>(ref.header)->layout_id
          : reinterpret_cast<const ExtendedHeader*>(ref.header)->layout_id;
  const nrq_object_layout* layout = layout_from_id(id);
  if (!layout) nrq_runtime_fatal("GC object names an unknown layout id");
  return layout;
}

// A thread remembers the last few (descriptor, id) pairs. Descriptors are few
// and long-lived; the id is wanted on every allocation, so the common case has
// to be a handful of compares rather than a locked table probe.
struct LayoutIdMemo {
  static constexpr size_t kCapacity = 8;
  const nrq_object_layout* layouts[kCapacity] = {};
  uint32_t ids[kCapacity] = {};
  size_t next = 0;
};
thread_local LayoutIdMemo layout_id_memo;

uint32_t layout_id_for(const nrq_object_layout* layout) {
  if (!layout) return 0;
  for (size_t index = 0; index < LayoutIdMemo::kCapacity; ++index) {
    if (layout_id_memo.layouts[index] == layout)
      return layout_id_memo.ids[index];
  }
  std::lock_guard<std::mutex> lock(layout_mutex);
  uint32_t id = 0;
  const auto existing = layout_id_pool.find(layout);
  if (existing != layout_id_pool.end()) {
    id = existing->second;
  } else {
    const uint32_t next =
        layout_id_count.load(std::memory_order_relaxed) + 1;
    if (next > kMaxLayoutIds) return 0;
    layout_by_id[next - 1].store(layout, std::memory_order_release);
    layout_id_count.store(next, std::memory_order_release);
    try {
      layout_id_pool.emplace(layout, next);
    } catch (...) {
      return 0;
    }
    id = next;
  }
  layout_id_memo.layouts[layout_id_memo.next] = layout;
  layout_id_memo.ids[layout_id_memo.next] = id;
  layout_id_memo.next = (layout_id_memo.next + 1) % LayoutIdMemo::kCapacity;
  return id;
}

// The ordinary `void*` root is the one shape used by every slot that holds a
// single managed pointer. Its descriptor is immutable and lives for the process,
// so it needs no intern lookup and its address is a stable identity. It is
// deliberately not in layout_pool: nrq_gc_layout_bytes and layout_bytes_total
// describe pool-owned descriptors and this process-lifetime constant is neither
// charged to them nor counted by nrq_gc_layout_count.
constexpr uint32_t kPointerRootOffset = 0;
const nrq_object_layout kPointerRootLayout{
    static_cast<uint32_t>(sizeof(void*)), 1, 0, &kPointerRootOffset,
    nullptr, nullptr, 0};

// Allocation must not take the pool lock. A thread remembers the last layout it
// asked for and proves the match by comparing the offset table itself, so a
// compiler-emitted constant table and a caller that rebuilds the same table on
// its stack both hit without touching the pool. Only a genuinely new layout
// reaches the interning table.
// The memo holds the interned descriptor and nothing else. Every field it checks
// comes from that descriptor, which the runtime owns for the life of the process,
// so a hit is decided without touching storage the caller owns.
//
// It must not cache the caller's offset table. The typed allocation entry points
// document that the collector copies the table, which means callers rebuild it on
// the stack and pass a pointer whose lifetime is the call: caching that pointer
// would read expired storage on the next allocation, and comparing two calls by
// it would accept a table that has since been rebuilt with different contents.
struct LayoutMemo {
  // The memo used to be a four-entry ring. That is enough for a handful of
  // interleaved shapes and nothing more: a program alternating eight shapes
  // evicted every entry before it came round again, so every allocation took the
  // interning path. It is now a direct-mapped table indexed by the shape itself,
  // which keeps the hit path at one hash and one comparison no matter how many
  // shapes a program uses, and gives unrelated shapes their own entries instead
  // of making them compete for a shared ring.
  // Sixteen sets, two ways each. A direct-mapped table is one comparison but a
  // single hash collision between two hot shapes would make both miss forever;
  // the second way absorbs exactly that case for 256 bytes of thread-local
  // storage.
  static constexpr size_t kSets = 16;
  static constexpr size_t kWays = 2;
  static constexpr size_t kCapacity = kSets * kWays;
  const nrq_object_layout* layouts[kCapacity] = {};
  // One replacement bit per set, flipped on every insert into that set.
  uint32_t victim = 0;

  static uint64_t mix(uint64_t value) {
    // splitmix64 finalizer: a few multiplies and shifts, no branches, and it
    // spreads the small, highly correlated sizes real layouts have.
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  // The shape is the key: element size, reference count, array/record kind, any
  // custom tracer, and the two offsets that distinguish tables of equal length.
  static size_t set_for(size_t element_size, size_t count, bool repeated,
                        nrq_gc_trace_fn trace, const size_t* offsets) {
    uint64_t key = mix(static_cast<uint64_t>(element_size) |
                       (static_cast<uint64_t>(count) << 32));
    key ^= mix(static_cast<uint64_t>(repeated ? 1u : 2u));
    key ^= mix(reinterpret_cast<uintptr_t>(trace));
    if (count != 0 && offsets) {
      key ^= mix(static_cast<uint64_t>(offsets[0]) + 0x51ED270B);
      key ^= mix(static_cast<uint64_t>(offsets[count - 1]) * 3u);
    }
    return static_cast<size_t>(key) & (kSets - 1);
  }
};
thread_local LayoutMemo layout_memo;
thread_local uint64_t layout_memo_hits = 0;
thread_local uint64_t layout_memo_misses = 0;

bool layout_memo_matches(const nrq_object_layout* layout, size_t bytes,
                         size_t element_size, nrq_gc_trace_fn trace,
                         const size_t* offsets, size_t count, bool repeated) {
  if (!layout) return false;
  // The descriptor describes exactly this shape, or it does not: same element
  // size, same tracing, same reference count, same array/record kind.
  if (layout->payload_size != element_size) return false;
  if (layout->trace != trace) return false;
  if (layout->reference_count != count) return false;
  const bool describes_an_array =
      (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0;
  if (describes_an_array != repeated) return false;
  if (count == 0) return true;
  const uint32_t* owned = layout->reference_offsets;
  if (!owned || !offsets) return false;
  // Compared against the descriptor's table, which is immutable and outlives
  // every caller. The incoming table is read only while its caller is on the
  // stack, which is exactly the window in which it is alive.
  for (size_t index = 0; index < count; ++index) {
    if (static_cast<size_t>(owned[index]) != offsets[index]) return false;
  }
  return true;
}

// The descriptor a payload uses. A record passes its size as both bytes and
// element size; an array passes its element stride, which repeats. Payload
// offsets are always relative to the element, which is the whole record for a
// fixed layout.
const nrq_object_layout* layout_for_allocation(size_t bytes,
                                               size_t element_size,
                                               nrq_gc_trace_fn trace,
                                               const size_t* offsets,
                                               size_t count, bool repeated) {
  const size_t payload_bytes = element_size;
  // `repeated` is stated by the entry point, not inferred from the byte counts:
  // a one-element container has bytes == element_size exactly like a fixed
  // record, and describing it as one would hide its element count.
  const size_t element_bytes = repeated ? element_size : 0;
  const size_t set = LayoutMemo::set_for(element_size, count, repeated, trace,
                                        offsets);
  const size_t base = set * LayoutMemo::kWays;
  for (size_t way = 0; way < LayoutMemo::kWays; ++way) {
    const nrq_object_layout* cached = layout_memo.layouts[base + way];
    if (cached && layout_memo_matches(cached, bytes, element_size, trace,
                                      offsets, count, repeated)) {
      ++layout_memo_hits;
      return cached;
    }
  }
  ++layout_memo_misses;
  const nrq_object_layout* layout = nrq_gc_layout_intern(
      payload_bytes, element_bytes, trace, offsets, count);
  if (layout) {
    const uint32_t bit = 1u << set;
    const size_t way = (layout_memo.victim & bit) != 0 ? 1 : 0;
    layout_memo.victim ^= bit;
    layout_memo.layouts[base + way] = layout;
  }
  return layout;
}

// Reclaimed storage is tracked per page so later allocations can reuse it;
// defined with the page pool below, declared here because sweeping reaches them.
void release_span_locked(GcPage& page, size_t offset, size_t span);
void set_page_max_free_span_locked(GcPage& page, size_t span, size_t count);
GcPage* take_free_span_locked(size_t span, size_t& offset, PageKind kind);

// Marking keeps a FIFO ring of pending headers. Contiguous storage avoids
// chasing an intrusive list through the heap and visits a graph level by level,
// which keeps related objects close in time. Objects beyond the ring capacity
// spill to an external vector; its capacity is retained across cycles, so the
// common path allocates only when a heap first exceeds the inline ring.
class MarkWorklist {
 public:
  static constexpr size_t kInlineCapacity = 256;

  bool empty() const { return count_ == 0 && overflow_.empty(); }

  void push(const ObjectRef& ref) {
    if (!ref.valid()) return;
    if (count_ < kInlineCapacity) {
      inline_[(head_ + count_) & (kInlineCapacity - 1)] = ref;
      ++count_;
      return;
    }
    try {
      overflow_.push_back(ref);
    } catch (...) {
      nrq_runtime_fatal("GC mark worklist allocation failed");
    }
  }

  ObjectRef pop() {
    if (count_ != 0) {
      const ObjectRef ref = inline_[head_];
      head_ = (head_ + 1) & (kInlineCapacity - 1);
      --count_;
      if (count_ != 0) {
        prefetch_object(inline_[head_]);
      } else if (!overflow_.empty()) {
        prefetch_object(overflow_.back());
      }
      return ref;
    }
    if (overflow_.empty()) return ObjectRef{};
    const ObjectRef ref = overflow_.back();
    overflow_.pop_back();
    if (!overflow_.empty()) prefetch_object(overflow_.back());
    return ref;
  }

  // Capacity is retained across cycles: the vector only allocates when a mark
  // slice first exceeds the inline ring, not once per object or once per cycle.
  void reset() {
    head_ = 0;
    count_ = 0;
    overflow_.clear();
  }

 private:
  std::array<ObjectRef, kInlineCapacity> inline_{};
  size_t head_ = 0;
  size_t count_ = 0;
  std::vector<ObjectRef> overflow_;
};

// The collector needs exact-address lookup while marking, but only allocation
// mutates this table. Open addressing keeps pointer metadata contiguous and
// avoids one node allocation per managed object.
class ObjectIndex {
 public:
  // Number of exact address entries. Small objects are found through their
  // page's start bitmap, so this only grows with large-object pages.
  size_t size() const { return size_; }

  ObjectRef find(void* object) const {
    if (!object || entries_.empty()) return ObjectRef{};
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      const Entry& entry = entries_[index];
      if (entry.object == nullptr) return ObjectRef{};
      if (entry.object == object) return ObjectRef{entry.header, entry.page};
      index = (index + 1) & mask;
    }
    return ObjectRef{};
  }

  // Large and fallback pages are not in the page directory, so their exact
  // entries carry the owning page as well as the header.
  GcPage* find_page(void* object) const {
    if (!object || entries_.empty()) return nullptr;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      const Entry& entry = entries_[index];
      if (entry.object == nullptr) return nullptr;
      if (entry.object == object) return entry.page;
      index = (index + 1) & mask;
    }
    return nullptr;
  }

  bool insert(void* object, unsigned char* header, GcPage* page) {
    if (!object || !header) return false;
    const bool needs_rehash = entries_.empty() ||
        size_ + tombstones_ + 1 >= entries_.size() - entries_.size() / 3;
    if (needs_rehash) {
      size_t next = 64;
      if (!entries_.empty()) {
        if (size_ >= entries_.size() / 2) {
          if (entries_.size() > SIZE_MAX / 2) return false;
          next = entries_.size() * 2;
        } else {
          next = entries_.size();
        }
      }
      if (!rehash(next)) return false;
    }
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    size_t tombstone = entries_.size();
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.object == object) return false;
      if (entry.object == tombstone_key() && tombstone == entries_.size())
        tombstone = index;
      if (entry.object == nullptr) {
        if (tombstone != entries_.size()) {
          index = tombstone;
          --tombstones_;
        }
        entries_[index] = Entry{object, header, page};
        ++size_;
        return true;
      }
      index = (index + 1) & mask;
    }
    if (tombstone != entries_.size()) {
      entries_[tombstone] = Entry{object, header, page};
      --tombstones_;
      ++size_;
      return true;
    }
    return false;
  }

  bool erase(void* object) {
    if (!object || entries_.empty()) return false;
    const size_t mask = entries_.size() - 1;
    size_t index = hash(object) & mask;
    for (size_t probes = 0; probes < entries_.size(); ++probes) {
      Entry& entry = entries_[index];
      if (entry.object == nullptr) return false;
      if (entry.object == object) {
        entry = Entry{tombstone_key(), nullptr};
        --size_;
        ++tombstones_;
        return true;
      }
      index = (index + 1) & mask;
    }
    return false;
  }

 private:
  struct Entry {
    void* object = nullptr;
    unsigned char* header = nullptr;
    GcPage* page = nullptr;
  };
  static void* tombstone_key() { return reinterpret_cast<void*>(uintptr_t{1}); }

  static size_t hash(void* object) {
    uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(object));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return static_cast<size_t>(value);
  }

  bool rehash(size_t capacity) {
    try {
      std::vector<Entry> replacement(capacity);
      for (const Entry& entry : entries_) {
        if (!entry.object || entry.object == tombstone_key()) continue;
        const size_t mask = capacity - 1;
        size_t index = hash(entry.object) & mask;
        while (replacement[index].object != nullptr)
          index = (index + 1) & mask;
        replacement[index] = entry;
      }
      entries_.swap(replacement);
      tombstones_ = 0;
      return true;
    } catch (...) {
      return false;
    }
  }

  std::vector<Entry> entries_;
  size_t size_ = 0;
  size_t tombstones_ = 0;
};

// Reference counts held by collector storage are registered per object, because
// a layout only describes traced edges and two payloads of the same geometry can
// differ in which words carry a count.

// A reclaimed payload has nothing to run before its span is reused, except the
// reference counts it still holds: sweeping the object makes the collector their
// last owner, so they are dropped here. Everything else is either a traced
// reference (the collector keeps it alive) or plain bytes.
inline void forget_object_storage(const ObjectRef& object) {
  nrq_gc_release_shared_fields(object_payload(object));
}

// The heap lock, instrumented with the one invariant the handshake depends on: no
// thread may hold it while waiting for a fast-barrier region to drain, because a
// region can itself need the lock to reserve a TLAB. That would be a deadlock, so
// wait_for_fast_barriers() checks the flag instead of trusting every caller.
thread_local bool heap_lock_held_by_this_thread = false;

class CheckedHeapMutex {
 public:
  void lock() {
    mutex_.lock();
    heap_lock_held_by_this_thread = true;
  }
  void unlock() {
    heap_lock_held_by_this_thread = false;
    mutex_.unlock();
  }
 private:
  std::mutex mutex_;
};

class HeapLockGuard {
 public:
  explicit HeapLockGuard(CheckedHeapMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
  HeapLockGuard(const HeapLockGuard&) = delete;
  HeapLockGuard& operator=(const HeapLockGuard&) = delete;
  ~HeapLockGuard() { mutex_.unlock(); }
 private:
  CheckedHeapMutex& mutex_;
};

CheckedHeapMutex gc_mutex;
// Serializes complete collection cycles while leaving marking slices and the
// sweep open to concurrent mutator execution.
std::mutex collection_cycle_mutex;

enum class CollectionPhase { Idle, Preparing, Marking, Sweeping };
constexpr size_t kRememberedBufferCapacity = 64;
constexpr size_t kTransferPinBufferCapacity = 32;

// A pending transfer-pin adjustment. Managed returns hand one pin off per call;
// routing every one of them through the global pin table would take the
// collector mutex on every callee return. A registered mutator instead records a
// signed delta in its own buffer, and the collector folds the deltas into the
// table when it prepares a cycle. Deltas are matched per object, so a handoff
// that is released before the next cycle leaves no trace at all.
struct TransferPinDelta {
  void* object = nullptr;
  int64_t delta = 0;
};

struct MutatorState {
  size_t depth = 0;
  bool parked = false;
  // Thread-local fast-allocation buffer shared with compiler-inlined sequences
  // via `nrq_gc_fast_tls`. Initialized on mutator registration and valid for
  // the lifetime of every registered entry in `mutators`.
  NrqGcFastTls* fast = nullptr;
  uint8_t* tlab_slab_end = nullptr;
  uint8_t* ext_slab_end = nullptr;
  size_t tlab_accounted_pending_bytes = 0;
  // Young flag of the first object in the current publication window. A window
  // is kept to one page and closed at every page switch, so this describes every
  // object the window's counters include; a change in the global generational
  // mode during the window falls back to the exact per-object walk.
  uint8_t window_young = 0;
  std::vector<NrqGcPendingRange> deferred_ranges;
  uint64_t tlab_generation = 0;
  const nrq_object_layout* h0_layout = nullptr;
  size_t h0_slot = 0;
  uint64_t h0_generation = 0;
  static constexpr size_t kPendingRangeCapacity = NRQ_GC_INLINE_PENDING_CAPACITY;
  // Depth of lock-free barrier regions this thread is currently inside. Each
  // mutator owns its own cache line, so a thread's store barrier never
  // invalidates another thread's copy; the collector reads the counters only
  // when it needs the handshake, and only over mutators that are registered.
  alignas(nirq::nrq_cache_line_bytes) std::atomic<uint32_t> barrier_depth{0};
  // Old-to-young stores buffer raw referents so ordinary stores do not need the
  // collector mutex. The owner drains this buffer at a safepoint/leave; the
  // collector drains every registered mutator after parking them.
  void* remembered[kRememberedBufferCapacity]{};
  size_t remembered_count = 0;
  // Transfer-pin deltas this thread has recorded but not yet folded into the
  // global pin table. Owner-only; the collector reads it after parking.
  TransferPinDelta transfer_pins[kTransferPinBufferCapacity]{};
  size_t transfer_pin_count = 0;
  // Short reclaimed runs this thread has taken but not handed out yet. A heap
  // whose free space is a scatter of few-object runs can only answer a refill
  // with a few cells, and paying the allocator lock once per few allocations
  // costs more than those cells are worth. The list is thread-owned, so taking a
  // run from it is a decrement, not a lock.
  //
  // It never survives a cycle boundary: prepare_cycle_locked returns whatever is
  // left to its page before the reclaimer runs. A page can only be recycled once
  // all of its space is either published or recorded as free, and a run held
  // here is neither.
  struct SpareRun {
    GcPage* page = nullptr;
    size_t offset = 0;
    size_t span = 0;
  };
  static constexpr size_t kSpareRunCapacity = 24;
  SpareRun spare_runs[kSpareRunCapacity];
  size_t spare_run_count = 0;
  // The list is shared by the ordinary reservation and the headerless one, and
  // each of those keeps its own cursor stamp. Without its own stamp a compact
  // refill could treat runs reserved by a headerless refill as stale and abandon
  // them: their space is then neither published nor free, so no page that held
  // them could ever be recycled.
  uint64_t spare_generation = 0;
  // Unused reservation space this thread has given up, and the largest single
  // tail. Owner-only: only the owner can move its cursor, so no lock is needed,
  // and a thread that leaves folds its totals into the global counters.
  uint64_t tlab_tail_bytes = 0;
  uint64_t tlab_tail_max_bytes = 0;
  uint64_t tlab_tail_reclaimed_bytes = 0;
};
void drain_transfer_pins_locked(MutatorState& mutator);

std::mutex mutator_mutex;
// Mutators wait here for a collection request to clear.
std::condition_variable mutator_condition;
// The collector waits here for mutators to park. Keeping it separate from
// mutator_condition means a parking mutator wakes only the collector instead of
// every thread that is already parked or waiting to enter the registry.
std::condition_variable collector_condition;
std::vector<MutatorState*> mutators;
// Number of registered mutators, maintained with mutator_mutex. The collector
// compares it against the parked count instead of walking the registry each time
// the wait predicate is evaluated.
size_t registered_mutator_count = 0;
// Number of mutators currently parked. Written by the mutator as it parks and as
// it resumes, under mutator_mutex and published with release ordering.
std::atomic<size_t> parked_mutator_count{0};
// Read by every safepoint poll without a lock. Writers hold mutator_mutex and
// publish with release ordering; waiters re-check while holding that mutex.
std::atomic<bool> collection_requested{false};
thread_local bool thread_is_collector = false;
thread_local MutatorState thread_mutator;
// Objects whose page cannot be walked by a start bitmap, in publication order.
// They are held as resolved references in one contiguous vector: large objects
// are rare, the vector is reused across cycles, and it replaces the intrusive
// link that used to cost every object a pointer slot.
std::vector<ObjectRef> unindexed_objects;
ObjectIndex object_index;
PageDirectory page_directory;

// Resolve an object's page without a per-object pointer. Ordinary pages are one
// direct-mapped directory probe (hash fallback only on a collision); large and
// fallback pages carry their page in the exact index entry that already resolves
// their object. Neither path allocates or takes a lock.
GcPage* page_for_object(const void* object) {
  if (!object) return nullptr;
  GcPage* page = page_directory.find(const_cast<void*>(object));
  if (page) return page;
  return object_index.find_page(const_cast<void*>(object));
}

// Resolve a published object through page-local start metadata first. The bitmap
// records span starts, so the header is `payload - header_bytes(page kind)` and
// the payload comparison is what makes an interior or stale pointer miss. Any
// page without a walkable bitmap falls back to the exact ObjectIndex. Callers
// hold gc_mutex.
ObjectRef find_object_locked(void* object);

// A narrow record's child is usually in the same page as the record itself: a
// chain is allocated sequentially, so consecutive nodes share a page far more
// often than not. Resolving the child against the page already in hand replaces a
// page-directory probe with one range compare. A child outside the page takes the
// ordinary path, and a slot this page does not consider an object start is the
// same decisive miss find_object_locked reports.
ObjectRef find_child_in_page_locked(void* object, const GcPage* page) {
  if (!page || !page->object_starts) return find_object_locked(object);
  if (page->starts_pending)
    materialize_page_starts_locked(*const_cast<GcPage*>(page));
  const auto* address = static_cast<const unsigned char*>(object);
  if (address < page->storage) return find_object_locked(object);
  const size_t payload_offset = static_cast<size_t>(address - page->storage);
  if (payload_offset >= page->capacity) return find_object_locked(object);
  const size_t header_bytes = page_header_bytes(*page);
  if (payload_offset < header_bytes) return ObjectRef{};
  const size_t span_offset = payload_offset - header_bytes;
  if (!test_object_start(*page, span_offset)) return ObjectRef{};
  return ObjectRef{page->storage + span_offset, const_cast<GcPage*>(page)};
}

ObjectRef find_object_locked(void* object) {
  if (!object) return ObjectRef{};
  GcPage* page = page_directory.find(object);
  if (page && page->object_starts) {
    // Publication recorded the run but not its start bits. The first reference
    // into the page writes them here, so a page nothing reaches is never walked.
    if (page->starts_pending) materialize_page_starts_locked(*page);
    const auto* address = static_cast<const unsigned char*>(object);
    if (address >= page->storage) {
      const size_t payload_offset = static_cast<size_t>(address - page->storage);
      const size_t header_bytes = page_header_bytes(*page);
      if (payload_offset >= header_bytes && payload_offset < page->capacity) {
        const size_t span_offset = payload_offset - header_bytes;
        if (test_object_start(*page, span_offset))
          return ObjectRef{page->storage + span_offset, page};
      }
    }
    // The address lies inside a page whose start bitmap is authoritative, so a
    // cleared slot is a decisive miss: interior pointers, stale addresses and
    // garbage all end here without paying for a hash probe of the global index.
    return ObjectRef{};
  }
  return object_index.find(static_cast<void*>(object));
}

// count * element_size, reporting overflow through the return value and the
// product through `bytes`. The obvious guard divides by element_size first, and a
// 64-bit divide is one of the most expensive instructions on the path - it cost
// more than the rest of the entry point's arithmetic. The high half of the product
// answers the same question with one multiply.
bool multiply_size_overflowed(size_t count, size_t element_size, size_t& bytes) {
#if defined(_MSC_VER) && defined(_M_X64)
  unsigned __int64 high = 0;
  bytes = static_cast<size_t>(_umul128(count, element_size, &high));
  return high != 0;
#else
  if (element_size != 0 && count > SIZE_MAX / element_size) return true;
  bytes = count * element_size;
  return false;
#endif
}

constexpr size_t kGcTlabBytes = kGcPageBytes;
// The smallest reclaimed run worth a reservation. A short run still serves
// allocations, but taking one for a slab spends a lock acquisition on a few
// cells; see reserve_tlab_locked.
constexpr size_t kGcTlabMinSlabBytes = kGcTlabBytes / 8;
std::vector<std::unique_ptr<GcPage>> gc_pages;

// Visits every live object whose page is in the set, in page and address order,
// then the objects whose page has no walkable bitmap. Diagnostics use this
// instead of a heap-wide list, which allocation no longer maintains. Callers
// hold gc_mutex and are outside a sweep slice; objects detached into the sweep
// list are not visited, which matches what the old single list reported while a
// cycle was sweeping.
// The only way a walk may read a page's start bits. Publication may have recorded
// a window of objects as runs without writing their start bits, so a scan that
// read the bitmap directly would travel a page whose live objects are invisible
// to it - and an unseen live object is how a collector reclaims something still
// in use. Every walk goes through here, so no new caller can forget. The sweep is
// the one exception: it reads the page's counters, not its bits.
const uint64_t* start_bits_for_scan_locked(GcPage& page) {
  if (page.starts_pending) materialize_page_starts_locked(page);
  if (page_uses_index(&page) || page.object_start_words == 0) return nullptr;
  return page.object_starts;
}

template <typename Visit>
void for_each_live_object_locked(Visit&& visit) {
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page) continue;
    const uint64_t* const start_bits = start_bits_for_scan_locked(*page);
    if (!start_bits) continue;
    for (size_t word = 0; word < page->object_start_words; ++word) {
      uint64_t bits = start_bits[word];
      while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        const size_t span_offset =
            (word * 64 + index) * kObjectStartAlignment;
        visit(ObjectRef{page->storage + span_offset, page});
      }
    }
  }
  for (const ObjectRef& ref : unindexed_objects) visit(ref);
}
// Pages that currently record at least one freed run. First-fit reuse used to
// scan every page in allocation order, so a refill cost grew with the heap even
// though almost every page had nothing to offer. The index holds only pages that
// can answer a request, and `GcPage::span_index` makes membership a swap-remove,
// so no path ever walks the page set to maintain it.
std::vector<GcPage*> span_pages;
// Number of pages marked `retired`, which are exactly the pages a cycle
// boundary can reclaim. The prepare step keeps this count so it can skip its
// page walk entirely when no page was emptied during the previous sweep.
size_t retired_page_count = 0;

// Join or leave the index of pages with reusable runs. Both operations are
// constant-time and neither allocates on the removal path. A page that cannot be
// indexed (the vector failed to grow) simply stays out of it: its runs remain
// recorded and are still merged, but the first-fit paths will not reach them,
// which is the behaviour the allocator had before the index existed.
void index_span_page_locked(GcPage& page) {
  if (page.span_index != GcPage::kNoSpanIndex) return;
  try {
    span_pages.push_back(&page);
    page.span_index = span_pages.size() - 1;
  } catch (...) {
    page.span_index = GcPage::kNoSpanIndex;
  }
}

// Pages with unused tail space. A bump allocator fills the pages it takes, so in
// a churn workload this index is empty most of the time - which is exactly the
// answer a refill needs, and the reverse scan could not give it without walking
// the entire page set. Membership is a swap-remove through `tail_index`.
std::vector<GcPage*> tail_pages;

// Upper bound on the tail space of any indexed page. A page whose tail cannot
// serve the request used to cost nothing because a reservation always charged
// the whole page; now that a tail is left behind, a large payload can face
// thousands of indexed pages that all hold too little. This answers "can any of
// them serve?" in one comparison, and a full scan that finds nothing recomputes
// it exactly, so the walk happens once per heap state rather than per refill.
size_t max_indexed_tail_bytes = 0;

size_t indexed_tail_bytes(const GcPage& page) {
  return page.capacity > page.used ? page.capacity - page.used : 0;
}

void index_tail_page_locked(GcPage& page) {
  if (page.tail_index != GcPage::kNoSpanIndex || page.retired) return;
  try {
    tail_pages.push_back(&page);
    page.tail_index = tail_pages.size() - 1;
  } catch (...) {
    page.tail_index = GcPage::kNoSpanIndex;
  }
}

void unindex_tail_page_locked(GcPage& page) {
  if (page.tail_index == GcPage::kNoSpanIndex) return;
  GcPage* last = tail_pages.back();
  tail_pages[page.tail_index] = last;
  last->tail_index = page.tail_index;
  tail_pages.pop_back();
  page.tail_index = GcPage::kNoSpanIndex;
}

// Keep the index in step with the one field it describes: a page leaves it the
// moment its bump cursor reaches the end of the page.
void note_page_tail_locked(GcPage& page) {
  if (page.used >= page.capacity) {
    unindex_tail_page_locked(page);
  } else {
    index_tail_page_locked(page);
    const size_t remaining = indexed_tail_bytes(page);
    if (remaining > max_indexed_tail_bytes) max_indexed_tail_bytes = remaining;
  }
}

// A page the sweep emptied stays in the page set until the next cycle boundary -
// the first moment its storage may go back to the recycle pool, because until then
// a mutator that has not reached a safepoint may still hold a cursor into it. The
// space it freed is not off limits, though: the sweep already decided about every
// object it covered, and an object materialised afterwards carries a birth epoch
// past the cycle, so handing that space out again is ordinary reuse inside a page
// the heap already owns. Clearing the mark is what tells the boundary the page is
// occupied again and must not be reclaimed.
void unretire_page_locked(GcPage& page) {
  if (!page.retired) return;
  page.retired = false;
  ++page.reuse_generation;
  if (retired_page_count != 0) --retired_page_count;
}

void unindex_span_page_locked(GcPage& page) {
  if (page.span_index == GcPage::kNoSpanIndex) return;
  GcPage* last = span_pages.back();
  span_pages[page.span_index] = last;
  last->span_index = page.span_index;
  span_pages.pop_back();
  page.span_index = GcPage::kNoSpanIndex;
}
// Empty pages keep their backing storage across cycles. Reusing them avoids an
// operator new / operator delete pair for every page a churn-heavy workload
// frees at each cycle boundary. The pool is bounded so idle programs do not
// retain an unbounded amount of address space.
std::vector<std::unique_ptr<GcPage>> recycled_pages;
// One run's worth of shells. The pool exists to skip the page-struct and start
// bitmap setup for a heap that is about to grow again, not to hold a heap's worth
// of storage for a program that has stopped allocating: a page that leaves the pool
// goes back to its run and its commit charge goes back to the OS.
constexpr size_t kMaxRecycledPages = 256;
// Large-object pages can be much bigger than kGcPageBytes. Retaining a pool of
// them would make the page-count bound meaningless, so only ordinary-sized
// pages are recycled and oversized pages return to the process heap.
constexpr size_t kMaxRecycledPageBytes = 2 * kGcPageBytes;
uint64_t recycled_page_reuse_count = 0;
// Counts thread-local slab reservations, the allocation call's slow path. A heap
// that cannot hand out a useful slab turns every few objects into a refill, so the
// count is the direct measure of that cost.
uint64_t tlab_refill_count = 0;

struct TlabDiagCounters {
  std::atomic<bool> enabled{false};
  std::atomic<bool> env_checked{false};
  std::atomic<bool> print_on_exit{false};
  std::atomic<uint64_t> published_tlab_objects{0};
  std::atomic<uint64_t> slow_planned_calls{0};
  std::atomic<uint64_t> slow_tlab_handled{0};
  std::atomic<uint64_t> slow_locked_fallbacks{0};
  std::atomic<uint64_t> refill_count{0};
  std::atomic<uint64_t> spare_run_hits{0};
  std::atomic<uint64_t> reason_cold_site_init{0};
  std::atomic<uint64_t> reason_gc_active{0};
  std::atomic<uint64_t> reason_collection_due{0};
  std::atomic<uint64_t> reason_debug_alloc_gate{0};
  std::atomic<uint64_t> reason_diag_roots_gate{0};
  std::atomic<uint64_t> reason_plan_ineligible{0};
  std::atomic<uint64_t> reason_accounting_batch{0};
  std::atomic<uint64_t> reason_tlab_kind_mismatch{0};
  std::atomic<uint64_t> reason_tlab_exhausted{0};
  std::atomic<uint64_t> slow_ns{0};
  std::atomic<uint64_t> publish_ns{0};
  std::atomic<uint64_t> install_memset_ns{0};
  std::atomic<uint64_t> reason_other{0};
  std::mutex site_mutex;
  struct SiteEntry {
    void** cache = nullptr;
    size_t element_size = 0;
    size_t last_count = 0;
    uint32_t expected_kind = NRQ_GC_INLINE_KIND_NONE;
    uint64_t slow_calls = 0;
  };
  static constexpr size_t kMaxSites = 64;
  SiteEntry sites[kMaxSites]{};
  size_t site_count = 0;
};
TlabDiagCounters tlab_diag;

void record_tlab_diag_slow_call(void** site_cache, size_t count,
                                size_t element_size, uint32_t plan_flags) {
  tlab_diag.slow_planned_calls.fetch_add(1, std::memory_order_relaxed);
  const uint64_t packed =
      std::atomic_ref<uint64_t>(nrq_gc_inline_config.packed_state)
          .load(std::memory_order_acquire);
  const uint32_t gate = nrq_gc_inline_unpack_gate(packed);
  const uint32_t repr_bits = NRQ_ALLOC_PLAN_REPR_OF(plan_flags);
  uint32_t expected_kind = NRQ_GC_INLINE_KIND_NONE;
  size_t header_bytes = 16;
  if ((plan_flags & NRQ_ALLOC_PLAN_REJECTED) == 0 &&
      (plan_flags & NRQ_ALLOC_PLAN_ARRAY) != 0) {
    if (repr_bits == NRQ_ALLOC_REPR_COMPACT && count == 1) {
      expected_kind = NRQ_GC_INLINE_KIND_COMPACT;
      header_bytes = 8;
    } else if (repr_bits == NRQ_ALLOC_REPR_EXTENDED) {
      expected_kind = NRQ_GC_INLINE_KIND_EXTENDED;
      header_bytes = 16;
    }
  }
  const size_t payload_bytes = count * element_size;
  const size_t span =
      (header_bytes + payload_bytes + NRQ_GC_INLINE_SPAN_ALIGNMENT - 1) &
      ~size_t{NRQ_GC_INLINE_SPAN_ALIGNMENT - 1};
  const NrqGcFastTls* fast = thread_mutator.fast;

  if (site_cache && *site_cache == nullptr) {
    tlab_diag.reason_cold_site_init.fetch_add(1, std::memory_order_relaxed);
  } else if ((gate & NRQ_GC_INLINE_GATE_PHASE_BIT) != 0) {
    tlab_diag.reason_gc_active.fetch_add(1, std::memory_order_relaxed);
  } else if ((gate & NRQ_GC_INLINE_GATE_DUE_BIT) != 0) {
    tlab_diag.reason_collection_due.fetch_add(1, std::memory_order_relaxed);
  } else if ((gate & NRQ_GC_INLINE_GATE_DEBUG_ALLOC_BIT) != 0) {
    tlab_diag.reason_debug_alloc_gate.fetch_add(1, std::memory_order_relaxed);
  } else if ((gate & NRQ_GC_INLINE_GATE_DIAG_ROOTS_BIT) != 0) {
    tlab_diag.reason_diag_roots_gate.fetch_add(1, std::memory_order_relaxed);
  } else if (expected_kind == NRQ_GC_INLINE_KIND_NONE ||
             span > NRQ_GC_INLINE_TLAB_BYTES) {
    tlab_diag.reason_plan_ineligible.fetch_add(1, std::memory_order_relaxed);
  } else if (fast) {
    uint8_t* const cur = expected_kind == NRQ_GC_INLINE_KIND_EXTENDED
                             ? fast->ext_cursor
                             : fast->tlab_cursor;
    uint8_t* const end = expected_kind == NRQ_GC_INLINE_KIND_EXTENDED
                             ? fast->ext_end
                             : fast->tlab_end;
    uint8_t* const slab_end = expected_kind == NRQ_GC_INLINE_KIND_EXTENDED
                                  ? thread_mutator.ext_slab_end
                                  : thread_mutator.tlab_slab_end;
    if (!cur || !end || static_cast<size_t>(end - cur) < span) {
      if (cur && slab_end && slab_end >= cur + span) {
        tlab_diag.reason_accounting_batch.fetch_add(1, std::memory_order_relaxed);
      } else {
        tlab_diag.reason_tlab_exhausted.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      tlab_diag.reason_other.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    tlab_diag.reason_other.fetch_add(1, std::memory_order_relaxed);
  }

  std::lock_guard<std::mutex> lock(tlab_diag.site_mutex);
  for (size_t i = 0; i < tlab_diag.site_count; ++i) {
    if (tlab_diag.sites[i].cache == site_cache) {
      ++tlab_diag.sites[i].slow_calls;
      tlab_diag.sites[i].last_count = count;
      return;
    }
  }
  if (tlab_diag.site_count < TlabDiagCounters::kMaxSites) {
    tlab_diag.sites[tlab_diag.site_count++] = TlabDiagCounters::SiteEntry{
        site_cache, element_size, count, expected_kind, 1};
  }
}
// Counts allocations served from space freed by an earlier sweep rather than
// from a page's untouched tail.
uint64_t recycled_span_reuse_count = 0;
size_t total_free_spans = 0;
// Reclaimed space the bounded run list could not record. Counted globally as well
// as per page, so a fragmentation figure can say whether the bound - rather than
// the workload - is what stranded the space.
uint64_t dropped_free_bytes_total = 0;
uint64_t dropped_free_runs_total = 0;
// What the allocator did about payload zeroing, split into the bytes it wrote and
// the bytes a page already held as zeros.
nrq_gc_zeroing_facts zeroing_facts{};// AO-06 observation. `reused_span_bytes` is how much reclaimed space has been
// handed back out, which with the allocated total gives the reuse hit rate;
// `tlab_tail_bytes_total` folds in the reservations of threads that have left
// the registry, so the tail counters stay per-mutator and race-free.
uint64_t reused_span_bytes = 0;
uint64_t tlab_tail_bytes_total = 0;
uint64_t tlab_tail_max_bytes_total = 0;
// Reservation space returned to its page at a safepoint, which is what turns an
// abandoned slab tail from stranded bytes into reusable free space.
uint64_t tlab_tail_reclaimed_bytes_total = 0;
// Number of pages whose largest recorded run can host a TLAB slab. A refill asks
// this one word before searching the run index, so a fragmented heap answers
// "no slab anywhere" in constant time instead of proving it by walking every
// page once per few allocations. Maintained under gc_mutex with the page maxima.
size_t pages_with_slab_run = 0;
// Cache the current bump page per allocating thread. Collection increments the
// generation so a thread that resumes after another thread reclaimed its page
// discards the stale pointer before dereferencing it.
// Atomic because a mutator reads this on its allocation fast path
// without the collector lock, while the cycle boundary publishes a new
// value under that lock. The acquire loads below and the release store
// at the boundary are the handshake that tells a resuming mutator it
// may not keep using a page stamp it read before the cycle.
std::atomic<uint64_t> gc_page_generation{1};
// One cached bump page per page class. A workload that alternates two
// representations used to discard the single cache slot on every switch and
// rescan the tail index, which turned into a fresh page per allocation once the
// scan could not answer. The class is part of the key, so each shape keeps its
// own page resident.
constexpr size_t kPageClassCount = 5;
thread_local GcPage* thread_allocation_pages[kPageClassCount] = {};
thread_local uint64_t thread_allocation_generation = 0;

inline GcPage* cached_allocation_page(PageKind kind) {
  if (thread_allocation_generation != gc_page_generation.load(std::memory_order_acquire)) return nullptr;
  return thread_allocation_pages[static_cast<size_t>(kind)];
}

inline void cache_allocation_page(GcPage* page) {
  if (!page) return;
  if (thread_allocation_generation != gc_page_generation.load(std::memory_order_acquire)) {
    for (GcPage*& slot : thread_allocation_pages) slot = nullptr;
    thread_allocation_generation = gc_page_generation.load(std::memory_order_acquire);
  }
  thread_allocation_pages[static_cast<size_t>(page->kind)] = page;
}
struct RootInfo {
  size_t references = 0;
  size_t object_size = 0;
  size_t repeated_count = 0;
  // Roots describe layouts exactly like objects do, so they share the same
  // immutable descriptor instead of carrying their own offset copy. A repeated
  // array root is an array layout whose payload is one element.
  const nrq_object_layout* layout = nullptr;
};
std::unordered_map<void*, RootInfo> roots;
// Thread-local shadow stacks hold ordinary roots. Array/external roots stay in
// `roots`; ordinary roots never touch gc_mutex after the stack is registered.
struct ShadowRoot {
  void* slot = nullptr;
  const nrq_object_layout* layout = nullptr;
};
// Per-slot bookkeeping for an active shadow root. `count` preserves
// duplicate-registration semantics: a slot stays indexed while any frame still
// owns a registration for it. `layout` is the validated descriptor shared by
// every registration of that slot (and `layout->payload_size` equals the
// registered `object_size`).
struct SlotState {
  const nrq_object_layout* layout = nullptr;
  uint32_t count = 0;
  bool pointer_root = false;
};
// Thread-local index of active root slots. It answers the three questions the
// shadow stack needs -- how many registrations a slot currently has, whether a
// repeated registration agrees with the first one, and whether the slot is a
// plain pointer root -- without allocating a hash node per slot. See
// slot_index.h for the table's rehash policy and the PO-27 tombstone fix.

// Direct-mapped record of the slots this thread has already proven to be pointer
// roots. A rooted allocation validates its slot on every call; the hash index
// answers that in one probe, and this answers it in one compare for the slots a
// construction loop reuses. An entry is retired together with the registration
// it came from, so a hit can never outlive the slot state it describes.
struct PointerRootCache {
  static constexpr size_t kCapacity = 8;
  void* slots[kCapacity]{};

  static size_t index_of(void* slot) {
    return (reinterpret_cast<uintptr_t>(slot) >> 4) & (kCapacity - 1);
  }
  bool contains(void* slot) const { return slots[index_of(slot)] == slot; }
  void insert(void* slot) { slots[index_of(slot)] = slot; }
  void forget(void* slot) {
    void*& entry = slots[index_of(slot)];
    if (entry == slot) entry = nullptr;
  }
};

struct ShadowStack {
  std::mutex mutex;
  std::vector<NrqGcFastRoot> roots_buffer;
  std::vector<NrqGcFastFrame> frames_buffer;
  NrqGcFastTls* fast = nullptr;
  uint64_t next_marker = 1;
  // O(1) index of active root slots for dynamic registrations. Maintained by
  // the owning thread only and never read by the collector.
  SlotIndex<SlotState> slot_states;
  PointerRootCache pointer_roots;
  bool pointer_roots_has_unindexed = false;
};
std::vector<ShadowStack*> shadow_stacks;
thread_local ShadowStack* thread_shadow_stack = nullptr;

inline const void* encode_shadow_layout(const nrq_object_layout* layout,
                                        bool indexed) {
  if (!indexed && layout == &kPointerRootLayout) return nullptr;
  const uintptr_t bits = reinterpret_cast<uintptr_t>(layout);
  return reinterpret_cast<const void*>(indexed ? (bits | uintptr_t{1}) : bits);
}

inline const nrq_object_layout* decode_shadow_layout(const void* encoded) {
  if (!encoded) return &kPointerRootLayout;
  return reinterpret_cast<const nrq_object_layout*>(
      reinterpret_cast<uintptr_t>(encoded) & ~uintptr_t{1});
}

inline bool shadow_layout_is_indexed(const void* encoded) {
  return (reinterpret_cast<uintptr_t>(encoded) & uintptr_t{1}) != 0;
}

inline size_t shadow_root_count(const ShadowStack& stack) {
  if (!stack.fast || !stack.fast->root_cursor || stack.roots_buffer.empty())
    return 0;
  return static_cast<size_t>(stack.fast->root_cursor - stack.roots_buffer.data());
}

inline size_t shadow_frame_count(const ShadowStack& stack) {
  if (!stack.fast || !stack.fast->frame_cursor || stack.frames_buffer.empty())
    return 0;
  return static_cast<size_t>(stack.fast->frame_cursor - stack.frames_buffer.data());
}

// Cache of frame root descriptors this thread has already validated. Keying it
// by address alone would be enough for the compiler's immutable tables, but the
// entry point is a public C ABI: a caller may hand over a descriptor on the
// stack, and two activations can reuse that address with different content. A
// hit therefore requires identical words as well, so a cached verdict can only
// ever be reused for the descriptor it was computed from - the validation is
// deduplicated, never skipped. Comparing the words costs two vector compares for
// a typical frame, far less than the walk it replaces.
struct ValidatedDescriptorCache {
  static constexpr size_t kCapacity = 16;
  static constexpr size_t kMaxWords = 16;
  struct Entry {
    const size_t* descriptor = nullptr;
    size_t word_count = 0;
    size_t words[kMaxWords]{};
  };
  Entry entries[kCapacity]{};
  size_t count = 0;

  bool matches(const size_t* desc) const noexcept {
    const size_t word_count = desc[1];
    // A table wider than the memo is validated on every activation instead of
    // being stored truncated.
    if (word_count < 2 || word_count > kMaxWords) return false;
    for (size_t i = 0; i < count; ++i) {
      const Entry& entry = entries[i];
      if (entry.descriptor != desc) continue;
      return entry.word_count == word_count &&
             std::memcmp(entry.words, desc, word_count * sizeof(size_t)) == 0;
    }
    return false;
  }

  void insert(const size_t* desc) noexcept {
    const size_t word_count = desc[1];
    if (word_count < 2 || word_count > kMaxWords) return;
    for (size_t i = 0; i < count; ++i) {
      if (entries[i].descriptor != desc) continue;
      entries[i].word_count = word_count;
      std::memcpy(entries[i].words, desc, word_count * sizeof(size_t));
      return;
    }
    Entry& entry = count < kCapacity ? entries[count++] : entries[0];
    entry.descriptor = desc;
    entry.word_count = word_count;
    std::memcpy(entry.words, desc, word_count * sizeof(size_t));
  }
};
thread_local ValidatedDescriptorCache thread_validated_descriptors;
// Cleans up thread-local GC state when a thread terminates without unwinding
// its mutator/root frames. Without this the collector could later enumerate a
// shadow stack whose owning thread is gone and whose slots are dangling, and a
// thread-local allocation batch could be lost.
struct ThreadGcStateCleanup {
  bool armed = false;
  void arm() { armed = true; }
  ~ThreadGcStateCleanup();
};
thread_local ThreadGcStateCleanup thread_gc_cleanup;
std::unordered_map<void*, size_t> pinned_objects;
// Pinning is a diagnostic facility rather than a concurrency primitive: a
// reasonable program pins an object while a native call borrows it and unpins it
// immediately afterwards. Repeating the same pin far more often than that means
// the program is leaking pins (they are never released by the collector), so the
// runtime rejects further pins for that object and counts the rejection instead
// of failing the process.
constexpr size_t kMaxPinsPerObject = 16;
uint64_t pin_violations = 0;
// Reference diagnostics is the only reader that can look at a shadow stack while
// the collector is idle: during a collection every registered mutator is parked
// before roots are enumerated, so a running mutator's stack cannot be read. The
// fast paths therefore take the stack mutex only when a diagnostic query is
// possible, and plain, owner-only access otherwise.
std::atomic<bool> reference_diagnostics_enabled{false};

bool diagnostics_reading_roots() {
  return reference_diagnostics_enabled.load(std::memory_order_acquire);
}
// Collection is an explicit opt-in policy. Programs using the GC domain may
// call gc.enable() to request automatic triggers; explicit gc.collect() always
// works regardless of this state.
std::atomic<bool> automatic_collection_enabled{false};
std::atomic<bool> generational_collection_enabled{true};
// Every GC allocation updates these counters, from every mutator thread, so
// they are the hottest shared state in the runtime. They sit together on one
// cache line because they are updated together, and that line is isolated from
// the collector's own fields so a mutator's accounting does not invalidate the
// line a collection is reading (false sharing). The residual contention is true
// sharing: several threads do update these same words, which per-mutator
// accounting rather than layout has to remove.
// Every GC allocation used to update global atomics. Now each mutator batches
// its unflushed byte count locally and contributes it to this line only when a
// TLAB is published or a batch of this size accumulates. The counters therefore
// stay on one isolated line without becoming a per-allocation rendezvous point.
constexpr size_t kAccountingBatchBytes = kGcTlabBytes;
struct alignas(nirq::nrq_cache_line_bytes) AllocationAccounting {
  std::atomic<size_t> bytes_since_collection{0};
  std::atomic<size_t> collection_threshold{1024 * 1024};
};
AllocationAccounting allocation_accounting;
// Adaptive heap-growth policy. The trigger is not a constant: after every cycle
// the collector knows how much of the heap survived, so it can set the next
// trigger from the live set instead of collecting on a fixed byte count that is
// either wasteful for a small heap or far too eager for a large one. The policy
// is disabled as soon as a program sets its own threshold.
constexpr size_t kMinimumCollectionHeadroom = 512 * 1024;
struct CollectionPolicy {
  bool adaptive = true;
  size_t last_reclaimed_bytes = 0;
  size_t adaptations = 0;
};
CollectionPolicy collection_policy;
// Live bytes of the published heap, maintained incrementally by publication and
// reclamation. finish_cycle used to recompute this by walking every live object.
size_t live_heap_bytes = 0;
// Live bytes at the start of the cycle in progress, used to measure survival.
size_t cycle_start_live_bytes = 0;
nrq_gc_stats statistics{};
uint64_t minor_collections = 0;
uint64_t major_collections = 0;

// A collection is split into short mark slices. Mutators continue executing
// between slices; barriers below preserve the snapshot-at-the-beginning
// invariant. The sweep phase serializes GC API calls with gc_mutex but does not
// park mutators that are only executing ordinary code.
// Slice budgets are expressed as bytes of heap touched, not as a fixed object
// count. A constant count makes the number of lock rounds proportional to the
// heap: 2 M live objects at 64 objects per slice is 31 000 acquisitions of
// gc_mutex, each of which a refilling mutator can also be waiting on. A byte
// budget keeps a slice short in wall-clock terms for any object size while the
// number of rounds follows the bytes actually processed. Marking touches less
// per object than sweeping does, so its budget is the smaller of the two.
constexpr size_t kMarkSliceBytes = 64 * 1024;
constexpr size_t kSweepSliceBytes = 256 * 1024;
// A byte budget alone does not bound a sweep chunk: a page holding one live
// object contributes one span to the budget but still costs its whole start
// bitmap to examine, and an empty page costs a visit. Charge bitmap words as
// work too, so a sparse heap cannot hold the collector lock for a scan bounded
// only by its size. Dense pages reach the byte budget first (a 64 KiB page of
// 120-byte objects is ~512 words for 2184 objects); 4096 words bounds the sparse
// case at ~32 pages per chunk.
constexpr size_t kSweepBitmapWordsPerChunk = 4096;
constexpr uint8_t kPromotionAge = 2;
// A full collection is worth its cost only when it can reclaim something a young
// collection cannot: unreachable old objects. Those are invisible to minors, so
// they show up as heap growth the program did not ask for. The collector
// therefore runs a full cycle once the live heap has grown by half since the
// last one, with a floor so consecutive full cycles cannot happen and a ceiling
// so a heap that never grows is still checked periodically.
constexpr size_t kMinimumMinorInterval = 2;
constexpr size_t kMaximumMinorInterval = 32;
alignas(nirq::nrq_cache_line_bytes) std::atomic<CollectionPhase> collection_phase{
    CollectionPhase::Idle};
bool active_collection_is_minor = false;
// Live bytes observed after the most recent full collection, and the kind of
// cycle the next automatic collection should run. Both are planned while the
// heap lock is held and published for the triggered collection to consume.
size_t live_heap_bytes_at_last_major = 0;
std::atomic<bool> next_cycle_is_major{false};
uint64_t active_cycle_epoch = 0;
std::atomic<uint64_t> next_cycle_epoch{1};

// History of reclamation decisions, newest last. A stamp that vanishes under a
// concurrent collector says storage was freed, but not which cycle freed it or
// what the collector believed about the object; both questions have to be
// answerable after the fact, so every reclamation records one entry.
struct SweepHistoryRecord {
  void* address = nullptr;
  uint64_t cycle = 0;
  uint64_t offset = 0;
  uint32_t payload_bytes = 0;
  int32_t kind = 0;
  int32_t born = 0;
  int32_t marked = 0;
  int32_t whole_page = 0;
  int32_t live_objects = 0;
  // The object's generation at the moment it was reclaimed (0 young, 1 old) and
  // whether the cycle reclaiming it was a minor one. A victim that was young and
  // reachable from an old parent is a remembered-set miss; one that was unborn
  // and unmarked in a full cycle is a snapshot-barrier miss. The distinction
  // cannot be recovered from the address afterwards, so it is recorded here.
  int32_t generation = -1;
  int32_t full = -1;
};
constexpr size_t kSweepHistoryEntries = 512;
SweepHistoryRecord sweep_history[kSweepHistoryEntries];
std::atomic<size_t> sweep_history_cursor{0};

// Records what the collector believed about an object at the moment it reclaimed
// it. Every field is read from state the sweep already carries - the birth bitmap,
// the mark bitmap, the page's state table - so recording costs no lock, no walk,
// and no allocation, and the diagnostic runs at the speed the workload runs at.
// Its flags used to be passed in by the caller, which meant the free site recorded
// two hardcoded zeroes and the record could not distinguish a young remembered-set
// miss from an unmarked survivor.
void record_sweep_object_locked(const ObjectRef& object, bool whole_page) {
  if constexpr (!nirq::runtime_debug_enabled) return;
  const size_t index =
      sweep_history_cursor.fetch_add(1, std::memory_order_relaxed) %
      kSweepHistoryEntries;
  SweepHistoryRecord& record = sweep_history[index];
  const nrq_object_layout* layout = object_layout(object);
  record.address = object_payload(object);
  record.cycle = active_cycle_epoch;
  record.offset = static_cast<uint64_t>(object.header - object.page->storage);
  record.payload_bytes = layout ? layout->payload_size : 0;
  record.kind = static_cast<int32_t>(object.page->kind);
  record.born = object_born_in_cycle(object, active_cycle_epoch) ? 1 : 0;
  record.marked = object_marked(object) ? 1 : 0;
  record.whole_page = whole_page ? 1 : 0;
  record.live_objects = static_cast<int32_t>(object.page->live_objects);
  record.generation = object_young(object) ? 0 : 1;
  record.full = active_collection_is_minor ? 0 : 1;
}

void record_sweep_page_locked(const GcPage& page, size_t live) {
  if constexpr (!nirq::runtime_debug_enabled) return;
  const size_t index =
      sweep_history_cursor.fetch_add(1, std::memory_order_relaxed) %
      kSweepHistoryEntries;
  SweepHistoryRecord& record = sweep_history[index];
  record.address = page.storage;
  record.cycle = active_cycle_epoch;
  record.offset = 0;
  record.payload_bytes = 0;
  record.kind = static_cast<int32_t>(page.kind);
  record.born = 0;
  record.marked = 0;
  record.whole_page = 1;
  record.live_objects = static_cast<int32_t>(live);
  record.generation = -1;
  record.full = active_collection_is_minor ? 0 : 1;
}
std::atomic<size_t> minor_collections_since_major{0};
// A trace callback may visit many references. Resolve them in a short
// contiguous batch after the callback returns instead of interleaving a page
// lookup with every callback invocation. The inline array keeps the hot path
// allocation-free; an oversized fan-out resolves the overflow immediately.
constexpr size_t kReferenceBatchCapacity = 128;
// Per-phase cycle accounting. One collector runs at a time (collection cycles
// are serialized), so these are plain counters written by that thread. The
// timestamps come from the same monotonic clock the benchmark harness uses, so a
// phase figure is directly comparable with a whole-cycle figure measured
// outside the runtime.
using CycleClock = std::chrono::steady_clock;
inline uint64_t cycle_ns(CycleClock::time_point since) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(CycleClock::now() -
                                                          since)
          .count());
}
nrq_gc_cycle_timing cycle_timing{};
struct MarkContext {
  MarkWorklist worklist;
  bool young_only = false;
  // The cycle's birth epoch, copied from active_cycle_epoch when the context is
  // prepared. Every reference resolution compares against this copy instead of
  // the global, which keeps the marking path off shared state entirely.
  uint64_t epoch = 0;
  std::array<void*, kReferenceBatchCapacity> references{};
  size_t reference_count = 0;

  void reset() {
    worklist.reset();
    young_only = false;
    epoch = 0;
    reference_count = 0;
  }
};
MarkContext active_mark;
// During the sweep phase the snapshot list is detached from the allocation
// list. New allocations are therefore never inserted into a list the sweeper
// is traversing, and GC barriers can publish new marks between sweep slices.
std::vector<ObjectRef> sweep_list;
// Old objects are not traced during a minor collection. This list records young
// objects referenced by old objects; the root-store/root-copy barriers append
// here, and a minor collection treats the list as additional roots. Entries are
// unique because an object's `remembered` flag gates insertion, so the list
// needs no hashing on the barrier path and scans one contiguous array.
std::vector<ObjectRef> remembered_young;
// A thread may buffer a reference to an object that is still in another
// mutator's unpublished TLAB. These raw addresses are retried after all TLABs
// are published at the next cycle boundary; they are never dereferenced.
std::unordered_set<void*> unresolved_remembered;
// While every live managed object is young, a young-to-young assignment cannot
// create an old-to-young edge, so the write barrier can skip remembered-set
// work. `generational_barrier_active` is raised for the duration of every
// collection cycle (because a concurrent promotion can make an object old while
// mutators run) and lowered after a cycle that leaves no old objects. This
// keeps the idle fast path allocation-only for young workloads without
// weakening generational tracking once an old generation exists.
std::atomic<bool> generational_barrier_active{false};
// Single published switch for the store barrier: true exactly when generational
// tracking is enabled and remembered-set recording is required. Mutators test
// this one acquire load instead of the two flags it is derived from. It is
// rewritten by refresh_store_barrier() wherever either input changes, and by the
// mutator that allocates the first object of an old generation.
std::atomic<bool> store_barrier_active{false};
std::atomic<size_t> old_object_count{0};

// Every writer of either input publishes the combined switch around its own flag
// store: raised before `generational_barrier_active`, lowered after it, and
// raised before generational collection is enabled. A mutator that observes a
// stale value therefore records remembered-set entries it did not have to, never
// skips one it needed.
void raise_store_barrier() {
  std::atomic_ref<uint64_t>(nrq_gc_inline_config.packed_state)
      .fetch_or(uint64_t{NRQ_GC_INLINE_GATE_STORE_BARRIER_BIT},
                std::memory_order_acq_rel);
  std::atomic_ref<uint32_t>(nrq_gc_inline_config.store_barrier)
      .store(1u, std::memory_order_release);
  store_barrier_active.store(true, std::memory_order_release);
  generational_barrier_active.store(true, std::memory_order_release);
}

void lower_store_barrier() {
  generational_barrier_active.store(false, std::memory_order_release);
  store_barrier_active.store(false, std::memory_order_release);
  std::atomic_ref<uint32_t>(nrq_gc_inline_config.store_barrier)
      .store(0u, std::memory_order_release);
  std::atomic_ref<uint64_t>(nrq_gc_inline_config.packed_state)
      .fetch_and(~uint64_t{NRQ_GC_INLINE_GATE_STORE_BARRIER_BIT},
                 std::memory_order_acq_rel);
}

uint64_t saturated_add(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

std::atomic<bool> inline_debug_alloc_enabled{false};

void refresh_inline_config_locked() {
  const uint32_t phase_bit =
      collection_phase.load(std::memory_order_acquire) != CollectionPhase::Idle
          ? NRQ_GC_INLINE_GATE_PHASE_BIT
          : 0u;
  const size_t threshold =
      allocation_accounting.collection_threshold.load(std::memory_order_relaxed);
  const size_t since =
      allocation_accounting.bytes_since_collection.load(std::memory_order_relaxed);
  const uint32_t due_bit =
      (automatic_collection_enabled.load(std::memory_order_relaxed) &&
       since >= threshold)
          ? NRQ_GC_INLINE_GATE_DUE_BIT
          : 0u;
  const uint32_t debug_bit =
      inline_debug_alloc_enabled.load(std::memory_order_relaxed)
          ? NRQ_GC_INLINE_GATE_DEBUG_ALLOC_BIT
          : 0u;
  const uint32_t diag_roots_bit =
      reference_diagnostics_enabled.load(std::memory_order_relaxed)
          ? NRQ_GC_INLINE_GATE_DIAG_ROOTS_BIT
          : 0u;
  const bool sb_active = store_barrier_active.load(std::memory_order_acquire);
  const uint32_t sb_bit = sb_active ? NRQ_GC_INLINE_GATE_STORE_BARRIER_BIT : 0u;
  const uint32_t gate =
      phase_bit | due_bit | debug_bit | diag_roots_bit | sb_bit;
  const uint32_t initial_state =
      generational_collection_enabled.load(std::memory_order_relaxed)
          ? kYoungBit
          : 0u;
  const size_t batch =
      std::min(kAccountingBatchBytes, threshold == 0 ? size_t{1} : threshold);
  // Publish `accounting_batch` before `packed_state` with release ordering so
  // any fast-path reader whose 64-bit acquire load observes this `packed_state`
  // is ordered after the updated `accounting_batch` as well.
  const uint64_t packed = nrq_gc_inline_pack_state(gate, initial_state);
  std::atomic_ref<uint32_t>(nrq_gc_inline_config.store_barrier)
      .store(sb_active ? 1u : 0u, std::memory_order_release);
  std::atomic_ref<size_t>(nrq_gc_inline_config.accounting_batch)
      .store(batch, std::memory_order_release);
  std::atomic_ref<uint64_t>(nrq_gc_inline_config.packed_state)
      .store(packed, std::memory_order_release);
}

// `bytes_since_collection` has exactly one writer-path class - the collection
// and allocation code that runs under gc_mutex - so the update is a plain
// saturating load and store rather than a compare-exchange loop that can never
// retry. The trigger reads it without the lock and only needs a snapshot, which
// a relaxed store publishes.
void add_since_collection_locked(size_t amount) {
  const size_t current =
      allocation_accounting.bytes_since_collection.load(std::memory_order_relaxed);
  allocation_accounting.bytes_since_collection.store(
      amount > SIZE_MAX - current ? SIZE_MAX : current + amount,
      std::memory_order_relaxed);
  refresh_inline_config_locked();
}

void sync_inline_unflushed(MutatorState& mutator) {
  if (!mutator.fast) return;
  const size_t pending =
      std::atomic_ref<size_t>(mutator.fast->pending_bytes)
          .load(std::memory_order_relaxed);
  if (pending > mutator.tlab_accounted_pending_bytes) {
    mutator.fast->unflushed_bytes +=
        (pending - mutator.tlab_accounted_pending_bytes);
    mutator.tlab_accounted_pending_bytes = pending;
  }
}

void clamp_tlab_window(uint8_t* cursor, uint8_t*& end, uint8_t* slab_end,
                       size_t unflushed_bytes) {
  if (!cursor || !slab_end) return;
  if (slab_end <= cursor ||
      !automatic_collection_enabled.load(std::memory_order_relaxed)) {
    end = slab_end;
    return;
  }
  const size_t remaining_slab = static_cast<size_t>(slab_end - cursor);
  const size_t batch =
      std::atomic_ref<size_t>(nrq_gc_inline_config.accounting_batch)
          .load(std::memory_order_relaxed);
  const size_t budget =
      batch > unflushed_bytes ? batch - unflushed_bytes : 0;
  end = cursor + std::min(remaining_slab, budget);
}

void update_tlab_end_clamp(MutatorState& mutator) {
  if (!mutator.fast) return;
  if (mutator.fast->tlab_active_range) {
    clamp_tlab_window(mutator.fast->tlab_cursor, mutator.fast->tlab_end,
                      mutator.tlab_slab_end, mutator.fast->unflushed_bytes);
  }
  if (mutator.fast->ext_active_range) {
    clamp_tlab_window(mutator.fast->ext_cursor, mutator.fast->ext_end,
                      mutator.ext_slab_end, mutator.fast->unflushed_bytes);
  }
}

// Fold a mutator's locally batched allocation bytes into the shared trigger
// counter. Callers hold gc_mutex.
void flush_mutator_accounting_locked(MutatorState& mutator) {
  if (!mutator.fast) return;
  sync_inline_unflushed(mutator);
  if (mutator.fast->unflushed_bytes == 0) return;
  add_since_collection_locked(mutator.fast->unflushed_bytes);
  mutator.fast->unflushed_bytes = 0;
}

inline void sync_active_ranges(NrqGcFastTls& fast) {
  if (fast.tlab_active_range) fast.tlab_active_range->end = fast.tlab_cursor;
  if (fast.ext_active_range) fast.ext_active_range->end = fast.ext_cursor;
  if (fast.h0_active_range) fast.h0_active_range->end = fast.h0_cursor;
}

void clear_tlab_fast_path(NrqGcFastTls& fast) {
  if (fast.tlab_active_range) fast.tlab_active_range->end = fast.tlab_cursor;
  fast.tlab_active_range = nullptr;
  if (fast.ext_active_range) fast.ext_active_range->end = fast.ext_cursor;
  fast.ext_active_range = nullptr;
  fast.tlab_kind = NRQ_GC_INLINE_KIND_NONE;
  // Shrink the bump limit down to cursor (keeping `*_slab_end` intact) so the
  // next allocation attaches a fresh pending range via `allocate_tlab`.
  if (fast.tlab_cursor) fast.tlab_end = fast.tlab_cursor;
  if (fast.ext_cursor) fast.ext_end = fast.ext_cursor;
}

void clear_h0_fast_path(NrqGcFastTls& fast) {
  if (fast.h0_active_range) fast.h0_active_range->end = fast.h0_cursor;
  fast.h0_active_range = nullptr;
  fast.h0_fast_layout = nullptr;
}

// Declared here because publication precedes the marking primitives it uses once
// a sweep is already running.
void trace_children(const ObjectRef& object, nrq_gc_visit_fn visit,
                    void* context);
void mark_reference_value(void* reference, void* context);

void publish_one_range_locked(const NrqGcPendingRange& range,
                              bool collection_active,
                              uint64_t next_epoch) {
  if (!range.page || !range.begin) return;
  GcPage& page = *static_cast<GcPage*>(range.page);
  const PageKind kind = page.kind;
  const size_t header_bytes = page_header_bytes(kind);
  const bool indexed = page_uses_index(&page);
  const bool is_h0 = kind == PageKind::Headerless;
  const bool is_compact = kind == PageKind::Compact;
  uint64_t range_objects = 0;
  size_t range_bytes = 0;
  size_t range_young = 0;
  size_t range_old = 0;
  uint32_t cached_layout_id = 0;
  size_t cached_payload_bytes = 0;

  for (unsigned char* cursor = range.begin; cursor < range.end;) {
    const ObjectRef object{cursor, &page};
    size_t payload_bytes = 0;
    if (is_compact) {
      const auto* hdr = reinterpret_cast<const uint32_t*>(cursor);
      const uint32_t lid = hdr[1];
      if (lid != 0 && lid == cached_layout_id) {
        payload_bytes = cached_payload_bytes;
      } else {
        payload_bytes = object_payload_bytes(object);
        if (lid != 0) {
          cached_layout_id = lid;
          cached_payload_bytes = payload_bytes;
        }
      }
    } else {
      payload_bytes = object_payload_bytes(object);
    }
    const size_t span = is_h0 ? page.slot_bytes
                              : object_span_bytes(header_bytes, payload_bytes);
    if (span == 0 || cursor + span > range.end)
      nrq_runtime_fatal("GC pending range has an invalid span");
    if (indexed) {
      if (!object_index.insert(object_payload(object), cursor, &page))
        nrq_runtime_fatal("GC failed to publish a thread-local allocation");
      ++page.indexed_objects;
      try {
        unindexed_objects.push_back(object);
      } catch (...) {
        nrq_runtime_fatal("GC unindexed object list allocation failed");
      }
    } else {
      mark_object_start(page, static_cast<size_t>(cursor - page.storage));
    }
    ++range_objects;
    if (range_bytes > SIZE_MAX - payload_bytes)
      nrq_runtime_fatal("GC page live-byte count overflow");
    range_bytes += payload_bytes;
    if (collection_active) {
      mark_object_born(object, next_epoch);
      // Marking is already finished once the sweep runs, so an object that
      // appears then keeps its references alive only if they are marked as it
      // arrives: the transition pass cannot have seen this object. A page that
      // was reclaimed before this point is the only case left uncovered, and it
      // needs the object to have been allocated after its own referent's page was
      // already swept.
      if (collection_phase.load(std::memory_order_relaxed) ==
          CollectionPhase::Sweeping)
        trace_children(object, mark_reference_value, nullptr);
    }
    if (object_young(object)) {
      ++range_young;
    } else {
      ++range_old;
    }
    cursor += span;
  }

  if (page.live_objects > SIZE_MAX - range_objects)
    nrq_runtime_fatal("GC page live-object count overflow");
  release_uncommitted_objects(page, range_objects);
  page.live_objects += range_objects;
  if (page.live_payload_bytes > SIZE_MAX - range_bytes)
    nrq_runtime_fatal("GC page live-byte count overflow");
  page.live_payload_bytes += range_bytes;
  page.young_objects += range_young;
  if (range_old != 0)
    old_object_count.fetch_add(range_old, std::memory_order_relaxed);
  statistics.allocated_bytes =
      saturated_add(statistics.allocated_bytes, range_bytes);
  statistics.live_objects =
      saturated_add(statistics.live_objects, range_objects);
  live_heap_bytes = saturated_add(live_heap_bytes, range_bytes);
}

// The single page every open range belongs to, or null when they disagree. A
// publication window is closed at each page switch, so a disagreement means the
// window cannot be applied from its counters and the exact walk has to run.
GcPage* window_page_of(MutatorState& mutator) {
  if (!mutator.fast) return nullptr;
  NrqGcFastTls& fast = *mutator.fast;
  GcPage* page = nullptr;
  bool conflict = false;
  auto consider = [&](const NrqGcPendingRange& range) {
    if (!range.page) return;
    GcPage* const candidate = static_cast<GcPage*>(range.page);
    if (!page)
      page = candidate;
    else if (page != candidate)
      conflict = true;
  };
  for (const NrqGcPendingRange& range : mutator.deferred_ranges) consider(range);
  for (uint32_t index = 0; index < fast.pending_range_count; ++index)
    consider(fast.pending_ranges[index]);
  return conflict ? nullptr : page;
}

// Records the window's ranges as runs on the page. A contiguous run that is
// published again in a later window merges into the same entry, so a slab costs
// one entry however many refills it took.
void record_window_runs_locked(GcPage& page, MutatorState& mutator) {
  NrqGcFastTls& fast = *mutator.fast;
  auto append = [&](const NrqGcPendingRange& range) {
    if (!range.begin || !range.end || range.end <= range.begin) return;
    const size_t begin = static_cast<size_t>(range.begin - page.storage);
    const size_t end = static_cast<size_t>(range.end - page.storage);
    if (begin >= end || end > page.capacity) return;
    if (page.pending_start_count != 0) {
      GcPage::PendingStartRun& last =
          page.pending_starts[page.pending_start_count - 1];
      if (last.end == begin) {
        last.end = static_cast<uint32_t>(end);
        return;
      }
    }
    // A run the list cannot hold has to be written out now: its objects are
    // already counted live, so dropping the run would leave them with no start
    // bit at all. A reference into them would then fail to resolve, the marker
    // would never reach them, and the sweep would free storage a reachable chain
    // still points at. Writing the recorded runs and starting a fresh list keeps
    // the deferral for the common case without ever losing a run.
    if (page.pending_start_count >= GcPage::kMaxPendingStartRuns)
      materialize_page_starts_locked(page);
    page.pending_starts[page.pending_start_count++] =
        GcPage::PendingStartRun{static_cast<uint32_t>(begin),
                                static_cast<uint32_t>(end)};
  };
  for (const NrqGcPendingRange& range : mutator.deferred_ranges) append(range);
  for (uint32_t index = 0; index < fast.pending_range_count; ++index)
    append(fast.pending_ranges[index]);
  page.starts_pending = page.pending_start_count != 0;
}

// Applies a whole window to its page from the counters the allocator already
// kept. Every object the ranges describe is counted exactly once, and the
// statistics stay exact without a query having to walk anything.
void apply_window_locked(GcPage& page, size_t objects, size_t bytes,
                         bool young) {
  if (page.live_objects > SIZE_MAX - objects)
    nrq_runtime_fatal("GC page live-object count overflow");
  release_uncommitted_objects(page, objects);
  page.live_objects += objects;
  if (page.live_payload_bytes > SIZE_MAX - bytes)
    nrq_runtime_fatal("GC page live-byte count overflow");
  page.live_payload_bytes += bytes;
  if (young) {
    if (page.young_objects > SIZE_MAX - objects)
      nrq_runtime_fatal("GC page young-object count overflow");
    page.young_objects += objects;
  } else if (objects != 0) {
    old_object_count.fetch_add(objects, std::memory_order_relaxed);
  }
  statistics.allocated_bytes = saturated_add(statistics.allocated_bytes, bytes);
  statistics.live_objects = saturated_add(statistics.live_objects, objects);
  live_heap_bytes = saturated_add(live_heap_bytes, bytes);
}

// Writes the start bits for every run a page recorded. The spans come from the
// headers the objects already carry, so a page whose runs interleave payload
// sizes materialises exactly the way the eager publication walk did. The page's
// counters are already final; this only restores discovery. Idempotent.
void materialize_page_starts_locked(GcPage& page) {
  if (!page.starts_pending) return;
  const size_t header_bytes = page_header_bytes(page.kind);
  const bool is_h0 = page.kind == PageKind::Headerless;
  const bool is_compact = page.kind == PageKind::Compact;
  uint32_t cached_layout_id = 0;
  size_t cached_payload_bytes = 0;
  for (uint8_t index = 0; index < page.pending_start_count; ++index) {
    const GcPage::PendingStartRun run = page.pending_starts[index];
    if (run.end <= run.begin || run.end > page.capacity) continue;
    unsigned char* cursor = page.storage + run.begin;
    for (; cursor < page.storage + run.end;) {
      const ObjectRef object{cursor, &page};
      size_t payload_bytes = 0;
      if (is_compact) {
        const auto* header = reinterpret_cast<const uint32_t*>(cursor);
        const uint32_t layout_id = header[1];
        if (layout_id != 0 && layout_id == cached_layout_id) {
          payload_bytes = cached_payload_bytes;
        } else {
          payload_bytes = object_payload_bytes(object);
          if (layout_id != 0) {
            cached_layout_id = layout_id;
            cached_payload_bytes = payload_bytes;
          }
        }
      } else {
        payload_bytes = object_payload_bytes(object);
      }
      const size_t span = is_h0 ? page.slot_bytes
                                : object_span_bytes(header_bytes, payload_bytes);
      if (span == 0 || cursor + span > page.storage + run.end)
        nrq_runtime_fatal("GC pending range has an invalid span");
      mark_object_start(page, static_cast<size_t>(cursor - page.storage));
      cursor += span;
    }
    // A run was recorded from object starts, so walking its spans has to land
    // exactly on its end. A walk that stops short means a span was derived from
    // the wrong header, and every object after it would be left without a start
    // bit - invisible to the marker and therefore collected while reachable.
    if (cursor != page.storage + run.end)
      nrq_runtime_fatal("GC pending run does not end on an object boundary");
  }
  page.pending_start_count = 0;
  page.starts_pending = false;
}

void publish_tlab_locked(MutatorState& mutator) {
  if (!mutator.fast) return;
  const bool track_timing = tlab_diag.enabled.load(std::memory_order_relaxed);
  const auto publish_start = track_timing
                                 ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
  NrqGcFastTls& fast = *mutator.fast;
  sync_active_ranges(fast);
  const bool collection_active =
      collection_phase.load(std::memory_order_acquire) != CollectionPhase::Idle;
  const uint64_t next_epoch =
      collection_active ? next_cycle_epoch.load(std::memory_order_relaxed) : 0;
  // A window is closed at every page switch (see install_tlab), so its ranges
  // all describe the page its counters were spent on, and the mutator's own
  // counters give that page's object count and payload bytes without reading an
  // object. What is left is discovery, and that is recorded as runs and written
  // only when something actually resolves into the page.
  GcPage* window_page = window_page_of(mutator);
  const size_t window_objects = fast.pending_count;
  const size_t window_bytes = fast.pending_bytes;
  const bool young_now =
      generational_collection_enabled.load(std::memory_order_relaxed);
  const bool defer_window =
      !collection_active && window_page != nullptr && window_objects != 0 &&
      window_page->object_starts != nullptr &&
      !page_uses_index(window_page) &&
      (mutator.window_young != 0) == young_now &&
      static_cast<size_t>(window_page->pending_start_count) +
              mutator.deferred_ranges.size() + fast.pending_range_count <=
          GcPage::kMaxPendingStartRuns;
  if (defer_window) {
    record_window_runs_locked(*window_page, mutator);
    apply_window_locked(*window_page, window_objects, window_bytes, young_now);
  } else {
    for (const NrqGcPendingRange& range : mutator.deferred_ranges) {
      publish_one_range_locked(range, collection_active, next_epoch);
    }
    for (uint32_t range_index = 0; range_index < fast.pending_range_count;
         ++range_index) {
      publish_one_range_locked(fast.pending_ranges[range_index],
                               collection_active, next_epoch);
    }
  }
  mutator.deferred_ranges.clear();
  if (tlab_diag.enabled.load(std::memory_order_relaxed)) {
    tlab_diag.published_tlab_objects.fetch_add(
        fast.pending_count, std::memory_order_relaxed);
  }
  flush_mutator_accounting_locked(mutator);
  mutator.tlab_accounted_pending_bytes = 0;
  fast.pending_range_count = 0;
  clear_tlab_fast_path(fast);
  clear_h0_fast_path(fast);
  std::atomic_ref<size_t>(fast.pending_bytes).store(0, std::memory_order_relaxed);
  std::atomic_ref<size_t>(fast.pending_count).store(0, std::memory_order_relaxed);
  if (track_timing) {
    tlab_diag.publish_ns.fetch_add(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - publish_start)
                .count()),
        std::memory_order_relaxed);
  }
}

bool collection_is_requested() {
  return collection_requested.load(std::memory_order_acquire);
}

void wait_until_not_requested(std::unique_lock<std::mutex>& lock) {
  mutator_condition.wait(lock, [] {
    return !collection_requested.load(std::memory_order_acquire);
  });
}

// Park the calling mutator and wait for the request to clear. The caller holds
// mutator_mutex through `lock`, and the count it publishes is what the collector's
// wait predicate observes; parking mutators do not wake each other.
void park_mutator(std::unique_lock<std::mutex>& lock) {
  thread_mutator.parked = true;
  parked_mutator_count.fetch_add(1, std::memory_order_release);
  collector_condition.notify_one();
  wait_until_not_requested(lock);
  parked_mutator_count.fetch_sub(1, std::memory_order_release);
  thread_mutator.parked = false;
}

bool all_mutators_parked() {
  const size_t self = thread_mutator.depth != 0 ? 1 : 0;
  const size_t required =
      registered_mutator_count >= self ? registered_mutator_count - self : 0;
  return parked_mutator_count.load(std::memory_order_acquire) >= required;
}

void stop_mutators() {
  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) {
    if (thread_mutator.depth != 0) {
      park_mutator(lock);
    } else {
      wait_until_not_requested(lock);
    }
  }
  // Mutators observe the request at their next safepoint poll, so raising it only
  // has to wake this collector once each mutator reports in.
  collection_requested.store(true, std::memory_order_release);
  collector_condition.wait(lock, [] { return all_mutators_parked(); });
}

void resume_mutators() {
  std::lock_guard<std::mutex> lock(mutator_mutex);
  collection_requested.store(false, std::memory_order_release);
  mutator_condition.notify_all();
}

// Enter a lock-free barrier region. Only a registered mutator can be counted and
// therefore only a registered mutator may run the fast path; a thread that never
// entered the runtime is invisible to the handshake, so it has to use the paths
// guarded by gc_mutex. The increment is on the calling thread's own line, which
// turns what used to be a contended read-modify-write on shared state into a
// private one.
// Both sides of a barrier region are two relaxed stores and one acquire load.
// Out of line they cost a call and a register save on each side of every rooted
// allocation, which is a large fraction of a bump allocation; inlined, the
// caller sees that the region only publishes the depth around its own work.
__forceinline bool begin_fast_barrier() {
  if (thread_mutator.depth == 0) return false;
  // The counter lives on this thread's own cache line: only its owner writes it
  // and only the collector reads it, so no other thread can race the update.
  // A relaxed store plus an acquire-release fence is therefore equivalent to the
  // read-modify-write it replaces - the increment is ordered before everything
  // the region does, and the region's work is ordered before the decrement -
  // without a locked instruction. On x64 the fence is a compiler barrier only,
  // so entering a region costs a plain store instead of `lock xadd`.
  const uint32_t previous =
      thread_mutator.barrier_depth.load(std::memory_order_relaxed);
  thread_mutator.barrier_depth.store(previous + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acq_rel);
  if (collection_phase.load(std::memory_order_acquire) ==
      CollectionPhase::Idle)
    return true;
  std::atomic_thread_fence(std::memory_order_acq_rel);
  thread_mutator.barrier_depth.store(previous, std::memory_order_relaxed);
  return false;
}

__forceinline void end_fast_barrier() {
  const uint32_t previous =
      thread_mutator.barrier_depth.load(std::memory_order_relaxed);
  // Release the region's stores before the decrement becomes visible, so a
  // collector that observes a zero count has also observed the work.
  std::atomic_thread_fence(std::memory_order_acq_rel);
  thread_mutator.barrier_depth.store(previous - 1, std::memory_order_relaxed);
}

// Wait until every other registered mutator has left its barrier regions. The
// collector publishes Preparing first, so a mutator that observes the phase
// change retreats to the serialized path and never enters a new region; the loop
// only has to wait out the regions that were already in flight, and a mutator
// inside one finishes it before it can reach a safepoint.
//
// The calling thread's own entry is skipped. It can only be non-zero when the caller
// is itself inside a region, and waiting for it would spin until the caller's own
// region ended - which cannot happen, because the caller is this thread and it is
// busy waiting. Its region cannot be in flight against the enumeration the caller is
// about to perform either: one thread runs its stores and its enumeration in
// sequence, and the region ends before the next store.
void wait_for_fast_barriers() {
  if (heap_lock_held_by_this_thread)
    nrq_runtime_fatal(
        "GC lock order violated: heap lock held while waiting for barrier regions");
  for (;;) {
    bool busy = false;
    {
      std::lock_guard<std::mutex> lock(mutator_mutex);
      for (const MutatorState* mutator : mutators) {
        if (!mutator || mutator == &thread_mutator) continue;
        if (mutator->barrier_depth.load(std::memory_order_acquire) != 0 ||
            (mutator->fast &&
             std::atomic_ref<uint32_t>(mutator->fast->inline_barrier)
                     .load(std::memory_order_acquire) != 0)) {
          busy = true;
          break;
        }
      }
    }
    if (!busy) return;
    std::this_thread::yield();
  }
}

void trace_children(const ObjectRef& object, nrq_gc_visit_fn visit,
                    void* context) {
  // The descriptor is read once and holds the whole tracing contract, so the
  // common path is one dependent load rather than one per object field.
  const nrq_object_layout* layout = object_layout(object);
  if (!layout) nrq_runtime_fatal("GC object has no layout descriptor");
  const size_t payload_bytes = object_payload_bytes(object);
  if (layout->trace)
    layout->trace(object_payload(object), payload_bytes, visit, context);
  // A structure-of-arrays payload has no per-element reference map. Each managed
  // column is contiguous with its own stride, so it is walked column by column;
  // reading it as array elements would visit row-interleaved scalar slots.
  if (layout->soa_column_pairs != 0 && layout->soa_columns) {
    const size_t rows = layout->payload_size ? payload_bytes / layout->payload_size : 0;
    const auto* bytes =
        static_cast<const unsigned char*>(object_payload(object));
    for (uint32_t pair = 0; pair + 1 < layout->soa_column_pairs; pair += 2) {
      const size_t base = layout->soa_columns[pair];
      const size_t stride = layout->soa_columns[pair + 1];
      for (size_t row = 0; row < rows; ++row) {
        const size_t at = base + row * stride;
        if (at + sizeof(void*) > payload_bytes) break;
        void* referenced = nullptr;
        std::memcpy(&referenced, bytes + at, sizeof(void*));
        if (referenced) visit(referenced, context);
      }
    }
    return;
  }
  // The reference map lives in the descriptor: one table for all instances,
  // already proven to fit one element, so the walk needs one bounds test per
  // base rather than one per offset.
  if (layout->reference_count == 0) return;
  const size_t stride = (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY)
                            ? layout->payload_size
                            : payload_bytes;
  if (stride == 0) return;
  const auto* bytes =
      static_cast<const unsigned char*>(object_payload(object));
  for (size_t base = 0; base + stride <= payload_bytes; base += stride) {
    for (uint32_t offset_index = 0; offset_index < layout->reference_count;
         ++offset_index) {
      const size_t offset = layout->reference_offsets[offset_index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* child = nullptr;
      std::memcpy(&child, bytes + base + offset, sizeof(child));
      visit(child, context);
    }
  }
}

void enqueue_object_locked(const ObjectRef& object, MarkContext& context) {
  if (!object.valid()) return;
  GcPage& page = *object.page;
  // On a minor pass a page with zero young objects cannot contain a candidate.
  // On a major pass checking page.young_objects avoids touching object.header
  // during enqueue when the page holds only promoted objects.
  if (context.young_only && page.young_objects == 0) return;
  if (!page.mark_bits)
    nrq_runtime_fatal("GC object has no page mark bitmap");
  const size_t index = mark_bit_index(object);
  const size_t word_idx = index / 64;
  if (word_idx >= page.mark_words)
    nrq_runtime_fatal("GC object mark bit is outside its page");
  uint64_t& word = page.mark_bits[word_idx];
  const uint64_t bit = uint64_t{1} << (index % 64);
  if ((word & bit) != 0) return;
  if (object_born_in_cycle(object, context.epoch)) return;
  bool young = false;
  if (page.young_objects != 0) {
    young = object_young(object);
    if (context.young_only && !young) return;
  }
  word |= bit;
  ++page.marked_objects;
  if (young) ++page.marked_young_objects;
  context.worklist.push(object);
}

void resolve_reference_locked(void* object, MarkContext& context) {
  if (!object) return;
  // Roots may refer to static/native objects. Only collector-owned objects enter
  // the mark worklist, so marking never allocates auxiliary heap.
  enqueue_object_locked(find_object_locked(object), context);
}

void flush_reference_batch_locked(MarkContext& context) {
  cycle_timing.mark_references += context.reference_count;
  for (size_t index = 0; index < context.reference_count; ++index)
    resolve_reference_locked(context.references[index], context);
  context.reference_count = 0;
}

void enqueue_reference(void* object, void* opaque_context) {
  if (!object) return;
  auto* context = static_cast<MarkContext*>(opaque_context);
  if (context->reference_count < kReferenceBatchCapacity) {
    context->references[context->reference_count++] = object;
    return;
  }
  resolve_reference_locked(object, *context);
}

void mark_value_locked(void* object) {
  // Barriers resolve immediately: they run between marking slices and must be
  // visible before the collector can decide that marking is complete.
  if (!object || collection_phase.load(std::memory_order_relaxed) ==
                     CollectionPhase::Idle)
    return;
  resolve_reference_locked(object, active_mark);
}

void mark_reference_value(void* reference, void* /*context*/) {
  mark_value_locked(reference);
}

// An object allocated during a cycle is retained for that cycle without being
// traced: the sweep reads its birth bit and leaves it alone. The references it
// already carries were written into an unpublished object, so no barrier saw
// them, and they point at objects from earlier cycles. An untraced edge from a
// retained object is how a reachable object stays unmarked and is reclaimed -
// which is what a survivor whose stamp vanished looks like from outside. Marking
// those edges once, before the first sweep slice, is the allocation-is-black
// half of the rule the store barrier applies to every other new edge, and doing
// it here rather than at publication means no page can already have been swept.
void mark_born_references_locked() {
  const uint64_t epoch = active_cycle_epoch;
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page || page->born_this_cycle == 0) continue;
    const uint64_t* const start_bits = start_bits_for_scan_locked(*page);
    if (!start_bits) continue;
    for (size_t word = 0; word < page->object_start_words; ++word) {
      uint64_t bits = start_bits[word];
      while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        const ObjectRef object{
            page->storage + (word * 64 + index) * kObjectStartAlignment, page};
        if (!object_born_in_cycle(object, epoch)) continue;
        trace_children(object, mark_reference_value, nullptr);
      }
    }
  }
  for (const ObjectRef& object : unindexed_objects) {
    if (!object.valid() || !object_born_in_cycle(object, epoch)) continue;
    trace_children(object, mark_reference_value, nullptr);
  }
}

void mark_root_values_locked(void* storage, const nrq_object_layout* layout,
                             size_t repeated_count) {
  if (!layout || layout->reference_count == 0) return;
  auto* bytes = static_cast<unsigned char*>(storage);
  const size_t stride = layout->payload_size;
  const size_t elements = repeated_count != 0 ? repeated_count : 1;
  for (size_t element = 0; element < elements; ++element) {
    for (uint32_t index = 0; index < layout->reference_count; ++index) {
      const size_t offset = layout->reference_offsets[index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* reference = nullptr;
      std::memcpy(&reference, bytes + element * stride + offset,
                  sizeof(reference));
      mark_value_locked(reference);
    }
  }
}

void mark_root_values_locked(void* storage, const RootInfo& root) {
  mark_root_values_locked(storage, root.layout, root.repeated_count);
}

struct ReferenceCountTarget {
  void* object = nullptr;
  uint64_t count = 0;
};

// The diagnostic API asks about one object at a time. Comparing each visited
// reference against that target avoids building a map of every object in the
// heap and keeps the walk allocation-free.
void count_reference_to_target(void* object, void* opaque_context) {
  if (!object) return;
  auto* target = static_cast<ReferenceCountTarget*>(opaque_context);
  if (object == target->object)
    target->count = saturated_add(target->count, 1);
}

void enqueue_root_layout_locked(void* root_storage,
                                const nrq_object_layout* layout,
                                size_t repeated_count, MarkContext& context) {
  if (!layout || layout->reference_count == 0) return;
  auto* bytes = static_cast<const unsigned char*>(root_storage);
  const size_t stride = layout->payload_size;
  const size_t elements = repeated_count != 0 ? repeated_count : 1;
  for (size_t element = 0; element < elements; ++element) {
    const unsigned char* element_bytes = bytes + element * stride;
    for (uint32_t index = 0; index < layout->reference_count; ++index) {
      const size_t offset = layout->reference_offsets[index];
      if (offset > stride || sizeof(void*) > stride - offset) continue;
      void* reference = nullptr;
      std::memcpy(&reference, element_bytes + offset, sizeof(reference));
      enqueue_reference(reference, &context);
    }
  }
}

void enqueue_root_locked(void* root_storage, const RootInfo& root,
                         MarkContext& context) {
  enqueue_root_layout_locked(root_storage, root.layout, root.repeated_count,
                             context);
}

inline bool is_pointer_root_layout(const nrq_object_layout* layout);

template <typename Callback>
void for_each_fast_root_slot_locked(const ShadowStack& stack, Callback&& callback) {
  if (!stack.fast) return;
  if (stack.fast->root_cursor) {
    const NrqGcFastRoot* begin = stack.roots_buffer.data();
    const NrqGcFastRoot* end = stack.fast->root_cursor;
    for (const NrqGcFastRoot* root = begin; root < end; ++root) {
      callback(root->slot, decode_shadow_layout(root->layout));
    }
  }
  for (const NrqGcStackFrame* frame =
           std::atomic_ref<NrqGcStackFrame* const>(stack.fast->stack_frame_top)
               .load(std::memory_order_acquire);
       frame != nullptr; frame = frame->prev) {
    void* const* slots = reinterpret_cast<void* const*>(frame + 1);
    for (uint64_t index = 0; index < frame->slot_count; ++index) {
      callback(const_cast<void**>(&slots[index]), &kPointerRootLayout);
    }
  }
}

// Debug-only root audit at the sweep transition. A root slot that names a live
// object which this cycle neither marked nor stamped as born is the sharpest form
// of the mark-loss failure: the collector has already decided the object is not
// reachable while a mutator is about to read it. Only pointer-shaped roots are
// checked here, which is the shape a single slot with one reference has.
void verify_root_marks_locked() {
  if constexpr (!nirq::runtime_debug_enabled) return;
  static const bool enabled = std::getenv("NRQ_GC_VERIFY_MARKS") != nullptr;
  if (!enabled) return;
  size_t checked = 0;
  size_t lost = 0;
  auto audit = [&](void* slot, void* value) {
    if (!value) return;
    ++checked;
    const ObjectRef ref = find_object_locked(value);
    if (!ref.valid()) {
      ++lost;
      std::fprintf(stderr, "NRQ_ROOT_LOSS slot=%p value=%p unresolved\n", slot,
                   value);
      return;
    }
    if (object_born_in_cycle(ref, active_cycle_epoch) || object_marked(ref))
      return;
    // A minor cycle does not mark old objects: they are outside its reachability
    // question and only a full cycle can reclaim them. Reporting them would bury
    // the real loss.
    if (active_collection_is_minor && !object_young(ref)) return;
    ++lost;
    std::fprintf(stderr,
                 "NRQ_ROOT_LOSS slot=%p value=%p kind=%d live=%zu young=%zu "
                 "marked=%zu born=%zu\n",
                 slot, value, static_cast<int>(ref.page->kind),
                 ref.page->live_objects, ref.page->young_objects,
                 ref.page->marked_objects, ref.page->born_this_cycle);
  };
  for (const auto& root : roots) {
    if (!is_pointer_root_layout(root.second.layout)) continue;
    void* value = nullptr;
    std::memcpy(&value, root.first, sizeof(value));
    audit(root.first, value);
  }
  // A typed root registered through the runtime API lives on its owner's shadow
  // stack, not in the map above, so the audit has to read the same structures the
  // root snapshot reads.
  for (ShadowStack* stack : shadow_stacks) {
    if (!stack || !stack->fast) continue;
    std::lock_guard<std::mutex> stack_lock(stack->mutex);
    for_each_fast_root_slot_locked(
        *stack, [&](void* slot, const nrq_object_layout* layout) {
          if (!is_pointer_root_layout(layout)) return;
          void* value = nullptr;
          std::memcpy(&value, slot, sizeof(value));
          audit(slot, value);
        });
  }
  if (lost != 0)
    std::fprintf(stderr, "NRQ_ROOT_LOSS total=%zu of %zu\n", lost, checked);
}

void enqueue_roots_locked(MarkContext& context) {
  for (const auto& root : roots)
    enqueue_root_locked(root.first, root.second, context);
  // Local roots are owned by their threads. The mutex guards the vector while
  // the collector copies a consistent view; ordinary registration does not take
  // gc_mutex, so collection never blocks the whole world to add a stack root.
  for (ShadowStack* stack : shadow_stacks) {
    if (!stack || !stack->fast) continue;
    std::lock_guard<std::mutex> stack_lock(stack->mutex);
    for_each_fast_root_slot_locked(
        *stack, [&](void* slot, const nrq_object_layout* layout) {
          if (is_pointer_root_layout(layout)) {
            void* ref = nullptr;
            std::memcpy(&ref, slot, sizeof(ref));
            enqueue_reference(ref, &context);
          } else {
            enqueue_root_layout_locked(slot, layout, 0, context);
          }
        });
  }
  for (const auto& pinned : pinned_objects) {
    if (pinned.second != 0) enqueue_reference(pinned.first, &context);
  }
  if (context.young_only) {
    for (const ObjectRef& object : remembered_young)
      enqueue_object_locked(object, context);
  }
  flush_reference_batch_locked(context);
}

// Append a young object to the remembered set unless its flag says it is
// already there. The flag is what makes the flat list a set, so a repeated
// old-to-young store costs one bool test instead of a hash probe.
void add_remembered_locked(const ObjectRef& object) {
  if (!object.valid()) return;
  // The page bitmap is the deduplication test and no longer rewrites the
  // object's state word on every old-to-young store.
  if (object_remembered(object)) return;
  set_object_remembered(object, true);
  try {
    remembered_young.push_back(object);
  } catch (...) {
    set_object_remembered(object, false);
    nrq_runtime_fatal("GC remembered-set insertion failed");
  }
}

// Drop entries whose object was reclaimed or promoted since insertion. The list
// is only traversed between this point and the end of the cycle, so a stale
// entry is never dereferenced; compaction also releases the vector's capacity
// when the remembered set shrinks permanently.
void compact_remembered_locked() {
  remembered_young.erase(
      std::remove_if(remembered_young.begin(), remembered_young.end(),
                     [](const ObjectRef& object) {
                       // The flag alone is not proof of life: a reclaimed object's
                       // storage is not rewritten, and a whole-page reclaim can
                       // leave the old bit readable. Require the reference to
                       // still be the exact published start its payload resolves
                       // to.
                       if (!object.valid() || !object_remembered(object))
                         return true;
                       return find_object_locked(object_payload(object)).header !=
                              object.header;
                     }),
      remembered_young.end());
  if (remembered_young.capacity() > 1024 &&
      remembered_young.size() * 4 < remembered_young.capacity())
    std::vector<ObjectRef>(remembered_young.begin(), remembered_young.end())
        .swap(remembered_young);
}

void remember_reference(void* object, void* opaque_context) {
  (void)opaque_context;
  if (!object) return;
  const ObjectRef found = find_object_locked(object);
  if (!found.valid() || !object_young(found)) return;
  add_remembered_locked(found);
}

void remember_young_locked(void* object) {
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      !generational_barrier_active.load(std::memory_order_acquire) ||
      !object)
    return;
  const ObjectRef found = find_object_locked(object);
  if (found.valid() && object_young(found)) add_remembered_locked(found);
}

void remember_children_locked(const ObjectRef& object) {
  trace_children(object, remember_reference, nullptr);
}

void defer_remembered_locked(void* object) {
  if (!object || !generational_barrier_active.load(std::memory_order_acquire))
    return;
  const ObjectRef found = find_object_locked(object);
  if (found.valid() && object_young(found)) {
    add_remembered_locked(found);
    return;
  }
  if (!found.valid()) {
    try {
      unresolved_remembered.insert(object);
    } catch (...) {
      nrq_runtime_fatal("GC remembered-set overflow failed");
    }
  }
}

void drain_remembered_locked(MutatorState& mutator, bool defer_unresolved) {
  for (size_t index = 0; index < mutator.remembered_count; ++index) {
    void* object = mutator.remembered[index];
    if (!object) continue;
    const ObjectRef found = find_object_locked(object);
    if (found.valid() && object_young(found)) {
      add_remembered_locked(found);
    } else if (!found.valid() && defer_unresolved) {
      try {
        unresolved_remembered.insert(object);
      } catch (...) {
        nrq_runtime_fatal("GC remembered-set overflow failed");
      }
    }
  }
  mutator.remembered_count = 0;
}

ThreadGcStateCleanup::~ThreadGcStateCleanup() {
  if (!armed) return;
  ShadowStack* stack = thread_shadow_stack;
  // Match the mutator_leave lock order (mutator_mutex then gc_mutex) so a
  // thread that exits while another thread is leaving cannot deadlock.
  std::lock_guard<std::mutex> mutator_lock(mutator_mutex);
  if (thread_mutator.depth != 0) {
    {
      HeapLockGuard heap_lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
      drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
      drain_transfer_pins_locked(thread_mutator);
    }
    auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
    if (found != mutators.end()) {
      mutators.erase(found);
      --registered_mutator_count;
      collector_condition.notify_all();
    }
    thread_mutator.depth = 0;
    thread_mutator.parked = false;
    mutator_condition.notify_all();
  }
  if (stack) {
    // Remove the stack under gc_mutex: enqueue_roots_locked enumerates the
    // registry under the same lock, so the collector can no longer reach it.
    HeapLockGuard heap_lock(gc_mutex);
    auto found = std::find(shadow_stacks.begin(), shadow_stacks.end(), stack);
    if (found != shadow_stacks.end()) shadow_stacks.erase(found);
    nrq_gc_fast_tls.root_cursor = nullptr;
    nrq_gc_fast_tls.root_end = nullptr;
    nrq_gc_fast_tls.frame_cursor = nullptr;
    nrq_gc_fast_tls.frame_end = nullptr;
    std::atomic_ref<NrqGcStackFrame*>(nrq_gc_fast_tls.stack_frame_top)
        .store(nullptr, std::memory_order_release);
    stack->fast = nullptr;
    thread_shadow_stack = nullptr;
  }
  delete stack;
}

void resolve_unresolved_remembered_locked() {
  for (void* object : unresolved_remembered) {
    const ObjectRef found = find_object_locked(object);
    if (!found.valid() || !object_young(found)) continue;
    add_remembered_locked(found);
  }
  unresolved_remembered.clear();
}

void buffer_remembered(void* object);

// Append a referent to the thread's remembered buffer. The caller has already
// checked the published barrier switch, so this is the only work a barrier does.
void append_remembered(void* object) {
  if (!object) return;
  MutatorState& mutator = thread_mutator;
  if (mutator.remembered_count < kRememberedBufferCapacity) {
    mutator.remembered[mutator.remembered_count++] = object;
    return;
  }
  HeapLockGuard lock(gc_mutex);
  publish_tlab_locked(mutator);
  drain_remembered_locked(mutator, /*defer_unresolved=*/true);
  defer_remembered_locked(object);
}

void buffer_pointer_words(void* storage, size_t bytes) {
  // One acquire load covers the whole store: the switch is read here instead of
  // once per machine word. A switch that drops mid-loop only leaves extra
  // entries, which the next drain discards unless they name young objects.
  if (!store_barrier_active.load(std::memory_order_acquire)) return;
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    append_remembered(reference);
  }
}

void buffer_remembered(void* object) {
  if (!store_barrier_active.load(std::memory_order_acquire)) return;
  append_remembered(object);
}

// The ordinary record carries one or two pointer fields and a single element, and
// its layout is already in the header. For that shape the trace callback adds
// nothing: it walks the same two words the marker can read here, through an
// indirect call per object. Anything else - a callback, a typed array, a wider
// layout - still goes through trace_children.
void enqueue_narrow_children_locked(const ObjectRef& object,
                                    size_t header_bytes,
                                    const nrq_object_layout* layout,
                                    size_t payload_bytes,
                                    MarkContext& context) {
  const auto* bytes =
      static_cast<const unsigned char*>(object.header + header_bytes);
  // The descriptor and the payload size are the ones the caller already
  // resolved for its budget accounting and its narrow-shape test. Resolving them
  // again here cost a second descriptor lookup and a second element-count/size
  // multiply for every traced object, so they are passed in instead.
  //
  // Hot narrow records keep their two offsets inside the descriptor. That turns
  // the common case into one load from the descriptor line the marker already
  // read for `reference_count`, instead of a dependent load of the offset table.
  const bool inline_map =
      (layout->flags & NRQ_OBJECT_LAYOUT_INLINE_MAP) != 0;
  for (uint32_t index = 0; index < layout->reference_count; ++index) {
    const size_t offset = inline_map
                              ? layout->inline_reference_offsets[index]
                              : layout->reference_offsets[index];
    if (offset > payload_bytes || sizeof(void*) > payload_bytes - offset)
      continue;
    void* child = nullptr;
    std::memcpy(&child, bytes + offset, sizeof(child));
    if (child)
      enqueue_object_locked(find_child_in_page_locked(child, object.page),
                            context);
  }
}

bool mark_slice_locked(size_t budget_bytes) {
  size_t touched = 0;
  while (!active_mark.worklist.empty() && touched < budget_bytes) {
    ++cycle_timing.mark_objects;
    const ObjectRef object = active_mark.worklist.pop();
    const PageKind kind = object.page->kind;
    const size_t header_bytes = page_header_bytes(kind);
    const nrq_object_layout* layout = object_layout(object);
    const uint32_t elem_count =
        (kind == PageKind::Headerless || kind == PageKind::Compact)
            ? 1u
            : reinterpret_cast<const ExtendedHeader*>(object.header)->element_count;
    const size_t payload_bytes =
        static_cast<size_t>(elem_count) * layout->payload_size;
    touched += payload_bytes + header_bytes;
    if (active_mark.young_only && !object_young(object)) continue;
    const size_t reference_count = layout->reference_count;
    // A one-element array has the geometry of a record: its payload is the single
    // element and the descriptor's offsets are relative to that element, so the
    // offset walk answers it exactly. Only a larger array needs the layout
    // callback, because only that path knows the element stride.
    const bool narrow_shape = !layout->trace && reference_count != 0 &&
                              reference_count <= 2 &&
                              ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0 ||
                               elem_count == 1u);
    if (narrow_shape) {
      enqueue_narrow_children_locked(object, header_bytes, layout,
                                     payload_bytes, active_mark);
    } else {
      trace_children(object, enqueue_reference, &active_mark);
      if (active_mark.reference_count != 0)
        flush_reference_batch_locked(active_mark);
    }
  }
  if (active_mark.reference_count != 0)
    flush_reference_batch_locked(active_mark);
  return active_mark.worklist.empty() && active_mark.reference_count == 0;
}

// Debug-only closure diff at the sweep transition. Every managed object's mark is
// snapshotted, marks are cleared, and reachability is recomputed from the roots
// under the same lock. An object the second walk reaches that the first did not
// mark, and that was not born in this cycle, is an interior edge the marker
// dropped: the sweep is about to reclaim storage that is still reachable. The
// marks left behind are the recomputed closure, which is a superset of what the
// collector had, so running the diff cannot make the cycle less safe.
struct ClosureLoss
{
  void* object = nullptr;
  int32_t kind = 0;
  size_t live = 0;
  size_t marked = 0;
  size_t born = 0;
};

struct ParentSearch {
  void* target = nullptr;
  ObjectRef current;
  ObjectRef parent;
};

void closure_parent_visit(void* reference, void* opaque) {
  auto* search = static_cast<ParentSearch*>(opaque);
  if (search->parent.valid() || reference != search->target) return;
  search->parent = search->current;
}

// The mark set the transition produced, for the sweep to compare against. It is
// filled only by the verification walk below; the sweep reads it only when that
// walk ran for the same cycle.
std::vector<void*> transition_marks;
uint64_t transition_marks_epoch = 0;

void closure_diff_locked(int pass) {
  if constexpr (!nirq::runtime_debug_enabled) return;
  static const bool enabled = std::getenv("NRQ_GC_VERIFY_MARKS") != nullptr;
  if (!enabled) return;
  std::vector<std::pair<void*, ObjectRef>> was_marked;
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page) continue;
    const uint64_t* const start_bits = start_bits_for_scan_locked(*page);
    if (!start_bits) continue;
    for (size_t word = 0; word < page->object_start_words; ++word) {
      uint64_t bits = start_bits[word];
      while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        const ObjectRef object{
            page->storage + (word * 64 + index) * kObjectStartAlignment, page};
        if (object_marked(object))
          was_marked.emplace_back(object_payload(object), object);
      }
    }
  }
  for (const ObjectRef& object : unindexed_objects) {
    if (object.valid() && object_marked(object))
      was_marked.emplace_back(object_payload(object), object);
  }
  std::sort(was_marked.begin(), was_marked.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
  auto was_marked_contains = [&](void* payload) {
    const auto found = std::lower_bound(
        was_marked.begin(), was_marked.end(), payload,
        [](const std::pair<void*, ObjectRef>& entry, void* value) {
          return entry.first < value;
        });
    return found != was_marked.end() && found->first == payload;
  };

  for (auto& entry : gc_pages) {
    if (entry) clear_page_marks(*entry);
  }
  active_mark.reset();
  active_mark.young_only = active_collection_is_minor;
  active_mark.epoch = active_cycle_epoch;
  enqueue_roots_locked(active_mark);
  for (int slice = 0; slice < 1000000; ++slice) {
    if (mark_slice_locked(size_t{1} << 26)) break;
  }
  // The ordinary walk never traces a born object, so the recomputed closure is
  // only comparable to the collector's once the same pass has run against it.
  // Without this the diff would credit itself with every edge the born pass owns
  // and, worse, would clear marks the pass had already contributed.
  mark_born_references_locked();

  size_t losses = 0;
  auto report = [&](const ObjectRef& object) {
    if (!object_marked(object)) return;
    if (object_born_in_cycle(object, active_cycle_epoch)) return;
    void* const payload = object_payload(object);
    if (was_marked_contains(payload)) return;
    ++losses;
    if (losses > 8) return;
    ParentSearch search;
    search.target = payload;
    auto scan = [&](const ObjectRef& candidate) {
      if (search.parent.valid() || !candidate.valid()) return;
      search.current = candidate;
      trace_children(candidate, closure_parent_visit, &search);
    };
    for (auto& entry : gc_pages) {
      GcPage* page = entry.get();
      if (!page) continue;
      const uint64_t* const start_bits = start_bits_for_scan_locked(*page);
      if (!start_bits) continue;
      for (size_t word = 0; word < page->object_start_words; ++word) {
        uint64_t bits = start_bits[word];
        while (bits != 0) {
          const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
          bits &= bits - 1;
          scan(ObjectRef{
              page->storage + (word * 64 + index) * kObjectStartAlignment,
              page});
        }
      }
    }
    for (const ObjectRef& candidate : unindexed_objects) scan(candidate);
    std::fprintf(stderr,
                 "CLOSURE_LOSS pass=%d object=%p kind=%d live=%zu marked=%zu "
                 "born=%zu young=%d parent=%p parent_born=%d parent_marked=%d "
                 "parent_young=%d minor=%d young_only=%d\n",
                 pass, payload, static_cast<int>(object.page->kind),
                 object.page->live_objects, object.page->marked_objects,
                 object.page->born_this_cycle,
                 static_cast<int>(object_young(object)),
                 object_payload(search.parent),
                 search.parent.valid()
                     ? static_cast<int>(object_born_in_cycle(search.parent,
                                                             active_cycle_epoch))
                     : -1,
                 search.parent.valid()
                     ? static_cast<int>(object_marked(search.parent))
                     : -1,
                 search.parent.valid()
                     ? static_cast<int>(object_young(search.parent))
                     : -1,
                 static_cast<int>(active_collection_is_minor),
                 static_cast<int>(active_mark.young_only));
  };
  for (auto& entry : gc_pages) {
    GcPage* page = entry.get();
    if (!page) continue;
    const uint64_t* const start_bits = start_bits_for_scan_locked(*page);
    if (!start_bits) continue;
    for (size_t word = 0; word < page->object_start_words; ++word) {
      uint64_t bits = start_bits[word];
      while (bits != 0) {
        const unsigned index = static_cast<unsigned>(std::countr_zero(bits));
        bits &= bits - 1;
        report(ObjectRef{
            page->storage + (word * 64 + index) * kObjectStartAlignment, page});
      }
    }
  }
  for (const ObjectRef& object : unindexed_objects) report(object);
  if (losses != 0)
    std::fprintf(stderr, "CLOSURE_LOSS total=%zu of %zu marked pass=%d\n",
                 losses, was_marked.size(), pass);
  // Put the collector's own mark set back. The re-walk above was only ever a
  // question; leaving its answer in place would let the verification retain
  // storage the collector had decided to reclaim, which is how a diagnostic
  // turns into a silent repair and hides the very race it looks for.
  for (auto& entry : gc_pages) {
    if (entry) clear_page_marks(*entry);
  }
  for (const auto& entry : was_marked) set_object_marked(entry.second, true);
  transition_marks.clear();
  transition_marks.reserve(was_marked.size());
  for (const auto& entry : was_marked) transition_marks.push_back(entry.first);
  std::sort(transition_marks.begin(), transition_marks.end());
  transition_marks_epoch = active_cycle_epoch;
}

// Sweep state. A cycle reclaims along the page set instead of following one
// global list: pages are visited in allocation order and each page's start
// bitmap in address order, which is also the order a page's spans want to be
// released in, so each freed run extends the recorded one with no ordered
// insert. Objects whose page has no walkable bitmap - a large-object page, or a
// page the directory refused - keep the detached list and are swept after the
// pages.
struct SweepCursor {
  size_t page = 0;
  size_t page_limit = 0;
  size_t word = 0;
  // Set bits of `word` that this sweep has not examined yet. A bit position is
  // not enough: resuming mid-word by re-testing positions would visit every
  // zero bit between object starts, which is 94 % of a full page's bitmap.
  uint64_t mask = 0;
  // The page's reuse generation when this sweep last started on it. A mismatch
  // means the page was emptied and handed out again mid-sweep.
  uint64_t page_generation = 0;
};
SweepCursor sweep_cursor;

// True when a payload with a registered reference-count table lives inside this
// page's bump area. The whole-page reclaim path has to know: it never visits an
// object, so the tables it holds would survive the storage they describe.
bool page_holds_shared_field_registrations(const GcPage& page) {
  SharedFieldRegistrations& registry = shared_field_registrations();
  std::lock_guard<std::mutex> lock(registry.mutex);
  if (registry.fields.empty()) return false;
  const unsigned char* const begin = page.storage;
  const unsigned char* const end = page.storage + page.used;
  for (const auto& entry : registry.fields) {
    const auto* address = static_cast<const unsigned char*>(entry.first);
    if (address >= begin && address < end) return true;
  }
  return false;
}

bool sweep_chunk_locked(size_t budget_bytes) {
  size_t steps = 0;
  size_t touched = 0;
  size_t bitmap_words = 0;
  size_t empty_words = 0;
  std::chrono::steady_clock::time_point chunk_start;
  if constexpr (nirq::runtime_debug_enabled)
    chunk_start = std::chrono::steady_clock::now();
  // Promotion and reclamation both move objects between generations. The
  // generation counters used to be updated with one atomic per object, which
  // meant two shared read-modify-write operations for every survivor and one for
  // every reclaimed old object. They are accumulated here and applied once per
  // chunk; every other reader of these counters holds gc_mutex, which this
  // function holds for the whole chunk, so no one can observe the intermediate
  // value.
  size_t reclaimed_old = 0;
  size_t promoted_young = 0;
  bool promotion_raised_barrier = false;
  // Reclaims one object, or leaves it for the next cycle. Returns true when the
  // object was freed, so the caller cannot go on to treat it as a survivor.
  auto sweep_object = [&](const ObjectRef& object) -> bool {
    ++cycle_timing.sweep_objects;
    GcPage* const page = object.page;
    // One read of the state word answers every question this function asks:
    // young, marked, and the value the survivor path rewrites.
    const uint32_t state = object_state(object);
    const bool young = (state & kYoungBit) != 0;
    const bool marked = object_marked(object);
    const bool collectable = !active_collection_is_minor || young;
    if (!marked && collectable) {
      if constexpr (nirq::runtime_debug_enabled) {
        static const bool verify_marks =
            std::getenv("NRQ_GC_VERIFY_MARKS") != nullptr;
        if (verify_marks &&
            active_cycle_epoch == transition_marks_epoch &&
            std::binary_search(transition_marks.begin(), transition_marks.end(),
                               object_payload(object)))
          std::fprintf(stderr,
                       "SWEEP_LOST_MARK object=%p kind=%d cycle=%llu live=%zu\n",
                       object_payload(object), static_cast<int>(page->kind),
                       static_cast<unsigned long long>(active_cycle_epoch),
                       page->live_objects);
      }
      const size_t payload_bytes = object_payload_bytes(object);
      const size_t object_offset = static_cast<size_t>(
          object.header - page->storage);
      const size_t span = page->kind == PageKind::Headerless
                              ? page->slot_bytes
                              : object_span_bytes(page_header_bytes(*page),
                                                  payload_bytes);
      if (page_uses_index(page)) {
        object_index.erase(object_payload(object));
        if (page->indexed_objects == 0)
          nrq_runtime_fatal("GC page indexed-object count underflow");
        --page->indexed_objects;
      } else {
        clear_object_start(*page, object_offset);
      }
      // The object's storage becomes reusable space in its own page instead of
      // being lost to the bump cursor.
      release_span_locked(*page, object_offset, span);
      if (!young) ++reclaimed_old;
      statistics.collected_bytes = saturated_add(
          statistics.collected_bytes, static_cast<uint64_t>(payload_bytes));
      if (statistics.live_objects != 0) --statistics.live_objects;
      live_heap_bytes = live_heap_bytes > payload_bytes
                            ? live_heap_bytes - payload_bytes
                            : 0;
      if (page->live_objects == 0)
        nrq_runtime_fatal("GC page live-object count underflow");
      if (page->live_payload_bytes < payload_bytes)
        nrq_runtime_fatal("GC page live-byte count underflow");
      page->live_payload_bytes -= payload_bytes;
      --page->live_objects;
      if (young) --page->young_objects;
      set_object_remembered(object, false);
      if (page->live_objects == 0) {
        page->retired = true;
        ++retired_page_count;
      }
      record_sweep_object_locked(object, false);
      forget_object_storage(object);
      return true;
    }

    if (marked) {
      // The off-heap mark is cleared before any early return, so a promoted
      // object never leaves a stale survivor count behind.
      set_object_marked(object, false);
      // Everything the survivor needs to become one word: an aging or promoting
      // object rewrites its age. The fields left untouched keep their bits,
      // which a mask guarantees and a sequence of separate flag stores cannot.
      uint32_t survived = state;
      const bool was_young = (survived & kYoungBit) != 0;
      if (active_collection_is_minor) {
        if (was_young) {
          uint32_t age = (survived & kAgeMask) >> kAgeShift;
          if (age < UINT8_MAX) ++age;
          if (age >= kPromotionAge) {
            survived &= ~(kYoungBit | kAgeMask);
            set_object_remembered(object, false);
            set_object_state(object, survived);
            --page->young_objects;
            ++promoted_young;
            promotion_raised_barrier = true;
            remember_children_locked(object);
            debug_emit(NRQ_GC_EVENT_PROMOTE, 0, 0, object_payload(object));
            return false;
          }
          survived = (survived & ~kAgeMask) | (age << kAgeShift);
        }
        set_object_state(object, survived);
      } else {
        if (was_young) {
          --page->young_objects;
          ++promoted_young;
          promotion_raised_barrier = true;
        }
        survived &= ~(kYoungBit | kAgeMask);
        set_object_remembered(object, false);
        set_object_state(object, survived);
        if (was_young)
          debug_emit(NRQ_GC_EVENT_PROMOTE, 0, 0, object_payload(object));
      }
    }
    return false;
  };

  // Phase 1: pages that can be walked by their own start bitmap. Only set bits
  // are object starts, so each iteration consumes one set bit; zero bits are
  // never visited. The remaining mask is part of the cursor, which is what
  // makes a chunk that stops mid-word resume without re-testing anything.
  while (sweep_cursor.page < sweep_cursor.page_limit &&
         touched < budget_bytes &&
         bitmap_words < kSweepBitmapWordsPerChunk) {
    GcPage* page = gc_pages[sweep_cursor.page].get();
    if (sweep_cursor.word == 0 && sweep_cursor.mask == 0)
      sweep_cursor.page_generation = page ? page->reuse_generation : 0;
    if (page && page->reuse_generation != sweep_cursor.page_generation) {
      // Between two chunks this page was emptied and its storage handed back to
      // a mutator. Its start and mark bitmaps now describe objects that were
      // allocated after this cycle began, so a walk would reclaim storage the
      // mutator is still writing into. The next cycle owns those objects.
      bitmap_words += page->object_start_words != 0 ? page->object_start_words
                                                   : size_t{1};
      ++sweep_cursor.page;
      sweep_cursor.word = 0;
      sweep_cursor.mask = 0;
      continue;
    }
    if (!page || page_uses_index(page) || page->object_start_words == 0 ||
        page->live_objects == 0 ||
        (active_collection_is_minor && page->young_objects == 0)) {
      // A page transition is work even when it examines no object: an empty
      // page still has to be reached, tested and stepped over. Charge its whole
      // bitmap so a run of empty pages cannot outlast the word budget.
      bitmap_words += (page && page->object_start_words != 0)
                          ? page->object_start_words
                          : size_t{1};
      ++sweep_cursor.page;
      sweep_cursor.word = 0;
      sweep_cursor.mask = 0;
      continue;
    }
    // A fresh page can be classified from its counts. This is the whole-page
    // form of lazy sweep: either every object survives, in which case only the
    // page's marks need clearing, or none does, in which case the page can be
    // retired without reading a single header. Pages already part-way through
    // their bitmap keep using the per-object path below.
    if (sweep_cursor.word == 0 && sweep_cursor.mask == 0) {
      const size_t live = page->live_objects;
      const size_t young = page->young_objects;
      const size_t survivors = page->marked_objects + page->born_this_cycle;
      const bool all_live = active_collection_is_minor
          ? (page->marked_young_objects == 0 &&
             (young == 0 || page->born_this_cycle >= young))
          : (survivors >= live && page->marked_young_objects == 0);
      if (all_live) {
        clear_page_marks(*page);
        bitmap_words += page->object_start_words;
        ++sweep_cursor.page;
        sweep_cursor.word = 0;
        continue;
      }
      // A page is only entirely dead once every object a mutator has written
      // into it has been published. Until then the counters cannot see that
      // object, so retiring the page would free storage a mutator is still
      // filling - and the next reservation on the page would zero the header of
      // an object the collector is then asked to trace.
      const bool no_uncommitted =
          page->uncommitted_objects.load(std::memory_order_relaxed) == 0;
      const bool all_dead = no_uncommitted &&
          (active_collection_is_minor
               ? (live == young && page->marked_objects == 0 &&
                  page->born_this_cycle == 0)
               : (page->marked_objects == 0 && page->born_this_cycle == 0));
      // A page that holds a registered payload cannot take the whole-page path:
      // that path retires the page without reading a header, so a reference count
      // registered for one of its objects would never be dropped. Falling through
      // to the per-object sweep is what runs the finalizer for such a page.
      const bool holds_registered_payloads =
          all_dead && page_holds_shared_field_registrations(*page);
      if (all_dead && !holds_registered_payloads) {
        // Managed payloads have no destructors, so there is nothing to run per
        // object. The page keeps its storage until the next cycle boundary, but
        // its start bits and marks are cleared now; an allocation can reuse the
        // page immediately.
        const size_t payload_bytes = page->live_payload_bytes;
        statistics.collected_bytes = saturated_add(
            statistics.collected_bytes, static_cast<uint64_t>(payload_bytes));
        if (statistics.live_objects >= live) {
          statistics.live_objects -= live;
        } else {
          statistics.live_objects = 0;
        }
        live_heap_bytes = live_heap_bytes > payload_bytes
                              ? live_heap_bytes - payload_bytes
                              : 0;
        reclaimed_old += live - young;
        page->live_objects = 0;
        page->young_objects = 0;
        page->live_payload_bytes = 0;
        page->born_this_cycle = 0;
        // The counters were applied at publication, so the payload bytes above
        // are exact even though no object was read; the recorded runs describe
        // storage that is now free and must not outlive it.
        page->pending_start_count = 0;
        page->starts_pending = false;
        clear_page_marks(*page);
        clear_page_remembered(*page);
        if (page->object_starts && page->object_start_words != 0) {
          std::memset(page->object_starts, 0,
                      page->object_start_words * sizeof(uint64_t));
        }
        // The whole bump area is now reusable. Existing runs are all inside
        // that area, so discard them before recording one covering run; adding
        // it beside them would leave overlapping runs and break the free-span
        // invariant.
        if (page->free_span_count != 0) {
          unindex_span_page_locked(*page);
          total_free_spans -= page->free_span_count;
          page->free_span_count = 0;
          page->free_span_bytes = 0;
        }
        set_page_max_free_span_locked(*page, 0, 0);
        record_sweep_page_locked(*page, live);
        if (page->used != 0) release_span_locked(*page, 0, page->used);
        page->retired = true;
        ++retired_page_count;
        bitmap_words += page->object_start_words;
        ++sweep_cursor.page;
        sweep_cursor.word = 0;
        continue;
      }
    }
    if (sweep_cursor.mask == 0) {
      if (sweep_cursor.word >= page->object_start_words) {
        bitmap_words += page->object_start_words;
        ++sweep_cursor.page;
        sweep_cursor.word = 0;
        continue;
      }
      // Snapshot the word. A mutator may set a start bit between slices, but
      // that object's birth epoch is past this cycle and it is skipped below.
      // Only this sweep clears start bits, and only for objects it has already
      // consumed, so the snapshot cannot resurrect or lose one of those.
      sweep_cursor.mask = page->object_starts[sweep_cursor.word];
      ++bitmap_words;
      if (sweep_cursor.mask == 0) {
        ++empty_words;
        ++sweep_cursor.word;
        continue;
      }
    }
    const size_t word_index = sweep_cursor.word;
    const uint64_t mask = sweep_cursor.mask;
    const unsigned index = static_cast<unsigned>(std::countr_zero(mask));
    // Consume the bit before the object can clear it, and advance the word as
    // soon as it is empty. The cursor must be committed before any `continue`.
    sweep_cursor.mask = mask & (mask - uint64_t{1});
    if (sweep_cursor.mask == 0) ++sweep_cursor.word;
    const size_t slot = word_index * 64 + index;
    const size_t span_offset = slot * kObjectStartAlignment;
    const ObjectRef object{page->storage + span_offset, page};
    ++steps;
    touched += object_payload_bytes(object) + object_header_bytes(object);
    // An object allocated after the cycle started is outside the snapshot's
    // reachability analysis. It stays where it is for this cycle and the next
    // cycle considers it.
    if (object_born_in_cycle(object, active_cycle_epoch)) {
      set_object_marked(object, false);
      continue;
    }
    // A per-object free can empty this page in the middle of its own walk: the
    // page is retired the moment its last object is reclaimed, and the space goes
    // back to a mutator immediately. The cursor is still part-way through the
    // bitmap, so the generation has to be checked here, per object, and not only
    // where a page is entered: the storage under the next set bit may already
    // hold an object this cycle has never seen.
    if (page->reuse_generation != sweep_cursor.page_generation) {
      bitmap_words += page->object_start_words != 0 ? page->object_start_words
                                                   : size_t{1};
      ++sweep_cursor.page;
      sweep_cursor.word = 0;
      sweep_cursor.mask = 0;
      continue;
    }
    sweep_object(object);
  }

  // Phase 2: objects whose page has no walkable start bitmap.
  while (!sweep_list.empty() && touched < budget_bytes) {
    const ObjectRef object = sweep_list.back();
    sweep_list.pop_back();
    ++steps;
    touched += object_payload_bytes(object) + object_header_bytes(object);
    if (!object.valid())
      nrq_runtime_fatal("GC swept object has no owning page");
    const bool born = object_born_in_cycle(object, active_cycle_epoch);
    if (born) set_object_marked(object, false);
    if (!born && sweep_object(object)) continue;
    try {
      unindexed_objects.push_back(object);
    } catch (...) {
      nrq_runtime_fatal("GC unindexed object list allocation failed");
    }
  }

  if (reclaimed_old != 0) {
    const size_t current = old_object_count.load(std::memory_order_relaxed);
    old_object_count.store(current > reclaimed_old ? current - reclaimed_old : 0,
                           std::memory_order_relaxed);
  }
  if (promoted_young != 0) {
    const size_t current = old_object_count.load(std::memory_order_relaxed);
    old_object_count.store(
        current > SIZE_MAX - promoted_young ? SIZE_MAX : current + promoted_young,
        std::memory_order_relaxed);
  }
  // One release store per chunk is enough: the flag only has to be visible
  // before any mutator can publish an old-to-young edge, and mutators cannot run
  // new statements while this chunk holds the heap lock.
  if (promotion_raised_barrier) raise_store_barrier();
  debug_emit(NRQ_GC_EVENT_SWEEP, static_cast<uint32_t>(reclaimed_old), steps,
             nullptr);
  debug_emit(NRQ_GC_EVENT_SWEEP_WORK, static_cast<uint32_t>(steps),
             (static_cast<uint64_t>(bitmap_words) << 32) | empty_words,
             nullptr);
  if constexpr (nirq::runtime_debug_enabled) {
    const auto elapsed = std::chrono::steady_clock::now() - chunk_start;
    debug_emit(
        NRQ_GC_EVENT_SWEEP_CHUNK_NS, 0,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count()),
        nullptr);
  }
  return sweep_list.empty() && sweep_cursor.page >= sweep_cursor.page_limit;
}

// Largest run currently recorded in a page, and how many runs have that size.
// The count lets a take keep the maximum exact in constant time while another
// run of the same size remains; only the last one forces a rescan. The debug
// build re-derives both so a missed update fails the test suite instead of
// silently sending refills down the wrong path.
size_t largest_free_run(const GcPage& page, size_t& count) {
  size_t largest = 0;
  count = 0;
  for (size_t index = 0; index < page.free_span_count; ++index) {
    const size_t span = page.free_spans[index].span;
    if (span > largest) {
      largest = span;
      count = 1;
    } else if (span == largest) {
      ++count;
    }
  }
  return largest;
}

void set_page_max_free_span_locked(GcPage& page, size_t span, size_t count) {
  if constexpr (nirq::runtime_debug_enabled) {
    size_t actual_count = 0;
    const size_t actual = largest_free_run(page, actual_count);
    if (actual != span || actual_count != count)
      nrq_runtime_fatal("GC free-run size index is inconsistent");
  }
  const bool was_slab = page.max_free_span >= kGcTlabMinSlabBytes;
  page.max_free_span = span;
  page.max_free_span_count = count;
  const bool is_slab = span >= kGcTlabMinSlabBytes;
  if (was_slab == is_slab) return;
  if (is_slab) {
    ++pages_with_slab_run;
  } else {
    if (pages_with_slab_run == 0)
      nrq_runtime_fatal("GC slab-run page count underflow");
    --pages_with_slab_run;
  }
}

void refresh_page_max_free_span_locked(GcPage& page) {
  size_t count = 0;
  const size_t largest = largest_free_run(page, count);
  set_page_max_free_span_locked(page, largest, count);
}

// Account for one run changing from `before` bytes to `after` bytes (0 means it
// was inserted or removed). Maintaining the count of maximum-sized runs is what
// keeps this O(1): only the disappearance of the last run of the maximum size
// needs the run list walked again.
void adjust_page_max_free_span_locked(GcPage& page, size_t before,
                                      size_t after) {
  const size_t maximum = page.max_free_span;
  if (before == maximum && before != 0) {
    if (after > maximum) {
      set_page_max_free_span_locked(page, after, 1);
    } else if (after < maximum) {
      if (page.max_free_span_count > 1) {
        set_page_max_free_span_locked(page, maximum,
                                      page.max_free_span_count - 1);
      } else {
        refresh_page_max_free_span_locked(page);
      }
    }
    return;
  }
  // The changed run was below the maximum, so it can only raise it or join it.
  if (after > maximum) {
    set_page_max_free_span_locked(page, after, 1);
  } else if (after != 0 && after == maximum) {
    set_page_max_free_span_locked(page, maximum, page.max_free_span_count + 1);
  }
}

// Record space an object used to occupy so a later allocation can reuse it.
// Callers hold gc_mutex. A page whose span list is full keeps the space
// unusable, which is exactly the behaviour of the bump allocator.
//
// INVARIANT: `free_spans` is ascending by offset, runs never overlap, and
// `free_span_bytes` is exactly the sum of the runs. Every operation in this file
// depends on it - insertion finds its position by offset, first-fit scans from
// the lowest offset, and the run taken for a TLAB is picked from the head. A
// removal therefore has to close the hole by shifting the tail, never by moving
// the last entry into it: the last entry has the highest offset, and putting it
// in the middle leaves the array unsorted, after which adjacent runs stop
// merging and the page fragments until it can no longer serve a reservation.
void release_span_locked(GcPage& page, size_t offset, size_t span) {
  if (span == 0) return;
  // The storage may still hold the reclaimed object's bytes, so nothing about
  // this page may be assumed zero from here on: the known-zero suffix ends.
  page.zero_cursor = page.capacity;
  // A run the representation cannot describe would be recorded as a negative
  // length; such a page simply keeps behaving like the bump allocator it was.
  if (offset > UINT32_MAX || span > UINT32_MAX) return;
  // A run has to stay inside the page it is recorded on. Recording one that
  // crosses the end would let a later reservation take a span whose other half is
  // a live object, so it is dropped instead of corrupting the page.
  if (offset > page.capacity || span > page.capacity - offset) return;
  // Runs are kept ordered by offset and always merged with a neighbour they
  // touch. Sweeping visits objects in list order, not address order, so a run
  // that is freed in pieces has to coalesce back into one region; otherwise the
  // largest reusable block in a page would be a single object.
  size_t index = 0;
  while (index < page.free_span_count && page.free_spans[index].offset < offset)
    ++index;
  if (index > 0) {
    GcPage::FreeSpan& previous = page.free_spans[index - 1];
    if (previous.offset + previous.span == offset) {
      const size_t previous_before = previous.span;
      size_t next_before = 0;
      previous.span += span;
      page.free_span_bytes += span;
      if (index < page.free_span_count &&
          previous.offset + previous.span == page.free_spans[index].offset) {
        next_before = page.free_spans[index].span;
        previous.span += next_before;
        for (size_t move = index; move + 1 < page.free_span_count; ++move)
          page.free_spans[move] = page.free_spans[move + 1];
        --page.free_span_count;
        --total_free_spans;
      }
      adjust_page_max_free_span_locked(page, previous_before, previous.span);
      if (next_before != 0)
        adjust_page_max_free_span_locked(page, next_before, 0);
      return;
    }
  }
  if (index < page.free_span_count &&
      offset + span == page.free_spans[index].offset) {
    GcPage::FreeSpan& next = page.free_spans[index];
    const size_t before = next.span;
    next.offset = offset;
    next.span += span;
    page.free_span_bytes += span;
    adjust_page_max_free_span_locked(page, before, next.span);
    return;
  }
  if (page.free_span_count >= page.free_span_capacity) {
    // A page that shatters may need more entries than the inline array holds, so
    // the list grows before any run is discarded. The growth doubles, which keeps
    // the number of reallocations logarithmic in the number of dead objects a
    // page has accumulated.
    if (page.free_span_capacity < GcPage::kMaxGrownFreeSpans) {
      const size_t wanted =
          std::min(page.free_span_capacity * 2, GcPage::kMaxGrownFreeSpans);
      std::unique_ptr<GcPage::FreeSpan[]> grown(
          new (std::nothrow) GcPage::FreeSpan[wanted]);
      if (grown) {
        std::memcpy(grown.get(), page.free_spans,
                    page.free_span_count * sizeof(GcPage::FreeSpan));
        page.grown_free_spans = std::move(grown);
        page.free_spans = page.grown_free_spans.get();
        page.free_span_capacity = wanted;
      }
    }
  }
  if (page.free_span_count >= page.free_span_capacity) {
    // Only a page that has reached the grown limit drops anything now. Dropping
    // the newest run lets a page lose the space it just freed; dropping the
    // smallest recorded run keeps the runs a later request can actually use,
    // which is what a bound exists to preserve. A page whose recorded runs are
    // all at least as large as the new one keeps them and drops the new one.
    size_t smallest = 0;
    for (size_t scan = 1; scan < page.free_span_count; ++scan)
      if (page.free_spans[scan].span < page.free_spans[smallest].span)
        smallest = scan;
    const size_t smallest_span = page.free_spans[smallest].span;
    if (smallest_span >= span) {
      page.dropped_free_bytes += span;
      dropped_free_bytes_total += span;
      ++dropped_free_runs_total;
      return;
    }
    for (size_t move = smallest; move + 1 < page.free_span_count; ++move)
      page.free_spans[move] = page.free_spans[move + 1];
    --page.free_span_count;
    --total_free_spans;
    page.free_span_bytes -= smallest_span;
    page.dropped_free_bytes += smallest_span;
    dropped_free_bytes_total += smallest_span;
    ++dropped_free_runs_total;
    adjust_page_max_free_span_locked(page, smallest_span, 0);
    index = page.free_span_count;
    for (size_t scan = 0; scan < page.free_span_count; ++scan) {
      if (page.free_spans[scan].offset >= offset) {
        index = scan;
        break;
      }
    }
  }
  for (size_t move = page.free_span_count; move > index; --move)
    page.free_spans[move] = page.free_spans[move - 1];
  page.free_spans[index] = GcPage::FreeSpan{
      static_cast<uint32_t>(offset), static_cast<uint32_t>(span)};
  ++page.free_span_count;
  ++total_free_spans;
  page.free_span_bytes += span;
  adjust_page_max_free_span_locked(page, 0, span);
  index_span_page_locked(page);
}

// A reservation is charged to its page in full, so ending it part way through an
// object leaves a remainder that no allocation of that size can use - up to a
// whole slab for a payload close to the slab size. Grants are therefore cut to a
// whole number of objects, and everything past the last one stays free space in
// the page, where a smaller object can still have it.
size_t whole_object_grant(size_t grant, size_t span) {
  if (span == 0) return grant;
  const size_t whole = (grant / span) * span;
  return whole != 0 ? whole : grant;
}
// First-fit reuse of reclaimed space. The scan visits pages in allocation order
// and only pages that currently hold a free run, so a heap with nothing to reuse
// costs one branch per page. Callers hold gc_mutex.
GcPage* take_free_span_locked(size_t span, size_t& offset, PageKind kind,
                              const nrq_object_layout* uniform_layout) {
  if (span == 0 || total_free_spans == 0) return nullptr;
  // Two preferences, in this order:
  //
  //  1. a page already serving this size class, because its holes are the shape
  //     of this request and reusing it does not widen the page's mix;
  //  2. among equally suitable pages, the one whose largest run is closest to the
  //     request, because taking a small request out of a large run splits it into
  //     a remainder nothing large can use.
  //
  // A request that has no page of its own class still gets any page that can
  // serve it, which is what keeps a single-size workload's behaviour unchanged.
  for (GcPage* page : span_pages) {
    if (!page || page->kind != kind || page->free_span_count == 0 ||
        page->max_free_span < span)
      continue;
    if (uniform_layout && page->uniform_layout != uniform_layout) continue;
    for (size_t index = 0; index < page->free_span_count; ++index) {
      GcPage::FreeSpan& candidate = page->free_spans[index];
      if (candidate.span < span) continue;
      const size_t candidate_before = candidate.span;
      unretire_page_locked(*page);
      offset = candidate.offset;
      page->free_span_bytes -= span;
      if (candidate.span > span) {
        // The remainder stays a run in the same page. `span` is a multiple of
        // the payload alignment, so the new start keeps that alignment.
        candidate.offset += span;
        candidate.span -= span;
        adjust_page_max_free_span_locked(*page, candidate_before,
                                         candidate.span);
      } else {
        // Runs are ordered by offset, so removal shifts the tail down instead of
        // swapping the last run into the hole.
        for (size_t move = index; move + 1 < page->free_span_count; ++move)
          page->free_spans[move] = page->free_spans[move + 1];
        --page->free_span_count;
        --total_free_spans;
        // A page with no recorded run can never answer a request again until
        // something is freed into it, so it leaves the index with its last run.
        if (page->free_span_count == 0) unindex_span_page_locked(*page);
        adjust_page_max_free_span_locked(*page, candidate_before, 0);
      }
      recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
      reused_span_bytes = saturated_add(reused_span_bytes, span);
      return page;
    }
  }
  return nullptr;
}

// First-fit reuse for a thread-local reservation. `take_free_span_locked` takes
// exactly the span it is asked for, which is right for one object and wrong for a
// TLAB: a sweep merges a dead page into one large run, and carving it into 4 KB
// pieces re-fragmented the page, forced a reservation every few dozen objects,
// and left later reservations to open fresh pages - which grew the heap cycle
// after cycle and made sweeping cost more each round. This takes the whole run
// when it is no larger than `desired`, so one refill consumes one page's run
// instead of sixteen, and it still accepts a shorter run as long as it can serve
// one object. `granted` reports the length actually taken.
// Callers hold gc_mutex.
GcPage* take_free_run_locked(size_t desired, size_t minimum, size_t multiple,
                             size_t& offset, size_t& granted, PageKind kind,
                             const nrq_object_layout* uniform_layout) {
  if (desired == 0 || minimum == 0 || minimum > desired ||
      total_free_spans == 0)
    return nullptr;
  for (GcPage* page : span_pages) {
    if (!page || page->kind != kind || page->free_span_count == 0 ||
        page->max_free_span < minimum)
      continue;
    if (uniform_layout && page->uniform_layout != uniform_layout) continue;
    for (size_t index = 0; index < page->free_span_count; ++index) {
      GcPage::FreeSpan& candidate = page->free_spans[index];
      if (candidate.span < minimum) continue;
      const size_t candidate_before = candidate.span;
      const size_t span = whole_object_grant(
          std::min(static_cast<size_t>(candidate.span), desired), multiple);
      if (span < minimum) continue;
      unretire_page_locked(*page);
      offset = candidate.offset;
      page->free_span_bytes -= span;
      if (candidate.span > span) {
        // `span` is a multiple of the payload alignment, so the remainder keeps
        // both its alignment and its position in the ordered run list.
        candidate.offset += span;
        candidate.span -= span;
        adjust_page_max_free_span_locked(*page, candidate_before,
                                         candidate.span);
      } else {
        for (size_t move = index; move + 1 < page->free_span_count; ++move)
          page->free_spans[move] = page->free_spans[move + 1];
        --page->free_span_count;
        --total_free_spans;
        if (page->free_span_count == 0) unindex_span_page_locked(*page);
        adjust_page_max_free_span_locked(*page, candidate_before, 0);
      }
      recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
      reused_span_bytes = saturated_add(reused_span_bytes, span);
      granted = span;
      return page;
    }
  }
  return nullptr;
}
// Reserves a batch of runs from the first page in the index that can serve the
// request, in one pass over that page's run list. Taking runs one at a time made
// the list maintenance the cost of a fragmented refill: every take closed its gap
// by shifting the tail of an array that can hold 96 runs, so handing out a page's
// holes one by one moved ~24x more entries than it handed out bytes. Here the
// runs come off the top of the list, a partial take only moves its own start, and
// the removals are compacted in a single pass afterwards.
//
// Callers hold gc_mutex. `out` must have room for `max_runs` entries. Returns how
// many runs were written and, in `page`, the page they came from - a batch is one
// page's holes, which keeps the space a thread reserves in one place.
size_t take_free_runs_locked(size_t minimum, size_t byte_budget, size_t max_runs,
                             size_t multiple, GcPage*& page,
                             GcPage::FreeSpan* out, PageKind kind,
                             const nrq_object_layout* uniform_layout) {
  page = nullptr;
  if (minimum == 0 || max_runs == 0 || total_free_spans == 0) return 0;
  GcPage* target = nullptr;
  for (GcPage* candidate : span_pages) {
    if (!candidate || candidate->kind != kind ||
        candidate->free_span_count == 0 ||
        candidate->max_free_span < minimum)
      continue;
    if (uniform_layout && candidate->uniform_layout != uniform_layout) continue;
    target = candidate;
    break;
  }
  if (!target) return 0;
  unretire_page_locked(*target);

  size_t taken = 0;
  size_t taken_bytes = 0;
  bool removed_any = false;
  size_t index = target->free_span_count;
  while (index != 0 && taken < max_runs) {
    --index;
    GcPage::FreeSpan& run = target->free_spans[index];
    if (run.span < minimum) continue;
    const size_t grant = whole_object_grant(
        std::min(static_cast<size_t>(run.span), kGcTlabBytes), multiple);
    if (grant < minimum) continue;
    if (taken != 0 && taken_bytes + grant > byte_budget) break;
    // A run is taken from its top, so a partial take only moves the run's own
    // start and the entry keeps its place in the ascending list.
    out[taken].offset = run.offset + (run.span - grant);
    out[taken].span = grant;
    ++taken;
    taken_bytes += grant;
    target->free_span_bytes -= grant;
    recycled_span_reuse_count = saturated_add(recycled_span_reuse_count, 1);
    if (grant == run.span) {
      run.span = 0;  // tombstone: compacted below, never observed elsewhere
      removed_any = true;
      --total_free_spans;
    } else {
      run.span -= grant;
    }
  }
  if (removed_any) {
    size_t write = 0;
    for (size_t read = 0; read < target->free_span_count; ++read) {
      if (target->free_spans[read].span == 0) continue;
      if (write != read) target->free_spans[write] = target->free_spans[read];
      ++write;
    }
    target->free_span_count = write;
    if (write == 0) unindex_span_page_locked(*target);
  }
  if (taken != 0) refresh_page_max_free_span_locked(*target);
  page = target;
  return taken;
}

// Records the unused remainder of the reservation this thread is leaving behind.
// The space stays charged to its page until the page is recycled, so this is the
// only measurement of what an abandoned slab costs.
void account_tlab_tail_slot(MutatorState& mutator, uint8_t* cursor,
                            uint8_t* end, uint8_t* slab_end_ptr) {
  uint8_t* const slab_end = slab_end_ptr ? slab_end_ptr : end;
  if (!cursor || slab_end <= cursor) return;
  const size_t tail = static_cast<size_t>(slab_end - cursor);
  mutator.tlab_tail_bytes = saturated_add(mutator.tlab_tail_bytes, tail);
  if (tail > mutator.tlab_tail_max_bytes) mutator.tlab_tail_max_bytes = tail;
}

void release_one_tlab_slot_locked(MutatorState& mutator, void*& page_ref,
                                  uint8_t*& cursor_ref, uint8_t*& end_ref,
                                  uint8_t*& slab_end_ref,
                                  NrqGcPendingRange*& active_range_ref) {
  GcPage* const page = static_cast<GcPage*>(page_ref);
  uint8_t* const slab_end = slab_end_ref ? slab_end_ref : end_ref;
  if (page && cursor_ref && slab_end > cursor_ref) {
    const size_t offset = static_cast<size_t>(cursor_ref - page->storage);
    const size_t span = static_cast<size_t>(slab_end - cursor_ref);
    mutator.tlab_tail_reclaimed_bytes =
        saturated_add(mutator.tlab_tail_reclaimed_bytes, span);
    release_span_locked(*page, offset, span);
  }
  if (active_range_ref) active_range_ref->end = cursor_ref;
  active_range_ref = nullptr;
  page_ref = nullptr;
  cursor_ref = nullptr;
  end_ref = nullptr;
  slab_end_ref = nullptr;
}

void account_tlab_tail(MutatorState& mutator) {
  if (!mutator.fast) return;
  account_tlab_tail_slot(mutator, mutator.fast->tlab_cursor,
                         mutator.fast->tlab_end, mutator.tlab_slab_end);
  account_tlab_tail_slot(mutator, mutator.fast->ext_cursor,
                         mutator.fast->ext_end, mutator.ext_slab_end);
}

void release_tlab_tail_locked(MutatorState& mutator) {
  if (!mutator.fast) return;
  NrqGcFastTls& fast = *mutator.fast;
  release_one_tlab_slot_locked(mutator, fast.tlab_page, fast.tlab_cursor,
                               fast.tlab_end, mutator.tlab_slab_end,
                               fast.tlab_active_range);
  release_one_tlab_slot_locked(mutator, fast.ext_page, fast.ext_cursor,
                               fast.ext_end, mutator.ext_slab_end,
                               fast.ext_active_range);
  clear_tlab_fast_path(fast);
  GcPage* const h0_page = static_cast<GcPage*>(fast.h0_page);
  if (h0_page && fast.h0_cursor && fast.h0_end > fast.h0_cursor) {
    const size_t offset = static_cast<size_t>(fast.h0_cursor - h0_page->storage);
    const size_t span = static_cast<size_t>(fast.h0_end - fast.h0_cursor);
    mutator.tlab_tail_reclaimed_bytes =
        saturated_add(mutator.tlab_tail_reclaimed_bytes, span);
    release_span_locked(*h0_page, offset, span);
  }
  fast.h0_page = nullptr;
  fast.h0_cursor = nullptr;
  fast.h0_end = nullptr;
  fast.h0_state_cursor = nullptr;
  fast.h0_slot_bytes = 0;
  clear_h0_fast_path(fast);
  mutator.h0_layout = nullptr;
  mutator.h0_slot = 0;
}

// Closes the open publication window when the allocator moves to another page.
// A window that spanned two pages could not be applied from its counters without
// guessing which page each object went to, so the switch is where the window
// ends. Callers may or may not hold the collector lock.
void flush_window_for_page_switch(MutatorState& mutator, GcPage* page) {
  if (!mutator.fast || !page) return;
  GcPage* const open = window_page_of(mutator);
  if (!open || open == page) return;
  if (heap_lock_held_by_this_thread) {
    publish_tlab_locked(mutator);
    return;
  }
  HeapLockGuard lock(gc_mutex);
  publish_tlab_locked(mutator);
}

void install_tlab(MutatorState& mutator, GcPage* page, size_t offset, size_t span) {
  // A reservation is storage inside one page. Anything else is bookkeeping that
  // has already gone wrong, and handing it out would let the tail release write a
  // free run that overlaps a live object - the failure that zeroes a survivor's
  // header and makes the collector read an object that is no longer there.
  if (!page || offset > page->capacity || span > page->capacity - offset)
    nrq_runtime_fatal("GC thread-local reservation escapes its page");
  sync_inline_unflushed(mutator);
  flush_window_for_page_switch(mutator, page);
  NrqGcFastTls& fast = *mutator.fast;
  const bool is_ext = page->kind == PageKind::Extended;
  uint8_t*& cursor_ref = is_ext ? fast.ext_cursor : fast.tlab_cursor;
  uint8_t*& end_ref = is_ext ? fast.ext_end : fast.tlab_end;
  void*& page_ref = is_ext ? fast.ext_page : fast.tlab_page;
  uint8_t*& slab_end_ref = is_ext ? mutator.ext_slab_end : mutator.tlab_slab_end;
  NrqGcPendingRange*& active_ref =
      is_ext ? fast.ext_active_range : fast.tlab_active_range;

  account_tlab_tail_slot(mutator, cursor_ref, end_ref, slab_end_ref);
  if (active_ref) active_ref->end = cursor_ref;
  active_ref = nullptr;
  page_ref = page;
  cursor_ref = page->storage + offset;
  slab_end_ref = cursor_ref + span;
  clamp_tlab_window(cursor_ref, end_ref, slab_end_ref, fast.unflushed_bytes);
  if (!is_ext) fast.tlab_kind = NRQ_GC_INLINE_KIND_COMPACT;
  mutator.tlab_generation = gc_page_generation.load(std::memory_order_acquire);
  // Ensure the entire reservation is zero-filled at install time so inline
  // allocation never pays a per-object zeroing branch or memset.
  const bool payload_zero = offset >= page->zero_cursor;
  if (!payload_zero && span != 0) {
    const bool track_timing = tlab_diag.enabled.load(std::memory_order_relaxed);
    const auto z_start = track_timing
                             ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    std::memset(page->storage + offset, 0, span);
    if (track_timing) {
      tlab_diag.install_memset_ns.fetch_add(
          static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - z_start)
                  .count()),
          std::memory_order_relaxed);
    }
  }
  fast.tlab_payload_zero = 1u;
  const size_t end = offset + span;
  if (end > page->zero_cursor) page->zero_cursor = end;
  zeroing_facts.payload_bytes += span;
  if (payload_zero)
    zeroing_facts.prezeroed_bytes += span;
  else
    zeroing_facts.filled_bytes += span;
  cache_allocation_page(page);
}
// Returns every run this thread reserved and did not hand out. The caller holds
// gc_mutex and this mutator is not running: the cycle boundary flushes before it
// reclaims empty pages, and a thread leaving the registry flushes so the space it
// reserved is not lost with the mutator.
void flush_spare_runs_locked(MutatorState& mutator) {
  while (mutator.spare_run_count != 0) {
    const MutatorState::SpareRun spare =
        mutator.spare_runs[--mutator.spare_run_count];
    if (!spare.page) continue;
    release_span_locked(*spare.page, spare.offset, spare.span);
  }
  mutator.spare_generation = gc_page_generation.load(std::memory_order_acquire);
}

// Pops a reserved run into the thread-local cursor. No lock: the list is owned by
// this thread and every entry was already removed from its page under the lock,
// so no other thread can be handed the same cells. A run too small for this
// request stays on the list for a smaller object.
bool take_spare_run(MutatorState& mutator, size_t minimum_span, PageKind kind) {
  size_t index = mutator.spare_run_count;
  while (index != 0) {
    --index;
    const MutatorState::SpareRun spare = mutator.spare_runs[index];
    if (spare.span < minimum_span || !spare.page || spare.page->kind != kind)
      continue;
    mutator.spare_runs[index] = mutator.spare_runs[--mutator.spare_run_count];
    install_tlab(mutator, spare.page, spare.offset, spare.span);
    return true;
  }
  return false;
}

// Reserves short runs in one pass and leaves them on the thread's list. Each
// reservation is the same first-fit take the single-run path made, so the page's
// run list, its largest-run record and the index stay exact; what changes is that
// one lock acquisition now answers the next `kSpareRunCapacity` refills. The batch
// is bounded by bytes as well as by count, so a heap that still has medium runs
// cannot have a large slice reserved by a thread that may not need it.
bool reserve_spare_runs_locked(MutatorState& mutator, size_t minimum_span,
                               PageKind kind) {
  constexpr size_t byte_budget = kGcTlabBytes * 2;
  // The list this fills is fixed-size, and a refill can arrive while it already
  // holds runs a smaller request left behind. Asking for a full batch then wrote
  // past the end of `spare_runs`, and the fields that follow it are the slab
  // cursors and the generation stamp a later tail release reads: one bogus tail
  // released that way records a free run overlapping a live object, and the next
  // reservation zeroes it. A batch therefore sizes itself to the free room.
  const size_t room =
      mutator.spare_run_count < MutatorState::kSpareRunCapacity
          ? MutatorState::kSpareRunCapacity - mutator.spare_run_count
          : 0;
  if (room == 0) return false;
  GcPage::FreeSpan runs[MutatorState::kSpareRunCapacity];
  GcPage* page = nullptr;
  const size_t taken = take_free_runs_locked(minimum_span, byte_budget, room,
                                             minimum_span, page, runs, kind,
                                             nullptr);
  if (taken == 0) return false;
  for (size_t index = 0; index < taken; ++index) {
    MutatorState::SpareRun& spare = mutator.spare_runs[mutator.spare_run_count];
    spare.page = page;
    spare.offset = runs[index].offset;
    spare.span = runs[index].span;
    ++mutator.spare_run_count;
  }
  // The runs just reserved belong to the page set as it is now; a later cycle
  // that flushes this list stamps its own generation over this one.
  mutator.spare_generation = gc_page_generation.load(std::memory_order_acquire);
  return true;
}

// Drops a cursor the cycle boundary invalidated. Reserved runs are returned by
// that boundary too, so a non-empty list here means that step was skipped and the
// pages it points into may already have been recycled: debug builds trap it, a
// release build abandons the list rather than touch storage that may be gone.
void discard_stale_tlab(MutatorState& mutator) {
  account_tlab_tail(mutator);
  if (mutator.fast) {
    mutator.fast->tlab_page = nullptr;
    mutator.fast->tlab_cursor = nullptr;
    mutator.fast->tlab_end = nullptr;
    mutator.fast->ext_page = nullptr;
    mutator.fast->ext_cursor = nullptr;
    mutator.fast->ext_end = nullptr;
    clear_tlab_fast_path(*mutator.fast);
  }
  mutator.tlab_slab_end = nullptr;
  mutator.ext_slab_end = nullptr;
  mutator.tlab_generation = gc_page_generation.load(std::memory_order_acquire);
  // Only the runs a cycle boundary failed to return are abandoned here, and only
  // those whose reservation predates the current page set. A run reserved after
  // the boundary is still valid even though the cursor that named this mutator
  // belonged to the previous cycle, which is the normal state after a headerless
  // refill reserved a batch that a compact refill then found.
  if (mutator.spare_run_count != 0 &&
      mutator.spare_generation !=
          gc_page_generation.load(std::memory_order_acquire)) {
    if constexpr (nirq::runtime_debug_enabled)
      nrq_runtime_fatal("GC reserved runs outlived a cycle boundary");
    mutator.spare_run_count = 0;
  }
}

// Take a page of at least `capacity` bytes from the recycle pool, or create a
// fresh one. The caller inherits storage ownership and must either publish the
// page into gc_pages or release the storage explicitly.
// Page storage comes straight from the OS. A GC page needs kGcPageBytes
// alignment, and VirtualAlloc returns exactly that: it hands back a 64 KiB-aligned
// region and commits only the bytes requested. The over-aligned `operator new` it
// replaced has to reserve room to align the block itself, so every 64 KiB page
// carried up to another 64 KiB of reserved and committed space - a measured 2x
// private commit for a heap of ordinary pages. Release goes back through
// VirtualFree so the accounting stays symmetric.
//
// Ordinary pages are carved from runs instead of asking the OS for one page at a
// time. A growing heap used to pay a reservation and, later, a release per 64 KiB
// page; a run pays one reservation per 8 MiB and one commit per MiB. A page
// released while the heap is still busy goes back to its run's free list and is
// handed out again without a system call. The free lists are bounded, so a heap
// that shrinks returns its commit charge through MEM_DECOMMIT instead of holding
// it.
constexpr size_t kPageRunBytes = size_t{8} << 20;
constexpr size_t kPageRunCommitBytes = size_t{4} << 20;
// Commit granularity of the OS. A carve's extent is the commit extent as well.
constexpr size_t kPageCommitGranularity = size_t{4} << 10;

struct PageRun {
  unsigned char* base = nullptr;
  // Bytes committed from `base`, kept as a high-water mark: a carve is handed out
  // once, so nothing below the mark is ever un-committed here.
  size_t committed = 0;
  // Bytes carved from `base`. An ordinary page starts on a kGcPageBytes boundary
  // and occupies exactly one page; a larger page is carved at whatever 4 KiB
  // boundary comes next and occupies its own capacity, so a page that is not a
  // multiple of 64 KiB does not pay for the rounding.
  size_t carved = 0;
};
// A deque, so the carving cursor below stays valid when another run is recorded.
std::deque<PageRun> page_runs;
// The run carving continues from. Only it can have un-carved slots, so the
// allocation path never has to look at the other runs.
PageRun* active_page_run = nullptr;
// Storage handed back and ready to be used again. One entry is one carve: a single
// slot for an ordinary page, a contiguous group of slots for a page larger than
// 64 KiB. Entries are matched by exact size, which keeps every page's capacity and
// its mapping in step.
struct FreePageStorage {
  unsigned char* storage = nullptr;
  size_t bytes = 0;
};
std::vector<FreePageStorage> free_page_storage;
// Storage whose commit charge went back to the OS but whose address space the run
// still owns. It is recommitted when the heap needs it again, so a program that
// peaks and shrinks does not lose the addresses it already reserved.
std::vector<FreePageStorage> released_page_storage;
// One run's worth of bytes, so a busy heap never holds more than this much
// committed-but-unused storage in the free list. Anything beyond it is decommitted
// immediately.
constexpr size_t kMaxRetainedBytes = 4 * kPageRunBytes;
size_t retained_page_bytes = 0;
// The largest page a run will carve. A single object can be far larger than a run,
// and that case still needs the direct mapping.
constexpr size_t kPageRunMaxCarveBytes = kPageRunBytes / 2;

// ---------------------------------------------------------------------------
// 2 MiB aligned GC Region Arena & Region Pool (Phase Sigma)
// ---------------------------------------------------------------------------
// Windows VirtualAlloc guarantees 64 KiB alignment, not 2 MiB alignment. Each
// RegionArena reserves (kRegionsPerArena + 1) * 2 MiB and aligns its base up
// to a 2 MiB boundary so every GcRegion base satisfies:
//   (reinterpret_cast<uintptr_t>(region->base) & (kGcRegionBytes - 1)) == 0
// Commit happens one 2 MiB region at a time; reclaimed regions stay warm in
// `recycled_regions` up to `kMaxRecycledRegions` and decommit beyond that cap.
constexpr size_t kRegionsPerArena = 8;
constexpr size_t kRegionArenaReserveBytes =
    (kRegionsPerArena + 1) * kGcRegionBytes;
constexpr size_t kMaxRecycledRegions = 24;

struct RegionArena {
  unsigned char* raw_base = nullptr;
  unsigned char* aligned_base = nullptr;
  size_t next_region = 0;
};

std::deque<RegionArena> region_arenas;
RegionArena* active_region_arena = nullptr;
std::vector<std::unique_ptr<GcRegion>> gc_regions;
std::vector<std::unique_ptr<GcRegion>> recycled_regions;
std::vector<unsigned char*> decommitted_region_bases;
GcRegion* active_alloc_region = nullptr;
GcRegion* active_survivor_region = nullptr;

struct RegionTelemetry {
  uint64_t evacuated_regions = 0;
  uint64_t evacuated_objects = 0;
  uint64_t evacuated_bytes = 0;
  uint64_t bulk_reclaimed_regions = 0;
  uint64_t pinned_region_fallbacks = 0;
};
RegionTelemetry region_telemetry;
// Phi's public knobs. Both are read by the pacemaker only; a budget of 0 means
// "no budget", which is the behaviour every program had before Phi existed.
std::atomic<size_t> gc_region_limit{0};
// Percentage of the budget that may be allocated before a cycle starts, clamped
// to 1..100 on the way in so a bad value cannot disable the accelerator or force
// a collection on every allocation.
std::atomic<uint32_t> gc_trigger_percent{75};
// Set whenever a budget is declared. The safepoint poll reads this first so that
// the common case - no budget - pays one load and one predictable branch.
std::atomic<bool> pacemaker_budget_declared{false};
std::atomic<uint32_t> gc_evacuation_threshold_percent{50};

unsigned char* commit_region_storage_locked(bool& zeroed) {
  zeroed = false;
  if (!decommitted_region_bases.empty()) {
    unsigned char* base = decommitted_region_bases.back();
#if defined(_WIN32)
    if (!::VirtualAlloc(base, kGcRegionBytes, MEM_COMMIT, PAGE_READWRITE))
      return nullptr;
#endif
    decommitted_region_bases.pop_back();
    zeroed = true;
    return base;
  }
  for (;;) {
    if (!active_region_arena ||
        active_region_arena->next_region >= kRegionsPerArena) {
      try {
        RegionArena arena;
#if defined(_WIN32)
        arena.raw_base = static_cast<unsigned char*>(::VirtualAlloc(
            nullptr, kRegionArenaReserveBytes, MEM_RESERVE, PAGE_READWRITE));
#else
        arena.raw_base = static_cast<unsigned char*>(
            std::malloc(kRegionArenaReserveBytes));
#endif
        if (!arena.raw_base) return nullptr;
        const uintptr_t raw = reinterpret_cast<uintptr_t>(arena.raw_base);
        const uintptr_t aligned =
            (raw + kGcRegionBytes - 1) & ~(kGcRegionBytes - 1);
        arena.aligned_base = reinterpret_cast<unsigned char*>(aligned);
        arena.next_region = 0;
        region_arenas.push_back(arena);
      } catch (...) {
        return nullptr;
      }
      active_region_arena = &region_arenas.back();
    }
    unsigned char* base = active_region_arena->aligned_base +
                          active_region_arena->next_region * kGcRegionBytes;
#if defined(_WIN32)
    if (!::VirtualAlloc(base, kGcRegionBytes, MEM_COMMIT, PAGE_READWRITE)) {
      active_region_arena->next_region = kRegionsPerArena;
      active_region_arena = nullptr;
      continue;
    }
    zeroed = true;
#else
    std::memset(base, 0, kGcRegionBytes);
    zeroed = true;
#endif
    ++active_region_arena->next_region;
    return base;
  }
}

GcRegion* acquire_region_locked(bool survivor) {
  std::unique_ptr<GcRegion> region;
  if (!recycled_regions.empty()) {
    region = std::move(recycled_regions.back());
    recycled_regions.pop_back();
    region->next_slot = 0;
    region->active_pages = 0;
    region->is_survivor = survivor;
    region->has_unpublished = false;
    region->pages.fill(nullptr);
    region->pinned_objects = 0;
    region->live_objects = 0;
    region->live_payload_bytes = 0;
    region->marked_objects = 0;
    region->cycle_pinned_objects = 0;
  } else {
    bool zeroed = false;
    unsigned char* base = commit_region_storage_locked(zeroed);
    if (!base) return nullptr;
    if constexpr (nirq::runtime_debug_enabled) {
      if ((reinterpret_cast<uintptr_t>(base) & (kGcRegionBytes - 1)) != 0)
        nrq_runtime_fatal("GC region base is not 2 MiB aligned");
    }
    try {
      region = std::make_unique<GcRegion>();
    } catch (...) {
#if defined(_WIN32)
      ::VirtualFree(base, kGcRegionBytes, MEM_DECOMMIT);
#endif
      try {
        decommitted_region_bases.push_back(base);
      } catch (...) {
      }
      return nullptr;
    }
    region->base = base;
    region->capacity = kGcRegionBytes;
    region->is_survivor = survivor;
    region->recycled_dirty = !zeroed;
  }
  GcRegion* raw = region.get();
  try {
    raw->region_index = gc_regions.size();
    gc_regions.push_back(std::move(region));
  } catch (...) {
    raw->region_index = SIZE_MAX;
    try {
      recycled_regions.push_back(std::move(region));
    } catch (...) {
    }
    return nullptr;
  }
  return raw;
}

void recycle_region_locked(std::unique_ptr<GcRegion> region) {
  if (!region) return;
  if (active_alloc_region == region.get()) active_alloc_region = nullptr;
  if (active_survivor_region == region.get()) active_survivor_region = nullptr;
  region->region_index = SIZE_MAX;
  region->next_slot = 0;
  region->active_pages = 0;
  region->is_survivor = false;
  region->has_unpublished = false;
  region->pages.fill(nullptr);
  region->pinned_objects = 0;
  region->live_objects = 0;
  region->live_payload_bytes = 0;
  region->marked_objects = 0;
  region->cycle_pinned_objects = 0;
  if (recycled_regions.size() < kMaxRecycledRegions) {
    region->recycled_dirty = true;
    try {
      recycled_regions.push_back(std::move(region));
      return;
    } catch (...) {
    }
  }
  unsigned char* base = region->base;
#if defined(_WIN32)
  if (base && ::VirtualFree(base, kGcRegionBytes, MEM_DECOMMIT)) {
    try {
      decommitted_region_bases.push_back(base);
    } catch (...) {
    }
  }
#endif
}

void remove_gc_region_locked(GcRegion* region) {
  if (!region || region->region_index >= gc_regions.size()) return;
  const size_t idx = region->region_index;
  std::unique_ptr<GcRegion> owned = std::move(gc_regions[idx]);
  if (idx + 1 != gc_regions.size()) {
    gc_regions[idx] = std::move(gc_regions.back());
    gc_regions[idx]->region_index = idx;
  }
  gc_regions.pop_back();
  owned->region_index = SIZE_MAX;
  recycle_region_locked(std::move(owned));
  region_telemetry.bulk_reclaimed_regions =
      saturated_add(region_telemetry.bulk_reclaimed_regions, 1);
}

unsigned char* carve_region_page_locked(bool survivor, GcRegion*& out_region,
                                        uint8_t& out_slot, bool& out_zeroed) {
  out_region = nullptr;
  out_slot = 0;
  out_zeroed = false;
  GcRegion*& active = survivor ? active_survivor_region : active_alloc_region;
  if (!active || active->next_slot >= kPagesPerRegion) {
    active = acquire_region_locked(survivor);
    if (!active) return nullptr;
  }
  const uint8_t slot = active->next_slot++;
  ++active->active_pages;
  out_region = active;
  out_slot = slot;
  out_zeroed = !active->recycled_dirty;
  return active->base + static_cast<size_t>(slot) * kGcPageBytes;
}

PageRun* find_page_run(unsigned char* storage) {
  for (PageRun& run : page_runs) {
    if (storage >= run.base && storage < run.base + kPageRunBytes) return &run;
  }
  return nullptr;
}

// Commit enough of a run to cover `end` bytes. Returns false when the OS refuses.
bool commit_run_bytes(PageRun& run, size_t end) {
  if (end <= run.committed) return true;
  size_t target = run.committed + kPageRunCommitBytes;
  if (target < end) target = end;
  if (target > kPageRunBytes) target = kPageRunBytes;
  if (!::VirtualAlloc(run.base + run.committed, target - run.committed,
                      MEM_COMMIT, PAGE_READWRITE))
    return false;
  run.committed = target;
  return true;
}

// Hand back a released carve of exactly `bytes`, recommitting it when its commit
// charge had already been returned.
unsigned char* take_released_page_storage(size_t bytes, bool& zeroed) {
  zeroed = false;
  for (auto entry = free_page_storage.rbegin(); entry != free_page_storage.rend();
       ++entry) {
    if (entry->bytes != bytes) continue;
    unsigned char* storage = entry->storage;
    *entry = free_page_storage.back();
    free_page_storage.pop_back();
    if (retained_page_bytes >= bytes) retained_page_bytes -= bytes;
    // The commit charge was never returned, so this page still holds whatever the
    // object that used it last left behind.
    return storage;
  }
  for (auto entry = released_page_storage.rbegin();
       entry != released_page_storage.rend(); ++entry) {
    if (entry->bytes != bytes) continue;
    if (!::VirtualAlloc(entry->storage, bytes, MEM_COMMIT, PAGE_READWRITE))
      return nullptr;
    unsigned char* storage = entry->storage;
    *entry = released_page_storage.back();
    released_page_storage.pop_back();
    // Recommitting a range that was decommitted zero-fills it again.
    zeroed = true;
    return storage;
  }
  return nullptr;
}

// Carve a page of `bytes` from a run. Ordinary pages keep the 64 KiB alignment
// the page directory keys on; a larger page needs no such alignment (it is never
// indexed by address) and is carved at its own size.
unsigned char* carve_page_run(size_t bytes) {
  const size_t extent =
      (bytes + kPageCommitGranularity - 1) & ~(kPageCommitGranularity - 1);
  for (;;) {
    if (!active_page_run) {
      try {
        PageRun run;
        run.base = static_cast<unsigned char*>(
            ::VirtualAlloc(nullptr, kPageRunBytes, MEM_RESERVE, PAGE_READWRITE));
        if (!run.base) return nullptr;
        page_runs.push_back(std::move(run));
      } catch (...) {
        return nullptr;
      }
      active_page_run = &page_runs.back();
    }
    size_t offset = active_page_run->carved;
    if (bytes <= kGcPageBytes)
      offset = (offset + kGcPageBytes - 1) & ~(kGcPageBytes - 1);
    if (offset >= kPageRunBytes || extent > kPageRunBytes - offset) {
      // A run that cannot serve this request is closed; the next request opens a
      // new one instead of retrying this one forever.
      active_page_run->carved = kPageRunBytes;
      active_page_run = nullptr;
      continue;
    }
    if (!commit_run_bytes(*active_page_run, offset + extent)) {
      active_page_run->carved = kPageRunBytes;
      active_page_run = nullptr;
      continue;
    }
    active_page_run->carved = offset + extent;
    return active_page_run->base + offset;
  }
}

// Storage for one page: a released carve of the same size, a fresh carve from a
// run, or a direct mapping when the page is too large for a run or the runs cannot
// serve the request.
unsigned char* commit_page_storage(size_t bytes, bool& zeroed) {
  zeroed = false;
  if (bytes <= kPageRunMaxCarveBytes) {
    if (unsigned char* storage = take_released_page_storage(bytes, zeroed))
      return storage;
    if (unsigned char* storage = carve_page_run(bytes)) {
      // Committing a reserved range zero-fills it, which is what lets the
      // allocator skip its own fill for the first pass over the page.
      zeroed = true;
      return storage;
    }
  }
  unsigned char* mapping = static_cast<unsigned char*>(
      ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  zeroed = mapping != nullptr;
  return mapping;
}

// Return storage to the run that owns it, or to the OS when it was a direct
// mapping. Carves are kept for reuse until the free list is full.
void release_page_storage(unsigned char* storage, size_t bytes) {
  if (!storage) return;
  if (find_page_run(storage)) {
    if (bytes <= kGcPageBytes &&
        retained_page_bytes + bytes <= kMaxRetainedBytes) {
      try {
        free_page_storage.push_back(FreePageStorage{storage, bytes});
        retained_page_bytes += bytes;
        return;
      } catch (...) {
      }
    }
    if (::VirtualFree(storage, bytes, MEM_DECOMMIT)) {
      try {
        released_page_storage.push_back(FreePageStorage{storage, bytes});
      } catch (...) {
      }
    }
    return;
  }
  ::VirtualFree(storage, 0, MEM_RELEASE);
}

// Every field the collector derives from a page is reset here, at the one point
// where a page's storage can be handed out again. Keeping initialization and
// recycling on the same helper prevents a new page class from forgetting a
// bitmap or counter that the sweep fast paths rely on.
void reset_page_gc_metadata_locked(GcPage& page) {
  // This is the one point where a page's storage is described from scratch, and a
  // sweep that is already part-way through the page's bitmaps is describing the
  // previous generation of objects. Bumping here - rather than only where an
  // emptied page is un-retired - makes every reuse path, including the ones that
  // go through the recycle pool, invalidate a captured cursor.
  ++page.reuse_generation;
  page.used = 0;
  page.live_objects = 0;
  page.indexed_objects = 0;
  page.live_payload_bytes = 0;
  page.born_this_cycle = 0;
  page.born_epoch = 0;
  page.young_objects = 0;
  // The shell held a page's objects before, so nothing about its bytes may be
  // assumed zero, and the class it was serving is a fact about the objects it
  // used to hold rather than about the ones it will.
  page.zero_cursor = page.capacity;
  page.dropped_free_bytes = 0;
  // A recycled shell keeps its grown run list, but starts again at the inline
  // array: the entries it holds are stale, and a page that shatters again will
  // grow back to whatever size that actually needs.
  page.grown_free_spans.reset();
  page.free_spans = page.inline_free_spans.data();
  page.free_span_capacity = GcPage::kMaxFreeSpans;
  // A recycled page may have been a directory fallback. Its objects are gone, so
  // the exact-index mode must not survive into the new page class: leaving it set
  // would make every future sweep skip the page as unwalkable.
  page.use_object_index = false;
  page.retired = false;
  page.pending_start_count = 0;
  page.starts_pending = false;
  page.uncommitted_objects.store(0, std::memory_order_relaxed);
  if (!page.grown_bitmaps && page.mark_words != 0) {
    std::memset(page.inline_bitmaps.data(), 0,
                4 * page.mark_words * sizeof(uint64_t));
    page.marked_objects = 0;
    page.marked_young_objects = 0;
    page.remembered_objects = 0;
  } else {
    clear_page_marks(page);
    clear_page_remembered(page);
    if (page.object_starts && page.object_start_words != 0) {
      std::memset(page.object_starts, 0,
                  page.object_start_words * sizeof(uint64_t));
    }
    if (page.born_bits && page.born_words != 0) {
      std::memset(page.born_bits, 0, page.born_words * sizeof(uint64_t));
    }
  }
  if (page.h0_states) {
    std::memset(page.h0_states.get(), 0, page.h0_state_capacity);
  }
}

void release_page_backing_locked(GcPage& page) {
  if (page.region) {
    GcRegion* reg = page.region;
    const uint8_t slot = page.region_slot;
    if (slot < kPagesPerRegion && reg->pages[slot] == &page)
      reg->pages[slot] = nullptr;
    if (reg->active_pages > 0) --reg->active_pages;
    page.region = nullptr;
    page.region_slot = 0;
    page.storage = nullptr;
    if (reg->active_pages == 0 && reg->next_slot > 0) {
      remove_gc_region_locked(reg);
    }
    return;
  }
  release_page_storage(page.storage, page.capacity);
  page.storage = nullptr;
}

std::unique_ptr<GcPage> acquire_page(size_t capacity, PageKind kind,
                                     bool survivor = false) {
  const bool ordinary = capacity == kGcPageBytes;
  if (ordinary) {
    GcRegion* region = nullptr;
    uint8_t slot = 0;
    bool storage_is_zero = false;
    unsigned char* storage =
        carve_region_page_locked(survivor, region, slot, storage_is_zero);
    if (!storage) return nullptr;
    if constexpr (nirq::runtime_debug_enabled) {
      if ((reinterpret_cast<uintptr_t>(storage) & (kGcPageBytes - 1)) != 0)
        nrq_runtime_fatal("GC region page is not 64 KiB aligned");
      if (storage_is_zero) {
        const size_t step = capacity / 8 > sizeof(uint64_t) ? capacity / 8
                                                            : sizeof(uint64_t);
        for (size_t at = 0; at + sizeof(uint64_t) <= capacity; at += step) {
          uint64_t word = 0;
          std::memcpy(&word, storage + at, sizeof(word));
          if (word != 0) nrq_runtime_fatal("fresh GC region page was not zero");
        }
      }
    }
    std::unique_ptr<GcPage> page;
    bool reused_shell = false;
    for (auto entry = recycled_pages.rbegin(); entry != recycled_pages.rend();
         ++entry) {
      if ((*entry)->capacity != capacity) continue;
      page = std::move(*entry);
      if (&*entry != &recycled_pages.back()) {
        *entry = std::move(recycled_pages.back());
      }
      recycled_pages.pop_back();
      reused_shell = true;
      break;
    }
    if (page) {
      if constexpr (nirq::runtime_debug_enabled) {
        if (page->retired || page->free_span_count != 0 || page->used != 0 ||
            page->live_objects != 0 || page->use_object_index ||
            page->max_free_span != 0 || page->marked_objects != 0 ||
            page->remembered_objects != 0) {
          nrq_runtime_fatal("recycled GC page carried stale metadata");
        }
      }
      page->storage = storage;
      page->region = region;
      page->region_slot = slot;
      page->zero_cursor = storage_is_zero ? 0 : capacity;
      page->kind = page->object_starts ? kind : PageKind::Fallback;
      page->slot_bytes = 0;
      page->slot_capacity = 0;
      page->uniform_layout = nullptr;
      region->pages[slot] = page.get();
      recycled_page_reuse_count = saturated_add(recycled_page_reuse_count, 1);
      return page;
    }
    try {
      page = std::make_unique<GcPage>();
      page->storage = storage;
      page->capacity = capacity;
      page->region = region;
      page->region_slot = slot;
      page->zero_cursor = storage_is_zero ? 0 : capacity;
      const size_t slots = capacity / kObjectStartAlignment;
      const size_t mark_words = (slots + 63) / 64;
      std::memset(page->inline_bitmaps.data(), 0,
                  4 * mark_words * sizeof(uint64_t));
      page->mark_bits = page->inline_bitmaps.data();
      page->remembered_bits = page->mark_bits + mark_words;
      page->born_bits = page->remembered_bits + mark_words;
      page->object_starts = page->born_bits + mark_words;
      page->object_start_words = mark_words;
      page->mark_words = mark_words;
      page->remembered_words = mark_words;
      page->born_words = mark_words;
      page->kind = kind;
      region->pages[slot] = page.get();
      if (!storage_is_zero && !reused_shell)
        recycled_page_reuse_count = saturated_add(recycled_page_reuse_count, 1);
      return page;
    } catch (...) {
      if (region->active_pages > 0) --region->active_pages;
      if (region->active_pages == 0 && region->next_slot > 0)
        remove_gc_region_locked(region);
      return nullptr;
    }
  }

  for (auto entry = recycled_pages.rbegin(); entry != recycled_pages.rend();
       ++entry) {
    if ((*entry)->capacity != capacity || !(*entry)->storage) continue;
    std::unique_ptr<GcPage> page = std::move(*entry);
    if (&*entry != &recycled_pages.back()) {
      *entry = std::move(recycled_pages.back());
    }
    recycled_pages.pop_back();
    if constexpr (nirq::runtime_debug_enabled) {
      if (page->retired || page->free_span_count != 0 || page->used != 0 ||
          page->live_objects != 0 || page->use_object_index ||
          page->max_free_span != 0 || page->marked_objects != 0 ||
          page->remembered_objects != 0) {
        nrq_runtime_fatal("recycled GC page carried stale metadata");
      }
    }
    const PageKind requested = page->capacity > kGcPageBytes
                                   ? PageKind::Large
                                   : kind;
    page->kind = page->object_starts ? requested : PageKind::Fallback;
    page->slot_bytes = 0;
    page->slot_capacity = 0;
    page->uniform_layout = nullptr;
    recycled_page_reuse_count = saturated_add(recycled_page_reuse_count, 1);
    return page;
  }
  bool storage_is_zero = false;
  unsigned char* storage = commit_page_storage(capacity, storage_is_zero);
  if (!storage) return nullptr;
  if constexpr (nirq::runtime_debug_enabled) {
    if (storage_is_zero) {
      const size_t step = capacity / 8 > sizeof(uint64_t) ? capacity / 8
                                                          : sizeof(uint64_t);
      for (size_t at = 0; at + sizeof(uint64_t) <= capacity; at += step) {
        uint64_t word = 0;
        std::memcpy(&word, storage + at, sizeof(word));
        if (word != 0) nrq_runtime_fatal("fresh GC page storage was not zero");
      }
    }
  }
  try {
    auto page = std::make_unique<GcPage>();
    page->storage = storage;
    page->capacity = capacity;
    page->zero_cursor = storage_is_zero ? 0 : capacity;
    if constexpr (nirq::runtime_debug_enabled) {
      if (capacity <= kGcPageBytes &&
          (reinterpret_cast<uintptr_t>(storage) & (kGcPageBytes - 1)) != 0)
        nrq_runtime_fatal("GC page is not aligned to its own size");
    }
    const size_t slots = capacity / kObjectStartAlignment;
    const size_t mark_words = (slots + 63) / 64;
    if (mark_words <= GcPage::kInlineBitmapWords) {
      std::memset(page->inline_bitmaps.data(), 0,
                  4 * mark_words * sizeof(uint64_t));
      page->mark_bits = page->inline_bitmaps.data();
      page->remembered_bits = page->mark_bits + mark_words;
      page->born_bits = page->remembered_bits + mark_words;
      if (capacity <= kGcPageBytes) {
        page->object_starts = page->born_bits + mark_words;
        page->object_start_words = mark_words;
      }
    } else {
      page->grown_bitmaps.reset(new (std::nothrow) uint64_t[3 * mark_words]());
      if (!page->grown_bitmaps) {
        release_page_storage(storage, capacity);
        return nullptr;
      }
      page->mark_bits = page->grown_bitmaps.get();
      page->remembered_bits = page->mark_bits + mark_words;
      page->born_bits = page->remembered_bits + mark_words;
    }
    page->mark_words = mark_words;
    page->remembered_words = mark_words;
    page->born_words = mark_words;
    page->kind = capacity > kGcPageBytes ? PageKind::Large : kind;

    return page;
  } catch (...) {
    release_page_storage(storage, capacity);
    return nullptr;
  }
}

// Return an empty page to the bounded recycle pool, or release its storage when
// the pool is full. Must be called with gc_mutex held at a cycle boundary, when
// no mutator can still hold a cursor into the page.
void recycle_page(std::unique_ptr<GcPage> page) {
  if (!page) return;
  unindex_span_page_locked(*page);
  if (page->free_span_count != 0) {
    total_free_spans -= page->free_span_count;
    page->free_span_count = 0;
    page->free_span_bytes = 0;
  }
  set_page_max_free_span_locked(*page, 0, 0);
  if (page->region != nullptr) {
    release_page_backing_locked(*page);
    if (recycled_pages.size() >= kMaxRecycledPages) return;
    page->retired = false;
    page->free_span_count = 0;
    page->free_span_bytes = 0;
    reset_page_gc_metadata_locked(*page);
    try {
      recycled_pages.push_back(std::move(page));
    } catch (...) {
    }
    return;
  }
  if (recycled_pages.size() >= kMaxRecycledPages ||
      page->capacity < kGcPageBytes ||
      page->capacity > kMaxRecycledPageBytes) {
    release_page_storage(page->storage, page->capacity);
    return;
  }
  page->retired = false;
  page->free_span_count = 0;
  page->free_span_bytes = 0;
  reset_page_gc_metadata_locked(*page);
  GcPage* raw = page.get();
  try {
    recycled_pages.push_back(std::move(page));
  } catch (...) {
    release_page_storage(raw->storage, raw->capacity);
  }
}

// Choose the next automatic-collection trigger from what the finished cycle
// observed. The trigger is an allocation budget, so it is expressed as the live
// heap plus headroom: no headroom would make the collector run again as soon as
// anything is allocated, and unbounded headroom would grow the heap forever.
// Headroom is at least a fixed minimum and otherwise half the live heap, so the
// budget scales with the program's actual working set, and a cycle that barely
// reclaimed anything (a mostly live heap) gets more room next time instead of
// being repeated for almost no gain.
void adapt_collection_policy_locked() {
  const size_t live = live_heap_bytes;
  const size_t before = cycle_start_live_bytes;
  collection_policy.last_reclaimed_bytes =
      before > live ? before - live : 0;
  if (!collection_policy.adaptive) return;

  size_t headroom = std::max(kMinimumCollectionHeadroom, live / 2);
  const bool heap_is_mostly_live = before != 0 && live > (before / 8) * 7;
  if (heap_is_mostly_live) headroom += live / 2;
  size_t threshold = live + headroom;
  if (threshold < live) threshold = live;  // saturating add of a huge heap

  allocation_accounting.collection_threshold.store(threshold,
                                                  std::memory_order_relaxed);
  refresh_inline_config_locked();
  ++collection_policy.adaptations;
}

ObjectRef find_pending_header(MutatorState& mutator, void* object);

// An entry is active only while it has both an object and a nonzero delta. A
// handoff that is cancelled before the next drain leaves nothing to fold, so the
// slot has to stop matching: a zero-delta entry that still matched would absorb
// later pins and keep its slot forever, which is how the buffer used to fill with
// entries that describe nothing.
TransferPinDelta* find_transfer_delta(MutatorState& mutator, void* object) {
  if (!object) return nullptr;
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    const TransferPinDelta& entry = mutator.transfer_pins[index];
    if (entry.object == object && entry.delta != 0)
      return &mutator.transfer_pins[index];
  }
  return nullptr;
}

// Releases a delta entry that has cancelled out. Keeping the object pointer would
// leave a slot that looks occupied to every later scan.
void clear_transfer_delta(TransferPinDelta& entry) {
  entry.object = nullptr;
  entry.delta = 0;
}

// Returns an empty slot, reusing a retired entry before extending the logical
// high-water count. Without reuse a workload that returns and immediately releases
// distinct objects would fill all 32 entries with retired slots and push every
// later handoff onto the global pin table for the rest of the process.
TransferPinDelta* acquire_transfer_delta_slot(MutatorState& mutator) {
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    TransferPinDelta& entry = mutator.transfer_pins[index];
    if (!entry.object) {
      entry.delta = 0;
      return &entry;
    }
  }
  if (mutator.transfer_pin_count >= kTransferPinBufferCapacity) return nullptr;
  TransferPinDelta& entry = mutator.transfer_pins[mutator.transfer_pin_count];
  ++mutator.transfer_pin_count;
  entry.object = nullptr;
  entry.delta = 0;
  return &entry;
}

// Handoffs that found no reusable slot. Zero here means the buffered fast path
// carried every handoff; a rising count means the buffer is genuinely full of live
// deltas, which is the only case that should reach the global pin table.
std::atomic<uint64_t> transfer_pin_fallbacks{0};

// Fold a mutator's buffered transfer-pin deltas into the global pin table. Called
// with gc_mutex held, either by the collector after parking the mutator or by the
// mutator itself, so the buffer cannot be written while it is being read.
void drain_transfer_pins_locked(MutatorState& mutator) {
  if (mutator.fast && mutator.fast->transfer_pin_fast) {
    void* obj = mutator.fast->transfer_pin_fast;
    mutator.fast->transfer_pin_fast = nullptr;
    if (!find_object_locked(obj).valid() &&
        !find_pending_header(mutator, obj).valid()) {
      pin_violations = saturated_add(pin_violations, 1);
    } else {
      try {
        size_t& count = pinned_objects[obj];
        if (count == SIZE_MAX)
          nrq_runtime_fatal("GC transfer pin count overflow");
        ++count;
      } catch (...) {
        nrq_runtime_fatal("GC transfer pin registration failed");
      }
    }
  }
  for (size_t index = 0; index < mutator.transfer_pin_count; ++index) {
    const TransferPinDelta& entry = mutator.transfer_pins[index];
    if (!entry.object || entry.delta == 0) continue;
    if (entry.delta > 0) {
      // A pin of a not-yet-published object is legitimate: cycle preparation
      // publishes every TLAB before this drain runs. Anything else is a pin of a
      // pointer the heap does not own, which the buffered path can only detect
      // here, so it is recorded as a rejection instead of being counted.
      if (!find_object_locked(entry.object).valid() &&
          !find_pending_header(mutator, entry.object).valid()) {
        pin_violations =
            saturated_add(pin_violations, static_cast<uint64_t>(entry.delta));
        continue;
      }
      try {
        size_t& count = pinned_objects[entry.object];
        const uint64_t total =
            static_cast<uint64_t>(count) + static_cast<uint64_t>(entry.delta);
        if (total > static_cast<uint64_t>(SIZE_MAX))
          nrq_runtime_fatal("GC transfer pin count overflow");
        count = static_cast<size_t>(total);
      } catch (...) {
        nrq_runtime_fatal("GC transfer pin registration failed");
      }
      continue;
    }
    auto found = pinned_objects.find(entry.object);
    if (found == pinned_objects.end()) continue;  // nothing left to release
    const uint64_t release = static_cast<uint64_t>(-entry.delta);
    if (release >= static_cast<uint64_t>(found->second))
      pinned_objects.erase(found);
    else
      found->second -= static_cast<size_t>(release);
  }
  mutator.transfer_pin_count = 0;
}

void prepare_cycle_locked(bool young_only) {
  for (MutatorState* mutator : mutators)
    if (mutator) publish_tlab_locked(*mutator);
  // Every reserved-but-unused run goes back to its page here, before the
  // generation stamp moves and before the reclaimer decides which pages are
  // empty. A page can only be recycled once all of its space is either published
  // or recorded as free, and a run sitting on a mutator's list is neither.
  for (MutatorState* mutator : mutators)
    if (mutator) flush_spare_runs_locked(*mutator);
  // The unused end of each live reservation is the same argument one level
  // closer: it holds no published object, so at this safepoint it is free space
  // and the next reservation in that page can use it.
  for (MutatorState* mutator : mutators)
    if (mutator) release_tlab_tail_locked(*mutator);
  // A fast old-to-young store may have filled its buffer without taking the
  // collector lock. Publish every mutator's TLAB first, then drain, so buffered
  // references to objects that were still thread-local become visible. Any
  // reference deferred by a thread exit is resolved after all TLABs are live.
  for (MutatorState* mutator : mutators)
    if (mutator) drain_remembered_locked(*mutator, /*defer_unresolved=*/false);
  resolve_unresolved_remembered_locked();
  // Buffered transfer pins are invisible until they reach the table, so fold them
  // in before the cycle enumerates pinned roots.
  for (MutatorState* mutator : mutators)
    if (mutator) drain_transfer_pins_locked(*mutator);

  // No mutator can be inside an allocator while stop_mutators() has everyone
  // parked. Invalidate stale TLAB cursors before reclaiming empty pages.
  //
  // `retired_page_count` is the number of pages the previous sweep emptied, so
  // the page set is only walked when there is something to reclaim. Every page
  // that becomes empty is counted where it becomes empty: the sweep that frees
  // its last object, or an allocation that reserved space and then failed.
  // Drop stale remembered entries before a retired page can be recycled and
  // its storage released. Validation uses the page directory and start bits,
  // which are still authoritative at this point.
  compact_remembered_locked();
  gc_page_generation.store(
      gc_page_generation.load(std::memory_order_relaxed) + 1,
      std::memory_order_release);
  if (retired_page_count != 0) {
    for (auto page = gc_pages.begin(); page != gc_pages.end();) {
      if ((*page)->live_objects == 0) {
        if ((*page)->retired) --retired_page_count;
        unindex_tail_page_locked(**page);
        page_directory.erase(page->get());
        auto empty_page = std::move(*page);
        page = gc_pages.erase(page);
        recycle_page(std::move(empty_page));
      } else {
        (*page)->retired = false;
        ++page;
      }
    }
    retired_page_count = 0;
  }

  sweep_list.clear();
  sweep_cursor = SweepCursor{};
  cycle_start_live_bytes = live_heap_bytes;
  active_cycle_epoch = next_cycle_epoch.fetch_add(1) + 1;
  active_collection_is_minor = young_only;
  // Birth counts describe only the cycle that is starting. The previous cycle's
  // sweep already retained those objects; this reset makes every page's
  // survivor count meaningful for the new snapshot.
  for (auto& entry : gc_pages) {
    if (entry) entry->born_this_cycle = 0;
  }
  active_mark.reset();
  active_mark.young_only = young_only;
  active_mark.epoch = active_cycle_epoch;
  enqueue_roots_locked(active_mark);
  collection_phase.store(CollectionPhase::Marking, std::memory_order_release);
}

// Decide whether the next automatic collection must be a full cycle. Young
// objects that die are reclaimed by minors, so the only signal a minor cannot
// act on is old garbage, which keeps the live heap growing without the program
// retaining more data. Callers hold gc_mutex.
void plan_next_cycle_locked() {
  if (!generational_collection_enabled.load(std::memory_order_relaxed)) {
    next_cycle_is_major.store(true, std::memory_order_relaxed);
    return;
  }
  const size_t minors = minor_collections_since_major.load(std::memory_order_relaxed);
  bool major = false;
  // A region budget is a bound, not a hint: once the live heap is at or past it,
  // only a major cycle can bring it back down, because a minor cannot reclaim an
  // old object that is no longer reachable.
  const size_t region_budget = gc_region_limit.load(std::memory_order_relaxed);
  if (region_budget != 0 && live_heap_bytes >= region_budget) major = true;
  if (minors >= kMaximumMinorInterval) {
    major = true;
  } else if (minors >= kMinimumMinorInterval) {
    const size_t baseline = live_heap_bytes_at_last_major;
    const size_t growth = live_heap_bytes > baseline ? live_heap_bytes - baseline : 0;
    const size_t growth_limit =
        std::max(kMinimumCollectionHeadroom, baseline / 2);
    major = growth >= growth_limit;
  }
  next_cycle_is_major.store(major, std::memory_order_relaxed);
}

void begin_cycle(bool young_only) {
  debug_emit(NRQ_GC_EVENT_CYCLE_BEGIN, young_only ? 1u : 0u, 0, nullptr);
  // Publish the transition before parking so new fast-path stores fall back to
  // the synchronized barrier. In-flight fast stores finish before the root
  // snapshot; the per-mutator barrier depth is the handshake that makes that
  // visible.
  {
    HeapLockGuard lock(gc_mutex);
    // A concurrent promotion can turn a reachable object old while mutators
    // run, so enable remembered-set recording for the whole cycle. The flag is
    // lowered again if the cycle finishes with no old objects.
    raise_store_barrier();
    collection_phase.store(CollectionPhase::Preparing,
                           std::memory_order_release);
    refresh_inline_config_locked();
  }
  stop_mutators();
  wait_for_fast_barriers();
  {
    HeapLockGuard lock(gc_mutex);
    prepare_cycle_locked(young_only);
  }
  resume_mutators();
}

// Development-only invariant check for the page metadata the collector now
// derives from. It runs at the end of a cycle, when marks must be gone and every
// per-page count is final, and catches a missed reset before a recycled page can
// carry stale state into the next cycle.
void audit_page_and_region_metadata_locked() {
  // Region totals are derived, never maintained by hand. One pass over the page
  // set recomputes them and checks the linkage they were derived from, so a page
  // that was added or retired without its region hearing about it is caught here
  // instead of silently biasing a pacemaker later. This runs in every
  // configuration - a debug-only oracle would leave release builds unverified.
  static std::vector<size_t> region_live;
  static std::vector<size_t> region_bytes;
  static std::vector<size_t> region_marked;
  region_live.assign(gc_regions.size(), 0);
  region_bytes.assign(gc_regions.size(), 0);
  region_marked.assign(gc_regions.size(), 0);
  for (const auto& entry : gc_pages) {
    const GcPage* page = entry.get();
    if (!page || !page->region) continue;
    const GcRegion* region = page->region;
    if (page->region_slot >= kPagesPerRegion)
      nrq_runtime_fatal("GC page carries an out-of-range region slot");
    if (region->region_index >= gc_regions.size() ||
        gc_regions[region->region_index].get() != region)
      nrq_runtime_fatal("GC page names a region that is not in the pool");
    if (region->pages[page->region_slot] != page)
      nrq_runtime_fatal("GC region slot does not name its page");
    region_live[region->region_index] += page->live_objects;
    region_bytes[region->region_index] += page->live_payload_bytes;
    region_marked[region->region_index] += page->marked_objects;
  }
  for (size_t index = 0; index < gc_regions.size(); ++index) {
    GcRegion* region = gc_regions[index].get();
    if (!region) continue;
    region->live_objects = region_live[index];
    region->live_payload_bytes = region_bytes[index];
    region->marked_objects = region_marked[index];
    if (region->live_objects == 0 && region->live_payload_bytes != 0)
      nrq_runtime_fatal("GC region retained bytes with no live objects");
    if (region->marked_objects > region->live_objects)
      nrq_runtime_fatal("GC region marked more objects than it holds");
  }
  for (const auto& entry : gc_pages) {
    const GcPage* page = entry.get();
    if (!page) continue;
    if (page->young_objects > page->live_objects ||
        page->indexed_objects > page->live_objects ||
        page->marked_objects > page->live_objects ||
        page->marked_young_objects > page->young_objects ||
        page->remembered_objects > page->young_objects)
      nrq_runtime_fatal("GC page metadata count is inconsistent");
    if (page->live_objects == 0 &&
        (page->young_objects != 0 || page->indexed_objects != 0 ||
         page->live_payload_bytes != 0 ||
         page->marked_objects != 0 || page->marked_young_objects != 0 ||
         page->remembered_objects != 0))
      nrq_runtime_fatal("GC empty page retained live metadata");
    if (page->marked_objects != 0 || page->marked_young_objects != 0)
      nrq_runtime_fatal("GC cycle ended with a page mark outstanding");
    // The Phase 4 contract, checked where the cycle already audits metadata:
    // no page class may describe a header larger than H16, and a headerless page
    // must hold everything needed to decode an object that has no header.
    if (page_header_bytes(page->kind) > 16)
      nrq_runtime_fatal("GC page describes a header larger than 16 bytes");
    if (page->kind == PageKind::Headerless) {
      if (page->slot_bytes == 0 || !page->uniform_layout || !page->h0_states ||
          page->h0_state_capacity < page->capacity / page->slot_bytes)
        nrq_runtime_fatal("GC headerless page cannot decode its objects");
    }
  }
}

void finish_cycle_locked(bool young_only) {
  audit_page_and_region_metadata_locked();
  statistics.collections = saturated_add(statistics.collections, 1);
  if (young_only) {
    minor_collections = saturated_add(minor_collections, 1);
    minor_collections_since_major.store(
        minor_collections_since_major.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
  } else {
    major_collections = saturated_add(major_collections, 1);
    minor_collections_since_major.store(0, std::memory_order_relaxed);
  }
  // The live heap size is maintained by publication and reclamation, so this
  // reports it directly instead of walking every live object each cycle.
  statistics.allocated_bytes = live_heap_bytes;
  adapt_collection_policy_locked();
  debug_emit(NRQ_GC_EVENT_CYCLE_END, young_only ? 1u : 0u,
             collection_policy.last_reclaimed_bytes, nullptr);
  if (!young_only) live_heap_bytes_at_last_major = live_heap_bytes;
  plan_next_cycle_locked();
  // Do not clear the remembered set here. A major collection erases promoted
  // survivors as it sweeps, while young objects allocated during the cycle
  // remain young and may be referenced only by old objects; their remembered
  // entries must survive into the next minor collection.
  // The barrier is only useful while a minor can run: with generations off they
  // never do, and with no old objects there is no old-to-young edge for the
  // remembered set to record. Leaving it raised in either case would put the
  // write barrier back on every store of a program that does not need it.
  if (!generational_collection_enabled.load(std::memory_order_relaxed) ||
      old_object_count.load(std::memory_order_relaxed) == 0)
    lower_store_barrier();
  active_mark.reset();
  collection_phase.store(CollectionPhase::Idle, std::memory_order_release);
  allocation_accounting.bytes_since_collection.store(0, std::memory_order_relaxed);
  refresh_inline_config_locked();
}

// A registered mutator may request a collection while another thread is already
// collecting. Waiting on collection_cycle_mutex without parking would deadlock
// the active collector, which waits for every mutator to reach a safepoint. Park
// before blocking on the cycle lock, then resume once the previous cycle exits.
std::unique_lock<std::mutex> acquire_collection_cycle() {
  std::unique_lock<std::mutex> cycle_lock(collection_cycle_mutex, std::defer_lock);
  if (cycle_lock.try_lock()) return cycle_lock;
  if (thread_mutator.depth == 0) {
    cycle_lock.lock();
    return cycle_lock;
  }
  {
    std::unique_lock<std::mutex> lock(mutator_mutex);
    thread_mutator.parked = true;
    parked_mutator_count.fetch_add(1, std::memory_order_release);
    collector_condition.notify_one();
  }
  cycle_lock.lock();
  {
    std::lock_guard<std::mutex> lock(mutator_mutex);
    parked_mutator_count.fetch_sub(1, std::memory_order_release);
    thread_mutator.parked = false;
  }
  return cycle_lock;
}

struct CollectorThreadGuard {
  bool previous = thread_is_collector;
  CollectorThreadGuard() { thread_is_collector = true; }
  ~CollectorThreadGuard() { thread_is_collector = previous; }
};

void run_collection(bool young_only) {
  CollectorThreadGuard collector_guard;
  // The phases are timed separately because a whole-cycle figure cannot say
  // which of them a change moved: the same graph is marked and then walked again
  // by the sweep, so the two costs land in one number.
  const CycleClock::time_point cycle_start = CycleClock::now();
  std::unique_lock<std::mutex> cycle_lock = acquire_collection_cycle();
  const CycleClock::time_point prepare_start = CycleClock::now();
  begin_cycle(young_only);
  cycle_timing.prepare_ns += cycle_ns(prepare_start);
  bool sweeping = false;
  for (;;) {
    bool finished = false;
    bool made_progress = false;
    {
      HeapLockGuard lock(gc_mutex);
      if (!sweeping) {
        const CycleClock::time_point mark_start = CycleClock::now();
        const bool marked = mark_slice_locked(kMarkSliceBytes);
        cycle_timing.mark_ns += cycle_ns(mark_start);
        made_progress = true;
        if (marked) {
          verify_root_marks_locked();
          closure_diff_locked(1);
          mark_born_references_locked();
          // Freeze the unindexed list and capture the page set before releasing
          // the lock for sweep slices. Allocations after this point either land
          // in a fresh page (outside the captured set) or carry a birth epoch
          // past the cycle, so the sweeper never has to reason about them.
          sweep_list.clear();
          sweep_list.swap(unindexed_objects);
          sweep_cursor = SweepCursor{};
          sweep_cursor.page_limit = gc_pages.size();
          collection_phase.store(CollectionPhase::Sweeping,
                                 std::memory_order_release);
          sweeping = true;
        }
      }
      if (sweeping) {
        // Drain barriers that ran between sweep slices before freeing the next
        // chunk. A pending marked parent may still enqueue its children.
        const CycleClock::time_point mark_start = CycleClock::now();
        const bool drained = mark_slice_locked(kMarkSliceBytes);
        cycle_timing.mark_ns += cycle_ns(mark_start);
        made_progress = true;
        if (drained) {
          const CycleClock::time_point sweep_start = CycleClock::now();
          const bool chunk_done = sweep_chunk_locked(kSweepSliceBytes);
          cycle_timing.sweep_ns += cycle_ns(sweep_start);
          if (chunk_done) {
            const CycleClock::time_point finish_start = CycleClock::now();
            finish_cycle_locked(young_only);
            cycle_timing.finish_ns += cycle_ns(finish_start);
            cycle_timing.pause_ns += cycle_ns(cycle_start);
            ++cycle_timing.cycles;
            finished = true;
          }
        }
      }
    }
    if (finished) return;
    if (!made_progress) std::this_thread::yield();
  }
}

// Phi: the region-utilisation pacemaker. A program that declares a region budget
// is asking the collector to start cycles from how much of that budget allocation
// has consumed, instead of from a byte count that bears no relation to it. With no
// budget set - the default - the threshold is SIZE_MAX and the decision is exactly
// the one the collector made before, so nothing changes for existing programs.
size_t region_pressure_threshold() {
  const size_t limit = gc_region_limit.load(std::memory_order_relaxed);
  if (limit == 0) return SIZE_MAX;
  const uint32_t percent = std::clamp<uint32_t>(
      gc_trigger_percent.load(std::memory_order_relaxed), 1u, 100u);
  const size_t whole = limit / 100 * percent;
  const size_t remainder = (limit % 100) * percent / 100;
  return whole > SIZE_MAX - remainder ? SIZE_MAX : whole + remainder;
}

// The safepoint poll's threshold: whichever of the two policies asks for a cycle
// first. Kept as a function of two relaxed atomic loads so a poll never has to
// take the lock, and a poll that decides nothing is a load, a compare and a
// predictable branch.
size_t safepoint_poll_threshold() {
  const size_t pressure = region_pressure_threshold();
  const size_t volume =
      allocation_accounting.collection_threshold.load(std::memory_order_relaxed);
  return pressure < volume ? pressure : volume;
}

void maybe_collect_automatically() {
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle)
    return;
  const bool should_collect = automatic_collection_enabled.load(
                                  std::memory_order_relaxed) &&
      allocation_accounting.bytes_since_collection.load(std::memory_order_relaxed) >=
          safepoint_poll_threshold();
  if (!should_collect) return;
  const bool young_only =
      generational_collection_enabled.load(std::memory_order_relaxed) &&
      !next_cycle_is_major.load(std::memory_order_relaxed);
  run_collection(young_only);
}

// Representation selection. It is a static decision from the layout's shape and
// size: the compiler can make the same one in Phase 5, and nothing on the access
// path has to re-decide it.
enum class GcRepresentation : uint8_t { Auto, Headerless, Compact, Extended };

// A headerless object's size comes from its page, so its payload has to fit one
// slot comfortably. Larger fixed records go to H8, where the header is smaller
// than the alignment waste a dedicated uniform page would leave.
constexpr size_t kHeaderlessMaxPayload = 256;

GcRepresentation select_representation(size_t payload_bytes, size_t elements,
                                       const nrq_object_layout* layout,
                                       GcRepresentation hint, bool& valid) {
  const bool array = (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0;
  const bool soa =
      layout->soa_column_pairs != 0 && layout->soa_columns != nullptr;
  const bool fits_page =
      object_span_bytes(0, payload_bytes) <= kGcPageBytes;
  if (array) {
    // Non-SoA one-element arrays whose 8-byte-header span fits within a single
    // page can use Compact (H8): Compact pages already imply element_count == 1,
    // while `layout->flags & NRQ_OBJECT_LAYOUT_ARRAY` distinguishes them from
    // fixed records. SoA containers read element_count at payload - 16 and keep H16.
    const bool compact_eligible =
        !soa && elements == 1 &&
        object_span_bytes(page_header_bytes(PageKind::Compact), payload_bytes) <=
            kGcPageBytes;
    switch (hint) {
      case GcRepresentation::Headerless:
        valid = false;
        return GcRepresentation::Extended;
      case GcRepresentation::Compact:
        valid = compact_eligible;
        return valid ? GcRepresentation::Compact : GcRepresentation::Extended;
      case GcRepresentation::Extended:
        valid = true;
        return GcRepresentation::Extended;
      case GcRepresentation::Auto:
        valid = true;
        return compact_eligible ? GcRepresentation::Compact
                                : GcRepresentation::Extended;
    }
    valid = false;
    return GcRepresentation::Extended;
  }
  switch (hint) {
    case GcRepresentation::Headerless:
      valid = payload_bytes <= kHeaderlessMaxPayload && fits_page;
      return valid ? GcRepresentation::Headerless : GcRepresentation::Extended;
    case GcRepresentation::Compact:
      valid = fits_page;
      return valid ? GcRepresentation::Compact : GcRepresentation::Extended;
    case GcRepresentation::Extended:
      valid = true;
      return GcRepresentation::Extended;
    case GcRepresentation::Auto:
      valid = true;
      if (payload_bytes <= kHeaderlessMaxPayload && fits_page)
        return GcRepresentation::Headerless;
      return fits_page ? GcRepresentation::Compact
                       : GcRepresentation::Extended;
  }
  valid = false;
  return GcRepresentation::Extended;
}

// Writes the header this representation uses and, unless the caller already knows
// the storage is zero, zeroes the payload. The layout id is resolved once here,
// never on a read path.
//
// `payload_is_zero` is decided by whoever handed out the storage, not by this
// function: asking the page per object cost more than the fill it sometimes
// saved, so the answer is carried in a register from the reservation that already
// had to look at the page.
void initialize_object(const ObjectRef& object, const nrq_object_layout* layout,
                       uint32_t elements, size_t payload_bytes, bool young,
                       bool payload_is_zero) {
  switch (object.page->kind) {
    case PageKind::Headerless:
      object.page->h0_states[h0_state_index(object)] = young ? 1u : 0u;
      break;
    case PageKind::Compact: {
      auto* header = reinterpret_cast<CompactHeader*>(object.header);
      header->state = young ? kYoungBit : 0u;
      header->layout_id = layout_id_for(layout);
      if (header->layout_id == 0)
        nrq_runtime_fatal("GC layout id space exhausted");
      break;
    }
    default: {
      auto* header = reinterpret_cast<ExtendedHeader*>(object.header);
      header->element_count = elements;
      header->state = young ? kYoungBit : 0u;
      header->layout_id = layout_id_for(layout);
      header->flags = 0;
      if (header->layout_id == 0)
        nrq_runtime_fatal("GC layout id space exhausted");
      break;
    }
  }
  // The payload is promised zero-initialised. Storage the OS has just committed
  // already holds zeros, so the reservation that took it from the page's
  // untouched tail passed `payload_is_zero` and the fill is skipped; anything that
  // may have held an object before is filled as before.
  if (payload_is_zero) return;
  unsigned char* payload = static_cast<unsigned char*>(object_payload(object));
  const size_t payload_offset = static_cast<size_t>(payload - object.page->storage);
  std::memset(payload, 0, payload_bytes);
  // The fill wrote zeros, so the known-zero suffix now starts at the end of this
  // payload: nothing past it has been handed out since the page was committed.
  const size_t payload_end = payload_offset + payload_bytes;
  if (payload_end > object.page->zero_cursor)
    object.page->zero_cursor = payload_end;
}

// Decides once, for a single object taken under the lock, whether the page's
// storage already holds the zeros the payload owes, and charges the object's span
// to the page's known-zero boundary. The value returned is what the caller passes
// to initialize_object.
bool take_page_zero_state_locked(GcPage& page, size_t header_offset,
                                 size_t span) {
  // Only a page the allocator dedicated to one object is worth asking about. The
  // fill there is a whole payload - hundreds of kilobytes for the large-object
  // shape - while a locked allocation on an ordinary page is a few dozen bytes,
  // where the branch and its bookkeeping would cost more than the fill it saves.
  // Those pages keep filling as they always did, and initialize_object advances
  // their boundary from the payload it wrote.
  if (page.capacity <= kGcPageBytes) return false;
  const bool storage_is_zero = header_offset >= page.zero_cursor;
  const size_t end = header_offset + span;
  if (end > page.zero_cursor) page.zero_cursor = end;
  zeroing_facts.payload_bytes += span;
  if (storage_is_zero)
    zeroing_facts.prezeroed_bytes += span;
  else
    zeroing_facts.filled_bytes += span;
  return storage_is_zero;
}

// Updates every counter a newly visible object contributes to. Publication and
// the serialized allocator share it so the two paths cannot account differently.
void account_new_object_locked(const ObjectRef& object, size_t payload_bytes,
                               bool young) {
  GcPage& page = *object.page;
  const bool indexed = page_uses_index(&page);
  if (indexed) {
    if (!object_index.insert(object_payload(object), object.header, &page))
      nrq_runtime_fatal("GC failed to publish a thread-local allocation");
    ++page.indexed_objects;
    try {
      unindexed_objects.push_back(object);
    } catch (...) {
      nrq_runtime_fatal("GC unindexed object list allocation failed");
    }
  } else {
    mark_object_start(page,
                      static_cast<size_t>(object.header - page.storage));
  }
  if (page.live_objects == SIZE_MAX)
    nrq_runtime_fatal("GC page live-object count overflow");
  ++page.live_objects;
  if (page.live_payload_bytes > SIZE_MAX - payload_bytes)
    nrq_runtime_fatal("GC page live-byte count overflow");
  page.live_payload_bytes += payload_bytes;
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle)
    {
      mark_object_born(object, next_cycle_epoch.load(std::memory_order_relaxed));
      // The same rule as the range path: an object appearing while a sweep is
      // already running keeps its references only if they are marked as it
      // arrives, because the transition pass cannot have seen this object.
      if (collection_phase.load(std::memory_order_relaxed) ==
          CollectionPhase::Sweeping)
        trace_children(object, mark_reference_value, nullptr);
    }
  if (young) {
    ++page.young_objects;
  } else {
    old_object_count.fetch_add(1, std::memory_order_relaxed);
  }
  statistics.allocated_bytes = saturated_add(
      statistics.allocated_bytes, static_cast<uint64_t>(payload_bytes));
  statistics.live_objects = saturated_add(statistics.live_objects, 1);
  live_heap_bytes = saturated_add(live_heap_bytes, payload_bytes);
}

// A headerless page is dedicated to one layout and one slot size. Reusing the
// most recently carved page of that shape keeps a run of identical records
// inside pages that are already resident.
GcPage* find_headerless_page_locked(const nrq_object_layout* layout,
                                    size_t slot) {
  for (auto candidate = tail_pages.rbegin(); candidate != tail_pages.rend();
       ++candidate) {
    GcPage* page = *candidate;
    if (!page || page->kind != PageKind::Headerless) continue;
    if (page->uniform_layout != layout || page->slot_bytes != slot) continue;
    if (page->used <= page->capacity && page->capacity - page->used >= slot)
      return page;
  }
  return nullptr;
}

GcPage* create_headerless_page_locked(const nrq_object_layout* layout,
                                      size_t slot) {
  auto new_page = acquire_page(kGcPageBytes, PageKind::Headerless);
  if (!new_page) return nullptr;
  GcPage* page = new_page.get();
  page->slot_bytes = slot;
  page->slot_capacity = kGcPageBytes / kObjectStartAlignment;
  page->uniform_layout = layout;
  // One state byte per object slot. A page reused for a different slot size
  // grows the table rather than reallocating a smaller one.
  const size_t state_bytes = page->capacity / slot;
  if (!page->h0_states || page->h0_state_capacity < state_bytes) {
    page->h0_states.reset(new (std::nothrow) uint8_t[state_bytes]());
    if (!page->h0_states) return nullptr;
    page->h0_state_capacity = state_bytes;
  }
  try {
    gc_pages.push_back(std::move(new_page));
  } catch (...) {
    release_page_backing_locked(*page);
    return nullptr;
  }
  if (!page_directory.insert(page)) {
    // A headerless object is found from its page; without a directory entry the
    // representation cannot be resolved, so the page is not used for one.
    page->use_object_index = true;
    page->kind = PageKind::Fallback;
    return nullptr;
  }
  index_tail_page_locked(*page);
  return page;
}

void* allocate_locked(size_t bytes, size_t element_size, size_t elements,
                      nrq_gc_trace_fn trace, const size_t* reference_offsets,
                      size_t reference_count,
                      const nrq_object_layout* resolved,
                      GcRepresentation hint, bool repeated = false) {
  if (bytes == 0 || element_size == 0 || elements == 0) return nullptr;
  // A caller that already owns an immutable descriptor hands it over directly:
  // it is the identity the object stores, so this path neither interns nor
  // compares tables.
  const nrq_object_layout* layout =
      resolved ? resolved
               : layout_for_allocation(bytes, element_size, trace,
                                       reference_offsets, reference_count,
                                       repeated);
  if (!layout) return nullptr;
  bool valid = true;
  const GcRepresentation representation = select_representation(
      layout->payload_size, elements, layout, hint, valid);
  if (!valid) return nullptr;
  const bool young =
      generational_collection_enabled.load(std::memory_order_relaxed);
  if (representation == GcRepresentation::Headerless) {
    const size_t slot = object_span_bytes(0, layout->payload_size);
    // Reuse a reclaimed slot before taking fresh page space, exactly as the
    // compact and extended paths do.
    size_t offset = 0;
    GcPage* page =
        take_free_span_locked(slot, offset, PageKind::Headerless, layout);
    if (page && page->slot_bytes != slot) page = nullptr;
    if (!page) {
      page = find_headerless_page_locked(layout, slot);
      if (!page) page = create_headerless_page_locked(layout, slot);
      if (!page) return nullptr;
      unretire_page_locked(*page);
      offset = page->used;
      page->used += slot;
      note_page_tail_locked(*page);
    }
    const ObjectRef object{page->storage + offset, page};
    initialize_object(object, layout, 1, layout->payload_size, young,
                      take_page_zero_state_locked(*page, offset, slot));
    account_new_object_locked(object, layout->payload_size, young);
    add_since_collection_locked(layout->payload_size);
    debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, layout->payload_size,
               object_payload(object));
    return object_payload(object);
  }

  const size_t header_bytes =
      representation == GcRepresentation::Compact ? 8 : 16;
  const PageKind kind =
      representation == GcRepresentation::Compact ? PageKind::Compact
                                                  : PageKind::Extended;
  const size_t span = object_span_bytes(header_bytes, bytes);
  if (span < header_bytes || span > SIZE_MAX - kObjectStartAlignment)
    return nullptr;
  GcPage* page = nullptr;
  size_t header_offset = 0;
  bool reused_span = false;
  GcPage* const cached = cached_allocation_page(kind);
  // Reclaimed space in an existing page comes first: reusing it keeps the
  // working set inside pages that are already resident.
  page = take_free_span_locked(span, header_offset, kind, nullptr);
  if (page) {
    reused_span = true;
  } else if (cached && cached->used <= cached->capacity &&
             span <= cached->capacity - cached->used) {
    page = cached;
  } else {
    for (auto candidate = tail_pages.rbegin(); candidate != tail_pages.rend();
         ++candidate) {
      GcPage* possible = *candidate;
      if (!possible || possible->kind != kind) continue;
      if (span <= possible->capacity - possible->used) {
        page = possible;
        break;
      }
    }
  }
  if (!page) {
    const size_t capacity = std::max(kGcPageBytes, span);
    auto new_page = acquire_page(capacity, kind);
    if (!new_page) return nullptr;
    page = new_page.get();
    try {
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      if (new_page) release_page_backing_locked(*new_page);
      return nullptr;
    }
    // A page larger than the directory's page size is never found by masking an
    // address, and it has no start bitmap, so its objects are tracked by the
    // exact index from the first allocation. A directory insert can succeed by
    // chunk key alone, which left such objects with no start bit and no index
    // entry: the sweep never saw them and they leaked.
    if (page->capacity > kGcPageBytes) {
      page->use_object_index = true;
      page->kind = PageKind::Large;
    } else if (!page_directory.insert(page)) {
      page->use_object_index = true;
      page->kind = PageKind::Fallback;
    }
    index_tail_page_locked(*page);
  }
  unretire_page_locked(*page);
  cache_allocation_page(page);
  if (!reused_span) {
    header_offset = page->used;
    page->used += span;
    note_page_tail_locked(*page);
  }
  const ObjectRef object{page->storage + header_offset, page};
  initialize_object(object, layout, static_cast<uint32_t>(elements), bytes,
                    young,
                    take_page_zero_state_locked(*page, header_offset, span));
  account_new_object_locked(object, bytes, young);
  add_since_collection_locked(bytes);
  debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, bytes, object_payload(object));
  return object_payload(object);
}

bool reserve_tlab_locked(MutatorState& mutator, size_t minimum_span,
                         PageKind kind) {
  tlab_refill_count = saturated_add(tlab_refill_count, 1);
  if (tlab_diag.enabled.load(std::memory_order_relaxed))
    tlab_diag.refill_count.fetch_add(1, std::memory_order_relaxed);
  if (mutator.tlab_generation != gc_page_generation.load(std::memory_order_acquire)) discard_stale_tlab(mutator);
  // A merged run of reclaimed space can host a whole thread-local reservation.
  // Taking it here is what turns a fragmented page back into a usable bump
  // region instead of leaving it permanently half empty - and taking all of it,
  // rather than a fixed small slice, is what keeps a recycled heap from being
  // carved into pieces that later reservations cannot use.
  //
  // A slab-sized run is preferred, but only when one exists:
  // `pages_with_slab_run` answers that in one load. Proving the negative by
  // walking the run index made a fragmented heap pay a full page scan for every
  // few allocations. Short runs are still reused, after the committed page tails
  // below, which cost nothing to hand out.
  auto take_run = [&](size_t wanted_minimum) {
    size_t run_offset = 0;
    size_t run_span = 0;
    GcPage* reused = take_free_run_locked(kGcTlabBytes, wanted_minimum,
                                          minimum_span, run_offset, run_span,
                                          kind, nullptr);
    if (!reused) return false;
    install_tlab(mutator, reused, run_offset, run_span);
    return true;
  };
  if (minimum_span > kGcTlabMinSlabBytes) {
    // A single object larger than the slab minimum needs a run of its own size;
    // the slab counter cannot answer for that, so search directly. Such
    // allocations are rare, so the walk is not on the hot path.
    if (take_run(minimum_span)) return true;
  } else if (pages_with_slab_run != 0 && take_run(kGcTlabMinSlabBytes)) {
    // A page can host a whole reservation, so take it: one refill then serves a
    // full slab instead of a handful of cells.
    return true;
  }
  GcPage* page = nullptr;
  size_t available = 0;
  GcPage* const cached = cached_allocation_page(kind);
  if (cached && cached->used <= cached->capacity) {
    available = cached->capacity - cached->used;
    if (available >= minimum_span) page = cached;
  }
  if (!page && max_indexed_tail_bytes >= minimum_span) {
    // Newest first, as the reverse scan did: the most recently carved page is the
    // one most likely to still be resident and partly free.
    for (auto candidate = tail_pages.rbegin(); candidate != tail_pages.rend();
         ++candidate) {
      GcPage* possible = *candidate;
      if (!possible || possible->kind != kind) continue;
      const size_t remaining = indexed_tail_bytes(*possible);
      if (remaining >= minimum_span) {
        page = possible;
        available = remaining;
        break;
      }
    }
    if (!page) {
      // Nothing indexed can serve this size. Record that exactly, so the next
      // refill of the same size skips the walk instead of repeating it.
      max_indexed_tail_bytes = 0;
      for (const GcPage* possible : tail_pages)
        if (possible && possible->kind == kind) {
          const size_t remaining = indexed_tail_bytes(*possible);
          if (remaining > max_indexed_tail_bytes)
            max_indexed_tail_bytes = remaining;
        }
    }
  }
  // Now reuse short reclaimed runs. This is the path that serves a heap whose
  // largest free block is a few cells; it is tried only after the committed page
  // tails, which need no index maintenance. One take would hand back a few
  // hundred bytes and make the next two or three allocations pay this lock again,
  // so the batch is reserved here and handed out by `take_spare_run` for free.
  if (!page && reserve_spare_runs_locked(mutator, minimum_span, kind) &&
      take_spare_run(mutator, minimum_span, kind)) {
    return true;
  }
  if (!page) {
    const size_t capacity = std::max(kGcPageBytes, kGcTlabBytes);
    auto new_page = acquire_page(capacity, kind);
    if (!new_page) return false;
    page = new_page.get();
    try {
      gc_pages.push_back(std::move(new_page));
    } catch (...) {
      if (new_page) release_page_backing_locked(*new_page);
      return false;
    }
    // A page larger than the directory's page size is never found by masking an
    // address, and it has no start bitmap, so its objects are tracked by the
    // exact index from the first allocation. A directory insert can succeed by
    // chunk key alone, which left such objects with no start bit and no index
    // entry: the sweep never saw them and they leaked.
    if (page->capacity > kGcPageBytes) {
      page->use_object_index = true;
      page->kind = PageKind::Large;
    } else if (!page_directory.insert(page)) {
      page->use_object_index = true;
      page->kind = PageKind::Fallback;
    }
    index_tail_page_locked(*page);
    available = page->capacity;
  }
  const size_t reserve =
      whole_object_grant(std::min(kGcTlabBytes, available), minimum_span);
  if (reserve < minimum_span) return false;
  unretire_page_locked(*page);
  install_tlab(mutator, page, page->used, reserve);
  page->used += reserve;
  note_page_tail_locked(*page);
  return true;
}

void account_tlab_allocation(MutatorState& mutator, size_t bytes) {
  NrqGcFastTls& fast = *mutator.fast;
  sync_inline_unflushed(mutator);
  // Both published counters are written only by this thread: publish_tlab_locked
  // clears them while the owner is parked (or is itself the publisher). A load
  // plus a store therefore has no lost update to guard against, and it keeps the
  // value immediately visible to a statistics query - unlike batching it, which
  // would make an in-flight allocation momentarily invisible. What it drops is
  // the locked read-modify-write, which cost more than the rest of this function.
  if (fast.pending_bytes > SIZE_MAX - bytes)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  const size_t next_pending_bytes = fast.pending_bytes + bytes;
  std::atomic_ref<size_t>(fast.pending_bytes)
      .store(next_pending_bytes, std::memory_order_relaxed);
  mutator.tlab_accounted_pending_bytes = next_pending_bytes;
  if (fast.pending_count == SIZE_MAX)
    nrq_runtime_fatal("GC thread-local allocation accounting overflow");
  std::atomic_ref<size_t>(fast.pending_count)
      .store(fast.pending_count + 1, std::memory_order_relaxed);
  // The trigger only needs batch granularity, so the trackable total behind it
  // stays owner-only until the batch closes.
  if (bytes > SIZE_MAX - fast.unflushed_bytes)
    nrq_runtime_fatal("GC thread-local batch accounting overflow");
  fast.unflushed_bytes += bytes;
  const size_t batch =
      std::atomic_ref<size_t>(nrq_gc_inline_config.accounting_batch)
          .load(std::memory_order_relaxed);
  if (fast.unflushed_bytes >= batch) {
    HeapLockGuard lock(gc_mutex);
    flush_mutator_accounting_locked(mutator);
  }
  update_tlab_end_clamp(mutator);
}

// Records that `span` bytes of header storage starting at `header_storage` were
// allocated from `page`. Consecutive allocations from one TLAB extend the same
// range; a page switch or a cursor jump starts a new one. The table is bounded,
// and a full table publishes the current batch under the collector lock instead
// of growing it, so publication still never allocates.
NrqGcPendingRange* note_pending_range(MutatorState& mutator, GcPage* page,
                                      unsigned char* header_storage, size_t span) {
  NrqGcFastTls& fast = *mutator.fast;
  auto try_extend = [&](NrqGcPendingRange* r) -> NrqGcPendingRange* {
    if (r && r->page == page && r->end == header_storage) {
      r->end += span;
      return r;
    }
    return nullptr;
  };
  if (NrqGcPendingRange* r = try_extend(fast.tlab_active_range)) return r;
  if (NrqGcPendingRange* r = try_extend(fast.ext_active_range)) return r;
  if (NrqGcPendingRange* r = try_extend(fast.h0_active_range)) return r;
  if (fast.pending_range_count != 0) {
    NrqGcPendingRange& last = fast.pending_ranges[fast.pending_range_count - 1];
    if (last.page == page && last.end == header_storage) {
      last.end += span;
      return &last;
    }
  }
  if (fast.pending_range_count == MutatorState::kPendingRangeCapacity) {
    if (collection_phase.load(std::memory_order_acquire) ==
        CollectionPhase::Idle) {
      sync_active_ranges(fast);
      try {
        mutator.deferred_ranges.insert(
            mutator.deferred_ranges.end(), fast.pending_ranges,
            fast.pending_ranges + fast.pending_range_count);
        fast.pending_range_count = 0;
        clear_tlab_fast_path(fast);
        clear_h0_fast_path(fast);
      } catch (...) {
        HeapLockGuard lock(gc_mutex);
        publish_tlab_locked(mutator);
      }
    } else {
      HeapLockGuard lock(gc_mutex);
      publish_tlab_locked(mutator);
    }
  }
  NrqGcPendingRange& range = fast.pending_ranges[fast.pending_range_count++];
  range.page = page;
  range.begin = header_storage;
  range.end = header_storage + span;
  // The first object of a window names the window's generation. Publication
  // compares this against the global flag and falls back to the exact walk when
  // the mode changed underneath it, so the flag can never disagree with the
  // objects the counters describe.
  if (fast.pending_count == 0)
    mutator.window_young = object_young(ObjectRef{header_storage, page}) ? 1 : 0;
  return &range;
}

// Thread-local bump allocation. The payload size has already been checked by the
// entry point that computed it, and `element_size` is only carried into the header,
// so the only precondition left is that this thread is a mutator with a usable
// TLAB. Keeping the checks at the boundary means the validated path and the slow
// path share one validation instead of each repeating it.
// Reserves a headerless slab for one layout. The page is dedicated to that
// layout and slot size, so the reservation is a plain bump of the page tail and
// publication can walk it without reading a header.
void install_h0_tlab(MutatorState& mutator, GcPage* page, size_t offset,
                     size_t span, const nrq_object_layout* layout,
                     size_t slot) {
  flush_window_for_page_switch(mutator, page);
  NrqGcFastTls& fast = *mutator.fast;
  clear_h0_fast_path(fast);
  fast.h0_page = page;
  fast.h0_cursor = page->storage + offset;
  fast.h0_end = fast.h0_cursor + span;
  fast.h0_state_cursor = page->h0_states.get() + (offset / slot);
  fast.h0_slot_bytes = slot;
  mutator.h0_layout = layout;
  mutator.h0_slot = slot;
  mutator.h0_generation = gc_page_generation.load(std::memory_order_acquire);
  // Same decision as the ordinary reservation: a slab taken from a page's
  // untouched tail needs no fill, and the reservation is charged to the page's
  // known-zero boundary once instead of per object.
  const bool payload_zero = offset >= page->zero_cursor;
  if (!payload_zero && span != 0) {
    const bool track_timing = tlab_diag.enabled.load(std::memory_order_relaxed);
    const auto z_start = track_timing
                             ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    std::memset(page->storage + offset, 0, span);
    if (track_timing) {
      tlab_diag.install_memset_ns.fetch_add(
          static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - z_start)
                  .count()),
          std::memory_order_relaxed);
    }
  }
  fast.h0_payload_zero = 1u;
  const size_t end = offset + span;
  if (end > page->zero_cursor) page->zero_cursor = end;
  zeroing_facts.payload_bytes += span;
  if (payload_zero)
    zeroing_facts.prezeroed_bytes += span;
  else
    zeroing_facts.filled_bytes += span;
}

// Pops a reserved headerless run. The list is thread-owned and the runs were
// removed from their pages under the lock already, so this needs no lock.
bool take_h0_spare_run(MutatorState& mutator, const nrq_object_layout* layout,
                       size_t slot) {
  size_t index = mutator.spare_run_count;
  while (index != 0) {
    --index;
    const MutatorState::SpareRun spare = mutator.spare_runs[index];
    if (!spare.page || spare.page->kind != PageKind::Headerless) continue;
    if (spare.page->uniform_layout != layout || spare.page->slot_bytes != slot)
      continue;
    if (spare.span < slot) continue;
    mutator.spare_runs[index] = mutator.spare_runs[--mutator.spare_run_count];
    install_h0_tlab(mutator, spare.page, spare.offset, spare.span, layout, slot);
    return true;
  }
  return false;
}

bool reserve_h0_tlab_locked(MutatorState& mutator,
                            const nrq_object_layout* layout, size_t slot) {
  // A headerless reservation is a refill like any other; the counter is the
  // measure of how often the allocator lock was taken.
  tlab_refill_count = saturated_add(tlab_refill_count, 1);
  // A fragmented page offers few-cell runs. Taking them one refill at a time
  // would put the allocator lock back on the allocation path, so a batch of them
  // is reserved and handed out from the thread list, exactly as the other
  // representations do.
  constexpr size_t byte_budget = kGcTlabBytes * 2;
  // Same bound as the compact batch: a headerless refill can also arrive while the
  // list still holds runs of another shape, and taking more than fits would both
  // overrun the list and strand the runs it could not store.
  const size_t room =
      mutator.spare_run_count < MutatorState::kSpareRunCapacity
          ? MutatorState::kSpareRunCapacity - mutator.spare_run_count
          : 0;
  if (room == 0) return false;
  GcPage::FreeSpan runs[MutatorState::kSpareRunCapacity];
  GcPage* run_page = nullptr;
  const size_t taken = take_free_runs_locked(
      slot, byte_budget, room, slot, run_page, runs, PageKind::Headerless,
      layout);
  NrqGcFastTls& fast = *mutator.fast;
  auto release_h0_tail = [&]() {
    GcPage* const h0_page = static_cast<GcPage*>(fast.h0_page);
    if (h0_page && fast.h0_cursor && fast.h0_end > fast.h0_cursor) {
      const size_t offset = static_cast<size_t>(fast.h0_cursor - h0_page->storage);
      const size_t span = static_cast<size_t>(fast.h0_end - fast.h0_cursor);
      mutator.tlab_tail_reclaimed_bytes =
          saturated_add(mutator.tlab_tail_reclaimed_bytes, span);
      release_span_locked(*h0_page, offset, span);
    }
  };
  if (taken != 0) {
    for (size_t index = 0; index < taken; ++index) {
      if (mutator.spare_run_count == MutatorState::kSpareRunCapacity) break;
      MutatorState::SpareRun& spare =
          mutator.spare_runs[mutator.spare_run_count++];
      spare.page = run_page;
      spare.offset = runs[index].offset;
      spare.span = runs[index].span;
    }
    mutator.spare_generation =
        gc_page_generation.load(std::memory_order_acquire);
    release_h0_tail();
    return take_h0_spare_run(mutator, layout, slot);
  }
  GcPage* page = find_headerless_page_locked(layout, slot);
  if (!page) page = create_headerless_page_locked(layout, slot);
  if (!page) return false;
  unretire_page_locked(*page);
  const size_t available = page->capacity - page->used;
  const size_t reserve = whole_object_grant(std::min(kGcTlabBytes, available), slot);
  if (reserve < slot) return false;
  release_h0_tail();
  install_h0_tlab(mutator, page, page->used, reserve, layout, slot);
  page->used += reserve;
  note_page_tail_locked(*page);
  return true;
}

void* allocate_tlab(size_t bytes, size_t element_size, size_t elements,
                    nrq_gc_trace_fn trace, const size_t* reference_offsets,
                    size_t reference_count, bool& handled,
                    const nrq_object_layout* resolved,
                    GcRepresentation hint, bool repeated = false) {
  handled = false;
  if (thread_mutator.depth == 0 || !thread_mutator.fast) return nullptr;
  NrqGcFastTls& fast = *thread_mutator.fast;
  const nrq_object_layout* layout =
      resolved ? resolved
               : layout_for_allocation(bytes, element_size, trace,
                                       reference_offsets, reference_count,
                                       repeated);
  if (!layout) return nullptr;
  bool valid = true;
  const GcRepresentation representation = select_representation(
      layout->payload_size, elements, layout, hint, valid);
  if (!valid) {
    handled = true;
    return nullptr;
  }
  const bool young =
      generational_collection_enabled.load(std::memory_order_relaxed);

  GcRepresentation actual = representation;
  if (actual == GcRepresentation::Headerless) {
    const size_t slot = object_span_bytes(0, layout->payload_size);
    if (thread_mutator.h0_generation != gc_page_generation.load(std::memory_order_acquire)) {
      fast.h0_page = nullptr;
      clear_h0_fast_path(fast);
    }
    const bool same_shape = fast.h0_cursor &&
                            thread_mutator.h0_layout == layout &&
                            thread_mutator.h0_slot == slot;
    const bool has_room =
        same_shape && static_cast<size_t>(fast.h0_end - fast.h0_cursor) >= slot;
    const bool slab_active =
        fast.h0_cursor && fast.h0_cursor < fast.h0_end;
    if (!has_room && slab_active) {
      // Another shape owns this thread's headerless slab. A uniform page holds
      // one layout, so serving this object here would evict a slab that is still
      // half full - the reservation thrash a mixed-size workload would pay on
      // every alternating allocation. Use the compact representation instead;
      // the shape that owns the slab keeps its headerless objects.
      actual = GcRepresentation::Compact;
    }
  }

  if (actual == GcRepresentation::Headerless) {
    handled = true;
    const size_t slot = object_span_bytes(0, layout->payload_size);
    if (!fast.h0_cursor ||
        thread_mutator.h0_layout != layout ||
        thread_mutator.h0_slot != slot ||
        static_cast<size_t>(fast.h0_end - fast.h0_cursor) < slot) {
      // A batch reserved earlier answers this without the allocator lock.
      if (!take_h0_spare_run(thread_mutator, layout, slot)) {
        HeapLockGuard lock(gc_mutex);
        if (!reserve_h0_tlab_locked(thread_mutator, layout, slot)) return nullptr;
      }
    }
    GcPage* const h0_page = static_cast<GcPage*>(fast.h0_page);
    const ObjectRef object{fast.h0_cursor, h0_page};
    sync_active_ranges(fast);
    fast.h0_cursor += slot;
    ++fast.h0_state_cursor;
    initialize_object(object, layout, 1, layout->payload_size, young,
                      fast.h0_payload_zero != 0);
    object.page->uncommitted_objects.fetch_add(1, std::memory_order_relaxed);
    fast.h0_active_range =
        note_pending_range(thread_mutator, object.page, object.header, slot);
    fast.h0_fast_layout = layout;
    account_tlab_allocation(thread_mutator, layout->payload_size);
    if (collection_phase.load(std::memory_order_acquire) !=
        CollectionPhase::Idle) {
      HeapLockGuard lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
    }
    debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, layout->payload_size,
               object_payload(object));
    return object_payload(object);
  }

  const size_t header_bytes = actual == GcRepresentation::Compact ? 8 : 16;
  const PageKind kind = actual == GcRepresentation::Compact ? PageKind::Compact
                                                            : PageKind::Extended;
  const size_t span = object_span_bytes(header_bytes, bytes);
  if (span > kGcTlabBytes) return nullptr;
  handled = true;

  if (thread_mutator.tlab_generation != gc_page_generation.load(std::memory_order_acquire))
    discard_stale_tlab(thread_mutator);
  sync_inline_unflushed(thread_mutator);
  {
    const size_t batch =
        std::atomic_ref<size_t>(nrq_gc_inline_config.accounting_batch)
            .load(std::memory_order_relaxed);
    if (fast.unflushed_bytes >= batch) {
      HeapLockGuard lock(gc_mutex);
      flush_mutator_accounting_locked(thread_mutator);
    }
  }
  const bool is_ext = kind == PageKind::Extended;
  uint8_t*& cursor_ref = is_ext ? fast.ext_cursor : fast.tlab_cursor;
  uint8_t*& end_ref = is_ext ? fast.ext_end : fast.tlab_end;
  void*& page_ref = is_ext ? fast.ext_page : fast.tlab_page;
  uint8_t*& slab_end_ref =
      is_ext ? thread_mutator.ext_slab_end : thread_mutator.tlab_slab_end;
  NrqGcPendingRange*& active_ref =
      is_ext ? fast.ext_active_range : fast.tlab_active_range;

  GcPage* tlab_page = static_cast<GcPage*>(page_ref);
  if (cursor_ref && tlab_page && tlab_page->kind == kind &&
      slab_end_ref && slab_end_ref > cursor_ref &&
      static_cast<size_t>(slab_end_ref - cursor_ref) >= span) {
    // Physical slab still has room; only the clamped accounting window expired
    // or was shrunk on batch publication.
    clamp_tlab_window(cursor_ref, end_ref, slab_end_ref, fast.unflushed_bytes);
  } else if (!cursor_ref || !end_ref || !tlab_page ||
             tlab_page->kind != kind || cursor_ref > end_ref ||
             span > static_cast<size_t>(end_ref - cursor_ref)) {
    if (take_spare_run(thread_mutator, span, kind)) {
      if (tlab_diag.enabled.load(std::memory_order_relaxed))
        tlab_diag.spare_run_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
      HeapLockGuard lock(gc_mutex);
      if (!reserve_tlab_locked(thread_mutator, span, kind)) return nullptr;
    }
    tlab_page = static_cast<GcPage*>(page_ref);
  }

  const ObjectRef object{cursor_ref, tlab_page};
  sync_active_ranges(fast);
  cursor_ref += span;
  initialize_object(object, layout, static_cast<uint32_t>(elements), bytes,
                    young, fast.tlab_payload_zero != 0);
  // The object exists but is not published yet: charge it to the page until the
  // range that names it is published, so the sweep cannot retire a page that is
  // still being filled.
  object.page->uncommitted_objects.fetch_add(1, std::memory_order_relaxed);
  NrqGcPendingRange* const active_range =
      note_pending_range(thread_mutator, object.page, object.header, span);
  if (page_is_representable(object.page->kind) &&
      (object.page->kind == PageKind::Compact ||
       object.page->kind == PageKind::Extended)) {
    active_ref = active_range;
    if (!is_ext) fast.tlab_kind = NRQ_GC_INLINE_KIND_COMPACT;
  }
  account_tlab_allocation(thread_mutator, bytes);
  clamp_tlab_window(cursor_ref, end_ref, slab_end_ref, fast.unflushed_bytes);

  // During a cycle an unpublished young object is invisible to the marker and
  // to the root barrier. Publish it immediately so a barrier can identify it.
  if (collection_phase.load(std::memory_order_acquire) !=
      CollectionPhase::Idle) {
    HeapLockGuard lock(gc_mutex);
    publish_tlab_locked(thread_mutator);
  }
  debug_emit(NRQ_GC_EVENT_ALLOCATION, 0, bytes, object_payload(object));
  return object_payload(object);
}

void store_root_value_locked(void** slot, void* value) {
  const CollectionPhase phase =
      collection_phase.load(std::memory_order_relaxed);
  if (phase != CollectionPhase::Idle) {
    // Snapshot-at-the-beginning: retain the old referent as well as the new
    // one while marking. Objects reachable at the snapshot are already marked
    // once sweeping starts, so only newly installed referents need marking.
    if (phase == CollectionPhase::Marking) mark_value_locked(*slot);
    mark_value_locked(value);
  }
  *slot = value;
  defer_remembered_locked(value);
}

void sync_shadow_tls(ShadowStack& stack) {
  if (!stack.fast) return;
  const size_t root_count = shadow_root_count(stack);
  const size_t frame_count = shadow_frame_count(stack);
  // Note: callers using ensure_shadow_stack() initialize cursors once; growth
  // updates them directly.
  (void)root_count;
  (void)frame_count;
}

bool grow_shadow_roots(ShadowStack& stack, size_t extra) {
  if (!stack.fast) return false;
  const size_t current = shadow_root_count(stack);
  if (extra > SIZE_MAX - current) return false;
  const size_t needed = current + extra;
  size_t next_cap = stack.roots_buffer.empty() ? 256 : stack.roots_buffer.size();
  while (next_cap < needed) {
    if (next_cap > SIZE_MAX / 2) return false;
    next_cap *= 2;
  }
  try {
    std::vector<NrqGcFastRoot> replacement(next_cap);
    NrqGcFastRoot* old_base = stack.roots_buffer.data();
    NrqGcFastRoot* new_base = replacement.data();
    if (current != 0)
      std::memcpy(new_base, old_base, current * sizeof(NrqGcFastRoot));
    // Rebase active frame prev_root_cursor pointers into the new buffer.
    NrqGcFastFrame* f_begin = stack.frames_buffer.data();
    NrqGcFastFrame* f_end = stack.fast->frame_cursor;
    for (NrqGcFastFrame* f = f_begin; f < f_end; ++f) {
      const size_t idx =
          static_cast<size_t>(f->prev_root_cursor - old_base);
      f->prev_root_cursor = new_base + idx;
    }
    stack.roots_buffer.swap(replacement);
    stack.fast->root_cursor = new_base + current;
    stack.fast->root_end = new_base + next_cap;
    return true;
  } catch (...) {
    return false;
  }
}

bool grow_shadow_frames(ShadowStack& stack, size_t extra) {
  if (!stack.fast) return false;
  const size_t current = shadow_frame_count(stack);
  if (extra > SIZE_MAX - current) return false;
  const size_t needed = current + extra;
  size_t next_cap =
      stack.frames_buffer.empty() ? 128 : stack.frames_buffer.size();
  while (next_cap < needed) {
    if (next_cap > SIZE_MAX / 2) return false;
    next_cap *= 2;
  }
  try {
    std::vector<NrqGcFastFrame> replacement(next_cap);
    if (current != 0) {
      std::memcpy(replacement.data(), stack.frames_buffer.data(),
                  current * sizeof(NrqGcFastFrame));
    }
    stack.frames_buffer.swap(replacement);
    stack.fast->frame_cursor = stack.frames_buffer.data() + current;
    stack.fast->frame_end = stack.frames_buffer.data() + next_cap;
    return true;
  } catch (...) {
    return false;
  }
}

inline void ensure_shadow_capacity(ShadowStack& stack, size_t extra_roots,
                                   size_t extra_frames) {
  NrqGcFastTls& fast = *stack.fast;
  if (extra_roots != 0 &&
      static_cast<size_t>(fast.root_end - fast.root_cursor) < extra_roots) {
    if (!grow_shadow_roots(stack, extra_roots))
      nrq_runtime_fatal("GC shadow-stack root buffer growth failed");
  }
  if (extra_frames != 0 &&
      static_cast<size_t>(fast.frame_end - fast.frame_cursor) < extra_frames) {
    if (!grow_shadow_frames(stack, extra_frames))
      nrq_runtime_fatal("GC shadow-stack frame buffer growth failed");
  }
}

ShadowStack& ensure_shadow_stack() {
  if (thread_shadow_stack) return *thread_shadow_stack;
  thread_gc_cleanup.arm();
  auto* stack = new (std::nothrow) ShadowStack();
  if (!stack) nrq_runtime_fatal("GC shadow-stack allocation failed");
  try {
    stack->roots_buffer.resize(256);
    stack->frames_buffer.resize(128);
  } catch (...) {
    delete stack;
    nrq_runtime_fatal("GC shadow-stack allocation failed");
  }
  stack->fast = &nrq_gc_fast_tls;
  nrq_gc_fast_tls.root_cursor = stack->roots_buffer.data();
  nrq_gc_fast_tls.root_end =
      stack->roots_buffer.data() + stack->roots_buffer.size();
  nrq_gc_fast_tls.frame_cursor = stack->frames_buffer.data();
  nrq_gc_fast_tls.frame_end =
      stack->frames_buffer.data() + stack->frames_buffer.size();
  {
    HeapLockGuard lock(gc_mutex);
    try {
      shadow_stacks.push_back(stack);
    } catch (...) {
      nrq_gc_fast_tls.root_cursor = nullptr;
      nrq_gc_fast_tls.root_end = nullptr;
      nrq_gc_fast_tls.frame_cursor = nullptr;
      nrq_gc_fast_tls.frame_end = nullptr;
      delete stack;
      nrq_runtime_fatal("GC shadow-stack registration failed");
    }
  }
  thread_shadow_stack = stack;
  return *stack;
}

inline bool is_pointer_root_layout(const nrq_object_layout* layout) {
  if (layout == &kPointerRootLayout) return true;
  return layout && layout->payload_size == sizeof(void*) &&
         layout->reference_count == 1 && layout->reference_offsets &&
         layout->reference_offsets[0] == 0;
}

bool is_pointer_root(const RootInfo& root) {
  return root.repeated_count == 0 && is_pointer_root_layout(root.layout);
}

// The descriptor for a root registration: the same interning path objects use.
// A repeated array root is described by one element, so the stride and the
// offset bounds are the element's; a single record is described by its size.
// A descriptor the compiler emits is read-only data, so the production
// configuration trusts it: no validation runs on an allocation path. The
// development configuration proves every field, because a table that disagrees
// with its payload would corrupt tracing rather than fail an allocation.
//
// It proves a caller-supplied descriptor on every call. That descriptor's
// address is not a stable identity to cache: the contract asks for storage
// that outlives the objects, but an address can be reused by a different
// descriptor once the first one is gone, and a pointer key would then skip the
// check that exists to catch exactly that. Only the interned pool has
// addresses whose identity the runtime owns.
bool layout_is_consistent(const nrq_object_layout& layout) {
  if (layout.payload_size == 0) return false;
  if ((layout.flags & ~static_cast<uint32_t>(NRQ_OBJECT_LAYOUT_ARRAY |
                                              NRQ_OBJECT_LAYOUT_INLINE_MAP)) != 0)
    return false;
  if (layout.reference_count == 0) return true;
  if (!layout.reference_offsets) return false;
  const bool inline_map =
      (layout.flags & NRQ_OBJECT_LAYOUT_INLINE_MAP) != 0;
  if (inline_map &&
      (layout.reference_count > 2 ||
       (layout.flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0))
    return false;
  for (uint32_t index = 0; index < layout.reference_count; ++index) {
    const size_t offset = layout.reference_offsets[index];
    if (offset > layout.payload_size ||
        sizeof(void*) > layout.payload_size - offset)
      return false;
    if (inline_map && layout.inline_reference_offsets[index] != offset)
      return false;
  }
  return true;
}

const nrq_object_layout* root_layout(size_t object_size, size_t element_size,
                                     size_t repeated_count,
                                     const size_t* reference_offsets,
                                     size_t reference_count) {
  // Exactly one pointer word, one reference at offset zero, no repeated
  // elements and no trace callback: this is the shape the static descriptor
  // describes, so answer before touching the memo. Everything else keeps the
  // existing memo and interning path.
  if (repeated_count == 0 && object_size == sizeof(void*) &&
      reference_count == 1 && reference_offsets &&
      reference_offsets[0] == 0) {
    if constexpr (nirq::runtime_debug_enabled) {
      if (!layout_is_consistent(kPointerRootLayout))
        nrq_runtime_fatal("static pointer-root layout is inconsistent");
    }
    return &kPointerRootLayout;
  }
  // Registration runs once per root per frame entry, so it uses the same
  // thread-local memo as allocation: a slot with the layout this thread saw last
  // resolves without the interning lock, and only a genuinely new shape pays for
  // the table. The compiler's tables are module constants, so the memo compares
  // them by pointer.
  if (repeated_count != 0)
    return layout_for_allocation(object_size, element_size, nullptr,
                                 reference_offsets, reference_count, true);
  return layout_for_allocation(object_size, object_size, nullptr,
                               reference_offsets, reference_count, false);
}

void add_slot_state(ShadowStack& stack, void* slot,
                    const nrq_object_layout* layout, bool pointer_root) {
  SlotState* state = stack.slot_states.insert(slot);
  if (!state)
    nrq_runtime_fatal("GC root slot index failed to grow");
  if (state->count == 0) {
    state->count = 1;
    state->layout = layout;
    state->pointer_root = pointer_root;
  } else {
    // Descriptor identity is layout identity (payload_size is part of the
    // interning key), so agreement is one compare.
    if (state->layout != layout || state->pointer_root != pointer_root)
      nrq_runtime_fatal(
          "GC root slot was registered with an incompatible layout");
    if (state->count == UINT32_MAX)
      nrq_runtime_fatal("GC root slot registration count overflow");
    ++state->count;
  }
  // A slot that is a pointer root stays one while it is registered, so the
  // allocation path can answer its check from here instead of the index.
  if (pointer_root) stack.pointer_roots.insert(slot);
}

void remove_slot_state(ShadowStack& stack, void* slot, uint32_t count = 1) {
  bool fully_released = false;
  if (!stack.slot_states.release(slot, count, &fully_released))
    nrq_runtime_fatal("GC root slot registration bookkeeping is inconsistent");
  // The last registration of a slot takes its pointer-root memo with it, so a
  // later registration of the same address with another layout is re-validated.
  if (fully_released) stack.pointer_roots.forget(slot);
}

// Frame ends usually release a handful of roots, and a frame can name the same
// slot more than once. Counting them first means each distinct slot probes the
// index once for the whole frame. Frames larger than this batch fall back to one
// release per registration, which is still a single probe chain each.
constexpr size_t kMaxBatchedSlotRemovals = 32;

struct PendingSlotRemoval {
  void* slot;
  uint32_t count;
};

void remove_frame_slots(ShadowStack& stack, NrqGcFastRoot* first,
                        NrqGcFastRoot* end) {
  if (first >= end) return;
  if (stack.slot_states.size() != 0) {
    const size_t total = static_cast<size_t>(end - first);
    if (total == 1) {
      if (shadow_layout_is_indexed(first->layout))
        remove_slot_state(stack, first->slot, 1);
    } else {
      PendingSlotRemoval batch[kMaxBatchedSlotRemovals];
      size_t batch_size = 0;
      bool overflowed = false;
      for (const NrqGcFastRoot* r = first; r < end; ++r) {
        if (!shadow_layout_is_indexed(r->layout)) continue;
        void* slot = r->slot;
        size_t position = 0;
        while (position < batch_size && batch[position].slot != slot) ++position;
        if (position == batch_size) {
          if (batch_size == kMaxBatchedSlotRemovals) {
            overflowed = true;
            break;
          }
          batch[batch_size].slot = slot;
          batch[batch_size].count = 0;
          ++batch_size;
        }
        ++batch[position].count;
      }
      if (overflowed) {
        for (const NrqGcFastRoot* r = first; r < end; ++r) {
          if (shadow_layout_is_indexed(r->layout))
            remove_slot_state(stack, r->slot);
        }
      } else {
        for (size_t position = 0; position < batch_size; ++position)
          remove_slot_state(stack, batch[position].slot, batch[position].count);
      }
    }
  }
  if (stack.pointer_roots_has_unindexed) {
    for (const NrqGcFastRoot* r = first; r < end; ++r)
      stack.pointer_roots.forget(r->slot);
    if (first == stack.roots_buffer.data())
      stack.pointer_roots_has_unindexed = false;
  }
}

__forceinline bool shadow_has_pointer_root(ShadowStack& stack, void* slot) {
  // The memo answers for the slots a loop reuses; anything else checks the
  // dynamic slot index and then active frame roots from top down.
  if (stack.pointer_roots.contains(slot)) return true;
  const SlotState* found = stack.slot_states.find(slot);
  if (found != nullptr && found->pointer_root) {
    stack.pointer_roots.insert(slot);
    return true;
  }
  if (stack.fast) {
    if (stack.fast->root_cursor) {
      const NrqGcFastRoot* begin = stack.roots_buffer.data();
      for (const NrqGcFastRoot* r = stack.fast->root_cursor; r > begin;) {
        --r;
        if (r->slot != slot) continue;
        const nrq_object_layout* layout = decode_shadow_layout(r->layout);
        if (is_pointer_root_layout(layout)) {
          stack.pointer_roots.insert(slot);
          stack.pointer_roots_has_unindexed = true;
          return true;
        }
        return false;
      }
    }
    for (const NrqGcStackFrame* frame =
             std::atomic_ref<NrqGcStackFrame* const>(stack.fast->stack_frame_top)
                 .load(std::memory_order_acquire);
         frame != nullptr; frame = frame->prev) {
      void* const* slots = reinterpret_cast<void* const*>(frame + 1);
      if (slot >= slots && slot < slots + frame->slot_count) {
        stack.pointer_roots.insert(slot);
        stack.pointer_roots_has_unindexed = true;
        return true;
      }
    }
  }
  return false;
}

ObjectRef find_pending_header_in_range(const NrqGcPendingRange& range,
                                         void* object) {
  if (!range.page || !range.begin) return ObjectRef{};
  const auto* obj_bytes = static_cast<const unsigned char*>(object);
  if (obj_bytes < range.begin || obj_bytes >= range.end) return ObjectRef{};
  GcPage* const page = static_cast<GcPage*>(range.page);
  const size_t header_bytes = page_header_bytes(page->kind);
  for (unsigned char* cursor = range.begin; cursor < range.end;) {
    const ObjectRef candidate{cursor, page};
    if (object_payload(candidate) == object) return candidate;
    const size_t span =
        page->kind == PageKind::Headerless
            ? page->slot_bytes
            : object_span_bytes(header_bytes,
                                object_payload_bytes(candidate));
    if (span == 0 || cursor + span > range.end) break;
    cursor += span;
  }
  return ObjectRef{};
}

ObjectRef find_pending_header(MutatorState& mutator, void* object) {
  if (!mutator.fast || !object) return ObjectRef{};
  NrqGcFastTls& fast = *mutator.fast;
  sync_active_ranges(fast);
  const uint32_t range_count = fast.pending_range_count;
  for (uint32_t index = 0; index < range_count; ++index) {
    const ObjectRef found =
        find_pending_header_in_range(fast.pending_ranges[index], object);
    if (found.valid()) return found;
  }
  for (const NrqGcPendingRange& range : mutator.deferred_ranges) {
    const ObjectRef found = find_pending_header_in_range(range, object);
    if (found.valid()) return found;
  }
  return ObjectRef{};
}

void print_tlab_diag_report();

}  // namespace

extern "C" {

void* nrq_gc_allocate(size_t bytes, nrq_gc_trace_fn trace) {
  if (bytes == 0 || bytes > SIZE_MAX - 16) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, bytes, 1, trace, nullptr, 0, handled,
                               nullptr, GcRepresentation::Auto);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, bytes, 1, trace, nullptr, 0, nullptr,
                         GcRepresentation::Auto);
}

void* nrq_gc_allocate_repr(size_t bytes, nrq_gc_trace_fn trace,
                           int32_t representation) {
  GcRepresentation hint = GcRepresentation::Auto;
  switch (representation) {
    case NRQ_GC_REPR_AUTO: hint = GcRepresentation::Auto; break;
    case NRQ_GC_REPR_HEADERLESS: hint = GcRepresentation::Headerless; break;
    case NRQ_GC_REPR_COMPACT: hint = GcRepresentation::Compact; break;
    case NRQ_GC_REPR_EXTENDED: hint = GcRepresentation::Extended; break;
    default: return nullptr;
  }
  if (bytes == 0 || bytes > SIZE_MAX - 16) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, bytes, 1, trace, nullptr, 0, handled,
                               nullptr, hint);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, bytes, 1, trace, nullptr, 0, nullptr, hint);
}

int32_t nrq_gc_object_representation(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectRef ref = find_object_locked(object);
  if (!ref.valid()) return NRQ_GC_REPR_AUTO;
  switch (ref.page->kind) {
    case PageKind::Headerless: return NRQ_GC_REPR_HEADERLESS;
    case PageKind::Compact: return NRQ_GC_REPR_COMPACT;
    default: return NRQ_GC_REPR_EXTENDED;
  }
}

size_t nrq_gc_object_header_size(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectRef ref = find_object_locked(object);
  return ref.valid() ? object_header_bytes(ref) : 0;
}

nrq_gc_representation_facts nrq_gc_get_representation_facts(void) {
  nrq_gc_representation_facts facts{};
  facts.headerless_header_bytes = page_header_bytes(PageKind::Headerless);
  facts.compact_header_bytes = page_header_bytes(PageKind::Compact);
  facts.extended_header_bytes = page_header_bytes(PageKind::Extended);
  facts.max_header_bytes = facts.extended_header_bytes;
  facts.headerless_payload_alignment = kObjectStartAlignment;
  // A compact payload begins after the 8-byte header, so it keeps min(8, grid).
  facts.compact_payload_alignment =
      std::min<size_t>(8, kObjectStartAlignment);
  facts.extended_payload_alignment = kObjectStartAlignment;
  facts.headerless_state_bytes_per_object = 1;
  return facts;
}

nrq_gc_representation_stats nrq_gc_get_representation_stats(void) {
  HeapLockGuard lock(gc_mutex);
  nrq_gc_representation_stats stats{};
  for (const auto& entry : gc_pages) {
    const GcPage* page = entry.get();
    if (!page) continue;
    // What this page actually spends off-heap. `h0_state_capacity` is zero for
    // every page that is not headerless, so one expression covers all three.
    const size_t metadata_bytes =
        (page->mark_words + page->remembered_words + page->born_words +
         page->object_start_words) *
            sizeof(uint64_t) +
        page->h0_state_capacity;
    switch (page->kind) {
      case PageKind::Headerless:
        ++stats.headerless_pages;
        stats.headerless_objects += page->live_objects;
        stats.headerless_metadata_bytes += metadata_bytes;
        break;
      case PageKind::Compact:
        ++stats.compact_pages;
        stats.compact_objects += page->live_objects;
        stats.compact_metadata_bytes += metadata_bytes;
        break;
      case PageKind::Extended:
        ++stats.extended_pages;
        stats.extended_objects += page->live_objects;
        stats.extended_metadata_bytes += metadata_bytes;
        break;
      case PageKind::Large:
        ++stats.large_pages;
        stats.extended_objects += page->live_objects;
        stats.extended_metadata_bytes += metadata_bytes;
        break;
      case PageKind::Fallback:
        ++stats.fallback_pages;
        stats.extended_objects += page->live_objects;
        stats.extended_metadata_bytes += metadata_bytes;
        break;
    }
  }
  // What the same live set would have spent on extended headers instead.
  const uint64_t extended = page_header_bytes(PageKind::Extended);
  stats.header_bytes_saved_vs_extended =
      stats.headerless_objects * extended +
      stats.compact_objects *
          (extended - page_header_bytes(PageKind::Compact));
  return stats;
}

void* nrq_gc_allocate_array(size_t count, size_t element_size,
                            nrq_gc_trace_fn trace) {
  if (count == 0 || element_size == 0) return nullptr;
  // The instance records its element count in 32 bits, so a longer array is
  // rejected rather than truncated.
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, element_size, count, trace, nullptr, 0,
                               handled, nullptr, GcRepresentation::Auto, true);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, element_size, count, trace, nullptr, 0, nullptr,
                         GcRepresentation::Auto, true);
}

void* nrq_gc_allocate_array_typed(size_t count, size_t element_size,
                                  const size_t* reference_offsets,
                                  size_t reference_count) {
  if (count == 0 || element_size == 0) return nullptr;
  // The instance records its element count in 32 bits, so a longer array is
  // rejected rather than truncated.
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, element_size, count, nullptr,
                               reference_offsets, reference_count, handled,
                               nullptr, GcRepresentation::Auto, true);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, element_size, count, nullptr,
                         reference_offsets, reference_count, nullptr,
                         GcRepresentation::Auto, true);
}

void* allocate_array_rooted_impl(void** root_slot, size_t count,
                                 size_t element_size,
                                 const size_t* reference_offsets,
                                 size_t reference_count,
                                 const nrq_object_layout* resolved,
                                 GcRepresentation hint =
                                     GcRepresentation::Auto);


void* nrq_gc_allocate_layout(const nrq_object_layout* layout) {
  if (!layout || (layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) != 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  const size_t bytes = layout->payload_size;
  if (bytes == 0 || bytes > SIZE_MAX - 16) return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, bytes, 1, layout->trace, nullptr, 0,
                               handled, layout, GcRepresentation::Auto);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, bytes, 1, layout->trace, nullptr, 0, layout,
                         GcRepresentation::Auto);
}

void* nrq_gc_allocate_array_layout(const nrq_object_layout* layout,
                                   size_t count) {
  if (!layout || count == 0) return nullptr;
  if ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, layout->payload_size, bytes))
    return nullptr;
  maybe_collect_automatically();
  bool handled = false;
  void* object = allocate_tlab(bytes, layout->payload_size, count,
                               layout->trace, nullptr, 0, handled, layout,
                               GcRepresentation::Auto);
  if (handled) return object;
  HeapLockGuard lock(gc_mutex);
  return allocate_locked(bytes, layout->payload_size, count, layout->trace,
                         nullptr, 0, layout, GcRepresentation::Auto);
}

void* nrq_gc_allocate_array_layout_rooted(void** root_slot,
                                          const nrq_object_layout* layout,
                                          size_t count) {
  if (!root_slot || !layout || count == 0) return nullptr;
  if ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) return nullptr;
  if constexpr (nirq::runtime_debug_enabled)
    if (!layout_is_consistent(*layout)) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  return allocate_array_rooted_impl(root_slot, count, layout->payload_size,
                                    nullptr, 0, layout);
}
// Rooted allocation shared by the offset-table and the descriptor entry points:
// the root slot is published inside the same barrier region as the allocation,
// so a collection can never see the object before the root names it.
void* allocate_array_rooted_impl(void** root_slot, size_t count,
                                 size_t element_size,
                                 const size_t* reference_offsets,
                                 size_t reference_count,
                                 const nrq_object_layout* resolved,
                                 GcRepresentation hint) {
  if (!root_slot || count == 0 || element_size == 0) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  size_t bytes = 0;
  if (multiply_size_overflowed(count, element_size, bytes)) return nullptr;
  maybe_collect_automatically();
  if (thread_mutator.depth != 0) {
    ShadowStack& stack = ensure_shadow_stack();
    // O(1) root slot validation through the per-thread slot index.
    if (!shadow_has_pointer_root(stack, root_slot))
      nrq_runtime_fatal(
          "rooted GC allocation requires a registered pointer root slot");
    // The reader count spans allocation and root publication. The collector
    // cannot take its root snapshot in that window, so the object becomes
    // reachable before any snapshot can decide it is garbage.
    if (begin_fast_barrier()) {
      bool handled = false;
      void* object = allocate_tlab(bytes, element_size, count, nullptr,
                                   reference_offsets, reference_count, handled,
                                   resolved, hint, true);
      if (handled) {
        if (tlab_diag.enabled.load(std::memory_order_relaxed))
          tlab_diag.slow_tlab_handled.fetch_add(1, std::memory_order_relaxed);
        *root_slot = object;
        end_fast_barrier();
        return object;
      }
      end_fast_barrier();
    }
  }

  HeapLockGuard lock(gc_mutex);
  if (tlab_diag.enabled.load(std::memory_order_relaxed))
    tlab_diag.slow_locked_fallbacks.fetch_add(1, std::memory_order_relaxed);
  if (thread_shadow_stack &&
      !shadow_has_pointer_root(*thread_shadow_stack, root_slot))
    nrq_runtime_fatal(
        "rooted GC allocation requires a registered pointer root slot");
  void* object = allocate_locked(bytes, element_size, count, nullptr,
                                 reference_offsets, reference_count, resolved,
                                 hint, true);
  store_root_value_locked(root_slot, object);
  return object;
}

void* nrq_gc_allocate_planned(void** root_slot, size_t bytes,
                              const size_t* reference_offsets,
                              size_t reference_count, void** site_cache,
                              uint32_t plan_flags) {
  if (!root_slot || bytes == 0 || bytes > SIZE_MAX - 16) return nullptr;
  // A plan that names deterministic storage describes a site the collector does
  // not own. Honouring it here would let the allocator choose the program's
  // lifetime strategy, so the entry point declines instead and the compiler's
  // fallback decides what happens to that site.
  if ((plan_flags & NRQ_ALLOC_PLAN_DETERMINISTIC) != 0) return nullptr;
  // A record plan names H0, H8 or H16. `NONE` means deterministic storage, which
  // is not this entry point's job, and an array plan is for an array.
  const uint32_t representation = NRQ_ALLOC_PLAN_REPR_OF(plan_flags);
  GcRepresentation hint = GcRepresentation::Auto;
  if ((plan_flags & (NRQ_ALLOC_PLAN_REJECTED | NRQ_ALLOC_PLAN_ARRAY)) == 0) {
    switch (representation) {
      case NRQ_ALLOC_REPR_HEADERLESS: hint = GcRepresentation::Headerless; break;
      case NRQ_ALLOC_REPR_COMPACT: hint = GcRepresentation::Compact; break;
      case NRQ_ALLOC_REPR_EXTENDED: hint = GcRepresentation::Extended; break;
      default: hint = GcRepresentation::Auto; break;
    }
  }
  const nrq_object_layout* layout = nullptr;
  if (site_cache) layout = static_cast<const nrq_object_layout*>(*site_cache);
  if (layout &&
      !layout_memo_matches(layout, bytes, bytes, nullptr, reference_offsets,
                           reference_count, false))
    layout = nullptr;
  if (!layout) {
    layout = layout_for_allocation(bytes, bytes, nullptr, reference_offsets,
                                   reference_count, false);
    if (!layout) return nullptr;
    if (site_cache) *site_cache = const_cast<nrq_object_layout*>(layout);
  }
  return allocate_array_rooted_impl(root_slot, 1, bytes, reference_offsets,
                                    reference_count, layout, hint);
}

void* nrq_gc_allocate_array_typed_rooted(
    void** root_slot, size_t count, size_t element_size,
    const size_t* reference_offsets, size_t reference_count) {
  return allocate_array_rooted_impl(root_slot, count, element_size,
                                    reference_offsets, reference_count,
                                    nullptr);
}

void* nrq_gc_allocate_array_planned(void** root_slot, size_t count,
                                   size_t element_size,
                                   const size_t* reference_offsets,
                                   size_t reference_count, void** site_cache,
                                   uint32_t plan_flags) {
  // See nrq_gc_allocate_soa_planned for the compiled-plan and site-cache rules;
  // this is the interleaved-reference-table form of the same contract.
  if (!root_slot || count == 0 || element_size == 0) return nullptr;
  if (count > UINT32_MAX) return nullptr;
  // See nrq_gc_allocate_planned: a deterministic plan is not this entry point's
  // to honour.
  if ((plan_flags & NRQ_ALLOC_PLAN_DETERMINISTIC) != 0) return nullptr;
  const bool track_slow = tlab_diag.enabled.load(std::memory_order_relaxed);
  const auto slow_start = track_slow
                              ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
  if (track_slow)
    record_tlab_diag_slow_call(site_cache, count, element_size, plan_flags);
  // Honour a usable array plan: Compact for eligible single-element arrays, or
  // Extended for multi-element, SoA, or explicit Extended requests. Anything else
  // falls back to Auto so an oversized or invalid hint never corrupts headers.
  const uint32_t repr_bits = NRQ_ALLOC_PLAN_REPR_OF(plan_flags);
  GcRepresentation hint = GcRepresentation::Auto;
  if ((plan_flags & NRQ_ALLOC_PLAN_REJECTED) == 0 &&
      (plan_flags & NRQ_ALLOC_PLAN_ARRAY) != 0) {
    if (repr_bits == NRQ_ALLOC_REPR_COMPACT) {
      if (count == 1 && object_span_bytes(8, element_size) <= kGcPageBytes)
        hint = GcRepresentation::Compact;
    } else if (repr_bits == NRQ_ALLOC_REPR_EXTENDED) {
      hint = GcRepresentation::Extended;
    }
  }
  // A cached descriptor is only reused after it is checked against this call's
  // shape. The cache word is compiler-emitted and writable, so it is treated as
  // untrusted input, not as a guarantee.
  const nrq_object_layout* layout = nullptr;
  if (site_cache)
    layout = static_cast<const nrq_object_layout*>(*site_cache);
  if (layout &&
      !layout_memo_matches(layout, element_size, element_size, nullptr,
                           reference_offsets, reference_count, true))
    layout = nullptr;
  if (!layout) {
    layout = layout_for_allocation(element_size, element_size, nullptr,
                                   reference_offsets, reference_count, true);
    if (!layout) return nullptr;
    if (site_cache)
      *site_cache = const_cast<nrq_object_layout*>(layout);
  }
  void* result = allocate_array_rooted_impl(
      root_slot, count, element_size, reference_offsets, reference_count,
      layout, hint);
  if (track_slow) {
    tlab_diag.slow_ns.fetch_add(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - slow_start)
                .count()),
        std::memory_order_relaxed);
  }
  return result;
}

// Structure-of-arrays descriptors are interned by their column geometry. They keep
// their own pool because the key is a pair table rather than an offset table, and a
// shared pool would let one key mean two different layouts.
struct SoaPairsHash {
  size_t operator()(const std::vector<uint32_t>& pairs) const {
    size_t hash = static_cast<size_t>(0xCBF29CE484222325ull);
    for (const uint32_t value : pairs)
      hash = (hash ^ value) * static_cast<size_t>(0x100000001B3ull);
    return hash;
  }
};
std::unordered_map<std::vector<uint32_t>, const nrq_object_layout*, SoaPairsHash>
    soa_layout_pool;

// A geometry that has been interned recently is answered from this thread's own
// memory. The descriptor lives for the life of the process, so caching its
// address is safe; a site with a fixed column table then never takes the layout
// lock, and a per-object SoA allocation no longer builds two vectors to look up
// a descriptor it has already seen. That lookup, not the allocation, was most of
// the gap between this entry point and the plain array one.
struct SoaGeometryMemo {
  static constexpr size_t kCapacity = 4;
  static constexpr size_t kMaxPairs = 16;
  struct Entry {
    size_t element_bytes = 0;
    size_t pair_count = SIZE_MAX;
    uint32_t pairs[kMaxPairs] = {};
    const nrq_object_layout* layout = nullptr;
  };
  Entry entries[kCapacity];
  size_t next = 0;

  const nrq_object_layout* find(size_t element_bytes, const uint32_t* pairs,
                                size_t count) const {
    if (count > kMaxPairs) return nullptr;
    for (const Entry& entry : entries) {
      if (entry.layout == nullptr) continue;
      if (entry.element_bytes != element_bytes || entry.pair_count != count)
        continue;
      if (count != 0 &&
          std::memcmp(entry.pairs, pairs, count * sizeof(uint32_t)) != 0)
        continue;
      return entry.layout;
    }
    return nullptr;
  }

  void remember(size_t element_bytes, const uint32_t* pairs, size_t count,
                const nrq_object_layout* layout) {
    if (!layout || count > kMaxPairs) return;
    Entry& entry = entries[next];
    next = (next + 1) % kCapacity;
    entry.element_bytes = element_bytes;
    entry.pair_count = count;
    if (count != 0) std::memcpy(entry.pairs, pairs, count * sizeof(uint32_t));
    entry.layout = layout;
  }
};
thread_local SoaGeometryMemo soa_geometry_memo;
// Column tables are narrowed into this thread's own storage: a vector that keeps
// its capacity allocates once per thread instead of once per object.
thread_local std::vector<uint32_t> soa_pair_scratch;

const nrq_object_layout* soa_layout_for(size_t element_bytes,
                                       const uint32_t* pairs,
                                       size_t pair_count) {
  if (const nrq_object_layout* cached =
          soa_geometry_memo.find(element_bytes, pairs, pair_count))
    return cached;
  // The key must include the element size: two layouts can carry identical
  // reference geometry and still differ in payload per row, and sharing a
  // descriptor between them would trace the wrong number of rows. The descriptor
  // itself stores only the pairs, because payload_size already carries the size.
  std::vector<uint32_t> key;
  key.reserve(pair_count + 1);
  key.push_back(static_cast<uint32_t>(element_bytes));
  key.insert(key.end(), pairs, pairs + pair_count);
  std::lock_guard<std::mutex> lock(layout_mutex);
  const auto found = soa_layout_pool.find(key);
  if (found != soa_layout_pool.end()) {
    soa_geometry_memo.remember(element_bytes, pairs, pair_count, found->second);
    return found->second;
  }
  auto* stored = new uint32_t[pair_count];
  std::copy(pairs, pairs + pair_count, stored);
  auto* layout = new nrq_object_layout{};
  layout->payload_size = static_cast<uint32_t>(element_bytes);
  layout->reference_count = 0;
  layout->flags = NRQ_OBJECT_LAYOUT_ARRAY;
  layout->reference_offsets = nullptr;
  layout->trace = nullptr;
  layout->soa_columns = stored;
  layout->soa_column_pairs = static_cast<uint32_t>(pair_count);
  soa_layout_pool.emplace(std::move(key), layout);
  soa_geometry_memo.remember(element_bytes, pairs, pair_count, layout);
  return layout;
}
// Validates a structure-of-arrays column table and narrows it to the uint32 pairs
// a descriptor stores. A column base must leave room for a pointer inside one
// element, so a malformed table is refused here rather than described to the
// collector. The narrowed table is returned through the caller's view of this
// thread's scratch storage, so a valid call does not allocate.
static bool collect_soa_pairs(size_t element_bytes, const size_t* column_pairs,
                              size_t pair_count, const uint32_t*& pairs,
                              size_t& count) {
  if (!column_pairs || pair_count == 0 || (pair_count % 2) != 0) return false;
  if (element_bytes == 0 || element_bytes > UINT32_MAX) return false;
  soa_pair_scratch.resize(pair_count);
  for (size_t index = 0; index < pair_count; ++index) {
    if (column_pairs[index] > UINT32_MAX) return false;
    soa_pair_scratch[index] = static_cast<uint32_t>(column_pairs[index]);
    if ((index % 2) == 0 &&
        static_cast<size_t>(soa_pair_scratch[index]) + sizeof(void*) >
            element_bytes)
      return false;
  }
  pairs = soa_pair_scratch.data();
  count = pair_count;
  return true;
}

// Whether an interned descriptor really describes this column table. A stale or
// overwritten site cache can name a descriptor of the right size with different
// columns, which would trace a pointer at the wrong offset.
static bool soa_layout_matches(const nrq_object_layout* layout,
                               size_t element_bytes, const uint32_t* pairs,
                               size_t count) {
  if (!layout) return false;
  if (layout->payload_size != element_bytes) return false;
  if (layout->trace != nullptr) return false;
  if ((layout->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0) return false;
  if (layout->soa_column_pairs != count) return false;
  if (!layout->soa_columns) return false;
  return count == 0 || std::equal(pairs, pairs + count, layout->soa_columns);
}

void* nrq_gc_allocate_soa_rooted(void** root_slot, size_t count,
                                 size_t element_bytes,
                                 const size_t* column_pairs, size_t pair_count) {
  const uint32_t* pairs = nullptr;
  size_t pair_values = 0;
  if (!collect_soa_pairs(element_bytes, column_pairs, pair_count, pairs,
                         pair_values))
    return nullptr;
  const nrq_object_layout* layout =
      soa_layout_for(element_bytes, pairs, pair_values);
  return allocate_array_rooted_impl(root_slot, count, element_bytes, nullptr, 0,
                                    layout);
}

void* nrq_gc_allocate_soa_planned(void** root_slot, size_t count,
                                  size_t element_bytes,
                                  const size_t* column_pairs, size_t pair_count,
                                  void** site_cache, uint32_t plan_flags) {
  if (!root_slot || count == 0 || count > UINT32_MAX) return nullptr;
  // See nrq_gc_allocate_planned: a deterministic plan is not this entry point's
  // to honour.
  if ((plan_flags & NRQ_ALLOC_PLAN_DETERMINISTIC) != 0) return nullptr;
  const uint32_t* pairs = nullptr;
  size_t pair_values = 0;
  if (!collect_soa_pairs(element_bytes, column_pairs, pair_count, pairs,
                         pair_values))
    return nullptr;
  const bool plan_usable =
      (plan_flags & NRQ_ALLOC_PLAN_REJECTED) == 0 &&
      (plan_flags & NRQ_ALLOC_PLAN_ARRAY) != 0 &&
      NRQ_ALLOC_PLAN_REPR_OF(plan_flags) == NRQ_ALLOC_REPR_EXTENDED;
  const GcRepresentation hint =
      plan_usable ? GcRepresentation::Extended : GcRepresentation::Auto;
  const nrq_object_layout* layout = nullptr;
  if (site_cache) layout = static_cast<const nrq_object_layout*>(*site_cache);
  if (!soa_layout_matches(layout, element_bytes, pairs, pair_values))
    layout = nullptr;
  if (!layout) {
    layout = soa_layout_for(element_bytes, pairs, pair_values);
    if (!layout) return nullptr;
    if (site_cache) *site_cache = const_cast<nrq_object_layout*>(layout);
  }
  return allocate_array_rooted_impl(root_slot, count, element_bytes, nullptr, 0,
                                    layout, hint);
}
void nrq_gc_root_register(void** slot) {
  static constexpr size_t pointer_offset = 0;
  nrq_gc_root_register_typed(slot, sizeof(void*), &pointer_offset, 1);
}

void nrq_gc_root_register_typed(void* slot, size_t object_size,
                                const size_t* reference_offsets,
                                size_t reference_count) {
  // A failed explicit allocation has no storage to describe. Registration is
  // therefore a no-op for a null slot, matching unregistration; a non-null slot
  // with a malformed descriptor is still rejected.
  if (!slot) return;
  if (object_size == 0 || reference_count == 0 || !reference_offsets)
    nrq_runtime_fatal("GC root registration received an invalid root descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > object_size || sizeof(void*) > object_size - offset)
      nrq_runtime_fatal("GC root registration contains an out-of-range pointer offset");
  }
  const nrq_object_layout* layout =
      root_layout(object_size, object_size, 0, reference_offsets,
                  reference_count);
  if (!layout)
    nrq_runtime_fatal("GC root registration received an undescribable layout");

  ShadowStack& stack = ensure_shadow_stack();
  const bool is_ptr_root = is_pointer_root_layout(layout);
  const void* encoded = encode_shadow_layout(layout, /*indexed=*/true);
  ensure_shadow_capacity(stack, 1, 0);
  auto push_root = [&]() {
    try {
      add_slot_state(stack, slot, layout, is_ptr_root);
      *stack.fast->root_cursor++ = NrqGcFastRoot{slot, encoded};
    } catch (...) {
      nrq_runtime_fatal("GC root registration failed to track the current thread");
    }
  };

  // Fast path: no collector mutex, and no stack mutex unless a diagnostic query
  // could be reading this stack right now.
  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      push_root();
    } else {
      push_root();
    }
    end_fast_barrier();
    return;
  }

  HeapLockGuard heap_lock(gc_mutex);
  {
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    // Re-read under gc_mutex. If the collector snapshotted before this push,
    // prepare_cycle has already advanced the phase to Marking; if it has not,
    // the root will be part of the snapshot.
    const CollectionPhase phase =
        collection_phase.load(std::memory_order_acquire);
    if (phase == CollectionPhase::Marking ||
        phase == CollectionPhase::Sweeping)
      mark_root_values_locked(slot, layout, 0);
    push_root();
  }
}

// Word 0 is the slot count and word 1 is the table's own length.
constexpr size_t kFrameRootHeaderWords = 2;

// Shared body of the two frame-registration entry points. `validate` is a
// compile-time constant at both call sites, so the development configuration
// keeps every check and the production configuration for a compiler-built table
// contains none of them.
static void validate_frame_descriptor(void* const* slots, size_t slot_count,
                                      const size_t* descriptor) {
  if (descriptor[0] != slot_count)
    nrq_runtime_fatal(
        "GC frame root registration descriptor does not match its slot count");
  for (size_t index = 0; index < slot_count; ++index) {
    if (!slots[index])
      nrq_runtime_fatal("GC frame root registration contains a null slot");
  }
  if (!thread_validated_descriptors.matches(descriptor)) {
    const size_t word_count = descriptor[1];
    if (word_count < 2 || word_count < 2 + 2 * slot_count)
      nrq_runtime_fatal(
          "GC frame root registration descriptor is shorter than its roots");
    size_t cursor = 2;
    for (size_t index = 0; index < slot_count; ++index) {
      const size_t object_size = descriptor[cursor++];
      const size_t reference_count = descriptor[cursor++];
      if (object_size == 0 || reference_count == 0)
        nrq_runtime_fatal("GC frame root registration contains an empty root");
      for (size_t reference = 0; reference < reference_count; ++reference) {
        const size_t offset = descriptor[cursor + reference];
        if (offset > object_size || sizeof(void*) > object_size - offset)
          nrq_runtime_fatal(
              "GC frame root registration contains an out-of-range pointer offset");
      }
      cursor += reference_count;
    }
    if (cursor != word_count)
      nrq_runtime_fatal(
          "GC frame root registration descriptor length does not match its roots");
    thread_validated_descriptors.insert(descriptor);
  }
}

// Shared body of the frame-registration entry points. `validate` and
// `index_slots` are compile-time constants at call sites.
static void register_frame_roots(void* const* slots, size_t slot_count,
                                 const size_t* descriptor, bool validate,
                                 bool index_slots) {
  if (slot_count == 0) return;
  if (!slots || !descriptor)
    nrq_runtime_fatal("GC frame root registration received an invalid descriptor");

  if (validate) validate_frame_descriptor(slots, slot_count, descriptor);

  ShadowStack& stack = ensure_shadow_stack();
  ensure_shadow_capacity(stack, slot_count, 0);

  auto read_slot_layout = [&](size_t& cursor, bool& is_ptr_root) {
    const size_t object_size = descriptor[cursor++];
    const size_t reference_count = descriptor[cursor++];
    if (object_size == sizeof(void*) && reference_count == 1 &&
        descriptor[cursor] == 0) {
      ++cursor;
      is_ptr_root = true;
      return &kPointerRootLayout;
    }
    const nrq_object_layout* layout = root_layout(
        object_size, object_size, 0, descriptor + cursor, reference_count);
    if (!layout)
      nrq_runtime_fatal("GC root registration received an undescribable layout");
    cursor += reference_count;
    is_ptr_root = is_pointer_root_layout(layout);
    return layout;
  };

  auto push_roots = [&](bool mark_active) {
    size_t cursor = kFrameRootHeaderWords;
    NrqGcFastRoot* dst = stack.fast->root_cursor;
    for (size_t index = 0; index < slot_count; ++index) {
      bool is_ptr_root = false;
      const nrq_object_layout* layout = read_slot_layout(cursor, is_ptr_root);
      if (mark_active) mark_root_values_locked(slots[index], layout, 0);
      if (index_slots) {
        add_slot_state(stack, slots[index], layout, is_ptr_root);
      }
      dst[index] = NrqGcFastRoot{slots[index],
                                 encode_shadow_layout(layout, index_slots)};
    }
    stack.fast->root_cursor = dst + slot_count;
  };

  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      push_roots(false);
    } else {
      push_roots(false);
    }
    end_fast_barrier();
    return;
  }

  HeapLockGuard heap_lock(gc_mutex);
  {
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    const CollectionPhase phase =
        collection_phase.load(std::memory_order_acquire);
    push_roots(phase == CollectionPhase::Marking ||
               phase == CollectionPhase::Sweeping);
  }
}

// Dynamic entry point: the descriptor is supplied by the caller, so it is
// validated on every activation (memoised only for an identical table) and slots
// are tracked in `SlotIndex` for duplicate/layout contract validation.
void nrq_gc_root_register_frame(void* const* slots, size_t slot_count,
                                const size_t* descriptor) {
  register_frame_roots(slots, slot_count, descriptor, /*validate=*/true,
                       /*index_slots=*/true);
}

// Static-descriptor entry point. Only the lowering emits this, with an
// immutable module table for fresh local stack slots.
void nrq_gc_root_register_frame_static(void* const* slots, size_t slot_count,
                                       const size_t* descriptor) {
  register_frame_roots(slots, slot_count, descriptor,
                       /*validate=*/nirq::runtime_debug_enabled,
                       /*index_slots=*/false);
}

void nrq_gc_root_register_array(void* data, size_t count, size_t element_size,
                                const size_t* reference_offsets,
                                size_t reference_count) {
  if (!data || count == 0) return;
  if (element_size == 0 || reference_count == 0 || !reference_offsets ||
      count > SIZE_MAX / element_size)
    nrq_runtime_fatal("GC array-root registration received an invalid descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > element_size || sizeof(void*) > element_size - offset)
      nrq_runtime_fatal("GC array-root registration contains an invalid pointer offset");
  }
  HeapLockGuard lock(gc_mutex);
  auto found = roots.find(data);
  if (found != roots.end()) {
    nrq_runtime_fatal("GC array-root storage address is already registered");
  }
  try {
    RootInfo root;
    root.references = 1;
    root.object_size = count * element_size;
    root.repeated_count = count;
    root.layout = root_layout(root.object_size, element_size, count,
                              reference_offsets, reference_count);
    if (!root.layout)
      nrq_runtime_fatal(
          "GC array-root registration received an undescribable layout");
    roots.emplace(data, std::move(root));
  } catch (...) {
    nrq_runtime_fatal("GC array-root registration failed to allocate metadata");
  }
}

void nrq_gc_root_clear_array_references(void* data, size_t count,
                                        size_t element_size,
                                        const size_t* reference_offsets,
                                        size_t reference_count) {
  if (!data || count == 0) return;
  if (element_size == 0 || reference_count == 0 || !reference_offsets ||
      count > SIZE_MAX / element_size)
    nrq_runtime_fatal("GC array-root initialization received an invalid descriptor");
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    if (offset > element_size || sizeof(void*) > element_size - offset)
      nrq_runtime_fatal("GC array-root initialization contains an invalid pointer offset");
  }
  HeapLockGuard lock(gc_mutex);
  if (roots.find(data) != roots.end())
    nrq_runtime_fatal("GC array-root initialization requires unregistered storage");
  auto* bytes = static_cast<unsigned char*>(data);
  for (size_t element = 0; element < count; ++element) {
    unsigned char* element_bytes = bytes + element * element_size;
    for (size_t index = 0; index < reference_count; ++index)
      std::memset(element_bytes + reference_offsets[index], 0, sizeof(void*));
  }
}

void nrq_gc_root_unregister(void** slot) {
  if (!slot) return;
  if (thread_shadow_stack && thread_shadow_stack->fast &&
      thread_shadow_stack->fast->root_cursor) {
    ShadowStack& stack = *thread_shadow_stack;
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    NrqGcFastRoot* begin = stack.roots_buffer.data();
    NrqGcFastRoot* end = stack.fast->root_cursor;
    for (NrqGcFastRoot* entry = end; entry > begin;) {
      --entry;
      if (entry->slot != slot) continue;
      if (shadow_layout_is_indexed(entry->layout)) {
        remove_slot_state(stack, slot);
      } else if (stack.pointer_roots_has_unindexed) {
        stack.pointer_roots.forget(slot);
      }
      const size_t tail = static_cast<size_t>(end - (entry + 1));
      if (tail != 0)
        std::memmove(entry, entry + 1, tail * sizeof(NrqGcFastRoot));
      stack.fast->root_cursor = end - 1;
      NrqGcFastFrame* f_begin = stack.frames_buffer.data();
      NrqGcFastFrame* f_end = stack.fast->frame_cursor;
      for (NrqGcFastFrame* f = f_begin; f < f_end; ++f) {
        if (f->prev_root_cursor > entry) --f->prev_root_cursor;
      }
      return;
    }
  }
  // Array/external roots live in the global table and may be released by their
  // owner from any thread.
  HeapLockGuard lock(gc_mutex);
  auto found = roots.find(slot);
  // Compiler cleanup is emitted for owned arrays even when their element type
  // contains no managed references and no root region was registered. Preserve
  // the historical no-op for an unknown external root.
  if (found == roots.end()) return;
  if (found->second.references == 0)
    nrq_runtime_fatal("GC root unregistration count underflow");
  if (--found->second.references == 0) roots.erase(found);
}

void nrq_gc_root_store(void** slot, void* value) {
  if (!slot) return;
  // Rewriting the referent a root already holds creates no new edge, so the store
  // is not observable by the collector and needs neither the barrier nor the
  // handshake. A snapshot-at-the-beginning marker would only re-mark a value the
  // slot never stopped holding.
  if (*slot == value) return;
  // With no cycle in progress the store cannot be observed by any collector: a
  // cycle only snapshots this thread's roots after the thread reaches a
  // safepoint and parks, and parking happens after the store. Nothing needs
  // marking and nothing can be swept, so the handshake buys nothing here; the
  // old-to-young buffer, when the barrier is raised, is append-only and is
  // drained under the heap lock at the next cycle. This is the common case for
  // a mutator that collects only at its own safepoints.
  if (thread_mutator.depth != 0 &&
      collection_phase.load(std::memory_order_acquire) ==
          CollectionPhase::Idle) {
    *slot = value;
    buffer_remembered(value);
    return;
  }
  // Language-generated stores execute inside a registered mutator. The fast
  // path is just a pointer store plus a reader-count handshake; the collector
  // waits for in-flight readers before snapshotting roots.
  if (thread_mutator.depth != 0 && begin_fast_barrier()) {
    *slot = value;
    // Clearing a root cannot create an old-to-young edge, so only a non-null
    // referent is worth recording.
    if (value) buffer_remembered(value);
    end_fast_barrier();
    return;
  }
  HeapLockGuard lock(gc_mutex);
  store_root_value_locked(slot, value);
}

void scan_pointer_words_locked(void* storage, size_t bytes, bool mark) {
  auto* address = static_cast<unsigned char*>(storage);
  for (size_t offset = 0; offset + sizeof(void*) <= bytes;
       offset += sizeof(void*)) {
    void* reference = nullptr;
    std::memcpy(&reference, address + offset, sizeof(reference));
    if (!reference) continue;
    if (mark) mark_value_locked(reference);
    defer_remembered_locked(reference);
  }
}

void nrq_gc_root_copy(void* destination, const void* source, size_t bytes) {
  if (!destination || !source || bytes == 0) return;
  if (thread_mutator.depth != 0 && begin_fast_barrier()) {
    std::memcpy(destination, source, bytes);
    buffer_pointer_words(destination, bytes);
    end_fast_barrier();
    return;
  }
  HeapLockGuard lock(gc_mutex);
  const CollectionPhase phase =
      collection_phase.load(std::memory_order_relaxed);
  if (phase == CollectionPhase::Marking)
    scan_pointer_words_locked(destination, bytes, /*mark=*/true);
  std::memcpy(destination, source, bytes);
  // The destination may now reference young objects even when the containing
  // object is old; record them for the next minor collection. During sweep,
  // marking new referents prevents the next slice from reclaiming them.
  scan_pointer_words_locked(destination, bytes,
                            phase != CollectionPhase::Idle);
}

static uint64_t start_root_frame() {
  ShadowStack& stack = ensure_shadow_stack();
  if (stack.next_marker == UINT64_MAX)
    nrq_runtime_fatal("GC root frame marker space exhausted");
  ensure_shadow_capacity(stack, 0, 1);
  const uint64_t marker = stack.next_marker++;
  *stack.fast->frame_cursor++ =
      NrqGcFastFrame{marker, stack.fast->root_cursor};
  debug_emit(NRQ_GC_EVENT_ROOT_FRAME, 1, marker, nullptr);
  return marker;
}

uint64_t nrq_gc_root_frame_begin(void) { return start_root_frame(); }

// The trusted fused entry. Pushes the frame record and publishes the static
// roots in one call and one shadow-stack check.
uint64_t nrq_gc_root_frame_open(void* const* slots, size_t slot_count,
                                const size_t* descriptor) {
  if (slot_count == 0) return start_root_frame();
  if (!slots || !descriptor)
    nrq_runtime_fatal("GC frame root registration received an invalid descriptor");
  if constexpr (nirq::runtime_debug_enabled) {
    validate_frame_descriptor(slots, slot_count, descriptor);
  }
  // A safepoint poll, and a deliberately guarded one: nothing is computed unless a
  // region budget has been declared, so a program that never uses the pacemaker
  // pays a relaxed load and a predictable branch per frame rather than a second
  // threshold calculation. It runs before this frame is published, so a collection
  // can never observe half a frame.
  if (pacemaker_budget_declared.load(std::memory_order_relaxed) &&
      allocation_accounting.bytes_since_collection.load(
          std::memory_order_relaxed) >= safepoint_poll_threshold())
    maybe_collect_automatically();
  ShadowStack& stack = ensure_shadow_stack();
  if (stack.next_marker == UINT64_MAX)
    nrq_runtime_fatal("GC root frame marker space exhausted");
  ensure_shadow_capacity(stack, slot_count, 1);
  const uint64_t marker = stack.next_marker++;
  NrqGcFastRoot* const prev_roots = stack.fast->root_cursor;
  *stack.fast->frame_cursor++ = NrqGcFastFrame{marker, prev_roots};
  debug_emit(NRQ_GC_EVENT_ROOT_FRAME, 1, marker, nullptr);

  auto push_static_roots = [&](bool mark_active) {
    // Fast path for the common case where every slot is a plain 8-byte pointer
    // root (`descriptor[1] == 2 + 3 * slot_count`).
    if (descriptor[1] == kFrameRootHeaderWords + 3 * slot_count) {
      bool all_pointer_roots = true;
      size_t cursor = kFrameRootHeaderWords;
      for (size_t index = 0; index < slot_count; ++index, cursor += 3) {
        if (descriptor[cursor] != sizeof(void*) ||
            descriptor[cursor + 1] != 1 || descriptor[cursor + 2] != 0) {
          all_pointer_roots = false;
          break;
        }
      }
      if (all_pointer_roots) {
        for (size_t index = 0; index < slot_count; ++index) {
          if (mark_active)
            mark_root_values_locked(slots[index], &kPointerRootLayout, 0);
          prev_roots[index] = NrqGcFastRoot{slots[index], nullptr};
        }
        stack.fast->root_cursor = prev_roots + slot_count;
        return;
      }
    }

    size_t cursor = kFrameRootHeaderWords;
    for (size_t index = 0; index < slot_count; ++index) {
      const size_t object_size = descriptor[cursor++];
      const size_t reference_count = descriptor[cursor++];
      const nrq_object_layout* layout = nullptr;
      if (object_size == sizeof(void*) && reference_count == 1 &&
          descriptor[cursor] == 0) {
        ++cursor;
        layout = &kPointerRootLayout;
      } else {
        layout = root_layout(object_size, object_size, 0,
                             descriptor + cursor, reference_count);
        if (!layout)
          nrq_runtime_fatal(
              "GC root registration received an undescribable layout");
        cursor += reference_count;
      }
      if (mark_active) mark_root_values_locked(slots[index], layout, 0);
      prev_roots[index] =
          NrqGcFastRoot{slots[index], encode_shadow_layout(layout, false)};
    }
    stack.fast->root_cursor = prev_roots + slot_count;
  };

  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> stack_lock(stack.mutex);
      push_static_roots(false);
    } else {
      push_static_roots(false);
    }
    end_fast_barrier();
    return marker;
  }

  HeapLockGuard heap_lock(gc_mutex);
  {
    std::lock_guard<std::mutex> stack_lock(stack.mutex);
    const CollectionPhase phase =
        collection_phase.load(std::memory_order_acquire);
    push_static_roots(phase == CollectionPhase::Marking ||
                      phase == CollectionPhase::Sweeping);
  }
  return marker;
}

void nrq_gc_root_frame_end(uint64_t marker) {
  if (!thread_shadow_stack || !thread_shadow_stack->fast ||
      !thread_shadow_stack->fast->frame_cursor)
    nrq_runtime_fatal("GC root frame ended without a shadow stack");
  ShadowStack& stack = *thread_shadow_stack;
  NrqGcFastTls& fast = *stack.fast;
  NrqGcFastFrame* const frames_begin = stack.frames_buffer.data();
  NrqGcFastRoot* const roots_begin = stack.roots_buffer.data();
  if (fast.frame_cursor <= frames_begin)
    nrq_runtime_fatal("GC root frames must end in LIFO order with a valid marker");
  const NrqGcFastFrame top = *(fast.frame_cursor - 1);
  if (top.marker != marker || top.prev_root_cursor < roots_begin ||
      top.prev_root_cursor > fast.root_cursor)
    nrq_runtime_fatal("GC root frames must end in LIFO order with a valid marker");
  --fast.frame_cursor;
  debug_emit(NRQ_GC_EVENT_ROOT_FRAME, 0, marker, nullptr);
  if (top.prev_root_cursor == fast.root_cursor) return;

  auto remove_owned_roots = [&]() {
    if (stack.slot_states.size() != 0 || stack.pointer_roots_has_unindexed)
      remove_frame_slots(stack, top.prev_root_cursor, fast.root_cursor);
    fast.root_cursor = top.prev_root_cursor;
  };

  if (begin_fast_barrier()) {
    if (diagnostics_reading_roots()) {
      std::lock_guard<std::mutex> lock(stack.mutex);
      remove_owned_roots();
    } else {
      remove_owned_roots();
    }
    end_fast_barrier();
    return;
  }
  std::lock_guard<std::mutex> lock(stack.mutex);
  remove_owned_roots();
}

#if NRQ_DEBUG_ENABLED
size_t nrq_gc_debug_snapshot_roots(nrq_gc_debug_root* out, size_t capacity) {
  if (!out || capacity == 0) return 0;
  bool previous = false;
  {
    HeapLockGuard lock(gc_mutex);
    previous = reference_diagnostics_enabled.exchange(true, std::memory_order_acq_rel);
    refresh_inline_config_locked();
  }
  struct RestoreFlag {
    bool previous;
    ~RestoreFlag() {
      HeapLockGuard lock(gc_mutex);
      reference_diagnostics_enabled.store(previous, std::memory_order_release);
      refresh_inline_config_locked();
    }
  } restore{previous};
#if defined(_WIN32)
  FlushProcessWriteBuffers();
#endif
  wait_for_fast_barriers();

  size_t written = 0;
  auto record = [&](void* slot, const nrq_object_layout* layout,
                    size_t repeated_count) {
    if (written == capacity) return;
    nrq_gc_debug_root& entry = out[written++];
    entry = nrq_gc_debug_root{};
    entry.slot = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(slot));
    entry.reference_count = layout ? layout->reference_count : 0u;
    entry.element_size = static_cast<uint32_t>(layout ? layout->payload_size : 0);
    entry.element_count = repeated_count != 0
                             ? static_cast<uint32_t>(repeated_count)
                             : 1u;
    entry.is_pointer_root =
        (repeated_count == 0 && is_pointer_root_layout(layout)) ? 1u : 0u;
    if (!slot || !layout || layout->reference_count == 0) return;
    const size_t span = layout->payload_size;
    const size_t offset = layout->reference_offsets[0];
    if (offset > span || sizeof(void*) > span - offset) return;
    void* referent = nullptr;
    std::memcpy(&referent, static_cast<const unsigned char*>(slot) + offset,
                sizeof(referent));
    entry.referent = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(referent));
  };

  {
    HeapLockGuard lock(gc_mutex);
    for (const auto& root : roots)
      record(root.first, root.second.layout, root.second.repeated_count);
    for (ShadowStack* stack : shadow_stacks) {
      if (!stack || !stack->fast) continue;
      std::lock_guard<std::mutex> stack_lock(stack->mutex);
      for_each_fast_root_slot_locked(
          *stack, [&](void* slot, const nrq_object_layout* layout) {
            record(slot, layout, 0);
          });
    }
  }
  return written;
}
#endif  // NRQ_DEBUG_ENABLED

void nrq_gc_enable(void) {
  {
    HeapLockGuard lock(gc_mutex);
    automatic_collection_enabled.store(true, std::memory_order_relaxed);
    refresh_inline_config_locked();
  }
  sync_inline_unflushed(thread_mutator);
  update_tlab_end_clamp(thread_mutator);
}

void nrq_gc_disable(void) {
  {
    HeapLockGuard lock(gc_mutex);
    automatic_collection_enabled.store(false, std::memory_order_relaxed);
    refresh_inline_config_locked();
  }
  update_tlab_end_clamp(thread_mutator);
}

void nrq_gc_collect(void) {
  run_collection(/*young_only=*/false);
}

void nrq_gc_collect_minor(void) {
  // With generations disabled every object is old, so a young-only cycle could
  // not reclaim anything. Treat the explicit request as a full collection.
  run_collection(
      generational_collection_enabled.load(std::memory_order_relaxed));
}

void nrq_gc_set_generational(int32_t enabled) {
  HeapLockGuard lock(gc_mutex);
  if (enabled) {
    // Raise the derived switch first: a mutator that still sees the switch raised
    // after generations are turned off only records extra entries.
    std::atomic_ref<uint64_t>(nrq_gc_inline_config.packed_state)
        .fetch_or(uint64_t{NRQ_GC_INLINE_GATE_STORE_BARRIER_BIT},
                  std::memory_order_acq_rel);
    std::atomic_ref<uint32_t>(nrq_gc_inline_config.store_barrier)
        .store(1u, std::memory_order_release);
    store_barrier_active.store(true, std::memory_order_release);
    generational_collection_enabled.store(true, std::memory_order_relaxed);
    refresh_inline_config_locked();
    return;
  }
  store_barrier_active.store(false, std::memory_order_release);
  std::atomic_ref<uint32_t>(nrq_gc_inline_config.store_barrier)
      .store(0u, std::memory_order_release);
  generational_collection_enabled.store(false, std::memory_order_relaxed);
  refresh_inline_config_locked();
}

int32_t nrq_gc_is_generational(void) {
  return generational_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

uint64_t nrq_gc_get_minor_collection_count(void) {
  HeapLockGuard lock(gc_mutex);
  return minor_collections;
}

uint64_t nrq_gc_get_major_collection_count(void) {
  HeapLockGuard lock(gc_mutex);
  return major_collections;
}

void nrq_gc_mutator_enter(void) {
  if (thread_mutator.depth == SIZE_MAX)
    nrq_runtime_fatal("GC mutator nesting depth overflow");
  if (!tlab_diag.env_checked.exchange(true, std::memory_order_relaxed)) {
    if (const char* env = std::getenv("NRQ_GC_TLAB_DIAG")) {
      if (*env == '1') {
        tlab_diag.enabled.store(true, std::memory_order_relaxed);
        tlab_diag.print_on_exit.store(true, std::memory_order_relaxed);
      }
    }
  }
  thread_gc_cleanup.arm();
  thread_mutator.fast = &nrq_gc_fast_tls;
  if (thread_mutator.depth++ != 0) return;
  (void)ensure_shadow_stack();

  std::unique_lock<std::mutex> lock(mutator_mutex);
  while (collection_is_requested()) wait_until_not_requested(lock);
  try {
    mutators.push_back(&thread_mutator);
    ++registered_mutator_count;
  } catch (...) {
    thread_mutator.depth = 0;
    nrq_runtime_fatal("GC mutator registration failed");
  }
}

void nrq_gc_mutator_leave(void) {
  if (thread_mutator.depth == 0)
    nrq_runtime_fatal("GC mutator leave without matching enter");
  if (--thread_mutator.depth != 0) return;

  bool should_print_tlab_diag = false;
  {
    std::lock_guard<std::mutex> lock(mutator_mutex);
    {
      HeapLockGuard heap_lock(gc_mutex);
      publish_tlab_locked(thread_mutator);
      // This thread is never visited by cycle preparation again, so the space it
      // reserved has to be returned to its pages here.
      flush_spare_runs_locked(thread_mutator);
      release_tlab_tail_locked(thread_mutator);
      tlab_tail_bytes_total =
          saturated_add(tlab_tail_bytes_total, thread_mutator.tlab_tail_bytes);
      if (thread_mutator.tlab_tail_max_bytes > tlab_tail_max_bytes_total)
        tlab_tail_max_bytes_total = thread_mutator.tlab_tail_max_bytes;
      tlab_tail_reclaimed_bytes_total = saturated_add(
          tlab_tail_reclaimed_bytes_total,
          thread_mutator.tlab_tail_reclaimed_bytes);
      drain_remembered_locked(thread_mutator, /*defer_unresolved=*/true);
      // A thread that leaves the registry is never visited by cycle preparation
      // again, so its buffered handoffs have to be settled here.
      drain_transfer_pins_locked(thread_mutator);
    }
    thread_mutator.parked = false;
    auto found = std::find(mutators.begin(), mutators.end(), &thread_mutator);
    if (found == mutators.end())
      nrq_runtime_fatal("GC mutator registry lost an active thread");
    mutators.erase(found);
    --registered_mutator_count;
    should_print_tlab_diag =
        registered_mutator_count == 0 &&
        tlab_diag.print_on_exit.load(std::memory_order_relaxed);
    // The collector's park predicate compares against the registry size, so a
    // shrinking registry has to wake it.
    collector_condition.notify_all();
    mutator_condition.notify_all();
  }
  if (should_print_tlab_diag) print_tlab_diag_report();
}

void nrq_gc_safepoint(void) {
  // The common path is a single acquire load. Only threads that observe a
  // request pay for the mutex and condition-variable protocol.
  if (!collection_is_requested()) return;
  if (thread_is_collector || thread_mutator.depth == 0) return;

  std::unique_lock<std::mutex> lock(mutator_mutex);
  if (!collection_is_requested()) return;
  park_mutator(lock);
}

void nrq_gc_set_threshold(size_t bytes) {
  // An explicit threshold is a policy decision by the program, so automatic
  // adaptation stops here until it is turned back on.
  {
    HeapLockGuard lock(gc_mutex);
    collection_policy.adaptive = false;
    collection_policy.adaptations = 0;
    allocation_accounting.collection_threshold.store(bytes == 0 ? 1 : bytes,
                                                     std::memory_order_relaxed);
    refresh_inline_config_locked();
  }
  sync_inline_unflushed(thread_mutator);
  update_tlab_end_clamp(thread_mutator);
}

size_t nrq_gc_get_threshold(void) {
  HeapLockGuard lock(gc_mutex);
  return allocation_accounting.collection_threshold.load(
      std::memory_order_relaxed);
}

void nrq_gc_set_adaptive(int32_t enabled) {
  HeapLockGuard lock(gc_mutex);
  collection_policy.adaptive = enabled != 0;
  // Re-enabling starts from the current live set instead of waiting for the next
  // cycle to correct a threshold the program may have pinned long ago.
  if (collection_policy.adaptive) adapt_collection_policy_locked();
  else refresh_inline_config_locked();
}

int32_t nrq_gc_policy_is_adaptive(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.adaptive ? 1 : 0;
}

uint64_t nrq_gc_policy_adaptation_count(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.adaptations;
}

uint64_t nrq_gc_last_reclaimed_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  return collection_policy.last_reclaimed_bytes;
}

size_t nrq_gc_object_header_bytes(void) { return 16; }

const nrq_object_layout* nrq_gc_layout_intern(
    size_t payload_bytes, size_t element_bytes, nrq_gc_trace_fn trace,
    const size_t* reference_offsets, size_t reference_count) {
  // The descriptor stores these as 32-bit fields; a payload larger than that is
  // left to a custom trace callback rather than described by a table.
  if (payload_bytes == 0 || payload_bytes > UINT32_MAX) return nullptr;
  if (reference_count > UINT32_MAX) return nullptr;
  // An array element and its payload are the same bytes: `element_bytes` only
  // says whether the payload repeats.
  if (element_bytes != 0 && element_bytes != payload_bytes) return nullptr;
  if (reference_count != 0 && !reference_offsets) return nullptr;
  LayoutKey key{payload_bytes, element_bytes, trace, {}};
  key.offsets.reserve(reference_count);
  for (size_t index = 0; index < reference_count; ++index) {
    const size_t offset = reference_offsets[index];
    // A reference word must fit inside the record (or one element, which is the
    // payload of an array). Rejecting it here keeps every reader able to load a
    // pointer without a bounds test.
    if (offset > payload_bytes || sizeof(void*) > payload_bytes - offset)
      return nullptr;
    key.offsets.push_back(static_cast<uint32_t>(offset));
  }

  std::lock_guard<std::mutex> lock(layout_mutex);
  const auto existing = layout_pool.find(key);
  if (existing != layout_pool.end()) return existing->second;

  auto* offsets = new (std::nothrow) uint32_t[key.offsets.size()];
  if (!offsets && !key.offsets.empty()) return nullptr;
  for (size_t index = 0; index < key.offsets.size(); ++index)
    offsets[index] = key.offsets[index];
  auto* descriptor = new (std::nothrow) nrq_object_layout{
      static_cast<uint32_t>(payload_bytes),
      static_cast<uint32_t>(key.offsets.size()),
      element_bytes != 0 ? static_cast<uint32_t>(NRQ_OBJECT_LAYOUT_ARRAY) : 0u,
      offsets, trace};
  if (!descriptor) {
    delete[] offsets;
    return nullptr;
  }
  if ((descriptor->flags & NRQ_OBJECT_LAYOUT_ARRAY) == 0 &&
      descriptor->reference_count != 0 && descriptor->reference_count <= 2) {
    descriptor->flags |= NRQ_OBJECT_LAYOUT_INLINE_MAP;
    for (uint32_t index = 0; index < descriptor->reference_count; ++index)
      descriptor->inline_reference_offsets[index] = offsets[index];
  }
  try {
    layout_bytes_total += sizeof(nrq_object_layout) + key.offsets.size() * sizeof(uint32_t);
    layout_pool.emplace(std::move(key), descriptor);
  } catch (...) {
    delete descriptor;
    delete[] offsets;
    return nullptr;
  }
  return descriptor;
}

uint64_t nrq_gc_layout_memo_hits(void) { return layout_memo_hits; }

uint64_t nrq_gc_layout_memo_misses(void) { return layout_memo_misses; }

size_t nrq_gc_layout_count(void) {
  std::lock_guard<std::mutex> lock(layout_mutex);
  return layout_pool.size();
}
// Pool-owned descriptor bytes only. The static pointer-root descriptor is not
// pool-owned and is not included: it is one process-lifetime constant no matter
// how many root registrations use it.
size_t nrq_gc_layout_bytes(void) {
  std::lock_guard<std::mutex> lock(layout_mutex);
  return layout_bytes_total;
}

nrq_gc_allocation_facts nrq_gc_get_allocation_facts(void) {
  nrq_gc_allocation_facts facts{};
  // The maximum standard header is the extended one; compact and headerless
  // objects spend less. The split is measured from the extended struct: eight
  // bytes that describe one instance (element count and state) and eight that
  // name shared metadata (the layout id and the reserved flag word).
  facts.object_header_bytes = sizeof(ExtendedHeader);
  facts.object_instance_bytes = offsetof(ExtendedHeader, layout_id);
  facts.object_shared_bytes =
      sizeof(ExtendedHeader) - offsetof(ExtendedHeader, layout_id);
  facts.payload_alignment = kObjectStartAlignment;
  facts.page_bytes = kGcPageBytes;
  facts.tlab_bytes = kGcTlabBytes;
  facts.tlab_min_slab_bytes = kGcTlabMinSlabBytes;
  facts.spare_run_capacity = MutatorState::kSpareRunCapacity;
  facts.layout_descriptors = nrq_gc_layout_count();
  facts.layout_bytes = nrq_gc_layout_bytes();
  return facts;
}

nrq_gc_page_metadata_facts nrq_gc_get_page_metadata_facts(void) {
  nrq_gc_page_metadata_facts facts{};
  facts.page_bytes = kGcPageBytes;
  const size_t slots = kGcPageBytes / kObjectStartAlignment;
  const size_t words = (slots + 63) / 64;
  const size_t bitmap_bytes = words * sizeof(uint64_t);
  facts.object_start_bitmap_bytes = bitmap_bytes;
  facts.mark_bitmap_bytes = bitmap_bytes;
  facts.remembered_bitmap_bytes = bitmap_bytes;
  facts.total_bitmap_bytes = bitmap_bytes * 3;
  return facts;
}

int32_t nrq_gc_get_allocation_profile(size_t payload_bytes,
                                  nrq_gc_allocation_profile* out) {
  if (!out) return 0;
  if (payload_bytes == 0 ||
      payload_bytes > SIZE_MAX - 16 - kObjectStartAlignment)
    return 0;
  // The auto policy's answer for a fixed record: headerless while the payload
  // fits a uniform slot, compact above it. A profile that did not follow the
  // policy would describe storage the allocator never hands out.
  const size_t header_bytes = payload_bytes <= kHeaderlessMaxPayload ? 0 : 8;
  const size_t raw_span = header_bytes + payload_bytes;
  const size_t span =
      (raw_span + kObjectStartAlignment - 1) & ~(kObjectStartAlignment - 1);
  // A page is at least as large as the span it must hold, exactly as
  // take_span_locked sizes it.
  const size_t capacity = std::max(kGcPageBytes, span);
  const size_t slots = capacity / span;
  nrq_gc_allocation_profile profile{};
  profile.payload_bytes = payload_bytes;
  profile.span_bytes = span;
  profile.slots_per_page = slots;
  profile.page_slack_bytes = capacity - slots * span;
  profile.page_bytes = capacity;
  // A reservation never spans a page boundary, so one slab serves the objects a
  // page holds - or one, when a single object fills the page.
  profile.objects_per_refill = slots;
  *out = profile;
  return 1;
}

size_t nrq_gc_page_count(void) {
  HeapLockGuard lock(gc_mutex);
  return gc_pages.size();
}

size_t nrq_gc_recycled_page_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_pages.size();
}

uint64_t nrq_gc_page_reuse_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_page_reuse_count;
}

uint64_t nrq_gc_span_reuse_count(void) {
  HeapLockGuard lock(gc_mutex);
  return recycled_span_reuse_count;
}

size_t nrq_gc_object_index_entries(void) {
  HeapLockGuard lock(gc_mutex);
  return object_index.size();
}

int32_t nrq_gc_pointer_is_managed(void* object) {
  HeapLockGuard lock(gc_mutex);
  return find_object_locked(object).valid() ? 1 : 0;
}

// Read without the heap lock: the caller is a mutator diagnosing damage it has
// already observed, and blocking it behind a collection cycle it may be parked
// against would turn a diagnostic into a hang. Entry tearing is acceptable here
// because every field is read for one address the caller already knows about.
size_t nrq_gc_sweep_history(struct nrq_gc_sweep_record* out, size_t max) {
  if (!out || max == 0) return 0;
  const size_t total = sweep_history_cursor.load(std::memory_order_relaxed);
  const size_t oldest = total > kSweepHistoryEntries ? total - kSweepHistoryEntries : 0;
  const size_t available = total - oldest;
  const size_t count = available < max ? available : max;
  const size_t first = total - count;
  for (size_t index = 0; index < count; ++index) {
    const SweepHistoryRecord& record =
        sweep_history[(first + index) % kSweepHistoryEntries];
    out[index].address = record.address;
    out[index].cycle = record.cycle;
    out[index].offset = record.offset;
    out[index].payload_bytes = record.payload_bytes;
    out[index].kind = record.kind;
    out[index].born = record.born;
    out[index].marked = record.marked;
    out[index].whole_page = record.whole_page;
    out[index].live_objects = record.live_objects;
    out[index].generation = record.generation;
    out[index].full = record.full;
  }
  return count;
}

const nrq_object_layout* nrq_gc_object_layout(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectRef ref = find_object_locked(object);
  if (!ref.valid()) return nullptr;
  const nrq_object_layout* layout = object_layout(ref);
  if constexpr (nirq::runtime_debug_enabled) {
    // Every live object must name a descriptor this runtime can read. The
    // development configuration checks the reference here rather than trusting it,
    // so an inspection reports the damage instead of handing back a pointer that
    // the next trace would fault on. Allocation already refuses an inconsistent
    // descriptor, so a failure here means the header itself was corrupted.
    if (!layout || !layout_is_consistent(*layout))
      nrq_runtime_fatal("GC object holds an invalid layout reference");
  }
  return layout;
}

uint32_t nrq_gc_object_state_bits(void* object) {
  HeapLockGuard lock(gc_mutex);
  const ObjectRef ref = find_object_locked(object);
  return ref.valid() ? object_state(ref) : 0;
}

int32_t nrq_gc_store_barrier_active(void) {
  return store_barrier_active.load(std::memory_order_acquire) ? 1 : 0;
}

uint64_t nrq_gc_tlab_refill_count(void) {
  HeapLockGuard lock(gc_mutex);
  return tlab_refill_count;
}
uint64_t nrq_gc_tlab_tail_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t total = tlab_tail_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator) total = saturated_add(total, mutator->tlab_tail_bytes);
  return total;
}

uint64_t nrq_gc_tlab_tail_max_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t largest = tlab_tail_max_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator && mutator->tlab_tail_max_bytes > largest)
      largest = mutator->tlab_tail_max_bytes;
  return largest;
}

uint64_t nrq_gc_tlab_tail_reclaimed_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  uint64_t total = tlab_tail_reclaimed_bytes_total;
  for (const MutatorState* mutator : mutators)
    if (mutator) total = saturated_add(total, mutator->tlab_tail_reclaimed_bytes);
  return total;
}
size_t nrq_gc_free_run_count(void) {
  HeapLockGuard lock(gc_mutex);
  return total_free_spans;
}

size_t nrq_gc_page_with_runs_count(void) {
  HeapLockGuard lock(gc_mutex);
  return span_pages.size();
}

uint64_t nrq_gc_reused_span_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  return reused_span_bytes;
}

uint64_t nrq_gc_dropped_free_run_bytes(void) {
  HeapLockGuard lock(gc_mutex);
  return dropped_free_bytes_total;
}

uint64_t nrq_gc_dropped_free_run_count(void) { return dropped_free_runs_total; }

nrq_gc_zeroing_facts nrq_gc_get_zeroing_facts(void) { return zeroing_facts; }

void nrq_gc_reset_zeroing_facts(void) { zeroing_facts = nrq_gc_zeroing_facts{}; }

nrq_gc_stats nrq_gc_get_stats(void) {
  // The registered-mutator vector is mutated under mutator_mutex. Take it
  // before gc_mutex to match the enter/leave lock order; taking only gc_mutex
  // would race a concurrent registration.
  std::lock_guard<std::mutex> registry_lock(mutator_mutex);
  HeapLockGuard lock(gc_mutex);
  nrq_gc_stats result = statistics;
  // Unpublished TLAB objects are not in `statistics` yet, but they are live.
  // Sum the per-mutator batches instead of maintaining a global pending pair
  // that every allocation would have to update.
  uint64_t pending_bytes = 0;
  uint64_t pending_count = 0;
  for (const MutatorState* mutator : mutators) {
    if (!mutator || !mutator->fast) continue;
    pending_bytes = saturated_add(
        pending_bytes, std::atomic_ref<size_t>(mutator->fast->pending_bytes)
                           .load(std::memory_order_relaxed));
    pending_count = saturated_add(
        pending_count, std::atomic_ref<size_t>(mutator->fast->pending_count)
                           .load(std::memory_order_relaxed));
  }
  result.allocated_bytes = saturated_add(result.allocated_bytes, pending_bytes);
  result.live_objects = saturated_add(result.live_objects, pending_count);
  return result;
}

uint64_t nrq_gc_get_allocated_bytes(void) {
  return nrq_gc_get_stats().allocated_bytes;
}

uint64_t nrq_gc_get_collected_bytes(void) {
  return nrq_gc_get_stats().collected_bytes;
}

uint64_t nrq_gc_get_collection_count(void) {
  return nrq_gc_get_stats().collections;
}

nrq_gc_cycle_timing nrq_gc_get_cycle_timing(void) { return cycle_timing; }

void nrq_gc_reset_cycle_timing(void) { cycle_timing = nrq_gc_cycle_timing{}; }

uint64_t nrq_gc_get_live_objects(void) {
  return nrq_gc_get_stats().live_objects;
}

void* nrq_gc_pin(void* object) {
  if (!object) return nullptr;
  HeapLockGuard lock(gc_mutex);
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!find_object_locked(object).valid()) return nullptr;
  if (collection_phase.load(std::memory_order_relaxed) !=
      CollectionPhase::Idle)
    mark_value_locked(object);
  auto existing = pinned_objects.find(object);
  if (existing != pinned_objects.end() && existing->second >= kMaxPinsPerObject) {
    pin_violations = saturated_add(pin_violations, 1);
    return nullptr;
  }
  try {
    ++pinned_objects[object];
    return object;
  } catch (...) {
    return nullptr;
  }
}

uint64_t nrq_gc_pin_count(void) {
  HeapLockGuard lock(gc_mutex);
  // Fold in this thread's buffered deltas so a query made right after a managed
  // return reflects it. Other threads' buffers stay private until the next cycle;
  // they are only ever folded while their owner is parked or holding this lock.
  drain_transfer_pins_locked(thread_mutator);
  return static_cast<uint64_t>(pinned_objects.size());
}

uint64_t nrq_gc_transfer_pin_fallbacks(void) {
  return transfer_pin_fallbacks.load(std::memory_order_relaxed);
}

uint64_t nrq_gc_pin_violations(void) {
  HeapLockGuard lock(gc_mutex);
  return pin_violations;
}

int32_t nrq_gc_is_enabled(void) {
  return automatic_collection_enabled.load(std::memory_order_relaxed) ? 1 : 0;
}

static void unpin_value(void* object);

void nrq_gc_unpin(void* object) {
  unpin_value(object);
}

// Publishes a pinned managed result into a root slot or field and releases the
// pin in one call, publication first. The release runs in this translation unit,
// so it is inlined into this entry instead of being a second call; one call
// boundary is what this saves. Both routines it calls still open their own
// fast-barrier region, so this entry enters two regions, not one.
void nrq_gc_root_store_release(void** slot, void* value) {
  if (slot) nrq_gc_root_store(slot, value);
  unpin_value(value);
}

static void unpin_value(void* object) {
  if (!object) return;
  bool buffer_fell_back = false;
  // Releasing a pin this thread buffered cancels it here, and releasing a pin the
  // table already knows about is queued as a negative delta. Either way the
  // collector mutex stays out of the path.
  if (begin_fast_barrier()) {
    MutatorState& mutator = thread_mutator;
    if (mutator.fast && mutator.fast->transfer_pin_fast == object) {
      mutator.fast->transfer_pin_fast = nullptr;
      end_fast_barrier();
      return;
    }
    TransferPinDelta* pending = find_transfer_delta(mutator, object);
    if (pending && pending->delta > 0) {
      // A buffered pin cancelled before the drain has no effect on the pin table,
      // so the entry stops being active as soon as its delta reaches zero.
      if (--pending->delta == 0) clear_transfer_delta(*pending);
      end_fast_barrier();
      return;
    }
    if (!pending) {
      if (TransferPinDelta* slot = acquire_transfer_delta_slot(mutator)) {
        slot->object = object;
        slot->delta = -1;
        end_fast_barrier();
        return;
      }
      transfer_pin_fallbacks.fetch_add(1, std::memory_order_relaxed);
      // The buffer holds this thread's earlier deltas. Applying this release to the
      // table first would let an earlier buffered pin arrive afterwards and leave the
      // object pinned with nothing left to release, so the buffer is settled in order
      // before the new operation.
      buffer_fell_back = true;
    }
    end_fast_barrier();
  }
  HeapLockGuard lock(gc_mutex);
  if (thread_mutator.fast && thread_mutator.fast->transfer_pin_fast == object) {
    thread_mutator.fast->transfer_pin_fast = nullptr;
    return;
  }
  if (buffer_fell_back) drain_transfer_pins_locked(thread_mutator);
  auto found = pinned_objects.find(object);
  if (found == pinned_objects.end()) return;
  if (--found->second == 0) pinned_objects.erase(found);
}

void* nrq_gc_pin_transfer(void* object) {
  if (!object) return nullptr;
  bool buffer_fell_back = false;
  // Registered mutators record the handoff in their own buffer instead of taking
  // the collector mutex. The buffer is folded into the pin table during cycle
  // preparation, before pinned roots are enumerated, so a buffered pin protects
  // its referent for exactly the collections a table pin would. Validity of the
  // pointer is checked at that point; code generation only ever transfers managed
  // results.
  if (begin_fast_barrier()) {
    MutatorState& mutator = thread_mutator;
    if (mutator.fast && mutator.fast->transfer_pin_fast == nullptr) {
      mutator.fast->transfer_pin_fast = object;
      end_fast_barrier();
      return object;
    }
    TransferPinDelta* pending = find_transfer_delta(mutator, object);
    if (pending) {
      // A pin cancels a buffered release, which is the same net effect a table pin
      // followed by the queued release would have had.
      if (++pending->delta == 0) clear_transfer_delta(*pending);
      end_fast_barrier();
      return object;
    }
    if (TransferPinDelta* slot = acquire_transfer_delta_slot(mutator)) {
      slot->object = object;
      slot->delta = 1;
      end_fast_barrier();
      return object;
    }
    transfer_pin_fallbacks.fetch_add(1, std::memory_order_relaxed);
    // Every slot holds a live delta. Leave the barrier region before taking the
    // collector mutex, because a collector holding it waits for barrier regions to
    // drain.
    buffer_fell_back = true;
    end_fast_barrier();
  }
  HeapLockGuard lock(gc_mutex);
  if (buffer_fell_back) drain_transfer_pins_locked(thread_mutator);
  // A managed return usually refers to an object that is still in this
  // thread's unpublished TLAB. Publishing the whole pending batch here would
  // defeat TLAB batching for every helper that returns a managed pointer. The
  // object is already protected: cycle preparation publishes every registered
  // mutator's TLAB before enumerating pinned roots, so a pending pin becomes
  // visible for exactly the collection that needs it.
  if (!find_object_locked(object).valid() &&
      !find_pending_header(thread_mutator, object).valid())
    return nullptr;
  if (collection_phase.load(std::memory_order_relaxed) !=
      CollectionPhase::Idle)
    mark_value_locked(object);
  try {
    size_t& count = pinned_objects[object];
    if (count == SIZE_MAX)
      nrq_runtime_fatal("GC transfer pin count overflow");
    ++count;
    return object;
  } catch (...) {
    nrq_runtime_fatal("GC transfer pin registration failed");
  }
}

void nrq_gc_ref_count_start(void) {
  {
    HeapLockGuard lock(gc_mutex);
    reference_diagnostics_enabled.store(true, std::memory_order_release);
    refresh_inline_config_locked();
  }
#if defined(_WIN32)
  FlushProcessWriteBuffers();
#endif
  wait_for_fast_barriers();
}

void nrq_gc_ref_count_stop(void) {
  HeapLockGuard lock(gc_mutex);
  reference_diagnostics_enabled.store(false, std::memory_order_release);
  refresh_inline_config_locked();
}

int64_t nrq_gc_get_ref_count(void* object) {
  // Diagnostics are off in normal execution, so a disabled query touches nothing.
  if (!diagnostics_reading_roots()) return -1;
  // Drain any fast-barrier or inline root sections that began before the flag
  // became visible.
#if defined(_WIN32)
  FlushProcessWriteBuffers();
#endif
  wait_for_fast_barriers();
  HeapLockGuard lock(gc_mutex);
  if (thread_mutator.depth != 0) publish_tlab_locked(thread_mutator);
  if (!object || !find_object_locked(object).valid()) return 0;
  try {
    ReferenceCountTarget target{object, 0};
    // The diagnostic walks exactly what the marker walks, through the same
    // descriptor, so a reference count can never disagree with marking.
    for_each_live_object_locked([&](const ObjectRef& object) {
      trace_children(object, count_reference_to_target, &target);
    });
    // Counts exactly what marking reads: the same descriptor, the same walk.
    auto count_root = [&](const void* storage, const nrq_object_layout* layout,
                          size_t repeated_count) {
      if (!layout || layout->reference_count == 0) return;
      const auto* bytes = static_cast<const unsigned char*>(storage);
      const size_t stride = layout->payload_size;
      const size_t elements = repeated_count != 0 ? repeated_count : 1;
      for (size_t element = 0; element < elements; ++element) {
        for (uint32_t index = 0; index < layout->reference_count; ++index) {
          const size_t offset = layout->reference_offsets[index];
          if (offset > stride || sizeof(void*) > stride - offset) continue;
          void* reference = nullptr;
          std::memcpy(&reference, bytes + element * stride + offset,
                      sizeof(reference));
          count_reference_to_target(reference, &target);
        }
      }
    };
    for (const auto& root : roots)
      count_root(root.first, root.second.layout, root.second.repeated_count);
    for (ShadowStack* stack : shadow_stacks) {
      if (!stack || !stack->fast) continue;
      std::lock_guard<std::mutex> stack_lock(stack->mutex);
      for_each_fast_root_slot_locked(
          *stack, [&](void* slot, const nrq_object_layout* layout) {
            count_root(slot, layout, 0);
          });
    }
    auto pinned = pinned_objects.find(object);
    if (pinned != pinned_objects.end())
      target.count = saturated_add(target.count, pinned->second);
    return static_cast<int64_t>(target.count);
  } catch (...) {
    return -1;
  }
}

alignas(nirq::nrq_cache_line_bytes) NrqGcInlineConfig nrq_gc_inline_config = {
    nrq_gc_inline_pack_state(0u, kYoungBit), kAccountingBatchBytes, 0u, 0u};
NRQ_THREAD_LOCAL NrqGcFastTls nrq_gc_fast_tls = {};

static_assert(NRQ_GC_INLINE_TLAB_BYTES == kGcTlabBytes,
              "inline TLAB limit must match kGcTlabBytes");
static_assert(NRQ_GC_INLINE_SPAN_ALIGNMENT == kObjectStartAlignment,
              "inline span alignment must match kObjectStartAlignment");
static_assert(sizeof(NrqGcInlineConfig) == 24,
              "NrqGcInlineConfig size mismatch");
static_assert(offsetof(NrqGcInlineConfig, packed_state) == 0,
              "NrqGcInlineConfig::packed_state offset mismatch");
static_assert(offsetof(NrqGcInlineConfig, accounting_batch) == 8,
              "NrqGcInlineConfig::accounting_batch offset mismatch");
static_assert(offsetof(NrqGcInlineConfig, store_barrier) == 16,
              "NrqGcInlineConfig::store_barrier offset mismatch");
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free,
              "NrqGcInlineConfig::packed_state must be lock-free");
static_assert(std::atomic_ref<size_t>::is_always_lock_free,
              "NrqGcInlineConfig::accounting_batch must be lock-free");
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "NrqGcInlineConfig::store_barrier must be lock-free");
static_assert(sizeof(NrqGcPendingRange) == 24,
              "NrqGcPendingRange size mismatch");
static_assert(offsetof(NrqGcFastTls, tlab_cursor) == 0,
              "NrqGcFastTls::tlab_cursor offset mismatch");
static_assert(offsetof(NrqGcFastTls, tlab_end) == 8,
              "NrqGcFastTls::tlab_end offset mismatch");
static_assert(offsetof(NrqGcFastTls, stack_frame_top) == 16,
              "NrqGcFastTls::stack_frame_top offset mismatch");
static_assert(offsetof(NrqGcFastTls, transfer_pin_fast) == 24,
              "NrqGcFastTls::transfer_pin_fast offset mismatch");
static_assert(offsetof(NrqGcFastTls, pending_bytes) == 32,
              "NrqGcFastTls::pending_bytes offset mismatch");
static_assert(offsetof(NrqGcFastTls, pending_count) == 40,
              "NrqGcFastTls::pending_count offset mismatch");
static_assert(offsetof(NrqGcFastTls, ext_cursor) == 48,
              "NrqGcFastTls::ext_cursor offset mismatch");
static_assert(offsetof(NrqGcFastTls, ext_end) == 56,
              "NrqGcFastTls::ext_end offset mismatch");
static_assert(offsetof(NrqGcFastTls, frame_end) <
                  offsetof(NrqGcFastTls, pending_ranges),
              "NrqGcFastTls hot pointers must precede pending_ranges");
static_assert(offsetof(NrqGcFastTls, stack_frame_top) < 64,
              "NrqGcFastTls::stack_frame_top must sit in cache line 0");
static_assert(sizeof(NrqGcFastRoot) == 16,
              "NrqGcFastRoot size mismatch");
static_assert(offsetof(NrqGcFastRoot, slot) == 0,
              "NrqGcFastRoot::slot offset mismatch");
static_assert(offsetof(NrqGcFastRoot, layout) == 8,
              "NrqGcFastRoot::layout offset mismatch");
static_assert(sizeof(NrqGcFastFrame) == 16,
              "NrqGcFastFrame size mismatch");
static_assert(offsetof(NrqGcFastFrame, marker) == 0,
              "NrqGcFastFrame::marker offset mismatch");
static_assert(offsetof(NrqGcFastFrame, prev_root_cursor) == 8,
              "NrqGcFastFrame::prev_root_cursor offset mismatch");
static_assert(sizeof(NrqGcStackFrame) == 16,
              "NrqGcStackFrame size mismatch");
static_assert(offsetof(NrqGcStackFrame, prev) == 0,
              "NrqGcStackFrame::prev offset mismatch");
static_assert(offsetof(NrqGcStackFrame, slot_count) == 8,
              "NrqGcStackFrame::slot_count offset mismatch");

uint32_t nrq_gc_layout_id(const nrq_object_layout* layout) {
  return layout_id_for(layout);
}

void nrq_gc_set_inline_debug_alloc(int32_t enabled) {
  HeapLockGuard lock(gc_mutex);
  inline_debug_alloc_enabled.store(enabled != 0, std::memory_order_relaxed);
  refresh_inline_config_locked();
}

void nrq_gc_set_tlab_diag(int32_t enabled) {
  tlab_diag.enabled.store(enabled != 0, std::memory_order_relaxed);
}

struct nrq_gc_tlab_diag_facts nrq_gc_get_tlab_diag_facts(void) {
  std::lock_guard<std::mutex> registry_lock(mutator_mutex);
  uint64_t live_pending = 0;
  for (const MutatorState* mutator : mutators) {
    if (!mutator || !mutator->fast) continue;
    live_pending = saturated_add(
        live_pending, std::atomic_ref<size_t>(mutator->fast->pending_count)
                          .load(std::memory_order_relaxed));
  }
  const uint64_t total_tlab_objects = saturated_add(
      tlab_diag.published_tlab_objects.load(std::memory_order_relaxed),
      live_pending);
  const uint64_t slow_tlab =
      tlab_diag.slow_tlab_handled.load(std::memory_order_relaxed);

  nrq_gc_tlab_diag_facts facts{};
  facts.inline_fast_hits =
      total_tlab_objects >= slow_tlab ? total_tlab_objects - slow_tlab : 0;
  facts.slow_planned_calls =
      tlab_diag.slow_planned_calls.load(std::memory_order_relaxed);
  facts.slow_tlab_handled = slow_tlab;
  facts.slow_locked_fallbacks =
      tlab_diag.slow_locked_fallbacks.load(std::memory_order_relaxed);
  facts.refill_count =
      tlab_diag.refill_count.load(std::memory_order_relaxed);
  facts.spare_run_hits =
      tlab_diag.spare_run_hits.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> site_lock(tlab_diag.site_mutex);
    facts.site_count = tlab_diag.site_count;
  }
  facts.reason_cold_site_init =
      tlab_diag.reason_cold_site_init.load(std::memory_order_relaxed);
  facts.reason_gc_active =
      tlab_diag.reason_gc_active.load(std::memory_order_relaxed);
  facts.reason_collection_due =
      tlab_diag.reason_collection_due.load(std::memory_order_relaxed);
  facts.reason_debug_alloc_gate =
      tlab_diag.reason_debug_alloc_gate.load(std::memory_order_relaxed);
  facts.reason_diag_roots_gate =
      tlab_diag.reason_diag_roots_gate.load(std::memory_order_relaxed);
  facts.reason_plan_ineligible =
      tlab_diag.reason_plan_ineligible.load(std::memory_order_relaxed);
  facts.reason_accounting_batch =
      tlab_diag.reason_accounting_batch.load(std::memory_order_relaxed);
  facts.reason_tlab_kind_mismatch =
      tlab_diag.reason_tlab_kind_mismatch.load(std::memory_order_relaxed);
  facts.reason_tlab_exhausted =
      tlab_diag.reason_tlab_exhausted.load(std::memory_order_relaxed);
  facts.reason_other = tlab_diag.reason_other.load(std::memory_order_relaxed);
  return facts;
}

void nrq_gc_reset_tlab_diag_facts(void) {
  tlab_diag.published_tlab_objects.store(0, std::memory_order_relaxed);
  tlab_diag.slow_planned_calls.store(0, std::memory_order_relaxed);
  tlab_diag.slow_tlab_handled.store(0, std::memory_order_relaxed);
  tlab_diag.slow_locked_fallbacks.store(0, std::memory_order_relaxed);
  tlab_diag.refill_count.store(0, std::memory_order_relaxed);
  tlab_diag.spare_run_hits.store(0, std::memory_order_relaxed);
  tlab_diag.reason_cold_site_init.store(0, std::memory_order_relaxed);
  tlab_diag.reason_gc_active.store(0, std::memory_order_relaxed);
  tlab_diag.reason_collection_due.store(0, std::memory_order_relaxed);
  tlab_diag.reason_debug_alloc_gate.store(0, std::memory_order_relaxed);
  tlab_diag.reason_diag_roots_gate.store(0, std::memory_order_relaxed);
  tlab_diag.reason_plan_ineligible.store(0, std::memory_order_relaxed);
  tlab_diag.reason_accounting_batch.store(0, std::memory_order_relaxed);
  tlab_diag.reason_tlab_kind_mismatch.store(0, std::memory_order_relaxed);
  tlab_diag.reason_tlab_exhausted.store(0, std::memory_order_relaxed);
  tlab_diag.reason_other.store(0, std::memory_order_relaxed);
  std::lock_guard<std::mutex> site_lock(tlab_diag.site_mutex);
  tlab_diag.site_count = 0;
}

void nrq_gc_set_region_limit(size_t bytes) {
  // 0 means "no budget", which is the default and the behaviour of every program
  // written before the pacemaker existed.
  gc_region_limit.store(bytes, std::memory_order_relaxed);
  pacemaker_budget_declared.store(bytes != 0, std::memory_order_relaxed);
}

void nrq_gc_set_trigger_percent(uint32_t percent) {
  gc_trigger_percent.store(std::clamp<uint32_t>(percent, 1u, 100u),
                           std::memory_order_relaxed);
}

struct nrq_gc_region_stats nrq_gc_get_region_stats(void) {
  HeapLockGuard lock(gc_mutex);
  nrq_gc_region_stats stats{};
  stats.region_bytes = kGcRegionBytes;
  stats.active_regions = gc_regions.size();
  stats.recycled_regions = recycled_regions.size();
  size_t survivors = 0;
  for (const auto& region : gc_regions) {
    if (region && region->is_survivor) ++survivors;
  }
  stats.survivor_regions = survivors;
  stats.region_limit = gc_region_limit.load(std::memory_order_relaxed);
  stats.trigger_percent = gc_trigger_percent.load(std::memory_order_relaxed);
  stats.evacuation_threshold_percent =
      gc_evacuation_threshold_percent.load(std::memory_order_relaxed);
  stats.evacuated_regions = region_telemetry.evacuated_regions;
  stats.evacuated_objects = region_telemetry.evacuated_objects;
  stats.evacuated_bytes = region_telemetry.evacuated_bytes;
  stats.bulk_reclaimed_regions = region_telemetry.bulk_reclaimed_regions;
  stats.pinned_region_fallbacks = region_telemetry.pinned_region_fallbacks;
  return stats;
}

void nrq_gc_reset_region_stats(void) {
  HeapLockGuard lock(gc_mutex);
  region_telemetry = RegionTelemetry{};
}

}  // extern "C"

namespace {
void print_tlab_diag_report() {
  const nrq_gc_tlab_diag_facts f = nrq_gc_get_tlab_diag_facts();
  const nrq_gc_cycle_timing t = nrq_gc_get_cycle_timing();
  const uint64_t total_planned = f.inline_fast_hits + f.slow_planned_calls;
  const double hit_pct = total_planned > 0
                             ? (100.0 * static_cast<double>(f.inline_fast_hits)) /
                                   static_cast<double>(total_planned)
                             : 0.0;
  std::fprintf(
      stderr,
      "[NRQ_GC_TLAB_DIAG] inline_hits=%llu slow_calls=%llu hit_rate=%.4f%% "
      "slow_tlab=%llu slow_locked=%llu refills=%llu spare_hits=%llu sites=%llu "
      "reasons={cold=%llu gc_active=%llu due=%llu dbg=%llu diag=%llu inel=%llu "
      "batch=%llu kind=%llu exhaust=%llu other=%llu}\n",
      static_cast<unsigned long long>(f.inline_fast_hits),
      static_cast<unsigned long long>(f.slow_planned_calls), hit_pct,
      static_cast<unsigned long long>(f.slow_tlab_handled),
      static_cast<unsigned long long>(f.slow_locked_fallbacks),
      static_cast<unsigned long long>(f.refill_count),
      static_cast<unsigned long long>(f.spare_run_hits),
      static_cast<unsigned long long>(f.site_count),
      static_cast<unsigned long long>(f.reason_cold_site_init),
      static_cast<unsigned long long>(f.reason_gc_active),
      static_cast<unsigned long long>(f.reason_collection_due),
      static_cast<unsigned long long>(f.reason_debug_alloc_gate),
      static_cast<unsigned long long>(f.reason_diag_roots_gate),
      static_cast<unsigned long long>(f.reason_plan_ineligible),
      static_cast<unsigned long long>(f.reason_accounting_batch),
      static_cast<unsigned long long>(f.reason_tlab_kind_mismatch),
      static_cast<unsigned long long>(f.reason_tlab_exhausted),
      static_cast<unsigned long long>(f.reason_other));
  {
    std::lock_guard<std::mutex> site_lock(tlab_diag.site_mutex);
    for (size_t i = 0; i < tlab_diag.site_count; ++i) {
      const auto& s = tlab_diag.sites[i];
      std::fprintf(
          stderr,
          "[NRQ_GC_TLAB_SITE %zu] cache=%p elem_bytes=%zu last_count=%zu "
          "expected_kind=%u slow_calls=%llu\n",
          i, static_cast<void*>(s.cache), s.element_size, s.last_count,
          s.expected_kind, static_cast<unsigned long long>(s.slow_calls));
    }
  }
  std::fprintf(
      stderr,
      "[NRQ_GC_TLAB_TIMING] slow_ms=%.2f publish_ms=%.2f install_memset_ms=%.2f\n",
      static_cast<double>(tlab_diag.slow_ns.load(std::memory_order_relaxed)) /
          1e6,
      static_cast<double>(
          tlab_diag.publish_ns.load(std::memory_order_relaxed)) /
          1e6,
      static_cast<double>(
          tlab_diag.install_memset_ns.load(std::memory_order_relaxed)) /
          1e6);
  std::fprintf(
      stderr,
      "[NRQ_GC_CYCLE_TIMING] cycles=%llu pause_ms=%.2f prepare_ms=%.2f "
      "mark_ms=%.2f sweep_ms=%.2f finish_ms=%.2f mark_objs=%llu mark_refs=%llu "
      "sweep_objs=%llu\n",
      static_cast<unsigned long long>(t.cycles),
      static_cast<double>(t.pause_ns) / 1e6,
      static_cast<double>(t.prepare_ns) / 1e6,
      static_cast<double>(t.mark_ns) / 1e6,
      static_cast<double>(t.sweep_ns) / 1e6,
      static_cast<double>(t.finish_ns) / 1e6,
      static_cast<unsigned long long>(t.mark_objects),
      static_cast<unsigned long long>(t.mark_references),
      static_cast<unsigned long long>(t.sweep_objects));
}
}  // namespace