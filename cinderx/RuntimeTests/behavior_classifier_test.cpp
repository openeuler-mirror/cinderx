// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/behavior_classifier.h"
#include "cinderx/Jit/config.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <optional>

using namespace jit;

namespace {

BorrowedRef<PyCodeObject> codeFromFunc(Ref<>& func) {
  JIT_CHECK(PyFunction_Check(func), "expected a Python function");
  auto pyfunc = reinterpret_cast<PyFunctionObject*>(func.get());
  return BorrowedRef<PyCodeObject>{pyfunc->func_code};
}

class ScopedAutoJitConfig {
 public:
  ScopedAutoJitConfig()
      : compile_after_n_calls_{getMutableConfig().compile_after_n_calls},
        auto_classify_{getMutableConfig().auto_classify},
        enable_startup_init_policy_{
            getMutableConfig().enable_startup_init_policy},
        roi_backoff_enabled_{getMutableConfig().roi_backoff_enabled},
        roi_deopt_budget_base_{getMutableConfig().roi_deopt_budget_base},
        roi_backoff_max_rounds_{getMutableConfig().roi_backoff_max_rounds},
        roi_rewarm_factor_{getMutableConfig().roi_rewarm_factor},
        low_roi_warm_calls_{
            getMutableConfig().auto_classify_low_roi_warm_calls} {}

  ~ScopedAutoJitConfig() {
    getMutableConfig().compile_after_n_calls = compile_after_n_calls_;
    getMutableConfig().auto_classify = auto_classify_;
    getMutableConfig().enable_startup_init_policy = enable_startup_init_policy_;
    getMutableConfig().roi_backoff_enabled = roi_backoff_enabled_;
    getMutableConfig().roi_deopt_budget_base = roi_deopt_budget_base_;
    getMutableConfig().roi_backoff_max_rounds = roi_backoff_max_rounds_;
    getMutableConfig().roi_rewarm_factor = roi_rewarm_factor_;
    getMutableConfig().auto_classify_low_roi_warm_calls = low_roi_warm_calls_;
  }

 private:
  std::optional<uint32_t> compile_after_n_calls_;
  bool auto_classify_;
  bool enable_startup_init_policy_;
  bool roi_backoff_enabled_;
  size_t roi_deopt_budget_base_;
  size_t roi_backoff_max_rounds_;
  size_t roi_rewarm_factor_;
  size_t low_roi_warm_calls_;
};

// Steady-state verdicts are held until a process accumulates the held-call
// budget. Cases that are about a verdict rather than about the gate release
// it immediately, and reset the process-wide state so they do not depend on
// how much warming earlier cases did.
// Every case runs with the held-call budget released and its state reset:
// a test process is short-lived by nature and would otherwise never warm
// up. Cases about the budget itself override the config and reset again.
template <class Base>
class LowRoiReleasedFixture : public Base {
 protected:
  void SetUp() override {
    Base::SetUp();
    saved_warm_calls_ = getMutableConfig().auto_classify_low_roi_warm_calls;
    getMutableConfig().auto_classify_low_roi_warm_calls = 0;
    resetLowRoiReleaseState();
  }

  void TearDown() override {
    getMutableConfig().auto_classify_low_roi_warm_calls = saved_warm_calls_;
    resetLowRoiReleaseState();
    Base::TearDown();
  }

 private:
  size_t saved_warm_calls_{0};
};

} // namespace

class BehaviorClassifierTest : public LowRoiReleasedFixture<::testing::Test> {};
class BehaviorClassifierRuntimeTest
    : public LowRoiReleasedFixture<RuntimeTest> {};

TEST_F(BehaviorClassifierTest, LowRoiReleaseWaitsForHeldCallBudget) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().auto_classify_low_roi_warm_calls = 8;

  GateContext steady_state{false};
  StructureKey trivial{Family::Trivial};

  // Held while the process is still proving itself: no freeze (the reason
  // stays None) and the limit sits below the interpret-only line, so calls
  // keep accumulating and convert to a compile once the budget is reached.
  for (int i = 0; i < 7; ++i) {
    auto held = computeThreshold(trivial, steady_state, 2);
    EXPECT_EQ(held.limit, 65535) << "call " << i;
    EXPECT_EQ(held.branch_reason, BranchReason::None) << "call " << i;
  }

  // The budget is reached on this call, and the release is sticky.
  auto released = computeThreshold(trivial, steady_state, 2);
  EXPECT_EQ(released.limit, 2);
  EXPECT_EQ(released.branch_reason, BranchReason::None);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(computeThreshold(trivial, steady_state, 2).limit, 2);
  }
}

TEST_F(BehaviorClassifierTest, LowRoiHeldCallsOnlyCountReleasedShapes) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().auto_classify_low_roi_warm_calls = 4;

  GateContext steady_state{false};

  // A numeric loop was never deferred by this series, so gating it must not
  // consume the budget: the counter has to measure forgone opportunity.
  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 2;
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(computeThreshold(numeric_loop, steady_state, 2).limit, 2);
  }

  // Neither may loop-bearing branch state machines be held: the gate counts
  // calls, not iterations, so a hold would strand the loop's work in the
  // interpreter for as long as the budget takes to fill.
  StructureKey branch_loop{Family::BranchFSM};
  branch_loop.loop_score = 3;
  branch_loop.code_size_bucket = 1;
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(computeThreshold(branch_loop, steady_state, 2).limit, 2);
  }

  StructureKey trivial{Family::Trivial};
  EXPECT_EQ(computeThreshold(trivial, steady_state, 2).limit, 65535);
}

TEST_F(BehaviorClassifierTest, StructureKeyPackRoundTripsAllFields) {
  StructureKey key{
      Family::Mixed,
      encodeMixedShape(WorkDim::Dynamic, WorkDim::Dispatch),
      3,
      true,
      true,
      true,
      static_cast<uint8_t>(kRiskDynamic | kRiskException | kRiskHugeCode),
      2,
      static_cast<uint8_t>(
          activeDimMaskFor(WorkDim::Compute) |
          activeDimMaskFor(WorkDim::Object) |
          activeDimMaskFor(WorkDim::Dispatch)),
  };

  uint32_t payload = key.pack();
  EXPECT_EQ(payload & kSkeyValidBit, 0);
  EXPECT_EQ(payload & ~kSkeyPayloadMask, 0);

  StructureKey decoded = StructureKey::unpack(payload);
  EXPECT_EQ(decoded.family, Family::Mixed);
  EXPECT_EQ(
      decoded.mixed_shape,
      encodeMixedShape(WorkDim::Dynamic, WorkDim::Dispatch));
  EXPECT_EQ(decoded.loop_score, 3);
  EXPECT_TRUE(decoded.is_suspendable);
  EXPECT_TRUE(decoded.is_static);
  EXPECT_TRUE(decoded.is_eafp_benign);
  EXPECT_TRUE(decoded.highRisk());
  EXPECT_EQ(decoded.risk_reason, key.risk_reason);
  EXPECT_EQ(decoded.code_size_bucket, 2);
  EXPECT_EQ(decoded.active_dim_mask, key.active_dim_mask);
  EXPECT_TRUE(decoded.computeHint());
  EXPECT_FALSE(decoded.computeDominantHint());
  EXPECT_EQ(decoded.activeDimCount(), 3);
}

TEST_F(BehaviorClassifierTest, OpcodeClassGoldenExamples) {
  EXPECT_EQ(opcodeClassOf(LOAD_GLOBAL), OpcodeClass::Dynamic);
  EXPECT_EQ(opcodeClassOf(CALL), OpcodeClass::Dispatch);
  EXPECT_EQ(opcodeClassOf(BUILD_STRING), OpcodeClass::Dynamic);
  EXPECT_EQ(opcodeClassOf(TO_BOOL), OpcodeClass::Control);
  EXPECT_EQ(opcodeClassOf(SEND), OpcodeClass::Suspend);
  EXPECT_EQ(opcodeClassOf(LOAD_ATTR), OpcodeClass::Object);
  EXPECT_EQ(opcodeClassOf(PRIMITIVE_BINARY_OP), OpcodeClass::Compute);
  EXPECT_EQ(opcodeClassOf(CACHE), OpcodeClass::Ignored);
  EXPECT_EQ(opcodeClassOf(LOAD_CONST), OpcodeClass::Neutral);
  EXPECT_EQ(opcodeClassOf(-1), OpcodeClass::Invalid);
}

TEST_F(
    BehaviorClassifierTest,
    ComputeThresholdCompilesSteadyStateStartupLikeWork) {
  GateContext ctx{false};

  StructureKey trivial{Family::Trivial};
  auto trivial_decision = computeThreshold(trivial, ctx, 2);
  EXPECT_EQ(trivial_decision.limit, 2);
  EXPECT_EQ(trivial_decision.branch_reason, BranchReason::None);

  StructureKey static_trivial{Family::Trivial};
  static_trivial.is_static = true;
  auto static_decision = computeThreshold(static_trivial, ctx, 2);
  EXPECT_EQ(static_decision.limit, 2);
  EXPECT_EQ(static_decision.branch_reason, BranchReason::None);

  StructureKey suspendable_trivial{Family::Trivial};
  suspendable_trivial.is_suspendable = true;
  auto suspendable_trivial_decision =
      computeThreshold(suspendable_trivial, ctx, 2);
  EXPECT_EQ(suspendable_trivial_decision.limit, 1000);
  EXPECT_EQ(suspendable_trivial_decision.branch_reason, BranchReason::LowRoi);

  StructureKey risky_trivial{Family::Trivial};
  risky_trivial.risk_reason = kRiskHugeCode;
  auto risky_trivial_decision = computeThreshold(risky_trivial, ctx, 2);
  EXPECT_EQ(risky_trivial_decision.limit, 2);
  EXPECT_EQ(risky_trivial_decision.branch_reason, BranchReason::None);

  StructureKey risk_dispatch{Family::CallDispatcher};
  risk_dispatch.risk_reason = kRiskDynamic;
  auto risk_decision = computeThreshold(risk_dispatch, ctx, 2);
  EXPECT_GE(risk_decision.limit, 65536);
  EXPECT_EQ(risk_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, ctx, 2);
  EXPECT_GE(suspendable_decision.limit, 65536);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey huge_code{Family::ObjectManipulator};
  huge_code.risk_reason = kRiskHugeCode;
  huge_code.code_size_bucket = 3;
  auto huge_code_decision = computeThreshold(huge_code, ctx, 2);
  EXPECT_GE(huge_code_decision.limit, 65536);
  EXPECT_EQ(huge_code_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey hot_loop{Family::NumericLoop};
  hot_loop.loop_score = 2;
  hot_loop.risk_reason = kRiskHugeCode;
  auto loop_decision = computeThreshold(hot_loop, ctx, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, StartupContextDefersImportLikeWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext startup{true};
  StructureKey trivial{Family::Trivial};
  auto trivial_startup_decision = computeThreshold(trivial, startup, 2);
  EXPECT_GE(trivial_startup_decision.limit, 65536);
  EXPECT_EQ(trivial_startup_decision.branch_reason, BranchReason::StartupInit);

  StructureKey dispatcher{Family::CallDispatcher};
  auto startup_decision = computeThreshold(dispatcher, startup, 2);
  EXPECT_GE(startup_decision.limit, 65536);
  EXPECT_EQ(startup_decision.branch_reason, BranchReason::StartupInit);

  GateContext steady_state{false};
  auto steady_state_decision = computeThreshold(dispatcher, steady_state, 2);
  EXPECT_EQ(steady_state_decision.limit, 2);
  EXPECT_EQ(steady_state_decision.branch_reason, BranchReason::None);

  StructureKey loop{Family::NumericLoop};
  loop.loop_score = 2;
  auto loop_decision = computeThreshold(loop, startup, 2);
  EXPECT_EQ(loop_decision.limit, 2);
  EXPECT_EQ(loop_decision.branch_reason, BranchReason::None);

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend;
  auto suspendable_decision = computeThreshold(suspendable, startup, 2);
  EXPECT_GE(suspendable_decision.limit, 65536);
  EXPECT_EQ(suspendable_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey static_dispatcher{Family::CallDispatcher};
  static_dispatcher.is_static = true;
  auto static_decision = computeThreshold(static_dispatcher, startup, 2);
  EXPECT_EQ(static_decision.limit, 2);
  EXPECT_EQ(static_decision.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, StartupPolicyDefersRiskyImportWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext startup{true};
  StructureKey risky_branch{Family::BranchFSM};
  risky_branch.loop_score = 3;
  risky_branch.risk_reason = kRiskException | kRiskHugeCode;
  risky_branch.code_size_bucket = 3;
  auto risky_decision = computeThreshold(risky_branch, startup, 2);
  EXPECT_GE(risky_decision.limit, 65536);
  EXPECT_EQ(risky_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 3;
  numeric_loop.risk_reason = kRiskHugeCode;
  numeric_loop.code_size_bucket = 3;
  auto numeric_decision = computeThreshold(numeric_loop, startup, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, ImportWindowDefersHighCostNonnumericWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  GateContext import_window{true};
  GateContext steady_state{false};

  StructureKey large_branch{Family::BranchFSM};
  large_branch.loop_score = 2;
  large_branch.code_size_bucket = 2;
  large_branch.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Dispatch);
  auto large_branch_decision = computeThreshold(large_branch, import_window, 2);
  EXPECT_GE(large_branch_decision.limit, 65536);
  EXPECT_EQ(large_branch_decision.branch_reason, BranchReason::StartupInit);

  auto steady_state_decision = computeThreshold(large_branch, steady_state, 2);
  EXPECT_EQ(steady_state_decision.limit, 2);
  EXPECT_EQ(steady_state_decision.branch_reason, BranchReason::None);

  StructureKey medium_branch{Family::BranchFSM};
  medium_branch.loop_score = 1;
  medium_branch.code_size_bucket = 1;
  medium_branch.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Dispatch);
  auto medium_branch_decision =
      computeThreshold(medium_branch, import_window, 2);
  EXPECT_GE(medium_branch_decision.limit, 65536);
  EXPECT_EQ(medium_branch_decision.branch_reason, BranchReason::StartupInit);

  auto post_import_medium_branch =
      computeThreshold(medium_branch, steady_state, 2);
  EXPECT_EQ(post_import_medium_branch.limit, 2);
  EXPECT_EQ(post_import_medium_branch.branch_reason, BranchReason::None);

  StructureKey risky_object{Family::ObjectManipulator};
  risky_object.loop_score = 1;
  risky_object.risk_reason = kRiskHugeCode;
  risky_object.code_size_bucket = 3;
  risky_object.active_dim_mask = activeDimMaskFor(WorkDim::Object);
  auto risky_object_decision = computeThreshold(risky_object, import_window, 2);
  EXPECT_GE(risky_object_decision.limit, 65536);
  EXPECT_EQ(risky_object_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey low_cost_dispatcher{Family::CallDispatcher};
  low_cost_dispatcher.code_size_bucket = 0;
  low_cost_dispatcher.active_dim_mask = activeDimMaskFor(WorkDim::Dispatch);
  auto low_cost_decision =
      computeThreshold(low_cost_dispatcher, import_window, 2);
  EXPECT_GE(low_cost_decision.limit, 65536);
  EXPECT_EQ(low_cost_decision.branch_reason, BranchReason::StartupInit);

  auto post_import_low_cost_dispatcher =
      computeThreshold(low_cost_dispatcher, steady_state, 2);
  EXPECT_EQ(post_import_low_cost_dispatcher.limit, 2);
  EXPECT_EQ(post_import_low_cost_dispatcher.branch_reason, BranchReason::None);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 2;
  numeric_loop.risk_reason = kRiskHugeCode;
  numeric_loop.code_size_bucket = 3;
  numeric_loop.active_dim_mask =
      activeDimMaskFor(WorkDim::Compute) | activeDimMaskFor(WorkDim::Control);
  auto numeric_loop_decision = computeThreshold(numeric_loop, import_window, 2);
  EXPECT_EQ(numeric_loop_decision.limit, 2);
  EXPECT_EQ(numeric_loop_decision.branch_reason, BranchReason::None);

  StructureKey compute_mixed{Family::Mixed};
  compute_mixed.mixed_shape =
      encodeMixedShape(WorkDim::Compute, WorkDim::Object);
  compute_mixed.loop_score = 2;
  compute_mixed.risk_reason = kRiskHugeCode;
  compute_mixed.code_size_bucket = 2;
  compute_mixed.active_dim_mask = activeDimMaskFor(WorkDim::Compute) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Control);
  auto compute_mixed_decision =
      computeThreshold(compute_mixed, import_window, 2);
  EXPECT_EQ(compute_mixed_decision.limit, 2);
  EXPECT_EQ(compute_mixed_decision.branch_reason, BranchReason::None);

  StructureKey compute_object{Family::ObjectManipulator};
  compute_object.loop_score = 2;
  compute_object.risk_reason = kRiskHugeCode;
  compute_object.code_size_bucket = 2;
  compute_object.active_dim_mask = activeDimMaskFor(WorkDim::Object) |
      activeDimMaskFor(WorkDim::Compute) | activeDimMaskFor(WorkDim::Control);
  auto compute_object_decision =
      computeThreshold(compute_object, import_window, 2);
  EXPECT_GE(compute_object_decision.limit, 65536);
  EXPECT_EQ(compute_object_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey small_object{Family::ObjectManipulator};
  small_object.loop_score = 1;
  small_object.code_size_bucket = 0;
  small_object.active_dim_mask = activeDimMaskFor(WorkDim::Object);
  auto small_object_decision = computeThreshold(small_object, import_window, 2);
  EXPECT_GE(small_object_decision.limit, 65536);
  EXPECT_EQ(small_object_decision.branch_reason, BranchReason::StartupInit);

  StructureKey compute_hint_object{Family::ObjectManipulator};
  compute_hint_object.loop_score = 1;
  compute_hint_object.code_size_bucket = 0;
  compute_hint_object.active_dim_mask =
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Compute);
  auto compute_hint_object_decision =
      computeThreshold(compute_hint_object, import_window, 2);
  EXPECT_EQ(compute_hint_object_decision.limit, 2);
  EXPECT_EQ(compute_hint_object_decision.branch_reason, BranchReason::None);

  auto post_import_risky_object =
      computeThreshold(risky_object, steady_state, 2);
  EXPECT_EQ(post_import_risky_object.limit, 2);
  EXPECT_EQ(post_import_risky_object.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, SteadyStateAllowsStructuredNonBranchWork) {
  GateContext steady_state{false};

  StructureKey branch_big{Family::BranchFSM};
  branch_big.loop_score = 3;
  branch_big.risk_reason = kRiskHugeCode;
  branch_big.code_size_bucket = 2;
  auto branch_decision = computeThreshold(branch_big, steady_state, 2);
  EXPECT_EQ(branch_decision.limit, 1000);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey huge_only_loop{Family::ObjectManipulator};
  huge_only_loop.loop_score = 3;
  huge_only_loop.risk_reason = kRiskHugeCode;
  huge_only_loop.code_size_bucket = 3;
  auto huge_only_decision = computeThreshold(huge_only_loop, steady_state, 2);
  EXPECT_EQ(huge_only_decision.limit, 2);
  EXPECT_EQ(huge_only_decision.branch_reason, BranchReason::None);

  StructureKey numeric_big{Family::NumericLoop};
  numeric_big.loop_score = 3;
  numeric_big.risk_reason = kRiskException | kRiskHugeCode;
  numeric_big.code_size_bucket = 2;
  auto numeric_decision = computeThreshold(numeric_big, steady_state, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, SteadyStateWarmsUpLargeBranchStateMachines) {
  GateContext steady_state{false};

  StructureKey branch_loop{Family::BranchFSM};
  branch_loop.loop_score = 3;
  branch_loop.code_size_bucket = 1;
  branch_loop.risk_reason = kRiskException;
  auto branch_decision = computeThreshold(branch_loop, steady_state, 2);
  EXPECT_EQ(branch_decision.limit, 1000);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::LowRoi);

  StructureKey low_risk_branch_loop{Family::BranchFSM};
  low_risk_branch_loop.loop_score = 3;
  low_risk_branch_loop.code_size_bucket = 1;
  auto low_risk_branch_decision =
      computeThreshold(low_risk_branch_loop, steady_state, 2);
  EXPECT_EQ(low_risk_branch_decision.limit, 2);
  EXPECT_EQ(low_risk_branch_decision.branch_reason, BranchReason::None);

  StructureKey large_low_risk_branch_loop{Family::BranchFSM};
  large_low_risk_branch_loop.loop_score = 3;
  large_low_risk_branch_loop.code_size_bucket = 2;
  auto large_low_risk_branch_decision =
      computeThreshold(large_low_risk_branch_loop, steady_state, 2);
  EXPECT_EQ(large_low_risk_branch_decision.limit, 2);
  EXPECT_EQ(large_low_risk_branch_decision.branch_reason, BranchReason::None);

  StructureKey numeric_loop{Family::NumericLoop};
  numeric_loop.loop_score = 3;
  numeric_loop.code_size_bucket = 1;
  auto numeric_decision = computeThreshold(numeric_loop, steady_state, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);

  StructureKey object_loop{Family::ObjectManipulator};
  object_loop.loop_score = 1;
  object_loop.code_size_bucket = 3;
  auto object_decision = computeThreshold(object_loop, steady_state, 2);
  EXPECT_EQ(object_decision.limit, 2);
  EXPECT_EQ(object_decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierTest,
    SteadyStateRiskDefersHighCostExceptionFrameworkShapes) {
  GateContext steady_state{false};

  StructureKey branch_shape{Family::BranchFSM};
  branch_shape.loop_score = 3;
  branch_shape.code_size_bucket = 2;
  branch_shape.risk_reason = kRiskException | kRiskHugeCode;
  branch_shape.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Object);
  auto branch_decision = computeThreshold(branch_shape, steady_state, 2);
  EXPECT_GE(branch_decision.limit, 65536);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey object_shape{Family::ObjectManipulator};
  object_shape.loop_score = 2;
  object_shape.code_size_bucket = 2;
  object_shape.risk_reason = kRiskException | kRiskHugeCode;
  object_shape.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Object);
  auto object_decision = computeThreshold(object_shape, steady_state, 2);
  EXPECT_GE(object_decision.limit, 65536);
  EXPECT_EQ(object_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey reflection_shape{Family::ReflectionMeta};
  reflection_shape.loop_score = 3;
  reflection_shape.code_size_bucket = 3;
  reflection_shape.risk_reason = kRiskException | kRiskHugeCode;
  reflection_shape.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dynamic);
  auto reflection_decision =
      computeThreshold(reflection_shape, steady_state, 2);
  EXPECT_GE(reflection_decision.limit, 65536);
  EXPECT_EQ(reflection_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey huge_only_object{Family::ObjectManipulator};
  huge_only_object.loop_score = 3;
  huge_only_object.code_size_bucket = 3;
  huge_only_object.risk_reason = kRiskHugeCode;
  huge_only_object.active_dim_mask = activeDimMaskFor(WorkDim::Object);
  auto huge_only_decision = computeThreshold(huge_only_object, steady_state, 2);
  EXPECT_EQ(huge_only_decision.limit, 2);
  EXPECT_EQ(huge_only_decision.branch_reason, BranchReason::None);

  StructureKey numeric_shape{Family::NumericLoop};
  numeric_shape.loop_score = 3;
  numeric_shape.code_size_bucket = 2;
  numeric_shape.risk_reason = kRiskException | kRiskHugeCode;
  numeric_shape.active_dim_mask =
      activeDimMaskFor(WorkDim::Compute) | activeDimMaskFor(WorkDim::Control);
  auto numeric_decision = computeThreshold(numeric_shape, steady_state, 2);
  EXPECT_EQ(numeric_decision.limit, 2);
  EXPECT_EQ(numeric_decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierTest,
    SteadyStateRiskDefersExpectedExceptionLoopShape) {
  GateContext steady_state{false};

  StructureKey tuple_memo_miss{Family::BranchFSM};
  tuple_memo_miss.loop_score = 2;
  tuple_memo_miss.code_size_bucket = 1;
  tuple_memo_miss.risk_reason = kRiskException;
  tuple_memo_miss.active_dim_mask = activeDimMaskFor(WorkDim::Control);
  auto tuple_memo_miss_decision =
      computeThreshold(tuple_memo_miss, steady_state, 2);
  EXPECT_GE(tuple_memo_miss_decision.limit, 65536);
  EXPECT_EQ(tuple_memo_miss_decision.branch_reason, BranchReason::RiskDefer);

  StructureKey dispatching_exception_loop{Family::BranchFSM};
  dispatching_exception_loop.loop_score = 2;
  dispatching_exception_loop.code_size_bucket = 1;
  dispatching_exception_loop.risk_reason = kRiskException;
  dispatching_exception_loop.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Dispatch);
  auto dispatching_exception_loop_decision =
      computeThreshold(dispatching_exception_loop, steady_state, 2);
  EXPECT_EQ(dispatching_exception_loop_decision.limit, 1000);
  EXPECT_EQ(
      dispatching_exception_loop_decision.branch_reason, BranchReason::LowRoi);
}

TEST_F(
    BehaviorClassifierTest,
    SteadyStateCompilesMultidimNonnumericObjectGraphs) {
  GateContext steady_state{false};

  StructureKey reflection_object_graph{Family::ReflectionMeta};
  reflection_object_graph.loop_score = 1;
  reflection_object_graph.code_size_bucket = 1;
  reflection_object_graph.active_dim_mask = activeDimMaskFor(WorkDim::Object) |
      activeDimMaskFor(WorkDim::Dispatch) | activeDimMaskFor(WorkDim::Dynamic);
  auto reflection_decision =
      computeThreshold(reflection_object_graph, steady_state, 2);
  EXPECT_EQ(reflection_decision.limit, 2);
  EXPECT_EQ(reflection_decision.branch_reason, BranchReason::None);

  StructureKey call_dispatch_graph{Family::CallDispatcher};
  call_dispatch_graph.loop_score = 1;
  call_dispatch_graph.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch);
  auto call_dispatch_decision =
      computeThreshold(call_dispatch_graph, steady_state, 2);
  EXPECT_EQ(call_dispatch_decision.limit, 2);
  EXPECT_EQ(call_dispatch_decision.branch_reason, BranchReason::None);

  StructureKey branch_scheduler{Family::BranchFSM};
  branch_scheduler.loop_score = 2;
  branch_scheduler.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch) |
      activeDimMaskFor(WorkDim::Dynamic);
  auto branch_decision = computeThreshold(branch_scheduler, steady_state, 2);
  EXPECT_EQ(branch_decision.limit, 2);
  EXPECT_EQ(branch_decision.branch_reason, BranchReason::None);

  StructureKey compute_object_graph{Family::ObjectManipulator};
  compute_object_graph.loop_score = 1;
  compute_object_graph.active_dim_mask = activeDimMaskFor(WorkDim::Compute) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch) |
      activeDimMaskFor(WorkDim::Dynamic);
  auto compute_decision =
      computeThreshold(compute_object_graph, steady_state, 2);
  EXPECT_EQ(compute_decision.limit, 2);
  EXPECT_EQ(compute_decision.branch_reason, BranchReason::None);
}

TEST_F(BehaviorClassifierTest, SteadyStateCompilesTinyStartupLikeWork) {
  GateContext steady_state{false};

  StructureKey tiny_branch{Family::BranchFSM};
  tiny_branch.loop_score = 0;
  tiny_branch.code_size_bucket = 0;
  auto tiny_branch_decision = computeThreshold(tiny_branch, steady_state, 2);
  EXPECT_EQ(tiny_branch_decision.limit, 2);
  EXPECT_EQ(tiny_branch_decision.branch_reason, BranchReason::None);

  StructureKey tiny_object{Family::ObjectManipulator};
  tiny_object.loop_score = 0;
  tiny_object.code_size_bucket = 0;
  auto tiny_object_decision = computeThreshold(tiny_object, steady_state, 2);
  EXPECT_EQ(tiny_object_decision.limit, 2);
  EXPECT_EQ(tiny_object_decision.branch_reason, BranchReason::None);

  StructureKey medium_object{Family::ObjectManipulator};
  medium_object.loop_score = 0;
  medium_object.code_size_bucket = 1;
  auto medium_object_decision =
      computeThreshold(medium_object, steady_state, 2);
  EXPECT_EQ(medium_object_decision.limit, 2);
  EXPECT_EQ(medium_object_decision.branch_reason, BranchReason::None);

  StructureKey tiny_numeric{Family::NumericLoop};
  tiny_numeric.loop_score = 0;
  tiny_numeric.code_size_bucket = 0;
  auto tiny_numeric_decision = computeThreshold(tiny_numeric, steady_state, 2);
  EXPECT_EQ(tiny_numeric_decision.limit, 2);
  EXPECT_EQ(tiny_numeric_decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyCompilesTinyObjectHelpersOnPythonCallPath) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

class Grid:
    def __init__(self):
        self.width = 1
        self.data = [1]

    def _idx(self, x, y):
        return y * self.width + x

    def __getitem__(self, x_y):
        x, y = x_y
        return self.data[self._idx(x, y)]

grid = Grid()
for _ in range(16):
    Grid.__getitem__(grid, (0, 0))

assert jit.is_jit_compiled(Grid.__getitem__)
assert jit.count_interpreted_calls(Grid.__getitem__) <= 2
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyCompilesSteadyStateTrivialStateHelpers) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

class TaskState:
    def __init__(self):
        self.packetPending = False
        self.taskWaiting = False

    def isPacketPending(self):
        return self.packetPending

    def waitTask(self):
        self.taskWaiting = True
        return self

state = TaskState()
for _ in range(16):
    TaskState.isPacketPending(state)
    TaskState.waitTask(state)

stats = cinderjit._autojit_gate_stats()
assert jit.is_jit_compiled(TaskState.isPacketPending), stats
assert jit.is_jit_compiled(TaskState.waitTask), stats
assert stats["forced_compile"] >= 2, stats
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyCompilesLowRiskSparseBranchLoops) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
from array import array
import time
import cinderx.jit as jit

def sparse_like(M, y, val, row, col, x, num_iterations):
    range_it = range(num_iterations)
    t0 = time.perf_counter()

    for _ in range_it:
        for r in range(M):
            sa = 0.0
            for i in range(row[r], row[r + 1]):
                sa += x[col[i]] * val[i]
            y[r] = sa

    return time.perf_counter() - t0

N = 16
nr = 4
nz = N * nr
x = array("d", [0]) * N
y = array("d", [0]) * N
val = array("d", [0]) * nz
col = array("i", [0]) * nz
row = array("i", [0]) * (N + 1)
for r in range(N):
    row[r + 1] = row[r] + nr

for _ in range(7):
    sparse_like(N, y, val, row, col, x, 16)

assert jit.is_jit_compiled(sparse_like), jit.count_interpreted_calls(sparse_like)
)");
}

TEST_F(BehaviorClassifierRuntimeTest, DerivesTrivialForThinFunction) {
  Ref<> func = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");

  auto key = deriveStructureKey(codeFromFunc(func));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->family, Family::Trivial);
  EXPECT_EQ(key->mixed_shape, kMixedShapeNone);
  EXPECT_EQ(key->loop_score, 0);
  EXPECT_FALSE(key->highRisk());
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AllowsSteadyStateTrivialStatePredicatesAndMutators) {
  Ref<> predicate = compileStockAndGet(
      R"(
class TaskState:
    def isPacketPending(self):
        return self.packetPending

target = TaskState.isPacketPending
)",
      "target");
  Ref<> mutator = compileStockAndGet(
      R"(
class TaskState:
    def waitTask(self):
        self.taskWaiting = True
        return self

target = TaskState.waitTask
)",
      "target");

  GateContext steady_state{false};
  for (auto code : {codeFromFunc(predicate), codeFromFunc(mutator)}) {
    auto key = deriveStructureKey(code);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->family, Family::Trivial);
    EXPECT_EQ(key->loop_score, 0);
    EXPECT_FALSE(key->highRisk());

    auto decision = computeThresholdForCode(code, *key, steady_state, 2);
    EXPECT_EQ(decision.limit, 2);
    EXPECT_EQ(decision.branch_reason, BranchReason::None);
  }
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AllowsSteadyStateCompositeStatePredicates) {
  Ref<> holding_or_waiting = compileStockAndGet(
      R"(
class TaskState:
    def isTaskHoldingOrWaiting(self):
        return self.task_holding or (
            not self.packet_pending and self.task_waiting
        )

target = TaskState.isTaskHoldingOrWaiting
)",
      "target");
  Ref<> waiting_with_packet = compileStockAndGet(
      R"(
class TaskState:
    def isWaitingWithPacket(self):
        return (
            self.packet_pending
            and self.task_waiting
            and not self.task_holding
        )

target = TaskState.isWaitingWithPacket
)",
      "target");

  GateContext steady_state{false};
  for (auto code :
       {codeFromFunc(holding_or_waiting), codeFromFunc(waiting_with_packet)}) {
    auto key = deriveStructureKey(code);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->family, Family::BranchFSM);
    EXPECT_EQ(key->loop_score, 0);
    EXPECT_FALSE(key->highRisk());

    auto decision = computeThresholdForCode(code, *key, steady_state, 2);
    EXPECT_EQ(decision.limit, 2);
    EXPECT_EQ(decision.branch_reason, BranchReason::None);
  }
}

TEST_F(BehaviorClassifierRuntimeTest, AllowsSteadyStateProtocolDispatchCores) {
  Ref<> add_packet = compileStockAndGet(
      R"(
class Task:
    def addPacket(self, p, old):
        if self.input is None:
            self.input = p
            self.packet_pending = True
            if self.priority > old.priority:
                return self
        else:
            p.append_to(self.input)
        return old

target = Task.addPacket
)",
      "target");
  Ref<> device_fn = compileStockAndGet(
      R"(
tracing = False

class DeviceTaskRec:
    pass

class Task:
    def fn(self, pkt, r):
        d = r
        assert isinstance(d, DeviceTaskRec)
        if pkt is None:
            pkt = d.pending
            if pkt is None:
                return self.waitTask()
            d.pending = None
            return self.qpkt(pkt)
        d.pending = pkt
        if tracing:
            trace(pkt.datum)
        return self.hold()

target = Task.fn
)",
      "target");

  GateContext steady_state{false};
  for (auto code : {codeFromFunc(add_packet), codeFromFunc(device_fn)}) {
    auto key = deriveStructureKey(code);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->loop_score, 0);
    EXPECT_FALSE(key->highRisk());

    auto decision = computeThresholdForCode(code, *key, steady_state, 2);
    EXPECT_EQ(decision.limit, 2);
    EXPECT_EQ(decision.branch_reason, BranchReason::None);
  }
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateCompilesGlobalHeavyProtocolLikeWork) {
  Ref<> global_heavy = compileStockAndGet(
      R"(
G1 = True
G2 = False
G3 = False
G4 = False
G5 = False

class Helper:
    def globalHeavy(self, value):
        if G1 and self.enabled:
            return value
        if G2:
            return value
        if G3:
            return value
        if G4:
            return value
        if G5:
            return value
        return None

target = Helper.globalHeavy
)",
      "target");

  auto key = deriveStructureKey(codeFromFunc(global_heavy));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->loop_score, 0);
  EXPECT_FALSE(key->highRisk());

  auto decision =
      computeThresholdForCode(codeFromFunc(global_heavy), *key, {}, 2);
  EXPECT_EQ(decision.limit, 2);
  EXPECT_EQ(decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateCompilesCallOnlyProtocolLikeWrappers) {
  Ref<> call_only = compileStockAndGet(
      R"(
class Wrapper:
    def default(self):
        return None

    def convert(self, value):
        return value

    def callOnly(self, value):
        if value is None:
            return self.default()
        return self.convert(value)

target = Wrapper.callOnly
)",
      "target");

  auto key = deriveStructureKey(codeFromFunc(call_only));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->loop_score, 0);
  EXPECT_FALSE(key->highRisk());

  auto decision = computeThresholdForCode(codeFromFunc(call_only), *key, {}, 2);
  EXPECT_EQ(decision.limit, 2);
  EXPECT_EQ(decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DerivesLowRiskBranchForSparseMatMultShape) {
  Ref<> func = compileStockAndGet(
      R"(
import time

def sparse_like(M, y, val, row, col, x, num_iterations):
    range_it = range(num_iterations)
    t0 = time.perf_counter()

    for _ in range_it:
        for r in range(M):
            sa = 0.0
            for i in range(row[r], row[r + 1]):
                sa += x[col[i]] * val[i]
            y[r] = sa

    return time.perf_counter() - t0
)",
      "sparse_like");

  auto key = deriveStructureKey(codeFromFunc(func));
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(key->family, Family::BranchFSM);
  EXPECT_GT(key->loop_score, 0);
  EXPECT_EQ(key->code_size_bucket, 1);
  EXPECT_FALSE(key->highRisk());
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DetectsSuspendableFunctionsForEarlyAutoJitDefer) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().enable_startup_init_policy = true;

  Ref<> thin = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");
  GateContext steady_state{false};
  GateContext startup{true};
  EXPECT_FALSE(shouldDeferSuspendableAutoJitWithoutStructureKey(
      codeFromFunc(thin), steady_state));
  EXPECT_FALSE(shouldDeferSuspendableAutoJitWithoutStructureKey(
      codeFromFunc(thin), startup));

  Ref<> gen = compileStockAndGet(
      R"(
def gen():
    yield 1
)",
      "gen");
  EXPECT_TRUE(shouldDeferSuspendableAutoJitWithoutStructureKey(
      codeFromFunc(gen), startup));
  EXPECT_FALSE(shouldDeferSuspendableAutoJitWithoutStructureKey(
      codeFromFunc(gen), steady_state));
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    GetOrComputeStructureKeyCachesInCodeExtra) {
  Ref<> func = compileStockAndGet(
      R"(
def thin(x):
    return x
)",
      "thin");

  auto code = codeFromFunc(func);
  CodeExtra* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(Ci_code_extra_load_skey_acquire(extra) & kSkeyValidBit, 0u);

  auto first = getOrComputeStructureKey(code, extra);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->family, Family::Trivial);

  uint32_t cached_word = Ci_code_extra_load_skey_acquire(extra);
  EXPECT_NE(cached_word & kSkeyValidBit, 0u);

  auto second = getOrComputeStructureKey(code, extra);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->pack(), first->pack());
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifySkipsGateForNewFunctionsWithSteadyColdCode) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderjit
import cinderx.jit as jit

def make_inner():
    def inner(cache, key):
        try:
            return cache.fetch(key)
        except KeyError:
            return None
    return inner

class Cache:
    def fetch(self, key):
        return key

cache = Cache()
first = make_inner()
for value in range(4):
    first(cache, value)

assert not jit.is_jit_compiled(first)

for _ in range(4):
    make_inner()

cinderjit._clear_autojit_gate_stats()

for value in range(32):
    make_inner()(cache, value)

stats = cinderjit._autojit_gate_stats()
assert stats["classified_schedule_cold_skip"] >= 32, stats
assert stats["jit_vectorcall"] == 0, stats
assert stats["classified_defer_freeze"] == 0, stats
)");
}

TEST_F(BehaviorClassifierRuntimeTest, RoiBackoffUncompilesDeoptStorm) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 100;
  getMutableConfig().auto_classify = false;
  getMutableConfig().enable_startup_init_policy = false;
  getMutableConfig().roi_backoff_enabled = true;
  getMutableConfig().roi_deopt_budget_base = 32;
  getMutableConfig().roi_backoff_max_rounds = 2;
  getMutableConfig().roi_rewarm_factor = 64;

  runStockCode(R"(
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

def numeric_loop(value):
    total = 0
    for _ in range(8):
        total += value
    return total

for _ in range(120):
    numeric_loop(1)

assert jit.is_jit_compiled(numeric_loop), jit.count_interpreted_calls(numeric_loop)

for _ in range(80):
    numeric_loop(1.5)

stats = cinderjit._autojit_gate_stats()
assert stats["roi_uncompile"] >= 1, stats
assert stats["roi_frozen"] == 0, stats
assert not jit.is_jit_compiled(numeric_loop), stats

calls = jit.count_interpreted_calls(numeric_loop)
for _ in range(8):
    numeric_loop(1.5)
assert not jit.is_jit_compiled(numeric_loop), cinderjit._autojit_gate_stats()
assert jit.count_interpreted_calls(numeric_loop) > calls
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyAllowsLoopFreeNumericWork) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

def straight_compute(a, b):
    return (a + b) * 2

for value in range(16):
    straight_compute(value, value + 1)

assert jit.is_jit_compiled(straight_compute)
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateDefersStdlibAsyncioEventLoopFrameworkHelpers) {
  Ref<> func = compileStockAndGet(
      R"(
src = """
class BaseEventLoop:
    def call_soon(self, callback):
        return self._call_soon(callback)
"""
ns = {}
exec(compile(src, "/opt/python314/lib/python3.14/asyncio/base_events.py", "exec"), ns)
target = ns["BaseEventLoop"].call_soon
)",
      "target");

  StructureKey event_loop_helper{Family::ObjectManipulator};
  event_loop_helper.loop_score = 0;
  event_loop_helper.code_size_bucket = 0;
  event_loop_helper.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch);

  auto decision =
      computeThresholdForCode(codeFromFunc(func), event_loop_helper, {}, 2);
  EXPECT_GE(decision.limit, 65536);
  EXPECT_EQ(decision.branch_reason, BranchReason::LowRoi);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateDefersStdlibAsyncioBranchFrameworkHelpers) {
  Ref<> func = compileStockAndGet(
      R"(
src = """
class BaseSelectorEventLoop:
    def _process_events(self, event_list):
        for key, mask in event_list:
            if mask:
                self._ready.append((key, mask))
"""
ns = {}
exec(compile(src, "/opt/python314/lib/python3.14/asyncio/selector_events.py", "exec"), ns)
target = ns["BaseSelectorEventLoop"]._process_events
)",
      "target");

  StructureKey selector_helper{Family::BranchFSM};
  selector_helper.loop_score = 3;
  selector_helper.code_size_bucket = 1;
  selector_helper.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Object);

  auto decision =
      computeThresholdForCode(codeFromFunc(func), selector_helper, {}, 2);
  EXPECT_GE(decision.limit, 65536);
  EXPECT_EQ(decision.branch_reason, BranchReason::LowRoi);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateDefersStdlibAsyncioReflectionFrameworkHelpers) {
  Ref<> func = compileStockAndGet(
      R"(
src = """
def gather(*aws):
    return tuple(aws)
"""
ns = {}
exec(compile(src, "/opt/python314/lib/python3.14/asyncio/tasks.py", "exec"), ns)
target = ns["gather"]
)",
      "target");

  StructureKey task_helper{Family::ReflectionMeta};
  task_helper.loop_score = 2;
  task_helper.code_size_bucket = 2;
  task_helper.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch) |
      activeDimMaskFor(WorkDim::Dynamic);

  auto decision =
      computeThresholdForCode(codeFromFunc(func), task_helper, {}, 2);
  EXPECT_GE(decision.limit, 65536);
  EXPECT_EQ(decision.branch_reason, BranchReason::LowRoi);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateKeepsUserAsyncioLikePathsOnNormalPolicy) {
  Ref<> func = compileStockAndGet(
      R"(
src = """
class BaseEventLoop:
    def call_soon(self, callback):
        return self._call_soon(callback)
"""
ns = {}
exec(compile(src, "/tmp/project/asyncio/base_events.py", "exec"), ns)
target = ns["BaseEventLoop"].call_soon
)",
      "target");

  StructureKey user_helper{Family::ObjectManipulator};
  user_helper.loop_score = 0;
  user_helper.code_size_bucket = 0;
  user_helper.active_dim_mask = activeDimMaskFor(WorkDim::Control) |
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch);

  auto decision =
      computeThresholdForCode(codeFromFunc(func), user_helper, {}, 2);
  EXPECT_EQ(decision.limit, 2);
  EXPECT_EQ(decision.branch_reason, BranchReason::None);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DeriveStructureKeyMarksSelfContainedEafpCacheIdiom) {
  Ref<> benign = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache[key]
    except KeyError:
        pass
    try:
        return cache.attr
    except AttributeError:
        return None
target = probe
)",
      "target");
  auto benign_key = deriveStructureKey(codeFromFunc(benign));
  ASSERT_TRUE(benign_key.has_value());
  EXPECT_TRUE(benign_key->is_eafp_benign);
  EXPECT_NE(benign_key->risk_reason & kRiskException, 0);

  Ref<> calls_in_region = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache.fetch(key)
    except KeyError:
        return None
target = probe
)",
      "target");
  auto calls_key = deriveStructureKey(codeFromFunc(calls_in_region));
  ASSERT_TRUE(calls_key.has_value());
  EXPECT_FALSE(calls_key->is_eafp_benign);

  Ref<> raising = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache[key]
    except KeyError:
        raise ValueError(key)
target = probe
)",
      "target");
  auto raising_key = deriveStructureKey(codeFromFunc(raising));
  ASSERT_TRUE(raising_key.has_value());
  EXPECT_FALSE(raising_key->is_eafp_benign);

  Ref<> other_type = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache[key]
    except ZeroDivisionError:
        return None
target = probe
)",
      "target");
  auto other_key = deriveStructureKey(codeFromFunc(other_type));
  ASSERT_TRUE(other_key.has_value());
  EXPECT_FALSE(other_key->is_eafp_benign);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    EafpBenignWaivesExceptionRiskOutsideStartup) {
  GateContext steady_state{false};
  GateContext startup{true};

  StructureKey risky_predicate{Family::ObjectManipulator};
  risky_predicate.loop_score = 0;
  risky_predicate.risk_reason = kRiskException;
  risky_predicate.active_dim_mask =
      activeDimMaskFor(WorkDim::Control) | activeDimMaskFor(WorkDim::Object);

  auto deferred = computeThreshold(risky_predicate, steady_state, 2);
  EXPECT_GE(deferred.limit, 65536);
  EXPECT_EQ(deferred.branch_reason, BranchReason::RiskDefer);

  StructureKey benign_predicate = risky_predicate;
  benign_predicate.is_eafp_benign = true;
  EXPECT_EQ(StructureKey::unpack(benign_predicate.pack()).is_eafp_benign, true);

  Ref<> func = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache[key]
    except KeyError:
        return None
target = probe
)",
      "target");
  auto steady = computeThresholdForCode(
      codeFromFunc(func), benign_predicate, steady_state, 2);
  EXPECT_EQ(steady.limit, 2);
  EXPECT_EQ(steady.branch_reason, BranchReason::None);

  auto during_startup =
      computeThresholdForCode(codeFromFunc(func), benign_predicate, startup, 2);
  EXPECT_GE(during_startup.limit, 65536);
  EXPECT_EQ(during_startup.branch_reason, BranchReason::RiskDefer);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SteadyStateAllowsPlainGeneratorSuspendRisk) {
  Ref<> gen = compileStockAndGet(
      R"(
def gen():
    yield 1
)",
      "gen");
  Ref<> coro = compileStockAndGet(
      R"(
async def coro():
    return 1
)",
      "coro");
  Ref<> iterable_coro = compileStockAndGet(
      R"(
import types

@types.coroutine
def iterable_coro():
    yield
)",
      "iterable_coro");

  StructureKey suspendable{Family::AsyncStateMachine};
  suspendable.is_suspendable = true;
  suspendable.risk_reason = kRiskSuspend | kRiskException;

  GateContext steady_state{false};
  auto gen_decision =
      computeThresholdForCode(codeFromFunc(gen), suspendable, steady_state, 2);
  EXPECT_EQ(gen_decision.limit, 2);
  EXPECT_EQ(gen_decision.branch_reason, BranchReason::None);

  auto coro_decision =
      computeThresholdForCode(codeFromFunc(coro), suspendable, steady_state, 2);
  EXPECT_GE(coro_decision.limit, 65536);
  EXPECT_EQ(coro_decision.branch_reason, BranchReason::RiskDefer);

  auto iterable_coro_decision = computeThresholdForCode(
      codeFromFunc(iterable_coro), suspendable, steady_state, 2);
  EXPECT_GE(iterable_coro_decision.limit, 65536);
  EXPECT_EQ(iterable_coro_decision.branch_reason, BranchReason::RiskDefer);

  GateContext startup{true};
  auto startup_decision =
      computeThresholdForCode(codeFromFunc(gen), suspendable, startup, 2);
  EXPECT_GE(startup_decision.limit, 65536);
  EXPECT_EQ(startup_decision.branch_reason, BranchReason::RiskDefer);
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    AutoClassifyAllowsGeneratorsInSteadyState) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderx.jit as jit

def gen():
    yield 1

assert not jit.is_jit_compiled(gen)
gen()
gen()
gen()
assert jit.is_jit_compiled(gen)
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    ImportWindowDefersDispatchThenSteadyStateCompiles) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = true;

  runStockCode(R"(
import _cinderx
import cinderx.jit as jit

def callback():
    return 1

def dispatch(func):
    return func()

_cinderx._autojit_import_enter()
try:
    dispatch(callback)
    dispatch(callback)
    dispatch(callback)
    assert not jit.is_jit_compiled(dispatch)
finally:
    _cinderx._autojit_import_leave()

assert _cinderx._autojit_import_depth() == 0

# The window-touched dispatcher is pinned to the interpreter: import-time
# work never converts into a compile bill.
dispatch(callback)
assert not jit.is_jit_compiled(dispatch)

# A shape first seen in steady state compiles at the base threshold.
def dispatch2(func):
    value = func()
    return value

dispatch2(callback)
dispatch2(callback)
dispatch2(callback)
assert jit.is_jit_compiled(dispatch2)
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    HeldCallBudgetKeepsColdProcessCheapAcrossWindows) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = true;
  getMutableConfig().auto_classify_low_roi_warm_calls = 1000000;

  runStockCode(R"(
import _cinderx
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

def helper(x):
    return x

_cinderx._autojit_import_enter()
try:
    for _ in range(3):
        helper(1)
finally:
    _cinderx._autojit_import_leave()

stats = cinderjit._autojit_gate_stats()
assert stats["classified_defer_freeze"] >= 1, stats
assert stats["forced_compile"] == 0, stats
assert not jit.is_jit_compiled(helper)

# Window-touched code is pinned: calls stop counting once frozen.
pinned_calls = jit.count_interpreted_calls(helper)
for _ in range(5):
    helper(1)
assert jit.count_interpreted_calls(helper) == pinned_calls

# A fresh steady-state shape is held, not pinned: it stays interpreted on
# the unearned budget while its calls keep counting toward release.
def helper2(x):
    return x

for _ in range(5):
    helper2(1)
assert not jit.is_jit_compiled(helper2)
assert jit.count_interpreted_calls(helper2) >= 5
stats = cinderjit._autojit_gate_stats()
assert stats["forced_compile"] == 0, stats
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyCompilesTrivialWorkAtBase) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

def trivial(value):
    return value

for value in range(20):
    trivial(value)

stats = cinderjit._autojit_gate_stats()
assert stats["classified_defer_freeze"] == 0, stats
assert stats["forced_compile"] >= 1, stats
assert jit.is_jit_compiled(trivial), stats
)");
}

TEST_F(BehaviorClassifierRuntimeTest, AutoClassifyCompilesCallDispatchAtBase) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = false;

  runStockCode(R"(
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

def identity(value):
    return value

def dispatch(func, value):
    return func(value)

for value in range(2000):
    dispatch(identity, value)

stats = cinderjit._autojit_gate_stats()
assert stats["classified_defer_freeze"] == 0, stats
assert stats["forced_compile"] >= 1, stats
assert jit.is_jit_compiled(dispatch), stats
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    SetupDepthTracksSeparatelyFromImportDepth) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls = 2;
  getMutableConfig().auto_classify = true;
  getMutableConfig().enable_startup_init_policy = true;

  runStockCode(R"(
import _cinderx
import cinderjit
import cinderx.jit as jit

cinderjit._clear_autojit_gate_stats()

def helper(x):
    return x

_cinderx._autojit_setup_enter()
try:
    helper(1)
finally:
    _cinderx._autojit_setup_leave()

stats = cinderjit._autojit_gate_stats()
assert stats["jit_vectorcall"] >= 1, stats
assert stats["global_threshold_return"] >= 1, stats
assert not jit.is_jit_compiled(helper)
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    DefaultImportProviderTracksFindAndLoadDepth) {
  runStockCode(R"(
import os
import sys
os.environ.pop("CINDERX_AUTOJIT_IMPORT_PROVIDER", None)
old_jit_auto = os.environ.get("PYTHONJITAUTO")
os.environ["PYTHONJITAUTO"] = "auto:2"
try:
    import cinderx
    import _cinderx
    bootstrap = sys.modules["importlib._bootstrap"]

    observed_depths = []

    class ProbeFinder:
        def find_spec(self, fullname, path=None, target=None):
            if fullname == "autojit_probe_missing_default":
                observed_depths.append((
                    _cinderx._autojit_import_depth(),
                    _cinderx._autojit_import_scope_depth(),
                    _cinderx._autojit_setup_depth(),
                ))
            return None

    finder = ProbeFinder()
    sys.meta_path.insert(0, finder)
    try:
        try:
            __import__("autojit_probe_missing_default")
        except ModuleNotFoundError:
            pass
    finally:
        sys.meta_path.remove(finder)

    assert getattr(
        bootstrap._find_and_load,
        "_cinderx_autojit_import_provider",
        None,
    ) == "find_and_load"
    assert observed_depths, observed_depths
    assert all(depth[0] > 0 for depth in observed_depths), observed_depths
    assert all(depth[1] > 0 for depth in observed_depths), observed_depths
    assert all(depth[2] == 0 for depth in observed_depths), observed_depths
    assert _cinderx._autojit_import_depth() == 0
    assert _cinderx._autojit_import_scope_depth() == 0
    assert _cinderx._autojit_setup_depth() == 0
finally:
    if old_jit_auto is None:
        os.environ.pop("PYTHONJITAUTO", None)
    else:
        os.environ["PYTHONJITAUTO"] = old_jit_auto
)");
}

TEST_F(BehaviorClassifierRuntimeTest, ImportProviderOffLeavesDepthZero) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "off"
import cinderx
import _cinderx

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_off":
            observed_depths.append((
                _cinderx._autojit_import_depth(),
                _cinderx._autojit_import_scope_depth(),
                _cinderx._autojit_setup_depth(),
            ))
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_off")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert observed_depths and all(depth == (0, 0, 0) for depth in observed_depths)
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(BehaviorClassifierRuntimeTest, BuiltinsImportProviderTracksDepth) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "builtins"
import cinderx
import _cinderx
builtins = sys.modules["builtins"]

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_builtins":
            observed_depths.append((
                _cinderx._autojit_import_depth(),
                _cinderx._autojit_import_scope_depth(),
                _cinderx._autojit_setup_depth(),
            ))
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_builtins")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert getattr(
    builtins.__import__,
    "_cinderx_autojit_import_provider",
    None,
) == "builtins"
assert observed_depths, observed_depths
assert all(depth[0] > 0 for depth in observed_depths), observed_depths
assert all(depth[1] > 0 for depth in observed_depths), observed_depths
assert all(depth[2] == 0 for depth in observed_depths), observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(BehaviorClassifierRuntimeTest, FindAndLoadImportProviderTracksDepth) {
  runStockCode(R"(
import os
import sys
os.environ["CINDERX_AUTOJIT_IMPORT_PROVIDER"] = "find_and_load"
import cinderx
import _cinderx
bootstrap = sys.modules["importlib._bootstrap"]

observed_depths = []

class ProbeFinder:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "autojit_probe_missing_find_and_load":
            observed_depths.append((
                _cinderx._autojit_import_depth(),
                _cinderx._autojit_import_scope_depth(),
                _cinderx._autojit_setup_depth(),
            ))
        return None

finder = ProbeFinder()
sys.meta_path.insert(0, finder)
try:
    try:
        __import__("autojit_probe_missing_find_and_load")
    except ModuleNotFoundError:
        pass
finally:
    sys.meta_path.remove(finder)

assert getattr(
    bootstrap._find_and_load,
    "_cinderx_autojit_import_provider",
    None,
) == "find_and_load"
assert observed_depths, observed_depths
assert all(depth[0] > 0 for depth in observed_depths), observed_depths
assert all(depth[1] > 0 for depth in observed_depths), observed_depths
assert all(depth[2] == 0 for depth in observed_depths), observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    Lib2to3SetupProviderWrapsMainAndTracksDepth) {
  runStockCode(R"(
import os
import sys
import types
os.environ["CINDERX_AUTOJIT_SETUP_PROVIDER"] = "lib2to3_main"

import cinderx
import _cinderx

observed_depths = []

def main():
    observed_depths.append((
        _cinderx._autojit_import_depth(),
        _cinderx._autojit_import_scope_depth(),
        _cinderx._autojit_setup_depth(),
    ))
    return 42

module = types.ModuleType("lib2to3.main")
module.main = main
sys.modules["lib2to3.main"] = module

cinderx._maybe_install_autojit_setup_provider_for_module("lib2to3.main")

assert getattr(
    module.main,
    "_cinderx_autojit_setup_provider",
    None,
) == "lib2to3_main"
assert module.main() == 42
assert observed_depths and observed_depths[0][0] > 0
assert observed_depths[0][1] == 0
assert observed_depths[0][2] > 0
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    MultiprocessingPoolSetupProviderWrapsContextAndTracksDepth) {
  runStockCode(R"(
import os
import sys
import types
os.environ["CINDERX_AUTOJIT_SETUP_PROVIDER"] = "multiprocessing_pool"

import cinderx
import _cinderx

observed_depths = []

module = types.ModuleType("multiprocessing.pool")

class Pool:
    __module__ = "multiprocessing.pool"

    def __enter__(self):
        observed_depths.append(_cinderx._autojit_setup_depth())
        return self

    def __exit__(self, *exc):
        observed_depths.append(_cinderx._autojit_setup_depth())

    def imap(self, *args):
        observed_depths.append(_cinderx._autojit_setup_depth())
        return iter(())

class ThreadPool(Pool):
    __module__ = "multiprocessing.pool"

class IMapIterator:
    def next(self):
        return None

    __next__ = next

module.Pool = Pool
module.IMapIterator = IMapIterator
sys.modules["multiprocessing.pool"] = module

cinderx._maybe_install_autojit_setup_provider_for_module("multiprocessing.pool")

assert getattr(
    module.Pool.__enter__,
    "_cinderx_autojit_setup_provider",
    None,
) == "multiprocessing_pool"
assert getattr(
    module.Pool.imap,
    "_cinderx_autojit_setup_provider",
    None,
) == "multiprocessing_pool"
assert getattr(
    module.IMapIterator.next,
    "_cinderx_autojit_setup_provider",
    None,
) is None

with module.Pool():
    observed_depths.append(_cinderx._autojit_setup_depth())

assert observed_depths == [1, 1, 1], observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0

observed_depths.clear()
list(module.Pool().imap(None, ()))
assert observed_depths == [1], observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0

observed_depths.clear()
with ThreadPool():
    observed_depths.append(_cinderx._autojit_setup_depth())

assert observed_depths == [0, 0, 0], observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0

observed_depths.clear()
list(ThreadPool().imap(None, ()))
assert observed_depths == [0], observed_depths
assert _cinderx._autojit_import_depth() == 0
assert _cinderx._autojit_import_scope_depth() == 0
assert _cinderx._autojit_setup_depth() == 0
)");
}

TEST_F(
    BehaviorClassifierRuntimeTest,
    CompileAfterNCallsApiDisablesClassificationAndSchedulesExistingFunctions) {
  ScopedAutoJitConfig config_guard;
  getMutableConfig().compile_after_n_calls.reset();
  getMutableConfig().auto_classify = true;

  runStockCode(R"(
import cinderx.jit as jit

def target(x):
    return x + 1

jit.compile_after_n_calls(2)
assert jit.get_compile_after_n_calls() == 2
assert not jit.is_jit_compiled(target)
target(1)
target(2)
assert not jit.is_jit_compiled(target)
target(3)
assert jit.is_jit_compiled(target)
)");

  EXPECT_FALSE(getConfig().auto_classify);
}

namespace {

// Minimal CPython exception-table varint codec (6 bits per byte, bit 6 set
// on every byte but the last), mirroring the format consumed by
// parseExceptionTable in behavior_classifier.cpp.  Groups are emitted most
// significant first because both parseExceptionTable and
// decodeExceptionVarint accumulate with value = (value << 6) | group; a
// least-significant-first encoder would not round-trip multi-byte values
// and could make wrap-refusal regressions invisible (PR235 review).
std::string encodeExceptionVarint(uint32_t value) {
  char groups[6];
  int n = 0;
  do {
    groups[n++] = static_cast<char>(value & 63);
    value >>= 6;
  } while (value != 0 && n < 6);
  std::string out;
  for (int i = n - 1; i >= 0; i--) {
    char byte = groups[i];
    if (i != 0) {
      byte |= 64;
    }
    out.push_back(byte);
  }
  return out;
}

bool decodeExceptionVarint(
    const uint8_t*& pos,
    const uint8_t* end,
    uint32_t& value) {
  if (pos >= end) {
    return false;
  }
  uint8_t byte = *pos++;
  value = byte & 63;
  while (byte & 64) {
    if (pos >= end) {
      return false;
    }
    byte = *pos++;
    value = (value << 6) | (byte & 63);
  }
  return true;
}

} // namespace

TEST(BehaviorClassifierVarintCodec, RoundTripsMultiByteValues) {
  // PR235 review: the encoder used to emit the least significant 6-bit
  // group first while every decoder accumulates most significant group
  // first, so multi-byte values silently mis-encoded and the wrap-refusal
  // regression test below stayed green on the old (buggy) parser.  Pin the
  // round trip and the exact bytes of the wrapping probe value.
  for (uint32_t v :
       {0u,
        1u,
        63u,
        64u,
        0xFFFu,
        0x3FFFFFFu,
        0x4000000u,
        0x7FFFFFFFu,
        0xFFFFFFC3u,
        0xFFFFFFFFu}) {
    std::string enc = encodeExceptionVarint(v);
    const uint8_t* pos = reinterpret_cast<const uint8_t*>(enc.data());
    const uint8_t* end = pos + enc.size();
    uint32_t decoded = 0;
    ASSERT_TRUE(decodeExceptionVarint(pos, end, decoded)) << "v=" << v;
    EXPECT_EQ(pos, end) << "v=" << v;
    EXPECT_EQ(decoded, v);
  }
  // Most-significant-group-first bytes for the wrap probe: exactly what
  // parseExceptionTable must refuse (by the varint overflow or the
  // offset-sum check) instead of wrapping.
  EXPECT_EQ(
      encodeExceptionVarint(0xFFFFFFFF),
      std::string("\x43\x7f\x7f\x7f\x7f\x3f", 6));
}

#if PY_VERSION_HEX < 0x030C0000
// 3.11 has its own dedicated case because the shared one below cannot run
// there at all: 3.11 try/except bodies contain opcodes outside opcodeClassOf
// (e.g. POP_EXC_INFO), so scanCode bails before parseExceptionTable is ever
// reached and deriveStructureKey returns nullopt for any try/except code,
// regardless of the exception table's shape.  That early bail is itself the
// safe outcome on 3.11 -- the parser is unreachable, so a wrapping varint
// cannot fool it -- and this case pins it.  The wrap-refusal path itself is
// version-independent C++ and is covered on 3.12+ by the case below.  If
// opcodeClassOf ever learns the 3.11 exception opcodes, this case should be
// removed and the 3.12+ case made unconditional.
TEST_F(
    BehaviorClassifierRuntimeTest,
    MalformedExceptionTableClassifiesConservatively311) {
  Ref<> calls_in_region = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache.fetch(key)
    except KeyError:
        return None
target = probe
)",
      "target");
  BorrowedRef<PyCodeObject> code = codeFromFunc(calls_in_region);
  auto real_key = deriveStructureKey(code);
  ASSERT_FALSE(real_key.has_value());
}
#else
TEST_F(
    BehaviorClassifierRuntimeTest,
    MalformedExceptionTableClassifiesConservatively) {
  // A length varint that accumulates to 0xFFFFFFFF makes
  // (start + length) * 2 wrap below start * 2, which used to empty the
  // guarded region and skip the region-contains-call check for a function
  // that must not be EAFP-benign.  The hardened parser must refuse the
  // wrapping entry and keep the conservative verdict.
  Ref<> calls_in_region = compileStockAndGet(
      R"(
def probe(cache, key):
    try:
        return cache.fetch(key)
    except KeyError:
        return None
target = probe
)",
      "target");
  BorrowedRef<PyCodeObject> code = codeFromFunc(calls_in_region);
  auto real_key = deriveStructureKey(code);
  ASSERT_TRUE(real_key.has_value());
  EXPECT_FALSE(real_key->is_eafp_benign);

  PyObject* table = code->co_exceptiontable;
  ASSERT_NE(table, nullptr);
  ASSERT_TRUE(PyBytes_Check(table));
  const uint8_t* pos =
      reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(table));
  const uint8_t* end = pos + PyBytes_GET_SIZE(table);
  uint32_t start = 0, length = 0, target = 0, depth_lasti = 0;
  ASSERT_TRUE(decodeExceptionVarint(pos, end, start));
  ASSERT_TRUE(decodeExceptionVarint(pos, end, length));
  ASSERT_TRUE(decodeExceptionVarint(pos, end, target));
  ASSERT_TRUE(decodeExceptionVarint(pos, end, depth_lasti));

  // Same start/target/depth but a wrapping 6-group length varint.
  std::string malformed = encodeExceptionVarint(start);
  malformed += encodeExceptionVarint(0xFFFFFFFF);
  malformed += encodeExceptionVarint(target);
  malformed += encodeExceptionVarint(depth_lasti);

  PyObject* original = code->co_exceptiontable;
  Py_INCREF(original);
  auto malformed_table = Ref<>::steal(PyBytes_FromStringAndSize(
      malformed.data(), static_cast<Py_ssize_t>(malformed.size())));
  ASSERT_NE(malformed_table.get(), nullptr);
  code->co_exceptiontable = malformed_table.release();
  auto malformed_key = deriveStructureKey(code);
  ASSERT_TRUE(malformed_key.has_value());
  EXPECT_FALSE(malformed_key->is_eafp_benign);
  Py_DECREF(code->co_exceptiontable);
  code->co_exceptiontable = original;

  // The restored real table keeps its original (also non-benign) verdict.
  auto restored_key = deriveStructureKey(code);
  ASSERT_TRUE(restored_key.has_value());
  EXPECT_FALSE(restored_key->is_eafp_benign);
}
#endif // PY_VERSION_HEX < 0x030C0000
