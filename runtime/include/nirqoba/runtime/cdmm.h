// Explicit CDMM strategies: arena and fixed-slot pool.
//
// These are the deterministic-lifetime strategies that sit beside the collector
// and beside ordinary owned allocation. Neither carries an object header: an
// arena bumps through chunks it owns, and a pool derives the layout, slot size
// and liveness of every object from the chunk that contains it. That is the same
// page-dedicated reasoning Phase 4 uses for the headerless GC representation
// (H0), applied to explicitly managed storage.
//
// Lifetime contract, in one place:
//   * Every arena has a generation. Reset and destroy advance it, so a caller
//     that stores the generation beside a pointer can reject a reference that
//     outlived its arena with `nrq_cdmm_arena_generation`.
//   * Reset and destroy are refused while a scope is open on the arena or while
//     an allocation is in flight; the failure return leaves the arena untouched.
//   * Destructors registered with `nrq_cdmm_arena_allocate_destructible` run in
//     reverse allocation order on reset and destroy, so a nested owned resource
//     is destroyed before the object that owns it.
//   * Pool slots keep a 32-bit allocation generation when the pool is created
//     with `NRQ_CDMM_POOL_GENERATIONS`. `nrq_cdmm_pool_release_checked` then
//     rejects a release of a slot that has since been handed to another
//     allocation, which a plain address and a free bitmap cannot distinguish.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs on reset/destroy (arena) or release/reset/destroy (pool). The object is
// still readable; its storage is released after the call returns. A destructor
// must not allocate from the arena or pool it is being destroyed with.
typedef void (*NrqCdmmDestructor)(void* object, void* context);

typedef enum NrqCdmmStrategy {
  NRQ_CDMM_STRATEGY_NONE = 0,
  NRQ_CDMM_STRATEGY_ARENA = 1,
  NRQ_CDMM_STRATEGY_POOL = 2
} NrqCdmmStrategy;

// Pool creation flag: keep a 32-bit generation per slot. Costs four bytes per
// slot; without it a reused address and a stale pointer are indistinguishable.
#define NRQ_CDMM_POOL_GENERATIONS 1u

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

// `initial_bytes` is the first chunk and the size used for growth chunks;
// `max_bytes` caps the total committed bytes, and zero means no cap. A request
// that cannot be served without exceeding the cap returns null.
void* nrq_cdmm_arena_create(size_t initial_bytes, size_t max_bytes);
// Runs the registered destructors, releases every chunk, and invalidates the
// handle. Returns zero when the handle is unknown or a scope is still open.
int32_t nrq_cdmm_arena_destroy(void* arena);
// Runs the registered destructors in reverse allocation order and rewinds the
// arena so all but the first chunk is released. Returns zero when the handle is
// unknown or a scope is still open.
int32_t nrq_cdmm_arena_reset(void* arena);
void* nrq_cdmm_arena_allocate(void* arena, size_t bytes, size_t alignment);
void* nrq_cdmm_arena_allocate_destructible(void* arena, size_t bytes,
                                           size_t alignment,
                                           NrqCdmmDestructor destructor,
                                           void* context);
// Allocates `count` blocks and stores the successful prefix in `output`. Stops
// at the first failure, so the return value is the number of blocks written.
size_t nrq_cdmm_arena_allocate_bulk(void* arena, size_t bytes, size_t alignment,
                                    size_t count, void** output);
// Bytes handed out in the current generation, and bytes committed from the heap.
uint64_t nrq_cdmm_arena_used(const void* arena);
uint64_t nrq_cdmm_arena_committed(const void* arena);
// Advances on every successful reset. A pointer plus the generation it was
// allocated under can be validated after a reset.
uint64_t nrq_cdmm_arena_generation(const void* arena);
int32_t nrq_cdmm_arena_contains(const void* arena, const void* pointer);

// A scope pins the arena and hands out its allocation path without touching the
// registry on each call. A scope belongs to one thread and must be the only user
// of its arena while it is open; reset and destroy are refused while it is open.
typedef struct NrqCdmmArenaScope {
  void* arena;
  void* state;
  uint64_t generation;
} NrqCdmmArenaScope;

// Returns zero when the handle is unknown or already pinned by another scope.
int32_t nrq_cdmm_arena_begin(void* arena, NrqCdmmArenaScope* scope);
void* nrq_cdmm_arena_scope_allocate(NrqCdmmArenaScope* scope, size_t bytes,
                                    size_t alignment);
void* nrq_cdmm_arena_scope_allocate_destructible(NrqCdmmArenaScope* scope,
                                                 size_t bytes, size_t alignment,
                                                 NrqCdmmDestructor destructor,
                                                 void* context);
size_t nrq_cdmm_arena_scope_allocate_bulk(NrqCdmmArenaScope* scope,
                                          size_t bytes, size_t alignment,
                                          size_t count, void** output);
void nrq_cdmm_arena_end(NrqCdmmArenaScope* scope);

// ---------------------------------------------------------------------------
// Pool
// ---------------------------------------------------------------------------

// `slot_bytes` is the payload size of every slot; `slot_alignment` must be a
// power of two and divides the stride. `slots_per_chunk` of zero selects a
// chunk of at most 64 KiB. The optional destructor runs on every release, and
// on every live slot during reset and destroy, which keeps per-slot metadata to
// one liveness bit plus the optional generation word.
void* nrq_cdmm_pool_create(size_t slot_bytes, size_t slot_alignment,
                           size_t slots_per_chunk, uint32_t flags,
                           NrqCdmmDestructor destructor, void* context);
int32_t nrq_cdmm_pool_reset(void* pool);
int32_t nrq_cdmm_pool_destroy(void* pool);
void* nrq_cdmm_pool_allocate(void* pool);
size_t nrq_cdmm_pool_allocate_bulk(void* pool, size_t count, void** output);
// Returns zero for an unknown pool, a foreign address, or a double release. The
// slot is left in its previous state. A scope must be the only user of its pool
// while it is open, so a release does not also test for one.
int32_t nrq_cdmm_pool_release(void* pool, void* slot);
// Same, and also rejects `generation` when the slot has been reallocated since
// that generation was handed out.
int32_t nrq_cdmm_pool_release_checked(void* pool, void* slot, uint64_t generation);
size_t nrq_cdmm_pool_release_bulk(void* pool, void* const* slots, size_t count);
// The generation handed out with the slot's current allocation, or zero when the
// pool was created without `NRQ_CDMM_POOL_GENERATIONS` or the slot is foreign.
uint64_t nrq_cdmm_pool_generation(const void* pool, const void* slot);
uint64_t nrq_cdmm_pool_live(const void* pool);
uint64_t nrq_cdmm_pool_committed(const void* pool);
uint64_t nrq_cdmm_pool_slot_bytes(const void* pool);
int32_t nrq_cdmm_pool_contains(const void* pool, const void* pointer);

// A pool scope is the pool's bulk path: one registry lookup and one pin for a
// whole batch of allocations and releases. It carries the same exclusive-use
// rule as an arena scope.
typedef struct NrqCdmmPoolScope {
  void* pool;
  void* state;
} NrqCdmmPoolScope;

int32_t nrq_cdmm_pool_begin(void* pool, NrqCdmmPoolScope* scope);
void* nrq_cdmm_pool_scope_allocate(NrqCdmmPoolScope* scope);
size_t nrq_cdmm_pool_scope_allocate_bulk(NrqCdmmPoolScope* scope, size_t count,
                                         void** output);
int32_t nrq_cdmm_pool_scope_release(NrqCdmmPoolScope* scope, void* slot);
size_t nrq_cdmm_pool_scope_release_bulk(NrqCdmmPoolScope* scope,
                                        void* const* slots, size_t count);
void nrq_cdmm_pool_end(NrqCdmmPoolScope* scope);

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

// Which explicit strategy owns `pointer`. Returns NONE for unallocated memory
// and for collector-managed objects: the two metadata systems never claim each
// other's storage.
NrqCdmmStrategy nrq_cdmm_strategy_of(const void* pointer);

// Page-level facts about the object that owns `pointer`. `header_bytes` is zero
// for both explicit strategies, because slot size and layout come from the
// chunk; `object_bytes` is the pool slot size and zero for a variable-size
// arena. `generation` is the arena generation or the pool slot generation.
typedef struct NrqCdmmFacts {
  NrqCdmmStrategy strategy;
  uint64_t header_bytes;
  uint64_t object_bytes;
  uint64_t generation;
  uint64_t metadata_bytes;
} NrqCdmmFacts;

int32_t nrq_cdmm_object_facts(const void* pointer, NrqCdmmFacts* output);

#ifdef __cplusplus
}
#endif
