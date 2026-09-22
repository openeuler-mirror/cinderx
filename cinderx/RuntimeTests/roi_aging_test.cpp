// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/roi_backoff_aging.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <chrono>
#include <cstddef>
#include <limits>
#include <thread>

namespace {

using jit::advanceRoiAging;

constexpr uint32_t kIntervalMs = 1000;
constexpr uint32_t kBudget = 32;

// This header is shared with the C interpreter. The appended timestamp must
// remain naturally aligned in GIL and FT builds, including the 3.11 owner
// block.
static_assert(offsetof(CodeExtra, roi_aging_epoch_ms) % alignof(uint64_t) == 0);
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(CodeExtra) == 64);
static_assert(offsetof(CodeExtra, roi_recompile_floor) == 48);
static_assert(offsetof(CodeExtra, roi_aging_epoch_ms) == 56);
#endif

TEST(RoiAgingTest, DefaultIntervalIsOneMinute) {
  EXPECT_EQ(jit::Config{}.roi_aging_interval_ms, 60000);
}

TEST(RoiAgingTest, TimestampZeroIsNotAnInitializationSentinel) {
  auto first = advanceRoiAging(0, 9999, 0, kIntervalMs);
  EXPECT_EQ(first.count, 1);
  EXPECT_EQ(first.epoch_ms, 0);
  auto boundary =
      advanceRoiAging(first.count, first.epoch_ms, 1000, kIntervalMs);
  EXPECT_EQ(boundary.count, 1);
  EXPECT_EQ(boundary.count_reduced, 1);
  EXPECT_EQ(boundary.epoch_ms, 1000);
}

TEST(RoiAgingTest, CountsBeforeBoundaryAndHalvesAtBoundary) {
  auto before = advanceRoiAging(24, 1000, 1999, kIntervalMs);
  EXPECT_EQ(before.count, 25);
  EXPECT_EQ(before.count_reduced, 0);
  EXPECT_EQ(before.epoch_ms, 1000);
  auto boundary = advanceRoiAging(24, 1000, 2000, kIntervalMs);
  EXPECT_EQ(boundary.count, 13);
  EXPECT_EQ(boundary.count_reduced, 12);
  EXPECT_EQ(boundary.epoch_ms, 2000);
  auto after = advanceRoiAging(24, 1000, 2001, kIntervalMs);
  EXPECT_EQ(after.count, 13);
  EXPECT_EQ(after.count_reduced, 12);
  EXPECT_EQ(after.epoch_ms, 2000);
}

TEST(RoiAgingTest, FrequentEventsPreserveThePartialPeriod) {
  auto state = advanceRoiAging(24, 1000, 2500, kIntervalMs);
  EXPECT_EQ(state.count, 13);
  EXPECT_EQ(state.epoch_ms, 2000);
  state = advanceRoiAging(state.count, state.epoch_ms, 2999, kIntervalMs);
  EXPECT_EQ(state.count, 14);
  EXPECT_EQ(state.epoch_ms, 2000);
  state = advanceRoiAging(state.count, state.epoch_ms, 3000, kIntervalMs);
  EXPECT_EQ(state.count, 8);
  EXPECT_EQ(state.count_reduced, 7);
  EXPECT_EQ(state.epoch_ms, 3000);
}

TEST(RoiAgingTest, MultiplePeriodsAndLargeGapsAreBounded) {
  auto two = advanceRoiAging(24, 1000, 3000, kIntervalMs);
  EXPECT_EQ(two.count, 7);
  EXPECT_EQ(two.count_reduced, 18);
  constexpr auto max_count = std::numeric_limits<uint32_t>::max();
  auto thirty_one = advanceRoiAging(max_count, 0, 31000, kIntervalMs);
  EXPECT_EQ(thirty_one.count, 2);
  auto thirty_two = advanceRoiAging(max_count, 0, 32000, kIntervalMs);
  EXPECT_EQ(thirty_two.count, 1);
  EXPECT_EQ(thirty_two.count_reduced, max_count);
  EXPECT_EQ(thirty_two.epoch_ms, 32000);
  constexpr auto max_time = std::numeric_limits<uint64_t>::max();
  auto many = advanceRoiAging(max_count, 1, max_time, kIntervalMs);
  EXPECT_EQ(many.count, 1);
  EXPECT_EQ(many.count_reduced, max_count);
  EXPECT_LE(many.epoch_ms, max_time);
  EXPECT_LT(max_time - many.epoch_ms, kIntervalMs);
}

TEST(RoiAgingTest, CounterSaturatesAndBackwardsTimeDoesNotAge) {
  constexpr auto max_count = std::numeric_limits<uint32_t>::max();
  auto saturated = advanceRoiAging(max_count, 1000, 1999, kIntervalMs);
  EXPECT_EQ(saturated.count, max_count);
  EXPECT_EQ(saturated.count_reduced, 0);
  auto backwards = advanceRoiAging(24, 2000, 1999, kIntervalMs);
  EXPECT_EQ(backwards.count, 25);
  EXPECT_EQ(backwards.count_reduced, 0);
  EXPECT_EQ(backwards.epoch_ms, 2000);
}

TEST(RoiAgingTest, PrebudgetHistoryAgesBeforeNewMismatches) {
  jit::RoiAgingUpdate aged{24, 1000, 0};
  jit::RoiAgingUpdate disabled = aged;
  for (uint32_t i = 0; i < 8; ++i) {
    aged = advanceRoiAging(aged.count, aged.epoch_ms, 2000 + i, kIntervalMs);
    disabled = advanceRoiAging(disabled.count, disabled.epoch_ms, 2000 + i, 0);
  }
  EXPECT_EQ(aged.count, 20);
  EXPECT_LT(aged.count, kBudget);
  EXPECT_EQ(disabled.count, kBudget);
  EXPECT_EQ(disabled.epoch_ms, 1000);
}

TEST(RoiAgingTest, FreshDenseStormStillReachesTheOriginalBudget) {
  jit::RoiAgingUpdate state{0, 0, 0};
  for (uint32_t i = 0; i < kBudget; ++i) {
    state = advanceRoiAging(state.count, state.epoch_ms, 1000 + i, kIntervalMs);
    EXPECT_EQ(state.count, i + 1);
    EXPECT_EQ(state.count_reduced, 0);
  }
}

TEST(RoiAgingTest, BurstAcrossOnePeriodStillHasABoundedBudget) {
  // The prior interval boundary falls inside this burst. A single halving
  // can delay exhaustion, but cannot permit 2*budget events without exhaustion.
  jit::RoiAgingUpdate state{1, 0, 0};
  bool exhausted = false;
  for (uint32_t i = 0; i < 2 * kBudget; ++i) {
    state = advanceRoiAging(state.count, state.epoch_ms, 990 + i, kIntervalMs);
    if (state.count >= kBudget) {
      exhausted = true;
      break;
    }
  }
  EXPECT_TRUE(exhausted);
}

class RoiAgingRuntimeTest : public RuntimeTest {};

// These callback tests validate policy plumbing with a CodeRuntime, not
// natural guard generation. Natural-input execution is tested separately.
TEST_F(RoiAgingRuntimeTest, FreshCodeExtraIsZeroInitialized) {
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(func, nullptr);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 0);
  EXPECT_EQ(extra->roi_aging_epoch_ms, 0);
}

TEST_F(RoiAgingRuntimeTest, EnabledAgingInitializesAndPublishesTheEpoch) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.roi_backoff_enabled = true;
  config.compile_after_n_calls = 100;
  config.roi_deopt_budget_base = 100;
  config.roi_aging_interval_ms = std::numeric_limits<uint32_t>::max();
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(func, nullptr);
  jit::CodeRuntime runtime(func);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);
  const auto before = std::chrono::steady_clock::now();
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, false);
  const auto after = std::chrono::steady_clock::now();
  auto millis = [](auto time) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            time.time_since_epoch())
            .count());
  };
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 1);
  EXPECT_GE(extra->roi_aging_epoch_ms, millis(before));
  EXPECT_LE(extra->roi_aging_epoch_ms, millis(after));
  const auto epoch = extra->roi_aging_epoch_ms;
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 2);
  EXPECT_EQ(extra->roi_aging_epoch_ms, epoch);
}

TEST_F(RoiAgingRuntimeTest, DisabledAgingPreservesTheOriginalCounterPath) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.roi_backoff_enabled = true;
  config.compile_after_n_calls = 100;
  config.roi_deopt_budget_base = 100;
  config.roi_aging_interval_ms = 0;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(func, nullptr);
  jit::CodeRuntime runtime(func);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);
  Ci_code_extra_store_roi_deopt_count_relaxed(extra, 24);
  extra->roi_aging_epoch_ms = 123;
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 25);
  EXPECT_EQ(extra->roi_aging_epoch_ms, 123);
}

TEST_F(RoiAgingRuntimeTest, ExcludedEventsAndFrozenOrPendingCodeDoNotAge) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.roi_backoff_enabled = true;
  config.compile_after_n_calls = 100;
  config.roi_aging_interval_ms = 1;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(func, nullptr);
  jit::CodeRuntime runtime(func);
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  auto* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  Ci_code_extra_store_roi_deopt_count_relaxed(extra, 24);
  extra->roi_aging_epoch_ms = 1;
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, true);
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kPeriodicTaskFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 24);
  EXPECT_EQ(extra->roi_aging_epoch_ms, 1);
  Ci_code_extra_store_roi_ctl_release(extra, CI_CODE_EXTRA_ROI_PENDING_BIT);
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 24);
  EXPECT_EQ(extra->roi_aging_epoch_ms, 1);
  Ci_code_extra_store_roi_ctl_release(extra, CI_CODE_EXTRA_ROI_FROZEN_BIT);
  jit::recordDeoptForRoiBackoff(
      &runtime, jit::DeoptReason::kGuardFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 24);
  EXPECT_EQ(extra->roi_aging_epoch_ms, 1);
  EXPECT_FALSE(jit::roiBackoffAllowsCompile(code));
}

#ifdef Py_GIL_DISABLED
TEST_F(RoiAgingRuntimeTest, FreeThreadedUpdatesKeepCountAndEpochTogether) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.roi_backoff_enabled = true;
  config.compile_after_n_calls = 100;
  config.roi_deopt_budget_base = std::numeric_limits<uint32_t>::max();
  config.roi_aging_interval_ms = std::numeric_limits<uint32_t>::max();
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(func, nullptr);
  jit::CodeRuntime runtime(func);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);
  constexpr uint32_t kEventsPerThread = 1000;
  auto record = [&runtime]() {
    auto gil = PyGILState_Ensure();
    for (uint32_t i = 0; i < kEventsPerThread; ++i) {
      jit::recordDeoptForRoiBackoff(
          &runtime, jit::DeoptReason::kGuardFailure, false);
    }
    PyGILState_Release(gil);
  };
  Py_BEGIN_ALLOW_THREADS std::thread first(record);
  std::thread second(record);
  first.join();
  second.join();
  Py_END_ALLOW_THREADS EXPECT_EQ(
      Ci_code_extra_load_roi_deopt_count_relaxed(extra), 2 * kEventsPerThread);
  EXPECT_GT(extra->roi_aging_epoch_ms, 0);
}
#endif

} // namespace
