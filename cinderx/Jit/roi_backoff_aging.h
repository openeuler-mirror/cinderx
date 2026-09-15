// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstdint>
#include <limits>

namespace jit {

struct RoiAgingUpdate {
  uint32_t count;
  uint64_t epoch_ms;
  uint32_t count_reduced;
};

// Advance an unfrozen ROI counter and include the current counted deopt.
// This is pure arithmetic: the caller supplies steady-clock time and holds the
// GIL or FreeThreadedJITEntrypointGuard across reading and publishing the
// state. No timer runs on successful compiled calls. An interval of zero
// preserves the supplied epoch and performs only a saturating increment.
inline RoiAgingUpdate advanceRoiAging(
    uint32_t count,
    uint64_t epoch_ms,
    uint64_t now_ms,
    uint32_t interval_ms) {
  const uint32_t old_count = count;
  if (interval_ms != 0) {
    if (count == 0) {
      // A zero count denotes a fresh backoff round; timestamp zero itself is
      // valid and must not be used as the initialization sentinel.
      epoch_ms = now_ms;
    } else if (now_ms >= epoch_ms) {
      const uint64_t periods = (now_ms - epoch_ms) / interval_ms;
      count = periods >= std::numeric_limits<uint32_t>::digits
          ? 0
          : count >> periods;
      // Keep the unconsumed fraction of a period. Resetting to now_ms on each
      // deopt would prevent aging when events keep arriving within a period.
      epoch_ms += periods * interval_ms;
    }
    // A backwards timestamp does not decay history or move the epoch back.
  }
  const uint32_t count_reduced = old_count - count;
  if (count != std::numeric_limits<uint32_t>::max()) {
    ++count;
  }
  return {count, epoch_ms, count_reduced};
}

} // namespace jit
