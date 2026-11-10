// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/observe.h"

TEST(SchedulingPolicyStateTest, ConcurrentMarksPreserveOtherControlBits) {
  CodeExtra extra{};
  Ci_code_extra_jit311_disable_auto(&extra);
  std::atomic<int> marks{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&] {
      marks += Ci_code_extra_jit311_try_despecialize(&extra);
      for (int n = 0; n < 1000; ++n) {
        Ci_code_extra_jit311_note_attach(&extra);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(marks.load(), 1);
  EXPECT_EQ(Ci_code_extra_jit311_attach_count(&extra), 8000);
  EXPECT_TRUE(Ci_code_extra_jit311_auto_disabled(&extra));
  EXPECT_TRUE(Ci_code_extra_jit311_despecialized(&extra));
  Ci_code_extra_jit311_note_non_guard_deopt(&extra);
  EXPECT_TRUE(Ci_code_extra_jit311_has_non_guard_deopt(&extra));
  Ci_code_extra_jit311_clear_non_guard_deopt(&extra);
  EXPECT_FALSE(Ci_code_extra_jit311_has_non_guard_deopt(&extra));
  EXPECT_TRUE(Ci_code_extra_jit311_despecialized(&extra));
}

class SchedulingPolicyBuildTest : public RuntimeTest {};

TEST_F(
    SchedulingPolicyBuildTest,
    DespecializationDoesNotRetainOrContaminateCode) {
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  auto weak = Ref<>::steal(
      PyWeakref_NewRef(reinterpret_cast<PyObject*>(code), nullptr));
  ASSERT_NE(weak, nullptr);
  auto* extra = codeExtra(code);
  ASSERT_NE(extra, nullptr);
  ASSERT_TRUE(Ci_code_extra_jit311_try_despecialize(extra));
  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "f"), 0);
  func.reset();
  PyGC_Collect();
  EXPECT_EQ(PyWeakref_GetObject(weak), Py_None);

  Ref<PyFunctionObject> fresh(compileAndGet("def f(): return 1", "f"));
  auto* fresh_extra =
      codeExtra(reinterpret_cast<PyCodeObject*>(fresh->func_code));
  ASSERT_NE(fresh_extra, nullptr);
  EXPECT_FALSE(Ci_code_extra_jit311_despecialized(fresh_extra));
  EXPECT_FALSE(Ci_code_extra_jit311_has_non_guard_deopt(fresh_extra));
}

TEST_F(SchedulingPolicyBuildTest, SizeScalingIsOptInAndSaturates) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  std::string source = "def f(x):\n";
  for (int i = 0; i < 160; ++i) {
    source += "    x = x + 1\n";
  }
  source += "    return x\n";
  Ref<PyFunctionObject> func(compileAndGet(source.c_str(), "f"));
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  jit::getMutableConfig().auto_jit_size_scaling = false;
  EXPECT_EQ(Ci_JitShell311_RoiGateNeed(code, 10), 10);
  jit::getMutableConfig().auto_jit_size_scaling = true;
  uint64_t factor = Ci_JitShell311_RoiGateNeed(code, 1);
  ASSERT_TRUE(factor == 4 || factor == 16);
  EXPECT_EQ(Ci_JitShell311_RoiGateNeed(code, 10), 10 * factor);
  EXPECT_EQ(Ci_JitShell311_RoiGateNeed(code, 0), factor);
  auto limit = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(Ci_JitShell311_RoiGateNeed(code, limit), limit);

  std::string loop_source = "def loop(x):\n    for i in range(3):\n";
  for (int i = 0; i < 160; ++i) {
    loop_source += "        x += i\n";
  }
  loop_source += "    return x\n";
  Ref<PyFunctionObject> loop(compileAndGet(loop_source.c_str(), "loop"));
  ASSERT_GE(Py_SIZE(reinterpret_cast<PyCodeObject*>(loop->func_code)), 200);
  EXPECT_EQ(
      Ci_JitShell311_RoiGateNeed(
          reinterpret_cast<PyCodeObject*>(loop->func_code), 10),
      10);
}

class SchedulingPolicyRuntimeTest : public RuntimeTest {};

TEST_F(
    SchedulingPolicyRuntimeTest,
    GuardFailureEarnsOneGenericRetryThenFreezes) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1000000;
  config.roi_deopt_budget_base = 1;
  config.roi_backoff_max_rounds = 1;
  config.roi_aging_interval_ms = 0;
  config.auto_jit_despecialize = true;
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def first(x): return 1
def second(x): return 2
selected = first
def f(x): return selected(x=x)
)",
      "f"));
  for (int i = 0; i < 100; ++i) {
    auto result = Ref<>::steal(PyObject_CallFunction(func, "i", i));
    ASSERT_NE(result, nullptr);
  }
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  auto* extra = codeExtra(code);
  runStockCode("selected = second");
  auto before = jit::triggerStatsSnapshot();
  auto result = Ref<>::steal(PyObject_CallFunction(func, "d", 1.5));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 2);
  EXPECT_GT(
      jit::triggerStatsSnapshot().organic_deopt_hits,
      before.organic_deopt_hits);
  ASSERT_TRUE(Ci_code_extra_jit311_despecialized(extra));
  EXPECT_FALSE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  before = jit::triggerStatsSnapshot();
  result = Ref<>::steal(PyObject_CallFunction(func, "d", 2.5));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 2);
  EXPECT_GT(
      jit::triggerStatsSnapshot().machine_code_entries,
      before.machine_code_entries);
  EXPECT_EQ(
      jit::triggerStatsSnapshot().organic_deopt_hits,
      before.organic_deopt_hits);

  // A later guard storm must not reopen a second retry. The real first
  // deopt above proves the natural path; these callbacks pin the upper bound.
  auto compiled = Ref<jit::CompiledFunction>::create(
      reinterpret_cast<jit::CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func)));
  ASSERT_NE(compiled, nullptr);
  for (int i = 0; i < 64; ++i) {
    jit::recordDeoptForRoiBackoff(
        compiled->runtime(), jit::DeoptReason::kGuardFailure, false);
  }
  EXPECT_TRUE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
}

TEST_F(
    SchedulingPolicyRuntimeTest,
    ExceptionOrMixedRoundsCannotEarnAGuardRetry) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1000000;
  config.roi_deopt_budget_base = 2;
  config.roi_backoff_max_rounds = 1;
  config.roi_aging_interval_ms = 0;
  config.auto_jit_despecialize = true;
  for (auto last :
       {jit::DeoptReason::kUnhandledException,
        jit::DeoptReason::kGuardFailure}) {
    Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
    auto compiled = Ref<jit::CompiledFunction>::create(
        reinterpret_cast<jit::CompiledFunction*>(
            Ci_JitShell311_InstalledArtifact(func)));
    ASSERT_NE(compiled, nullptr);
    auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
    jit::recordDeoptForRoiBackoff(
        compiled->runtime(), jit::DeoptReason::kUnhandledException, false);
    jit::recordDeoptForRoiBackoff(compiled->runtime(), last, false);
    EXPECT_TRUE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
    EXPECT_FALSE(Ci_code_extra_jit311_despecialized(extra));
  }
}

TEST_F(SchedulingPolicyRuntimeTest, DeadObserverSlotRecomputesSizeScale) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.auto_jit_startup_grace_ms = 0;
  config.auto_jit_size_scaling = true;
  Ci_Observe311_Finalize();
  Ci_Observe311_SetResolvedAutoJitConfig(1, 2, 0, 1);
  ASSERT_EQ(Ci_Observe311_Configure(), 0);
  std::string source = "def f(x):\n";
  for (int i = 0; i < 40; ++i) {
    source += "    x = x + 1\n";
  }
  source += "    return x\n";
  Ref<PyFunctionObject> func(compileAndGet(source.c_str(), "f"));
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  ASSERT_GE(Ci_JitShell311_RoiGateNeed(code, 1), 4);
  Ci_Observe311_OnFrame(func, code, nullptr);
  Ci_Observe311_OnFrame(func, code, nullptr);
  uint64_t count = 0;
  int dispatched = 0;
  ASSERT_EQ(Ci_Observe311_GetCodeState(code, &count, &dispatched, nullptr), 1);
  EXPECT_EQ(count, 2);
  EXPECT_EQ(dispatched, 0);
  // Exercise the production death/re-entry contract at a fixed key, without
  // relying on the allocator to return a particular freed code address.
  Ci_Observe311_OnCodeDeath(code);
  EXPECT_EQ(Ci_Observe311_GetCodeState(code, nullptr, nullptr, nullptr), 0);
  config.auto_jit_size_scaling = false;
  Ci_Observe311_OnFrame(func, code, nullptr);
  Ci_Observe311_OnFrame(func, code, nullptr);
  ASSERT_EQ(Ci_Observe311_GetCodeState(code, &count, &dispatched, nullptr), 1);
  EXPECT_EQ(count, 2);
  EXPECT_EQ(dispatched, 1);
  EXPECT_NE(Ci_JitShell311_InstalledArtifact(func), nullptr);
}

TEST_F(SchedulingPolicyRuntimeTest, AgingPreservesMixedRoundHistory) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1000000;
  config.roi_deopt_budget_base = 2;
  config.roi_backoff_max_rounds = 1;
  config.roi_aging_interval_ms = 1;
  config.auto_jit_despecialize = true;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto compiled = Ref<jit::CompiledFunction>::create(
      reinterpret_cast<jit::CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func)));
  ASSERT_NE(compiled, nullptr);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  jit::recordDeoptForRoiBackoff(
      compiled->runtime(), jit::DeoptReason::kUnhandledException, false);
  auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  extra->roi_aging_epoch_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() -
      32;
  jit::recordDeoptForRoiBackoff(
      compiled->runtime(), jit::DeoptReason::kGuardFailure, false);
  EXPECT_EQ(Ci_code_extra_load_roi_deopt_count_relaxed(extra), 1);
  EXPECT_TRUE(Ci_code_extra_jit311_has_non_guard_deopt(extra));
  config.roi_aging_interval_ms = 0;
  jit::recordDeoptForRoiBackoff(
      compiled->runtime(), jit::DeoptReason::kGuardFailure, false);
  EXPECT_TRUE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
  EXPECT_FALSE(Ci_code_extra_jit311_despecialized(extra));
}

TEST_F(SchedulingPolicyBuildTest, BackoffQueriesDoNotConsumeRecompileFloor) {
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  jit::getMutableConfig().roi_backoff_enabled = true;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  auto* extra = codeExtra(code);
  Ci_code_extra_store_roi_recompile_floor_release(extra, 10);
  EXPECT_FALSE(jit::roiBackoffAllowsCompile311(code, 9));
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(jit::roiBackoffAllowsCompile311(code, 10));
    EXPECT_EQ(Ci_code_extra_load_roi_recompile_floor_relaxed(extra), 10);
  }
  Ci_code_extra_store_roi_ctl_release(extra, CI_CODE_EXTRA_ROI_FROZEN_BIT);
  EXPECT_FALSE(jit::roiBackoffAllowsCompile311(code, 10));
  EXPECT_EQ(Ci_code_extra_load_roi_recompile_floor_relaxed(extra), 10);
}

TEST_F(SchedulingPolicyRuntimeTest, ZeroConfiguredRoundsDisallowTheExtraRetry) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1000000;
  config.roi_deopt_budget_base = 1;
  config.roi_backoff_max_rounds = 0;
  config.roi_aging_interval_ms = 0;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto compiled = Ref<jit::CompiledFunction>::create(
      reinterpret_cast<jit::CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func)));
  ASSERT_NE(compiled, nullptr);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  jit::recordDeoptForRoiBackoff(
      compiled->runtime(), jit::DeoptReason::kGuardFailure, false);
  EXPECT_TRUE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
  EXPECT_FALSE(Ci_code_extra_jit311_despecialized(extra));
}
TEST_F(SchedulingPolicyRuntimeTest, OversizedRoundLimitStillFreezes) {
  SKIP_311_EXECUTABLE_COMPILE();
  auto saved = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1000000;
  config.roi_deopt_budget_base = 1;
  config.roi_backoff_max_rounds = std::numeric_limits<size_t>::max();
  config.roi_aging_interval_ms = 0;
  config.auto_jit_despecialize = false;
  Ref<PyFunctionObject> func(compileAndGet("def f(): return 1", "f"));
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto compiled = Ref<jit::CompiledFunction>::create(
      reinterpret_cast<jit::CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func)));
  ASSERT_NE(compiled, nullptr);
  auto* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  Ci_code_extra_store_roi_ctl_release(extra, CI_CODE_EXTRA_ROI_ROUND_MASK);
  Ci_code_extra_store_roi_deopt_count_relaxed(extra, (1u << 15) - 1);
  jit::recordDeoptForRoiBackoff(
      compiled->runtime(), jit::DeoptReason::kGuardFailure, false);
  EXPECT_TRUE(extra->roi_ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT);
}
#endif
