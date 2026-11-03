// Miniature precise mark-and-sweep collector used by the C/C++/Rust
// benchmark tracks.
//
// Purpose: C, C++, and Rust do not ship a tracing garbage collector. Comparing
// their native malloc/free or Box paths against a tracing collector would not
// measure the same algorithm. This header provides the same algorithmic shape
// as the Nirqoba runtime collector for the GC-specific workloads:
//
//   * every object carries a header with a trace callback, a size, a mark bit,
//     an all-objects link, and a free-list link;
//   * roots are explicit object-pointer slots (one root in these benchmarks);
//   * marking is a worklist traversal over precise reference fields supplied by
//     the trace callback;
//   * sweeping is a linear pass over the all-objects list that either clears the
//     mark bit or returns the object to a size-uniform free list;
//   * allocation is a bump pointer inside large blocks, with free-list reuse
//     after a collection.
//
// The object header deliberately mirrors the documented Nirqoba runtime header
// (mark, trace descriptor, size, next) rather than being a minimal test-only
// struct. That keeps the comparison about the language/runtime, not about one
// implementation hiding its metadata in fewer bytes.
#ifndef NIRQOBA_BENCH_STRESS_TEST_2_MINI_GC_H
#define NIRQOBA_BENCH_STRESS_TEST_2_MINI_GC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MiniGcObject MiniGcObject;

// Visits one managed reference. The collector calls the trace callback once per
// object; the callback calls visit for each reference field it owns.
typedef void (*MiniGcVisitFn)(MiniGcObject* referenced_object, void* context);
typedef void (*MiniGcTraceFn)(MiniGcObject* object, MiniGcVisitFn visit,
                              void* context);

struct MiniGcObject {
  MiniGcObject* next_all;   // intrusion into the all-objects list
  MiniGcObject* next_free;  // intrusion into the free list while dead
  MiniGcTraceFn trace;      // precise reference-field visitor
  uint32_t object_bytes;    // total size including this header
  uint32_t marked;          // mark bit
};

typedef struct MiniGcBlock {
  struct MiniGcBlock* next;
  size_t used;
  size_t capacity;
  unsigned char* data;
} MiniGcBlock;

typedef struct MiniGc {
  MiniGcBlock* blocks;
  MiniGcObject* all_objects;
  MiniGcObject* free_list;
  MiniGcObject* root;
  MiniGcObject** mark_stack;
  size_t mark_size;
  size_t mark_capacity;
  size_t live_objects;
  size_t collections;
  size_t total_allocations;
  size_t block_bytes;
} MiniGc;

static void mini_gc_init(MiniGc* gc, size_t block_bytes) {
  memset(gc, 0, sizeof(*gc));
  gc->block_bytes = block_bytes < 4096 ? 4096 : block_bytes;
}

static void mini_gc_destroy(MiniGc* gc) {
  MiniGcBlock* block = gc->blocks;
  while (block != NULL) {
    MiniGcBlock* next = block->next;
    free(block->data);
    free(block);
    block = next;
  }
  free(gc->mark_stack);
  memset(gc, 0, sizeof(*gc));
}

static void mini_gc_mark_push(MiniGc* gc, MiniGcObject* object) {
  if (object == NULL) {
    return;
  }
  if (gc->mark_size == gc->mark_capacity) {
    size_t capacity = gc->mark_capacity == 0 ? 1024 : gc->mark_capacity * 2;
    MiniGcObject** grown =
        (MiniGcObject**)realloc(gc->mark_stack, capacity * sizeof(*grown));
    if (grown == NULL) {
      abort();
    }
    gc->mark_stack = grown;
    gc->mark_capacity = capacity;
  }
  gc->mark_stack[gc->mark_size++] = object;
}

static void mini_gc_mark_visit(MiniGcObject* object, void* context) {
  mini_gc_mark_push((MiniGc*)context, object);
}

// Allocates one zero-initialized object. `bytes` includes sizeof(MiniGcObject)
// plus the caller's payload. The trace callback must visit only reference
// fields, exactly like the Nirqoba runtime contract.
static MiniGcObject* mini_gc_alloc(MiniGc* gc, size_t bytes,
                                   MiniGcTraceFn trace) {
  MiniGcObject* object;
  if (gc->free_list != NULL) {
    object = gc->free_list;
    gc->free_list = object->next_free;
  } else {
    MiniGcBlock* block = gc->blocks;
    if (block == NULL || block->used + bytes > block->capacity) {
      size_t capacity = gc->block_bytes < bytes ? bytes : gc->block_bytes;
      block = (MiniGcBlock*)malloc(sizeof(*block));
      if (block == NULL) {
        abort();
      }
      block->data = (unsigned char*)malloc(capacity);
      if (block->data == NULL) {
        abort();
      }
      block->used = 0;
      block->capacity = capacity;
      block->next = gc->blocks;
      gc->blocks = block;
    }
    object = (MiniGcObject*)(block->data + block->used);
    block->used += bytes;
  }

  memset(object, 0, bytes);
  object->next_all = gc->all_objects;
  gc->all_objects = object;
  object->next_free = NULL;
  object->trace = trace;
  object->object_bytes = (uint32_t)bytes;
  object->marked = 0;
  gc->live_objects++;
  gc->total_allocations++;
  return object;
}

static void mini_gc_set_root(MiniGc* gc, MiniGcObject* root) {
  gc->root = root;
}

// Marks from the single root, then sweeps every unmarked object into the free
// list. Returns the live object count after sweeping.
static size_t mini_gc_collect(MiniGc* gc) {
  gc->mark_size = 0;
  mini_gc_mark_push(gc, gc->root);

  while (gc->mark_size > 0) {
    MiniGcObject* object = gc->mark_stack[--gc->mark_size];
    if (object->marked != 0) {
      continue;
    }
    object->marked = 1;
    if (object->trace != NULL) {
      object->trace(object, mini_gc_mark_visit, gc);
    }
  }

  MiniGcObject** link = &gc->all_objects;
  while (*link != NULL) {
    MiniGcObject* object = *link;
    if (object->marked != 0) {
      object->marked = 0;
      link = &object->next_all;
    } else {
      *link = object->next_all;
      object->next_free = gc->free_list;
      gc->free_list = object;
      gc->live_objects--;
    }
  }

  gc->collections++;
  return gc->live_objects;
}

#endif  // NIRQOBA_BENCH_STRESS_TEST_2_MINI_GC_H
