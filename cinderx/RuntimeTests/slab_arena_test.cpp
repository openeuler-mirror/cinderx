// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/slab_arena.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstring>

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
