// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/slab.h"
#include "cinderx/Common/util.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

namespace jit {

// The fail-point step simulating the free list's own growth failing
// (the seam itself lives with the publication fail-points, Common/code.h).
inline constexpr int kSlabFreeListFailpointStep = 12;

template <class T>
struct ObjectSizeTrait {
  static constexpr size_t size() {
    return roundUp(sizeof(T), alignof(T));
  }
};

template <typename T, size_t kSlabSize>
class SlabArenaIterator {
 public:
  SlabArenaIterator() = default;

  explicit SlabArenaIterator(std::vector<Slab<T, kSlabSize>>* slabs)
      : slabs_{slabs} {
    if (slabs_ == nullptr) {
      return;
    }
    JIT_CHECK(slabs_->size() > 0, "Unexpected empty slabs list");
    slab_ = slabs_->begin();
    slab_iter_ = currentSlab().begin();
    if (isSlabEnd()) {
      *this = SlabArenaIterator{};
    }
  }

  bool operator==(const SlabArenaIterator& other) const = default;
  bool operator!=(const SlabArenaIterator& other) const = default;

  T& operator*() {
    return *slab_iter_;
  }

  const T& operator*() const {
    return *slab_iter_;
  }

  SlabArenaIterator& operator++() {
    slab_iter_++;
    if (isSlabEnd()) {
      slab_++;
      if (isArenaEnd()) {
        return *this = SlabArenaIterator{};
      }
      slab_iter_ = currentSlab().begin();
      JIT_CHECK(slab_iter_ != currentSlab().end(), "Unexpected empty slab");
    }
    return *this;
  }

  SlabArenaIterator operator++(int) {
    auto ret = *this;
    operator++();
    return ret;
  }

 private:
  bool isArenaEnd() const {
    return slabs_ == nullptr || slab_ == slabs_->end();
  }

  bool isSlabEnd() const {
    return isArenaEnd() || slab_iter_ == slab_->end();
  }

  Slab<T, kSlabSize>& currentSlab() const {
    return *slab_;
  }

  // Store a slab list, iterator to a slab within that list, and an iterator to
  // a position within that slab. Past-the-end and uninitialized iterators will
  // contain all value-initialized members.
  std::vector<Slab<T, kSlabSize>>* slabs_{nullptr};
  typename std::vector<Slab<T, kSlabSize>>::iterator slab_{};
  SlabIterator<T> slab_iter_;
};

// SlabArena is a simple arena allocator, using slabs that are multiples of the
// system's page size. Allocated objects never move after creation, and every
// object lives until the SlabArena is destroyed unless its slot is handed
// back with free(), which recycles it for a later allocate().
//
// It is intended to keep objects of a given type together on the same page,
// either to achieve desired certain copy-on-write behavior, or to mlock() all
// of the objects with minimal collateral damage (which can be managed with
// SlabArena::mlock() and SlabArena::munlock()).
//
// allocate(), mlock(), and munlock() are thread-safe. begin(), end(), and all
// operations on SlabArena::iterator are not thread-safe.
template <
    typename T,
    typename SizeTrait = ObjectSizeTrait<T>,
    size_t kPagesPerSlab = 4>
class SlabArena {
  static constexpr size_t kSlabSize = kPageSize * kPagesPerSlab;
  static_assert(
      sizeof(T) <= kSlabSize,
      "Cannot allocate objects larger than one slab");

 public:
  using iterator = SlabArenaIterator<T, kSlabSize>;

  SlabArena() {
    slabs_.emplace_back(SizeTrait::size());
  }

  // Allocate a new instance of T using the given constructor arguments.
  template <typename... Args>
  T* allocate(Args&&... args) {
    std::lock_guard<std::mutex> guard{mutex_};

#ifndef WIN32
    if (mlocked_) {
      // It's not necessarily an error to allocate after locking but it's
      // probably not what we expect to happen in the common forking case.
      JIT_DLOG("Allocating while locked");
    }
#endif

    // Reuse a freed slot before growing a slab.  The husk occupying it is
    // destroyed only now, so between free() and here it stayed a valid
    // object for iteration and for the slab's own teardown.
    if (!free_list_.empty()) {
      T* slot = free_list_.back();
      free_list_.pop_back();
      slot->~T();
      try {
        return new (slot) T(std::forward<Args>(args)...);
      } catch (...) {
        // The slot no longer holds a constructed object and T offers no
        // generic way to rebuild one after a failed construction.  Every
        // later iteration and the slab's teardown would treat this dead
        // storage as a live T, so fail closed at the fault site instead
        // of amplifying downstream, exactly like the double-recycle
        // check in free().
        //
        // abortNoAlloc, not JIT_ABORT: we are inside a catch handler, and
        // the throwing constructor may have been failing to allocate in
        // the first place (sustained OOM).  JIT_ABORT formats a heap
        // std::string before aborting; if that allocation throws here the
        // new exception escapes this handler, the corrupted slot survives
        // in the arena, and teardown hits UB anyway.  This termination
        // path must never allocate and never throw.
        abortNoAlloc(
            "JIT: construction in a recycled arena slot threw; the arena "
            "can no longer guarantee exactly one constructed object per "
            "slot\n");
      }
    }

    void* mem = slabs_.back().allocate();
    bool grew = false;
    if (mem == nullptr) {
      mem = slabs_.emplace_back(SizeTrait::size()).allocate();
      JIT_CHECK(mem != nullptr, "Empty slab failed to allocate");
      grew = true;
#ifndef WIN32
      if (mlocked_) {
        slabs_.back().mlock();
      }
#endif
    }
    try {
      return new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
      // Unlike a recycled slot, this slot never held a constructed object,
      // so un-advancing fill_ fully restores the one-constructed-object-
      // per-slot invariant and the exception can propagate safely to the
      // caller (callers such as forcedJitVectorcall() already handle it
      // by falling back to interpretation).  Without the rollback the
      // dead slot would sit inside [base_, fill_) and both teardown and
      // iteration would treat uninitialized storage as a live T.
      slabs_.back().rollbackLastAllocate();
      // If this allocation grew the arena, the rolled-back slab is now
      // completely empty and must also leave slabs_: the arena iterator
      // asserts non-empty slabs, so an empty tail slab would abort the
      // next iteration (Context::releaseReferences(),
      // lifecycleSnapshot311()) even though the caller already recovered
      // from the exception.  It is necessarily empty: grew means this was
      // the very first allocate() on it.
      if (grew) {
        slabs_.pop_back();
      }
      throw;
    }
  }

  // Whether the pointer names an allocated slot in this arena.
  bool contains(const T* obj) const {
    std::lock_guard<std::mutex> guard{mutex_};
    for (const auto& slab : slabs_) {
      if (slab.contains(obj)) {
        return true;
      }
    }
    return false;
  }

  // Hand an object's slot back for reuse by a later allocate().  The object
  // is deliberately NOT destroyed here: iteration may still visit the slot,
  // and the slab destroys its current occupant at arena teardown, so the
  // caller must leave the object in a state that is valid to iterate and to
  // destroy (a cleared husk).  Each slot holds exactly one constructed
  // object at all times.
  //
  // Best effort by contract, and noexcept: the callers sit under GC
  // hooks, object destructors and context teardown, where an allocation
  // failure growing the free list must not cross the C API as a C++
  // exception.  On failure the cleared husk simply stays where it is --
  // one lost reuse opportunity, never a correctness event.  Returns
  // whether the slot was actually banked for reuse.
  bool free(T* obj) noexcept {
    std::lock_guard<std::mutex> guard{mutex_};
    // A slot banked twice would hand the same storage to two later
    // allocate() calls -- a second-order fault amplifier for whatever
    // lifecycle bug caused the double recycle.  Recycling is cold, so the
    // linear scan is affordable fail-closed hardening.
    JIT_CHECK(
        std::find(free_list_.begin(), free_list_.end(), obj) ==
            free_list_.end(),
        "slot recycled twice");
    try {
      throwIfJitPublishStepArmedForTest(kSlabFreeListFailpointStep);
      free_list_.push_back(obj);
    } catch (const std::bad_alloc&) {
      return false;
    }
    return true;
  }

#ifndef WIN32
  // Pin the contents to physical memory.
  void mlock() {
    std::lock_guard<std::mutex> guard{mutex_};
    for (auto& slab : slabs_) {
      slab.mlock();
    }
  }

  // Unpin the contents from physical memory.
  void munlock() {
    std::lock_guard<std::mutex> guard{mutex_};
    for (auto& slab : slabs_) {
      slab.munlock();
    }
  }
#endif

  iterator begin() {
    return iterator{&slabs_};
  }

  iterator end() {
    return iterator{};
  }

 private:
  std::vector<Slab<T, kSlabSize>> slabs_;
  // Slots handed back by free(), still holding their husks.
  std::vector<T*> free_list_;
  mutable std::mutex mutex_;
#ifndef WIN32
  bool mlocked_{false};
#endif
};

} // namespace jit
