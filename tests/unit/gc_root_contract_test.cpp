// AOR Phase 8: the lifetime contract of a root slot.
//
// A root slot is an address the program promises to keep valid while it is
// registered. The runtime's job is to attach the right description to that
// address, to refuse a second owner that describes it differently, and to forget
// everything when the last registration goes away. These tests pin the last two:
// a released slot must not keep the descriptor of its previous owner, and a slot
// registered more than once needs exactly as many releases.
//
// The hazard this guards is address reuse: a slot on a reused stack frame can
// have the same address as another function's slot. If the descriptor outlived
// the registration, the second owner's object would be traced with the first
// owner's offsets, which is a stale-reference bug the collector cannot see.

#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "nirqoba/runtime/gc.h"

namespace {

struct Leaf {
  uint64_t value;
};

struct Node {
  void* child;
  uint64_t value;
};

// A slot with its own storage, so its address is never a reused stack address
// unless a test asks for that.
class Slot {
 public:
  explicit Slot(void* initial = nullptr) : storage_(new void*(initial)) {}
  ~Slot() { delete storage_; }
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;

  void** address() { return storage_; }
  void* value() const { return *storage_; }

 private:
  void** storage_;
};

const size_t kChildOffset[] = {0};
const size_t kValueOffset[] = {8};

void clear_heap() {
  nrq_gc_disable();
  nrq_gc_collect();
  nrq_gc_collect();
}

}  // namespace

TEST(GcRootContractTest, ReRegisteringASlotAfterReleaseUsesTheNewDescriptor) {
  // `object_size` describes the *root storage*, so a root record can hold more
  // words than the descriptor names. This test uses a two-word record and
  // registers each word in turn: the second registration must scan the second
  // word only, even though the first word still holds a pointer.
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();

  std::unique_ptr<void*[]> record(new void*[2]{nullptr, nullptr});
  void* first_object = nrq_gc_allocate(sizeof(Leaf), nullptr);
  void* second_object = nrq_gc_allocate(sizeof(Leaf), nullptr);
  ASSERT_NE(first_object, nullptr);
  ASSERT_NE(second_object, nullptr);
  record[0] = first_object;
  record[1] = second_object;

  // First owner: word 0 is the root, word 1 is not.
  nrq_gc_root_register_typed(record.get(), 2 * sizeof(void*), kChildOffset, 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(first_object), int32_t{1});
  ASSERT_EQ(nrq_gc_pointer_is_managed(second_object), int32_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});

  // The first owner is gone. Nothing about its descriptor may survive it.
  nrq_gc_root_unregister(reinterpret_cast<void**>(record.get()));

  // Second owner, same address, the other word. The first object is still alive
  // at this point because the old descriptor was keeping it alive: if that
  // descriptor survived this registration, the first object would stay alive
  // and this assertion would fail. The second word gets a fresh object, because
  // the original one in it was already collected.
  void* third_object = nrq_gc_allocate(sizeof(Leaf), nullptr);
  ASSERT_NE(third_object, nullptr);
  record[1] = third_object;
  nrq_gc_root_register_typed(record.get(), 2 * sizeof(void*), kValueOffset, 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(third_object), int32_t{1});
  ASSERT_EQ(nrq_gc_pointer_is_managed(first_object), int32_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});

  nrq_gc_root_unregister(reinterpret_cast<void**>(record.get()));
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(third_object), int32_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  nrq_gc_enable();
}

TEST(GcRootContractTest, ASlotRegisteredTwiceNeedsTwoReleases) {
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();
  Slot slot;
  void* node = nrq_gc_allocate(sizeof(Node), nullptr);
  ASSERT_NE(node, nullptr);
  nrq_gc_root_store(slot.address(), node);

  // Registering the same slot with the same description is a second owner of the
  // same address, not a no-op: the runtime counts it.
  nrq_gc_root_register_typed(slot.address(), sizeof(void*), kChildOffset, 1);
  nrq_gc_root_register_typed(slot.address(), sizeof(void*), kChildOffset, 1);
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(node), int32_t{1});

  // One release leaves the other owner's registration in force.
  nrq_gc_root_unregister(slot.address());
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(node), int32_t{1});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{1});

  // The last release hands the object back to the collector alone.
  nrq_gc_root_unregister(slot.address());
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_pointer_is_managed(node), int32_t{0});
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  nrq_gc_enable();
}

TEST(GcRootContractTest, AReusedAddressWithTheSameDescriptorIsAValidOwner) {
  // The common case the hazard report was about: a function returns, its stack
  // slot is released, and the next call to the same function lands on the same
  // address with the same description. That must work, repeatedly.
  clear_heap();
  const uint64_t base = nrq_gc_get_live_objects();
  // The node declares its first word as a reference, so the leaf is reachable
  // only through the chain the marker follows from the slot.
  const nrq_object_layout* layout =
      nrq_gc_layout_intern(sizeof(Node), 0, nullptr, kChildOffset, 1);
  ASSERT_NE(layout, nullptr);
  Slot slot;
  for (int round = 0; round < 64; ++round) {
    void* leaf = nrq_gc_allocate(sizeof(Leaf), nullptr);
    void* node = nrq_gc_allocate_layout(layout);
    ASSERT_NE(leaf, nullptr);
    ASSERT_NE(node, nullptr);
    static_cast<Node*>(node)->child = leaf;
    nrq_gc_root_store(slot.address(), node);
    nrq_gc_root_register_typed(slot.address(), sizeof(void*), kChildOffset, 1);
    nrq_gc_collect();
    ASSERT_EQ(nrq_gc_pointer_is_managed(leaf), int32_t{1});
    ASSERT_EQ(nrq_gc_pointer_is_managed(node), int32_t{1});
    nrq_gc_root_store(slot.address(), nullptr);
    nrq_gc_root_unregister(slot.address());
  }
  nrq_gc_collect();
  ASSERT_EQ(nrq_gc_get_live_objects() - base, uint64_t{0});
  nrq_gc_enable();
}
