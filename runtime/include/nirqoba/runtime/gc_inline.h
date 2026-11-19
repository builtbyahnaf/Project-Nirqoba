// gc_inline.h — shared ABI between compiler-inlined TLAB allocation and GC.
//
// Defines the read-mostly global gate (`nrq_gc_inline_config`) and the
// per-thread bump-pointer buffer (`nrq_gc_fast_tls`) used by compiler-generated
// fast paths to allocate H0/H8/H16 objects directly without entering the
// runtime dispatcher. Every field offset is part of the compiled-code ABI and is
// static_asserted in runtime/src/gc.cpp.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nrq_object_layout;

enum {
  NRQ_GC_INLINE_PENDING_CAPACITY = 32,
  NRQ_GC_INLINE_KIND_NONE = 0,
  NRQ_GC_INLINE_KIND_COMPACT = 1,
  NRQ_GC_INLINE_KIND_EXTENDED = 2,
  NRQ_GC_INLINE_TLAB_BYTES = 65536,
  NRQ_GC_INLINE_SPAN_ALIGNMENT = 8,
  NRQ_GC_INLINE_GATE_PHASE_BIT = 1u << 0,
  NRQ_GC_INLINE_GATE_DUE_BIT = 1u << 1,
  NRQ_GC_INLINE_GATE_DEBUG_ALLOC_BIT = 1u << 2,
};

typedef struct NrqGcPendingRange {
  void* page;
  uint8_t* begin;
  uint8_t* end;
} NrqGcPendingRange;

// Read-mostly collector configuration checked on every inline fast path.
// `gate` and `initial_state` sit adjacent in the low 64 bits so compiled code
// can load both in a single 64-bit acquire load on x86-64.
// `gate` is 0 iff the collector is Idle, automatic collection is not yet due,
// and debug allocation event tracing is disabled.
typedef struct NrqGcInlineConfig {
  uint32_t gate;
  uint32_t initial_state;
  size_t accounting_batch;
} NrqGcInlineConfig;

// Per-thread fast allocation state. Hot fields for H8/H16 bump allocation sit
// within the first 64-byte cache line (offsets 0..63).
// Zero-initialization leaves `tlab_kind == NRQ_GC_INLINE_KIND_NONE` and
// `h0_fast_layout == NULL`, routing unregistered threads to the slow path.
typedef struct NrqGcFastTls {
  uint8_t* tlab_cursor;
  uint8_t* tlab_end;
  NrqGcPendingRange* tlab_active_range;
  void* tlab_page;
  uint32_t tlab_kind;
  uint8_t tlab_payload_zero;
  uint8_t h0_payload_zero;
  uint8_t reserved0[2];
  size_t pending_bytes;
  size_t pending_count;
  size_t unflushed_bytes;
  uint8_t* h0_cursor;
  uint8_t* h0_end;
  NrqGcPendingRange* h0_active_range;
  void* h0_page;
  const void* h0_fast_layout;
  uint8_t* h0_state_cursor;
  uint32_t pending_range_count;
  uint32_t reserved1;
  size_t h0_slot_bytes;
  NrqGcPendingRange pending_ranges[NRQ_GC_INLINE_PENDING_CAPACITY];
} NrqGcFastTls;

#if defined(_MSC_VER)
#define NRQ_THREAD_LOCAL __declspec(thread)
#elif defined(__cplusplus)
#define NRQ_THREAD_LOCAL thread_local
#else
#define NRQ_THREAD_LOCAL _Thread_local
#endif

extern NrqGcInlineConfig nrq_gc_inline_config;
extern NRQ_THREAD_LOCAL NrqGcFastTls nrq_gc_fast_tls;

// Resolves a validated layout descriptor to its 32-bit header layout ID
// (0 on null/error). Called only on the cold first-site initialization path.
uint32_t nrq_gc_layout_id(const struct nrq_object_layout* layout);

// Mirrors NRQ_GC_EVENT_ALLOCATION selection into `nrq_gc_inline_config.gate`
// so development builds never skip debug allocation event emission.
void nrq_gc_set_inline_debug_alloc(int32_t enabled);

#ifdef __cplusplus
}  // extern "C"

namespace nirq::gc::inline_detail {

inline void commit_inline_bump(NrqGcFastTls& tls, uint8_t* new_cursor,
                               void* payload, size_t payload_bytes,
                               uint8_t payload_is_zero, void** root_slot) {
  if (!payload_is_zero) memset(payload, 0, payload_bytes);
  tls.pending_bytes += payload_bytes;
  ++tls.pending_count;
  tls.unflushed_bytes += payload_bytes;
  if (root_slot) *root_slot = payload;
  (void)new_cursor;
}

inline void* try_allocate_tlab_inline(uint32_t expected_kind, size_t header_bytes,
                                      uint32_t layout_id, uint32_t elements,
                                      size_t payload_bytes, size_t span,
                                      void** root_slot) {
  if (layout_id == 0 || span == 0 || span > NRQ_GC_INLINE_TLAB_BYTES)
    return nullptr;
  const NrqGcInlineConfig config = nrq_gc_inline_config;
  if (config.gate != 0) return nullptr;
  NrqGcFastTls& tls = nrq_gc_fast_tls;
  if (tls.tlab_kind != expected_kind) return nullptr;
  if (tls.unflushed_bytes > SIZE_MAX - payload_bytes ||
      tls.unflushed_bytes + payload_bytes >= config.accounting_batch)
    return nullptr;
  uint8_t* cursor = tls.tlab_cursor;
  uint8_t* end = tls.tlab_end;
  if (static_cast<size_t>(end - cursor) < span) return nullptr;
  uint8_t* new_cursor = cursor + span;
  tls.tlab_cursor = new_cursor;
  if (header_bytes == 8) {
    uint32_t* header = reinterpret_cast<uint32_t*>(cursor);
    header[0] = config.initial_state;
    header[1] = layout_id;
  } else {
    uint32_t* header = reinterpret_cast<uint32_t*>(cursor);
    header[0] = elements;
    header[1] = config.initial_state;
    header[2] = layout_id;
    header[3] = 0;
  }
  void* payload = cursor + header_bytes;
  commit_inline_bump(tls, new_cursor, payload, payload_bytes,
                     tls.tlab_payload_zero, root_slot);
  return payload;
}

}  // namespace nirq::gc::inline_detail

inline void* nrq_gc_inline_try_allocate_h8(uint32_t layout_id,
                                           size_t payload_bytes, size_t span,
                                           void** root_slot) {
  return nirq::gc::inline_detail::try_allocate_tlab_inline(
      NRQ_GC_INLINE_KIND_COMPACT, 8, layout_id, 1, payload_bytes, span,
      root_slot);
}

inline void* nrq_gc_inline_try_allocate_h16(uint32_t layout_id,
                                            uint32_t elements,
                                            size_t payload_bytes, size_t span,
                                            void** root_slot) {
  return nirq::gc::inline_detail::try_allocate_tlab_inline(
      NRQ_GC_INLINE_KIND_EXTENDED, 16, layout_id, elements, payload_bytes,
      span, root_slot);
}

inline void* nrq_gc_inline_try_allocate_h0(const void* layout,
                                           size_t payload_bytes,
                                           size_t slot_bytes,
                                           void** root_slot) {
  if (!layout || slot_bytes == 0) return nullptr;
  const NrqGcInlineConfig config = nrq_gc_inline_config;
  if (config.gate != 0) return nullptr;
  NrqGcFastTls& tls = nrq_gc_fast_tls;
  if (tls.h0_fast_layout != layout || tls.h0_slot_bytes != slot_bytes)
    return nullptr;
  if (tls.unflushed_bytes > SIZE_MAX - payload_bytes ||
      tls.unflushed_bytes + payload_bytes >= config.accounting_batch)
    return nullptr;
  uint8_t* cursor = tls.h0_cursor;
  uint8_t* end = tls.h0_end;
  if (static_cast<size_t>(end - cursor) < slot_bytes) return nullptr;
  uint8_t* new_cursor = cursor + slot_bytes;
  tls.h0_cursor = new_cursor;
  *tls.h0_state_cursor++ =
      static_cast<uint8_t>((config.initial_state >> 1) & 1u);
  nirq::gc::inline_detail::commit_inline_bump(
      tls, new_cursor, cursor, payload_bytes, tls.h0_payload_zero, root_slot);
  return cursor;
}
#endif
