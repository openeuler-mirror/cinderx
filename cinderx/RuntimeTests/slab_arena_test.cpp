// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstring>
#include <new>
#include <stdexcept>

namespace {

using namespace jit;

// Simple struct that only fits 3 to a page.
struct BigArray {
  std::array<char, kPageSize / 4 + 1> data;
};

void checkData(BigArray* arr, char c) {
  for (size_t i = 0; i < arr->data.size(); i++) {
    ASSERT_EQ(arr->data[i], c) << "i == " << i;
  }
}

} // namespace

TEST(SlabArenaTest, Allocate) {
  // Allocate at least two pages worth of structs and make sure they don't
  // overlap.
  SlabArena<BigArray, ObjectSizeTrait<BigArray>, 1> arena;

  BigArray* a = arena.allocate();
  a->data.fill(0xa);
  BigArray* b = arena.allocate();
  b->data.fill(0xb);
  BigArray* c = arena.allocate();
  c->data.fill(0xc);
  BigArray* d = arena.allocate();
  d->data.fill(0xd);

  EXPECT_NE(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
  EXPECT_NE(b, c);
  EXPECT_NE(b, d);
  EXPECT_NE(c, d);

  EXPECT_NO_FATAL_FAILURE(checkData(a, 0xa));
  EXPECT_NO_FATAL_FAILURE(checkData(b, 0xb));
  EXPECT_NO_FATAL_FAILURE(checkData(c, 0xc));
  EXPECT_NO_FATAL_FAILURE(checkData(d, 0xd));
}

namespace {

class Counter {
 public:
  Counter(int& c) : c_{c} {
    c_++;
  }
  ~Counter() {
    c_--;
  }

 private:
  int& c_;
};

} // namespace

TEST(SlabArenaTest, RunsDestructors) {
  int count = 0;
  {
    SlabArena<Counter, ObjectSizeTrait<Counter>, 1> arena;

    // Create at least two slabs full of structs
    const int kNumElems = kPageSize / sizeof(Counter) * 2;
    for (int i = 0; i < kNumElems; i++) {
      arena.allocate(count);
      ASSERT_EQ(count, i + 1);
    }
  }

  ASSERT_EQ(count, 0);
}

TEST(SlabArenaTest, Iterate) {
  SlabArena<int, ObjectSizeTrait<int>, 1> arena;

  for (UNUSED int value : arena) {
    FAIL() << "Arena should be empty";
  }

  // Create at least two slabs full of ints full of arbitrary data.
  const int kFactor = 3;
  const int kNumElems = kPageSize / sizeof(int) * 2;
  for (int i = 0; i < kNumElems; i++) {
    arena.allocate(i * kFactor);
  }

  int count = 0;
  for (int value : arena) {
    ASSERT_EQ(value, count * kFactor);
    count++;
  }
  ASSERT_EQ(count, kNumElems);
}

namespace {

const int kAlignment = 16;
struct alignas(kAlignment) AlignedStruct {
  int64_t a;
  int64_t b;
  int64_t c;
};

} // namespace

TEST(SlabArenaTest, AllocateWithCorrectAlignment) {
  SlabArena<AlignedStruct> arena;

  auto a = reinterpret_cast<intptr_t>(arena.allocate());
  auto b = reinterpret_cast<intptr_t>(arena.allocate());
  EXPECT_EQ(a, roundUp(a, kAlignment));
  EXPECT_EQ(b, roundUp(b, kAlignment));
}

TEST(SlabArenaTest, ContainsAcceptsOnlyExactSlotAddresses) {
  // The ownership checks built on contains() decide whether a runtime
  // pointer may be dereferenced; an interior pointer must never pass for
  // a live slot.
  SlabArena<BigArray, ObjectSizeTrait<BigArray>, 1> arena;
  BigArray* first = arena.allocate();
  BigArray* second = arena.allocate();
  EXPECT_TRUE(arena.contains(first));
  EXPECT_TRUE(arena.contains(second));
  auto interior = reinterpret_cast<const BigArray*>(
      reinterpret_cast<const char*>(first) + 1);
  EXPECT_FALSE(arena.contains(interior));
  BigArray local;
  EXPECT_FALSE(arena.contains(&local));
}

TEST(SlabArenaTest, RecyclingTheSameSlotTwiceAborts) {
  // A slot banked twice would let two later allocations share storage; a
  // lifecycle bug of that shape must die loudly at the recycle site
  // instead of amplifying downstream.
  SlabArena<BigArray, ObjectSizeTrait<BigArray>, 1> arena;
  BigArray* slot = arena.allocate();
  ASSERT_TRUE(arena.free(slot));
  EXPECT_DEATH(arena.free(slot), "recycled twice");
}

TEST(SlabArenaTest, FreeIsBestEffortAndReusesTheSlot) {
  // free() runs under GC hooks and destructors: an injected banking
  // failure must refuse -- never throw -- and cost exactly one reuse
  // opportunity.
  SlabArena<BigArray, ObjectSizeTrait<BigArray>, 1> arena;
  BigArray* slot = arena.allocate();
  failJitPublishStepForTest(kSlabFreeListFailpointStep);
  EXPECT_FALSE(arena.free(slot));
  failJitPublishStepForTest(0);
  BigArray* fresh = arena.allocate();
  EXPECT_NE(fresh, slot) << "a refused slot must not be reused";
  EXPECT_TRUE(arena.free(slot));
  BigArray* reused = arena.allocate();
  EXPECT_EQ(reused, slot);
}

namespace {

class ThrowOnDemand {
 public:
  explicit ThrowOnDemand(bool fail = false) {
    if (fail) {
      throw std::bad_alloc();
    }
  }
};

} // namespace

TEST(SlabArenaTest, ReuseConstructionFailureAborts) {
  // A constructor failure in a recycled slot leaves the slot holding a
  // destructed object with no generic way to rebuild one; iteration and
  // the slab's teardown would then treat dead storage as a live object.
  // The invariant must die loudly at the fault site instead.
  SlabArena<ThrowOnDemand, ObjectSizeTrait<ThrowOnDemand>, 1> arena;
  ThrowOnDemand* slot = arena.allocate();
  ASSERT_TRUE(arena.free(slot));
  EXPECT_DEATH(arena.allocate(true), "exactly one constructed object");
}

TEST(SlabArenaTest, NewSlotConstructionFailureRollsBackFill) {
  // PR235 review: the fresh-slot path advanced fill_ before running the
  // constructor, so a throwing constructor used to leave an unconstructed
  // slot inside [base_, fill_) that teardown and iteration would treat
  // as a live object.  The rollback restores the invariant and lets the
  // exception propagate, unlike the recycled-slot path above where the
  // dead husk forces a fail-closed abort.
  SlabArena<ThrowOnDemand, ObjectSizeTrait<ThrowOnDemand>, 1> arena;
  EXPECT_THROW(arena.allocate(true), std::bad_alloc);

  // The failed slot is invisible to iteration...
  size_t count = 0;
  for (UNUSED auto& obj : arena) {
    count++;
  }
  EXPECT_EQ(count, 0);

  // ...and the arena keeps working: the next construction succeeds and
  // is visible exactly once.
  ThrowOnDemand* obj = arena.allocate();
  EXPECT_NE(obj, nullptr);
  count = 0;
  for (UNUSED auto& obj2 : arena) {
    count++;
  }
  EXPECT_EQ(count, 1);
}

TEST(SlabArenaTest, GrowthConstructionFailureRemovesEmptySlab) {
  // PR235 review: growing the arena entered a brand-new slab; if the very
  // first construction there throws, rolling fill_ back alone is not
  // enough.  The now-empty slab must leave slabs_ entirely, or the next
  // iteration hits the iterator's "Unexpected empty slab" check and
  // aborts the process even though the caller recovered from the
  // exception.
  SlabArena<ThrowOnDemand, ObjectSizeTrait<ThrowOnDemand>, 1> arena;

  // Fill the first slab completely so the next allocate() must grow.
  const size_t kPerSlab = kPageSize / sizeof(ThrowOnDemand);
  for (size_t i = 0; i < kPerSlab; i++) {
    arena.allocate();
  }

  // First allocation in the fresh slab fails and must unwind the growth.
  EXPECT_THROW(arena.allocate(true), std::bad_alloc);

  // Iteration sees exactly the first slab's objects and does not abort on
  // an empty tail slab.
  size_t count = 0;
  for (UNUSED auto& obj : arena) {
    count++;
  }
  EXPECT_EQ(count, kPerSlab);

  // The arena still works afterwards: growing again succeeds and the new
  // object is visible exactly once.
  ThrowOnDemand* obj = arena.allocate();
  EXPECT_NE(obj, nullptr);
  count = 0;
  for (UNUSED auto& obj2 : arena) {
    count++;
  }
  EXPECT_EQ(count, kPerSlab + 1);
}
