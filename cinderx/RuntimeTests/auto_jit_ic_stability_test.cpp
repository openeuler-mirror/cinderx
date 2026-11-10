// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>

#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/auto_jit_ic_stability.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
#include "internal/pycore_code.h"
#endif

using namespace jit;

namespace {

ICStabilitySnapshot ready(uint64_t fingerprint = 11) {
  return {ICStabilitySnapshotStatus::Observed, fingerprint, 2, 2, 0};
}

ICStabilitySnapshot mixed(uint64_t fingerprint = 11) {
  return {ICStabilitySnapshotStatus::Observed, fingerprint, 2, 1, 1};
}

ICStabilitySnapshot noKnown(uint16_t sites = 2) {
  return {ICStabilitySnapshotStatus::NoKnownSites, 0, sites, 0, sites};
}

class AutoJitICStabilityRuntimeTest : public RuntimeTest {
 public:
  AutoJitICStabilityRuntimeTest() : RuntimeTest(static_cast<Flags>(0)) {}
};

PyCodeObject* functionCode(Ref<>& function) {
  return reinterpret_cast<PyCodeObject*>(
      reinterpret_cast<PyFunctionObject*>(function.get())->func_code);
}

} // namespace

TEST(AutoJitICStabilityTest, KeepsExistingCodeExtraAllocationSize) {
  if (sizeof(void*) == 8) {
    EXPECT_EQ(sizeof(CodeExtra), 64);
  }
}

TEST(AutoJitICStabilityTest, KnownLoopWorkPreservesOriginalAdmission) {
  for (uint8_t score : {uint8_t{1}, std::numeric_limits<uint8_t>::max()}) {
    StructureKey key;
    key.loop_score = score;
    EXPECT_EQ(
        icStabilityAdmissionBypassReason(ICStabilityReason::None, key),
        ICStabilityReason::LoopWork);
  }
  EXPECT_STREQ(icStabilityReasonName(ICStabilityReason::LoopWork), "loop_work");
}

TEST(AutoJitICStabilityTest, KnownNoLoopKeepsStabilityAdmission) {
  StructureKey key;
  key.family = Family::NumericLoop; // The score, not a family name, decides.
  key.loop_score = 0;
  EXPECT_EQ(
      icStabilityAdmissionBypassReason(ICStabilityReason::None, key),
      ICStabilityReason::None);
}

TEST(AutoJitICStabilityTest, MissingStructureDoesNotInventLoopWork) {
  EXPECT_EQ(
      icStabilityAdmissionBypassReason(ICStabilityReason::None, std::nullopt),
      ICStabilityReason::None);
}

TEST(AutoJitICStabilityTest, LoopWorkPreservesEarlierBypassPriority) {
  StructureKey key;
  key.loop_score = 1;
  for (auto reason :
       {ICStabilityReason::Disabled,
        ICStabilityReason::NonAuto,
        ICStabilityReason::Explicit,
        ICStabilityReason::ROI,
        ICStabilityReason::Unsupported}) {
    EXPECT_EQ(icStabilityAdmissionBypassReason(reason, key), reason);
    EXPECT_EQ(icStabilityAdmissionBypassReason(reason, std::nullopt), reason);
  }
}

TEST(AutoJitICStabilityTest, NoLoopKnownFeedbackStillNeedsSpacedSamples) {
  StructureKey key;
  ICStabilityState state{};
  auto feedback = ready();
  ASSERT_EQ(
      icStabilityAdmissionBypassReason(ICStabilityReason::None, key),
      ICStabilityReason::None);
  EXPECT_TRUE(updateICStabilityState(state, 2, &feedback).defer);
  EXPECT_TRUE(updateICStabilityState(state, 6, &feedback).defer);
  auto admitted = updateICStabilityState(state, 10, &feedback);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, ThreeReadySamplesMustBeFourCallsApart) {
  ICStabilityState state{};
  auto feedback = ready();
  auto first = updateICStabilityState(state, 64, &feedback);
  EXPECT_TRUE(first.defer);
  EXPECT_TRUE(first.sampled);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_EQ(state.first_calls, 64);

  // Repeated gate entries cannot turn one cached observation into three.
  for (uint64_t calls = 64; calls < 68; ++calls) {
    EXPECT_FALSE(icStabilitySampleDue(state, calls));
    auto waiting = updateICStabilityState(state, calls, &feedback);
    EXPECT_TRUE(waiting.defer);
    EXPECT_FALSE(waiting.sampled);
    EXPECT_EQ(state.consecutive_ready, 1);
  }
  EXPECT_TRUE(icStabilitySampleDue(state, 68));
  EXPECT_TRUE(updateICStabilityState(state, 68, &feedback).defer);
  EXPECT_EQ(state.consecutive_ready, 2);
  auto admitted = updateICStabilityState(state, 72, &feedback);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.samples, 3);
}

TEST(AutoJitICStabilityTest, AllUnknownBypassesAtTheOriginalThreshold) {
  ICStabilityState state{};
  auto feedback = noKnown();
  auto decision = updateICStabilityState(state, 100, &feedback);
  EXPECT_FALSE(decision.defer);
  EXPECT_FALSE(decision.sampled);
  EXPECT_EQ(decision.reason, ICStabilityReason::NoKnownSites);
  EXPECT_EQ(decision.sites, 2);
  EXPECT_EQ(decision.ready_sites, 0);
  EXPECT_EQ(decision.skipped_sites, 2);
}

TEST(AutoJitICStabilityTest, NoExecutionYetDoesNotProduceAStabilitySample) {
  ICStabilityState state{};
  auto known = ready();
  EXPECT_FALSE(icStabilitySampleDue(state, 0, false));
  auto warming = updateICStabilityState(state, 0, &known, false);
  EXPECT_FALSE(warming.defer);
  EXPECT_FALSE(warming.sampled);
  EXPECT_EQ(warming.samples, 0);
  EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
  EXPECT_TRUE(icStabilitySampleDue(state, 1, false));
}

TEST(AutoJitICStabilityTest, WarmupSamplesAreReusedAtTheOriginalThreshold) {
  ICStabilityState state{};
  auto feedback = ready();
  for (uint64_t calls = 1; calls < 64; calls += 4) {
    auto warming = updateICStabilityState(state, calls, &feedback, false);
    EXPECT_FALSE(warming.defer);
    EXPECT_TRUE(warming.sampled);
    EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
    EXPECT_FALSE(state.admission_started);
  }
  // The threshold is not itself a four-call sample boundary. Refresh the
  // current feedback, but do not count a duplicate stability sample.
  ASSERT_TRUE(icStabilitySampleDue(state, 64, true));
  auto admitted = updateICStabilityState(state, 64, &feedback, true);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.first_calls, 1);
  EXPECT_EQ(admitted.admission_calls, 64);
  EXPECT_EQ(admitted.samples, 17);
}

TEST(AutoJitICStabilityTest, StableWarmupDoesNotHideALateFeedbackChange) {
  ICStabilityState state{};
  auto a = ready(101);
  auto b = ready(202);
  updateICStabilityState(state, 1, &a, false);
  updateICStabilityState(state, 9, &a, false);
  auto initially_stable = updateICStabilityState(state, 17, &a, false);
  EXPECT_EQ(initially_stable.reason, ICStabilityReason::Stable);
  EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
  auto changed = updateICStabilityState(state, 25, &b, false);
  EXPECT_TRUE(changed.changed);
  EXPECT_FALSE(changed.defer);
  EXPECT_EQ(state.consecutive_ready, 1);
  auto at_threshold = updateICStabilityState(state, 26, &b, true);
  EXPECT_TRUE(at_threshold.defer);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_TRUE(updateICStabilityState(state, 33, &b, true).defer);
  EXPECT_EQ(
      updateICStabilityState(state, 41, &b, true).reason,
      ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, ThresholdRefreshRejectsAnObsoleteStableSnapshot) {
  ICStabilityState state{};
  auto a = ready(101);
  auto b = ready(202);
  updateICStabilityState(state, 1, &a, false);
  updateICStabilityState(state, 9, &a, false);
  updateICStabilityState(state, 17, &a, false);
  ASSERT_TRUE(icStabilitySampleDue(state, 18, true));
  auto changed = updateICStabilityState(state, 18, &b, true);
  EXPECT_TRUE(changed.defer);
  EXPECT_TRUE(changed.changed);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_TRUE(updateICStabilityState(state, 26, &b, true).defer);
  EXPECT_EQ(
      updateICStabilityState(state, 34, &b, true).reason,
      ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, EarlyUnknownDoesNotDisableLaterObservation) {
  ICStabilityState state{};
  auto unknown = noKnown();
  auto known = ready();
  auto cold = updateICStabilityState(state, 1, &unknown, false);
  EXPECT_FALSE(cold.defer);
  EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
  EXPECT_FALSE(icStabilitySampleDue(state, 2, false));
  EXPECT_TRUE(icStabilitySampleDue(state, 9, false));
  updateICStabilityState(state, 9, &known, false);
  updateICStabilityState(state, 17, &known, false);
  auto admitted = updateICStabilityState(state, 25, &known, true);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.first_calls, 9);
  EXPECT_EQ(admitted.samples, 3);
}

TEST(AutoJitICStabilityTest, EarlyUnknownIsRefreshedAtTheOriginalThreshold) {
  auto unknown = noKnown();
  auto known = ready();
  ICStabilityState still_unknown{};
  updateICStabilityState(still_unknown, 1, &unknown, false);
  ASSERT_TRUE(icStabilitySampleDue(still_unknown, 2, true));
  auto bypassed = updateICStabilityState(still_unknown, 2, &unknown, true);
  EXPECT_FALSE(bypassed.defer);
  EXPECT_EQ(bypassed.reason, ICStabilityReason::NoKnownSites);

  ICStabilityState now_known{};
  updateICStabilityState(now_known, 1, &unknown, false);
  auto waiting = updateICStabilityState(now_known, 2, &known, true);
  EXPECT_TRUE(waiting.defer);
  EXPECT_EQ(waiting.first_calls, 2);
  EXPECT_EQ(waiting.samples, 1);
  EXPECT_TRUE(updateICStabilityState(now_known, 6, &known, true).defer);
  EXPECT_EQ(
      updateICStabilityState(now_known, 10, &known, true).reason,
      ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, LowThresholdDoesNotCountARefreshAsAnotherSample) {
  ICStabilityState state{};
  auto known = ready();
  updateICStabilityState(state, 1, &known, false);
  auto refresh = updateICStabilityState(state, 2, &known, true);
  EXPECT_TRUE(refresh.defer);
  EXPECT_EQ(refresh.samples, 2);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_FALSE(icStabilitySampleDue(state, 2, true));
  EXPECT_FALSE(icStabilitySampleDue(state, 4, true));
  EXPECT_TRUE(updateICStabilityState(state, 5, &known, true).defer);
  EXPECT_EQ(
      updateICStabilityState(state, 9, &known, true).reason,
      ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, LossOfAllKnownFeedbackResetsWarmupReadiness) {
  ICStabilityState state{};
  auto known = ready();
  auto unknown = noKnown();
  updateICStabilityState(state, 1, &known, false);
  updateICStabilityState(state, 9, &known, false);
  updateICStabilityState(state, 17, &known, false);
  updateICStabilityState(state, 25, &unknown, false);
  EXPECT_EQ(state.consecutive_ready, 0);
  auto reappeared = updateICStabilityState(state, 33, &known, true);
  EXPECT_TRUE(reappeared.defer);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_TRUE(updateICStabilityState(state, 41, &known, true).defer);
  EXPECT_EQ(
      updateICStabilityState(state, 49, &known, true).reason,
      ICStabilityReason::Stable);
}

TEST(
    AutoJitICStabilityTest,
    ExtraCallBudgetStartsAtAdmissionNotEarlyObservation) {
  ICStabilityState state{};
  auto a = ready(101);
  auto b = ready(202);
  updateICStabilityState(state, 1, &a, false);
  updateICStabilityState(state, 257, &b, false);
  auto still_warming = updateICStabilityState(state, 513, &a, false);
  EXPECT_FALSE(still_warming.defer);
  EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
  auto admitted_to_gate = updateICStabilityState(state, 600, &b, true);
  EXPECT_TRUE(admitted_to_gate.defer);
  EXPECT_EQ(admitted_to_gate.admission_calls, 600);
  EXPECT_TRUE(updateICStabilityState(state, 727, &a, true).defer);
  EXPECT_FALSE(icStabilitySampleDue(state, 728, true));
  auto capped = updateICStabilityState(state, 728, nullptr, true);
  EXPECT_FALSE(capped.defer);
  EXPECT_EQ(capped.reason, ICStabilityReason::Cap);
  EXPECT_EQ(capped.first_calls, 1);
  EXPECT_EQ(capped.admission_calls, 600);
}

TEST(
    AutoJitICStabilityTest,
    LongWarmupSaturatesRatherThanWrappingSampleCounts) {
  ICStabilityState state{};
  auto known = ready();
  uint64_t calls = 1;
  for (uint32_t sample = 0; sample < 65540; ++sample) {
    auto warming = updateICStabilityState(state, calls, &known, false);
    ASSERT_FALSE(warming.defer);
    ASSERT_EQ(state.terminal_reason, ICStabilityReason::None);
    calls += 4;
  }
  EXPECT_EQ(state.samples, std::numeric_limits<uint16_t>::max());
  EXPECT_EQ(state.consecutive_ready, kICStabilityRequiredSamples);
  auto admitted = updateICStabilityState(state, calls, &known, true);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_FALSE(admitted.defer);
}

TEST(AutoJitICStabilityTest, WarmupOverflowCannotOverrideTheOriginalGate) {
  ICStabilityState state{};
  auto a = ready(101);
  auto b = ready(202);
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  updateICStabilityState(state, maximum - 4, &a, false);
  auto warming = updateICStabilityState(state, maximum - 3, &b, false);
  EXPECT_FALSE(warming.defer);
  EXPECT_EQ(state.terminal_reason, ICStabilityReason::None);
  auto admitted = updateICStabilityState(state, maximum - 2, &b, true);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Overflow);
}

TEST(AutoJitICStabilityTest, ReadyFingerprintChangeRestartsTheWindow) {
  ICStabilityState state{};
  auto a = ready(101);
  auto b = ready(202);
  EXPECT_TRUE(updateICStabilityState(state, 1, &a).defer);
  EXPECT_TRUE(updateICStabilityState(state, 9, &a).defer);
  auto changed = updateICStabilityState(state, 17, &b);
  EXPECT_TRUE(changed.defer);
  EXPECT_TRUE(changed.changed);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_TRUE(updateICStabilityState(state, 25, &b).defer);
  EXPECT_FALSE(updateICStabilityState(state, 33, &b).defer);
}

TEST(AutoJitICStabilityTest, MixedKnownSubsetIgnoresUnknownChurn) {
  ICStabilityState state{};
  auto feedback = mixed();
  auto first = updateICStabilityState(state, 1, &feedback);
  EXPECT_TRUE(first.defer);
  EXPECT_TRUE(first.unknown);
  EXPECT_TRUE(updateICStabilityState(state, 9, &feedback).defer);
  auto admitted = updateICStabilityState(state, 17, &feedback);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.ready_sites, 1);
  EXPECT_EQ(admitted.skipped_sites, 1);
}

TEST(AutoJitICStabilityTest, KnownMembershipAndValueChangesRestartTheWindow) {
  ICStabilityState state{};
  auto first_known_set = mixed(101);
  auto second_known_set = mixed(202);
  EXPECT_TRUE(updateICStabilityState(state, 1, &first_known_set).defer);
  EXPECT_TRUE(updateICStabilityState(state, 9, &first_known_set).defer);
  auto changed = updateICStabilityState(state, 17, &second_known_set);
  EXPECT_TRUE(changed.defer);
  EXPECT_TRUE(changed.changed);
  EXPECT_EQ(state.consecutive_ready, 1);
  EXPECT_TRUE(updateICStabilityState(state, 25, &second_known_set).defer);
  EXPECT_EQ(
      updateICStabilityState(state, 33, &second_known_set).reason,
      ICStabilityReason::Stable);
}

TEST(AutoJitICStabilityTest, KnownUnknownTransitionRestartsTheWindow) {
  ICStabilityState state{};
  ICStabilitySnapshot two_known{
      ICStabilitySnapshotStatus::Observed, 101, 3, 2, 1};
  ICStabilitySnapshot one_known{
      ICStabilitySnapshotStatus::Observed, 202, 3, 1, 2};
  EXPECT_TRUE(updateICStabilityState(state, 1, &two_known).defer);
  EXPECT_TRUE(updateICStabilityState(state, 9, &two_known).defer);
  auto changed = updateICStabilityState(state, 17, &one_known);
  EXPECT_TRUE(changed.defer);
  EXPECT_TRUE(changed.changed);
  EXPECT_EQ(state.consecutive_ready, 1);
}

TEST(AutoJitICStabilityTest, CapDoesNotWaitForTheNextSamplingBoundary) {
  ICStabilityState state{};
  auto first = ready(1);
  auto second = ready(2);
  EXPECT_TRUE(updateICStabilityState(state, 50, &first).defer);
  // A caller may reach a later sample after a gap in gate eligibility.
  EXPECT_TRUE(updateICStabilityState(state, 175, &second).defer);
  EXPECT_FALSE(icStabilitySampleDue(state, 178));
  auto decision = updateICStabilityState(state, 178, nullptr);
  EXPECT_FALSE(decision.defer);
  EXPECT_EQ(decision.reason, ICStabilityReason::Cap);
}

TEST(AutoJitICStabilityTest, FeedbackChurnDoesNotExtend128CallBudget) {
  ICStabilityState state{};
  for (uint64_t calls = 2; calls < 130; calls += 4) {
    auto changed = ready(calls);
    auto waiting = updateICStabilityState(state, calls, &changed);
    EXPECT_TRUE(waiting.defer);
    EXPECT_EQ(state.consecutive_ready, 1);
    EXPECT_EQ(state.admission_calls, 2);
  }
  EXPECT_FALSE(icStabilitySampleDue(state, 129));
  EXPECT_FALSE(icStabilitySampleDue(state, 130));
  auto capped = updateICStabilityState(state, 130, nullptr);
  EXPECT_FALSE(capped.defer);
  EXPECT_EQ(capped.reason, ICStabilityReason::Cap);
  EXPECT_EQ(capped.admission_calls, 2);
}

TEST(AutoJitICStabilityTest, TerminalStateCannotReopenOrCountAnotherRelease) {
  ICStabilityState state{};
  auto known = ready();
  updateICStabilityState(state, 1, &known);
  updateICStabilityState(state, 9, &known);
  updateICStabilityState(state, 17, &known);
  auto missing = mixed(99);
  auto reused = updateICStabilityState(state, 100, &missing);
  EXPECT_FALSE(reused.defer);
  EXPECT_TRUE(reused.terminal_reuse);
  EXPECT_FALSE(reused.sampled);
  EXPECT_EQ(reused.reason, ICStabilityReason::Stable);
  EXPECT_EQ(reused.samples, 3);
}

TEST(AutoJitICStabilityTest, UnsupportedAndEmptyObservationsBypassSafely) {
  for (auto status :
       {ICStabilitySnapshotStatus::Unsupported,
        ICStabilitySnapshotStatus::NoSites}) {
    ICStabilityState state{};
    ICStabilitySnapshot feedback{status, 0, 0, 0, 0};
    auto decision = updateICStabilityState(state, 64, &feedback);
    EXPECT_FALSE(decision.defer);
    EXPECT_EQ(
        decision.reason,
        status == ICStabilitySnapshotStatus::NoSites
            ? ICStabilityReason::NoSites
            : ICStabilityReason::Unsupported);
  }
  ICStabilityState missing_state{};
  EXPECT_EQ(
      updateICStabilityState(missing_state, 64, nullptr).reason,
      ICStabilityReason::Unsupported);
}

TEST(AutoJitICStabilityTest, NearCounterOverflowBypassesImmediately) {
  ICStabilityState state{};
  auto missing = mixed();
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  auto unsafe_budget = updateICStabilityState(state, maximum - 4, &missing);
  EXPECT_FALSE(unsafe_budget.defer);
  EXPECT_EQ(unsafe_budget.reason, ICStabilityReason::Overflow);
}

TEST(AutoJitICStabilityTest, InvalidSiteCountsBypassInsteadOfBecomingReady) {
  for (auto feedback :
       {ICStabilitySnapshot{ICStabilitySnapshotStatus::Observed, 1, 1, 2, 0},
        ICStabilitySnapshot{ICStabilitySnapshotStatus::Observed, 1, 33, 33, 0},
        ICStabilitySnapshot{ICStabilitySnapshotStatus::Observed, 1, 3, 1, 1}}) {
    ICStabilityState state{};
    auto decision = updateICStabilityState(state, 64, &feedback);
    EXPECT_FALSE(decision.defer);
    EXPECT_EQ(decision.reason, ICStabilityReason::Unsupported);
    EXPECT_EQ(state.consecutive_ready, 0);
  }
}

TEST(AutoJitICStabilityTest, UnknownSitesDoNotConsumeTheKnownSiteCap) {
  ICStabilityState state{};
  ICStabilitySnapshot feedback{
      ICStabilitySnapshotStatus::Observed, 17, 40, 1, 39};
  auto decision = updateICStabilityState(state, 64, &feedback);
  EXPECT_TRUE(decision.defer);
  EXPECT_TRUE(decision.sampled);
  EXPECT_TRUE(decision.unknown);
  EXPECT_EQ(decision.ready_sites, 1);
  EXPECT_EQ(decision.skipped_sites, 39);
}

TEST(AutoJitICStabilityTest, CounterWrapBypassesInsteadOfDeferringForever) {
  ICStabilityState state{};
  auto feedback = mixed();
  EXPECT_TRUE(updateICStabilityState(state, 17, &feedback).defer);
  auto wrapped = updateICStabilityState(state, 2, &feedback);
  EXPECT_FALSE(wrapped.defer);
  EXPECT_EQ(wrapped.reason, ICStabilityReason::Overflow);
}

TEST(AutoJitICStabilityTest, HighAbsoluteCallCountDoesNotOverflowTheCap) {
  ICStabilityState state{};
  auto first = ready(1);
  auto second = ready(2);
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  EXPECT_TRUE(updateICStabilityState(state, maximum - 128, &first).defer);
  EXPECT_TRUE(updateICStabilityState(state, maximum - 1, &second).defer);
  auto capped = updateICStabilityState(state, maximum, &second);
  EXPECT_FALSE(capped.defer);
  EXPECT_EQ(capped.reason, ICStabilityReason::Cap);
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED)

TEST_F(
    AutoJitICStabilityRuntimeTest,
    SlotInitializationIsIdempotentAndSeparate) {
  initAutoJitStabilityCodeExtraIndex();
  auto* module = cinderx::getModuleState();
  auto slot = module->auto_jit_ic_stability_extra_index;
  ASSERT_GE(slot, 0);
  EXPECT_NE(slot, codeExtraSlotIndex());
  initAutoJitStabilityCodeExtraIndex();
  EXPECT_EQ(module->auto_jit_ic_stability_extra_index, slot);
}

TEST_F(AutoJitICStabilityRuntimeTest, AdmissionPreservesThePendingException) {
  auto function = compileAndGet(
      R"(
def arithmetic(value):
    return value + 1.0
for _ in range(128):
    assert arithmetic(7.0) == 8.0
)",
      "arithmetic");
  initAutoJitStabilityCodeExtraIndex();
  auto exception = Ref<>::steal(
      PyObject_CallFunction(PyExc_ValueError, "s", "pending sentinel"));
  ASSERT_NE(exception, nullptr);
  PyErr_SetObject(PyExc_ValueError, exception.get());
  auto decision = checkICStabilityAdmission(functionCode(function), 64);
  auto restored = Ref<>::steal(PyErr_GetRaisedException());
  EXPECT_EQ(restored.get(), exception.get());
  EXPECT_TRUE(decision.defer);
  EXPECT_EQ(decision.samples, 1);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    MissingSlotBypassesWithoutChangingCodeExtra) {
  auto function = compileAndGet(
      R"(
def arithmetic(value):
    return value + 1.0
)",
      "arithmetic");
  auto code = functionCode(function);
  auto* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  // Admission owns neither the interpreter's count nor its ROI history.
  extra->calls = 123;
  extra->roi_deopt_count = 9;
  extra->roi_recompile_floor = 987;
  CodeExtra before = *extra;
  auto* module = cinderx::getModuleState();
  auto saved_slot = module->auto_jit_ic_stability_extra_index;
  module->auto_jit_ic_stability_extra_index = -1;
  auto decision = checkICStabilityAdmission(code, 64);
  module->auto_jit_ic_stability_extra_index = saved_slot;
  EXPECT_FALSE(decision.defer);
  EXPECT_EQ(decision.reason, ICStabilityReason::AllocationFailure);
  EXPECT_EQ(codeExtraIfExists(code), extra);
  EXPECT_EQ(std::memcmp(&before, extra, sizeof(before)), 0);
  EXPECT_FALSE(PyErr_Occurred());
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    WarmupObservationPreservesExceptionAndROI) {
  auto function = compileAndGet(
      R"(
def arithmetic(value):
    return value + 1.0
for _ in range(128):
    assert arithmetic(7.0) == 8.0
)",
      "arithmetic");
  initAutoJitStabilityCodeExtraIndex();
  auto code = functionCode(function);
  auto* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  extra->calls = 48;
  extra->roi_deopt_count = 9;
  extra->roi_recompile_floor = 987;
  CodeExtra before = *extra;
  auto exception = Ref<>::steal(
      PyObject_CallFunction(PyExc_ValueError, "s", "warmup sentinel"));
  ASSERT_NE(exception, nullptr);
  PyErr_SetObject(PyExc_ValueError, exception.get());
  auto warming = observeICStabilityWarmup(code, 48);
  auto restored = Ref<>::steal(PyErr_GetRaisedException());
  EXPECT_EQ(restored.get(), exception.get());
  EXPECT_FALSE(warming.defer);
  EXPECT_EQ(warming.samples, 1);
  EXPECT_EQ(std::memcmp(&before, extra, sizeof(before)), 0);
  EXPECT_FALSE(observeICStabilityWarmup(code, 56).defer);
  auto admitted = checkICStabilityAdmission(code, 64);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.first_calls, 48);
  EXPECT_EQ(admitted.admission_calls, 64);
  EXPECT_EQ(std::memcmp(&before, extra, sizeof(before)), 0);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    DifferentCodeObjectsKeepIndependentBudgets) {
  auto first = compileAndGet(
      R"(
def first(value):
    return value + 1.0
def second(value):
    return value + 1.0
for _ in range(128):
    assert first(7.0) == second(7.0) == 8.0
)",
      "first");
  auto second = getGlobal("second");
  initAutoJitStabilityCodeExtraIndex();
  auto first_code = functionCode(first);
  auto second_code = functionCode(second);
  ASSERT_NE(first_code, second_code);
  auto* original_extra = codeExtra(first_code);
  ASSERT_NE(original_extra, nullptr);
  CodeExtra original_fields = *original_extra;
  EXPECT_TRUE(checkICStabilityAdmission(first_code, 64).defer);
  EXPECT_TRUE(checkICStabilityAdmission(second_code, 200).defer);
  EXPECT_TRUE(checkICStabilityAdmission(first_code, 72).defer);
  auto released = checkICStabilityAdmission(first_code, 80);
  EXPECT_EQ(released.reason, ICStabilityReason::Stable);
  auto independent = checkICStabilityAdmission(second_code, 208);
  EXPECT_TRUE(independent.defer);
  EXPECT_EQ(independent.first_calls, 200);
  EXPECT_EQ(independent.samples, 2);
  EXPECT_EQ(
      checkICStabilityAdmission(second_code, 216).reason,
      ICStabilityReason::Stable);
  EXPECT_EQ(codeExtraIfExists(first_code), original_extra);
  EXPECT_EQ(
      std::memcmp(&original_fields, original_extra, sizeof(original_fields)),
      0);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    RealBytecodeBecomesReadyAfterNumericWarmup) {
  auto function = compileAndGet(
      R"(
def arithmetic(value):
    return (value + 1) * 3
)",
      "arithmetic");
  auto cold = observeICStability(functionCode(function));
  ASSERT_EQ(cold.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(cold.ready_sites, 0);
  EXPECT_EQ(cold.skipped_sites, cold.sites);
  runCode(R"(
for _ in range(128):
    assert arithmetic(7) == 24
)");
  auto warm = observeICStability(functionCode(function));
  ASSERT_EQ(warm.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_GT(warm.sites, 0);
  EXPECT_EQ(warm.ready_sites, warm.sites);
  EXPECT_NE(warm.fingerprint, cold.fingerprint);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    InPlaceNumericFeedbackCountsSitesAndTypeChanges) {
  auto function = compileAndGet(
      R"(
def arithmetic(value, operand):
    value += operand
    value -= operand
    value *= operand
    return value
)",
      "arithmetic");
  auto code = functionCode(function);
  auto cold = observeICStability(code);
  ASSERT_EQ(cold.status, ICStabilitySnapshotStatus::NoKnownSites);
  ASSERT_EQ(cold.sites, 3);
  ASSERT_EQ(cold.ready_sites, 0);
  ASSERT_EQ(cold.skipped_sites, 3);
  auto countOpcode = [code](int arg, int specialized) {
    int count = 0;
    for (const auto& instruction : BytecodeInstructionBlock{code}) {
      count += instruction.opcode() == BINARY_OP &&
          instruction.oparg() == arg &&
          instruction.specializedOpcode() == specialized;
    }
    return count;
  };
  runCode(R"(
for _ in range(128):
    assert arithmetic(7, 2) == 14
)");
  ASSERT_EQ(countOpcode(NB_INPLACE_ADD, BINARY_OP_ADD_INT), 1);
  ASSERT_EQ(countOpcode(NB_INPLACE_SUBTRACT, BINARY_OP_SUBTRACT_INT), 1);
  ASSERT_EQ(countOpcode(NB_INPLACE_MULTIPLY, BINARY_OP_MULTIPLY_INT), 1);
  auto integers = observeICStability(code);
  ASSERT_EQ(integers.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(integers.ready_sites, 3);
  EXPECT_EQ(integers.skipped_sites, 0);
  runCode(R"(
for _ in range(128):
    assert arithmetic(2.5, 1.5) == 3.75
)");
  ASSERT_EQ(countOpcode(NB_INPLACE_ADD, BINARY_OP_ADD_FLOAT), 1);
  ASSERT_EQ(countOpcode(NB_INPLACE_SUBTRACT, BINARY_OP_SUBTRACT_FLOAT), 1);
  ASSERT_EQ(countOpcode(NB_INPLACE_MULTIPLY, BINARY_OP_MULTIPLY_FLOAT), 1);
  auto floats = observeICStability(code);
  ASSERT_EQ(floats.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(floats.ready_sites, 3);
  EXPECT_EQ(floats.skipped_sites, 0);
  EXPECT_NE(integers.fingerprint, floats.fingerprint);
}

TEST_F(AutoJitICStabilityRuntimeTest, UnconsumableIntInPlaceSiteIsSkipped) {
  auto function = compileAndGet(
      R"(
def arithmetic(value, operand):
    value += operand
    return value
for _ in range(128):
    assert arithmetic(7, 2) == 9
)",
      "arithmetic");
  auto code = functionCode(function);
  int specialized_sites = 0;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    specialized_sites += instruction.oparg() == NB_INPLACE_ADD &&
        instruction.specializedOpcode() == BINARY_OP_ADD_INT;
  }
  ASSERT_EQ(specialized_sites, 1);
  auto feedback = observeICStability(code);
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 1);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 1);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    DedicatedUnicodeInPlaceFeedbackIsSkipped) {
  auto function = compileAndGet(
      R"(
def concatenate(value, suffix):
    value += suffix
    return value
for _ in range(128):
    assert concatenate("left", "right") == "leftright"
)",
      "concatenate");
  auto code = functionCode(function);
  int dedicated_sites = 0;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    int raw = _Py_OPCODE(codeUnit(code)[instruction.opcodeIndex().value()]);
    if (raw == BINARY_OP_INPLACE_ADD_UNICODE) {
      ++dedicated_sites;
      EXPECT_EQ(instruction.oparg(), NB_INPLACE_ADD);
      // specializedOpcode() only preserves builder-consumable variants.
      EXPECT_EQ(instruction.specializedOpcode(), BINARY_OP);
    }
  }
  ASSERT_EQ(dedicated_sites, 1);
  // The builder does not consume this dedicated Unicode specialization.
  auto feedback = observeICStability(code);
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 1);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 1);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    CacheCounterBytesDoNotChangeTheFingerprint) {
  auto function = compileAndGet(
      R"(
import dis
def arithmetic(value):
    return (value + 1) * 3
for _ in range(128):
    assert arithmetic(7) == 24
counter_byte_offset = next(
    instruction.offset + 2
    for instruction in dis.get_instructions(arithmetic, adaptive=True)
    if instruction.baseopname == 'BINARY_OP'
)
)",
      "arithmetic");
  auto offset_object = getGlobal("counter_byte_offset");
  auto offset = PyLong_AsSsize_t(offset_object.get());
  ASSERT_GE(offset, 0);
  ASSERT_FALSE(PyErr_Occurred());
  auto code = functionCode(function);
  auto before = observeICStability(code);
  ASSERT_EQ(before.ready_sites, before.sites);
  ASSERT_GT(before.sites, 0);
  auto* word = reinterpret_cast<unsigned char*>(_PyCode_CODE(code)) + offset;
  uint16_t saved;
  std::memcpy(&saved, word, sizeof(saved));
  uint16_t changed_counter = saved ^ 0xffff;
  std::memcpy(word, &changed_counter, sizeof(changed_counter));
  auto after = observeICStability(code);
  // Restore before any more Python execution, even if the expectations fail.
  std::memcpy(word, &saved, sizeof(saved));
  EXPECT_EQ(after.status, before.status);
  EXPECT_EQ(after.fingerprint, before.fingerprint);
  EXPECT_EQ(after.ready_sites, before.ready_sites);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    NumericTypePhaseChangesTheSemanticFingerprint) {
  // Keep both operands homogeneous in each phase. Float operations with the
  // integer literals 1 and 3 use BINARY_OP_EXTEND, which the scanner skips.
  auto function = compileAndGet(
      R"(
def arithmetic(value):
    return (value + value) * value
for _ in range(128):
    assert arithmetic(7) == 98
)",
      "arithmetic");
  auto countOpcode = [&function](int opcode) {
    int count = 0;
    for (const auto& instruction :
         BytecodeInstructionBlock{functionCode(function)}) {
      count += instruction.specializedOpcode() == opcode;
    }
    return count;
  };
  ASSERT_EQ(countOpcode(BINARY_OP_ADD_INT), 1);
  ASSERT_EQ(countOpcode(BINARY_OP_MULTIPLY_INT), 1);
  auto integers = observeICStability(functionCode(function));
  runCode(R"(
for _ in range(128):
    assert arithmetic(2.5) == 12.5
)");
  ASSERT_EQ(countOpcode(BINARY_OP_ADD_FLOAT), 1);
  ASSERT_EQ(countOpcode(BINARY_OP_MULTIPLY_FLOAT), 1);
  auto floats = observeICStability(functionCode(function));
  EXPECT_EQ(integers.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(floats.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(integers.ready_sites, 2);
  EXPECT_EQ(floats.ready_sites, 2);
  EXPECT_NE(integers.fingerprint, floats.fingerprint);
}

TEST_F(AutoJitICStabilityRuntimeTest, UnconsumableIntSiteIsSkipped) {
  auto function = compileAndGet(
      R"(
def one_add(value):
    return value + 1
for _ in range(128):
    assert one_add(7) == 8
)",
      "one_add");
  auto feedback = observeICStability(functionCode(function));
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 1);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 1);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    KnownSubsetMaturesWithUnvisitedAttribute) {
  auto function = compileAndGet(
      R"(
def cold_branch(flag, value):
    if flag:
        return value.payload
    return value + 1.0
for _ in range(128):
    assert cold_branch(False, 7.0) == 8.0
)",
      "cold_branch");
  auto feedback = observeICStability(functionCode(function));
  ASSERT_EQ(feedback.status, ICStabilitySnapshotStatus::Observed);
  ASSERT_GT(feedback.sites, feedback.ready_sites);
  ASSERT_GT(feedback.ready_sites, 0);
  ICStabilityState state{};
  EXPECT_TRUE(updateICStabilityState(state, 1, &feedback).defer);
  EXPECT_TRUE(updateICStabilityState(state, 9, &feedback).defer);
  auto admitted = updateICStabilityState(state, 17, &feedback);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(state.consecutive_ready, 3);
  EXPECT_EQ(feedback.skipped_sites, feedback.sites - feedback.ready_sites);
}

TEST_F(AutoJitICStabilityRuntimeTest, NoRelevantBytecodeHasNoSites) {
  auto function =
      compileAndGet("def identity(value):\n    return value\n", "identity");
  auto feedback = observeICStability(functionCode(function));
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoSites);
}

TEST_F(AutoJitICStabilityRuntimeTest, OversizedCodeBypassesTheBoundedScanner) {
  std::string source = "def oversized(value):\n";
  for (int index = 0; index < 1100; ++index) {
    source += "    value = value + 1\n";
  }
  source += "    return value\n";
  auto function = compileAndGet(source.c_str(), "oversized");
  auto feedback = observeICStability(functionCode(function));
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::Unsupported);
}

TEST_F(AutoJitICStabilityRuntimeTest, MoreThan32UnknownSitesDoNotTripKnownCap) {
  std::string source = "def many(receiver):\n    return (";
  for (int index = 0; index < 40; ++index) {
    if (index != 0) {
      source += ", ";
    }
    source += "receiver.attribute" + std::to_string(index);
  }
  source += ")\n";
  auto function = compileAndGet(source.c_str(), "many");
  auto feedback = observeICStability(functionCode(function));
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 40);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 40);
}

TEST_F(AutoJitICStabilityRuntimeTest, MoreThan32KnownSitesAreUnsupported) {
  // Forty ordinary instance fields exceed CPython's inline-values capacity;
  // slots make all forty access positions consumable by the current scanner.
  std::string source =
      "class Receiver:\n"
      "    __slots__ = tuple('attribute' + str(i) for i in range(40))\n"
      "receiver = Receiver()\n";
  for (int index = 0; index < 40; ++index) {
    source += "receiver.attribute" + std::to_string(index) + " = " +
        std::to_string(index) + "\n";
  }
  source += "def many(receiver):\n    return (";
  for (int index = 0; index < 40; ++index) {
    if (index != 0) {
      source += ", ";
    }
    source += "receiver.attribute" + std::to_string(index);
  }
  source += ")\nfor _ in range(128):\n    many(receiver)\n";
  auto function = compileAndGet(source.c_str(), "many");
  auto code = functionCode(function);
  ASSERT_LE(countIndices(code), kICStabilityMaxCodeUnits);
  int known_sites = 0;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    if (instruction.opcode() == LOAD_ATTR) {
      ASSERT_EQ(instruction.specializedOpcode(), LOAD_ATTR_SLOT);
      ASSERT_TRUE(jit::hir::icStabilitySlotCacheUsable(
          code, instruction, loadAttrIndex(instruction.oparg())));
      known_sites++;
    }
  }
  ASSERT_EQ(known_sites, 40);
  ASSERT_GT(known_sites, kICStabilityMaxSites);
  auto feedback = observeICStability(code);
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::Unsupported);
}

TEST_F(AutoJitICStabilityRuntimeTest, ExtendedArgIsStructurallyUnsupported) {
  std::string source = "def extended():\n";
  for (int index = 0; index < 260; ++index) {
    source += "    local" + std::to_string(index) + " = " +
        std::to_string(index) + "\n";
  }
  source += "    return local259\n";
  auto function = compileAndGet(source.c_str(), "extended");
  ASSERT_LE(countIndices(functionCode(function)), kICStabilityMaxCodeUnits);
  auto feedback = observeICStability(functionCode(function));
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::Unsupported);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    InstanceValueCacheIsSkippedWithoutLowering) {
  auto function = compileAndGet(
      R"(
class Receiver:
    def __init__(self):
        self.value = 41
receiver = Receiver()
def instance_value(receiver):
    return receiver.value
for _ in range(128):
    assert instance_value(receiver) == 41
)",
      "instance_value");
  auto code = functionCode(function);
  auto feedback = observeICStability(code);
  ASSERT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 1);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 1);
  ICStabilityState state{};
  auto admitted = updateICStabilityState(state, 64, &feedback);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::NoKnownSites);
  EXPECT_EQ(admitted.samples, 0);

  bool found = false;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    if (instruction.specializedOpcode() != LOAD_ATTR_INSTANCE_VALUE) {
      continue;
    }
    found = true;
    jit::hir::LoadAttrInstanceValueCacheInfo info;
    EXPECT_TRUE(jit::hir::icStabilityInstanceValueCacheUsable(
        code, instruction, loadAttrIndex(instruction.oparg()), &info));
    EXPECT_EQ(info.type_version, instruction.attrCacheTypeVersion());
    EXPECT_EQ(info.value_offset, instruction.attrCacheIndex());
  }
  EXPECT_TRUE(found);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    InstanceValueChangesDoNotResetKnownMethod) {
  auto function = compileAndGet(
      R"(
class ValueReceiver:
    def __init__(self):
        self.value = 41
class MethodReceiver:
    def __init__(self):
        self.padding = None
    def method(self):
        return 17
value_receiver = ValueReceiver()
method_receiver = MethodReceiver()
def combined(value_receiver, method_receiver):
    return value_receiver.value, method_receiver.method()
for _ in range(128):
    assert combined(value_receiver, method_receiver) == (41, 17)
)",
      "combined");
  auto code = functionCode(function);
  auto before = observeICStability(code);
  ASSERT_EQ(before.status, ICStabilitySnapshotStatus::Observed);
  ASSERT_EQ(before.sites, 2);
  ASSERT_EQ(before.ready_sites, 1);
  ASSERT_EQ(before.skipped_sites, 1);

  int instance_index = -1;
  int method_index = -1;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    if (instruction.specializedOpcode() == LOAD_ATTR_INSTANCE_VALUE) {
      instance_index = instruction.opcodeIndex().value();
    } else if (
        instruction.specializedOpcode() == LOAD_ATTR_METHOD_WITH_VALUES) {
      method_index = instruction.opcodeIndex().value();
    }
  }
  ASSERT_GE(instance_index, 0);
  ASSERT_GE(method_index, 0);

  // Mutate only the ignored IC's scalar payload. Restore it before executing
  // any Python, including assertion-reporting paths; no invalid cache runs.
  auto* instance_cache =
      reinterpret_cast<_PyAttrCache*>(codeUnit(code) + instance_index + 1);
  _PyAttrCache saved_instance;
  std::memcpy(&saved_instance, instance_cache, sizeof(saved_instance));
  uint32_t zero_version = 0;
  std::memcpy(instance_cache->version, &zero_version, sizeof(zero_version));
  instance_cache->index ^= 0xffff;
  auto ignored_change = observeICStability(code);
  std::memcpy(instance_cache, &saved_instance, sizeof(saved_instance));
  EXPECT_EQ(ignored_change.status, before.status);
  EXPECT_EQ(ignored_change.fingerprint, before.fingerprint);
  EXPECT_EQ(ignored_change.ready_sites, 1);
  EXPECT_EQ(ignored_change.skipped_sites, 1);

  ICStabilityState state{};
  updateICStabilityState(state, 48, &before, false);
  auto second = updateICStabilityState(state, 56, &ignored_change, false);
  EXPECT_FALSE(second.changed);
  auto admitted = updateICStabilityState(state, 64, &before);
  EXPECT_FALSE(admitted.defer);
  EXPECT_EQ(admitted.reason, ICStabilityReason::Stable);
  EXPECT_EQ(admitted.samples, 3);

  // The retained method specialization still contributes type-version data.
  auto* method_cache =
      reinterpret_cast<_PyLoadMethodCache*>(codeUnit(code) + method_index + 1);
  uint32_t saved_version;
  std::memcpy(
      &saved_version, method_cache->type_version, sizeof(saved_version));
  uint32_t changed_version = saved_version == 1 ? 2 : 1;
  std::memcpy(
      method_cache->type_version, &changed_version, sizeof(changed_version));
  auto method_change = observeICStability(code);
  std::memcpy(
      method_cache->type_version, &saved_version, sizeof(saved_version));
  EXPECT_EQ(method_change.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(method_change.ready_sites, 1);
  EXPECT_NE(method_change.fingerprint, before.fingerprint);
  ICStabilityState method_state{};
  updateICStabilityState(method_state, 48, &before, false);
  auto reset = updateICStabilityState(method_state, 56, &method_change, false);
  EXPECT_TRUE(reset.changed);
  EXPECT_EQ(method_state.consecutive_ready, 1);
}

TEST_F(
    AutoJitICStabilityRuntimeTest,
    InstanceValueToSlotStillChangesKnownSubset) {
  auto function = compileAndGet(
      R"(
class ValueReceiver:
    def __init__(self):
        self.value = 41
class SlotReceiver:
    __slots__ = ('value',)
    def __init__(self):
        self.value = 41
class MethodReceiver:
    def __init__(self):
        self.padding = None
    def method(self):
        return 17
value_receiver = ValueReceiver()
slot_receiver = SlotReceiver()
method_receiver = MethodReceiver()
def combined(value_receiver, method_receiver):
    return value_receiver.value, method_receiver.method()
for _ in range(128):
    assert combined(value_receiver, method_receiver) == (41, 17)
)",
      "combined");
  auto code = functionCode(function);
  auto before = observeICStability(code);
  ASSERT_EQ(before.status, ICStabilitySnapshotStatus::Observed);
  ASSERT_EQ(before.ready_sites, 1);
  ASSERT_EQ(before.skipped_sites, 1);
  runCode(R"(
for _ in range(128):
    assert combined(slot_receiver, method_receiver) == (41, 17)
)");
  auto after = observeICStability(code);
  ASSERT_EQ(after.status, ICStabilitySnapshotStatus::Observed);
  EXPECT_EQ(after.sites, 2);
  EXPECT_EQ(after.ready_sites, 2);
  EXPECT_EQ(after.skipped_sites, 0);
  EXPECT_NE(after.fingerprint, before.fingerprint);
  ICStabilityState state{};
  updateICStabilityState(state, 48, &before, false);
  auto reset = updateICStabilityState(state, 56, &after, false);
  EXPECT_TRUE(reset.changed);
  EXPECT_EQ(state.consecutive_ready, 1);
}

TEST_F(AutoJitICStabilityRuntimeTest, MoreThan32InstanceValueSitesAreSkipped) {
  std::string source =
      "class Receiver:\n    pass\nreceiver = Receiver()\nreceiver.value = 41\n"
      "def many(receiver):\n    return (";
  for (int index = 0; index < 40; ++index) {
    source += "receiver.value,";
  }
  source += ")\nfor _ in range(128):\n    many(receiver)\n";
  auto function = compileAndGet(source.c_str(), "many");
  auto code = functionCode(function);
  int instance_sites = 0;
  for (const auto& instruction : BytecodeInstructionBlock{code}) {
    instance_sites +=
        instruction.specializedOpcode() == LOAD_ATTR_INSTANCE_VALUE;
  }
  ASSERT_EQ(instance_sites, 40);
  auto feedback = observeICStability(code);
  EXPECT_EQ(feedback.status, ICStabilitySnapshotStatus::NoKnownSites);
  EXPECT_EQ(feedback.sites, 40);
  EXPECT_EQ(feedback.ready_sites, 0);
  EXPECT_EQ(feedback.skipped_sites, 40);
}

#else

TEST_F(
    AutoJitICStabilityRuntimeTest,
    OtherPythonOrFreeThreadingBypassesObservation) {
  auto function = compileAndGet(
      "def arithmetic(value):\n    return value + 1\n", "arithmetic");
  EXPECT_EQ(
      observeICStability(functionCode(function)).status,
      ICStabilitySnapshotStatus::Unsupported);
}

#endif
