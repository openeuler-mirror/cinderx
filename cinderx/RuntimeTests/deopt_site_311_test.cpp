// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/observe.h"

#define SKIP_311_EXECUTABLE_COMPILE()                                    \
  do {                                                                   \
    if (jit::getConfig().state != jit::State::kRunning) {                \
      GTEST_SKIP() << "3.11 executes machine code only in canary mode; " \
                      "set CINDERX_JIT_MODE=canary to run this";         \
    }                                                                    \
  } while (0)

namespace {

std::vector<uint64_t> siteIdsOf(BorrowedRef<PyFunctionObject> func) {
  std::vector<uint64_t> ids;
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  if (compiled == nullptr || compiled->runtime() == nullptr) {
    return ids;
  }
  for (const jit::DeoptMetadata& meta : compiled->runtime()->deoptMetadatas()) {
    if (!meta.frame_meta.empty()) {
      ids.push_back(meta.site_id);
    }
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace

class DeoptSite311Test : public RuntimeTest {};

TEST_F(DeoptSite311Test, SiteIdsAreStableAcrossRecompiles) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def loop(n):
    i = 0
    while i < n:
        i = i + 1
    return i
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "loop"));
  ASSERT_NE(func, nullptr);
  auto n = Ref<>::steal(PyLong_FromLong(8));
  auto args = Ref<>::steal(PyTuple_Pack(1, n.get()));
  for (int i = 0; i < 32; ++i) {
    auto warmed = Ref<>::steal(PyObject_Call(func, args, nullptr));
    ASSERT_NE(warmed, nullptr);
  }

  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  std::vector<uint64_t> first = siteIdsOf(func);
  ASSERT_FALSE(first.empty()) << "warm loop compiled with no deopt sites";
  EXPECT_EQ(std::adjacent_find(first.begin(), first.end()), first.end())
      << "two deopt sites share one stable id";

  auto jit_mod = Ref<>::steal(PyImport_ImportModule("cinderjit"));
  ASSERT_NE(jit_mod, nullptr);
  auto uncompiled = Ref<>::steal(
      PyObject_CallMethod(jit_mod, "force_uncompile", "O", func.get()));
  ASSERT_NE(uncompiled, nullptr);

  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  std::vector<uint64_t> second = siteIdsOf(func);
  EXPECT_EQ(std::adjacent_find(second.begin(), second.end()), second.end())
      << "recompile produced duplicate deopt site ids";
  EXPECT_EQ(first, second);
}

TEST_F(DeoptSite311Test, ForcedAndOrganicEnterTheSameRestore) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def loop(a, b, one):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + one
    return total
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "loop"));
  ASSERT_NE(func, nullptr);
  auto three = Ref<>::steal(PyLong_FromLong(3));
  auto five = Ref<>::steal(PyLong_FromLong(5));
  auto one = Ref<>::steal(PyLong_FromLong(1));
  auto args = Ref<>::steal(PyTuple_Pack(3, three.get(), five.get(), one.get()));
  for (int i = 0; i < 64; ++i) {
    auto warmed = Ref<>::steal(PyObject_Call(func, args, nullptr));
    ASSERT_NE(warmed, nullptr);
  }

  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  ASSERT_NE(compiled, nullptr);
  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  uint64_t guard_site = 0;
  std::size_t guard_deopt_id = 0;
  bool found = false;
  for (std::size_t i = 0; i < rt->deoptMetadatas().size(); ++i) {
    const jit::DeoptMetadata& meta = rt->deoptMetadatas()[i];
    if (meta.reason == jit::DeoptReason::kGuardFailure && meta.forceable &&
        !meta.frame_meta.empty()) {
      guard_site = meta.site_id;
      guard_deopt_id = i;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found) << "warm loop compiled with no forceable guard site";
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 0);

  for (const jit::DeoptMetadata& meta : rt->deoptMetadatas()) {
    if (meta.reason == jit::DeoptReason::kUnhandledException &&
        !meta.frame_meta.empty()) {
      EXPECT_FALSE(rt->armForcedDeopt(meta.site_id, 1, false));
    }
  }

  EXPECT_FALSE(rt->armForcedDeopt(guard_site, 0, false));
  ASSERT_TRUE(rt->armForcedDeopt(guard_site, 2, false));
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 1);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 0);
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 1);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 1);
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 0);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 0);
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 0);

  ASSERT_TRUE(rt->armForcedDeopt(guard_site, 2, true));
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 1);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 0);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 1);
  EXPECT_EQ(JITRT_ConsumeForcedDeopt(rt, guard_deopt_id), 1);
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 1);

  auto saved_config = jit::getConfig();
  SCOPE_EXIT(jit::getMutableConfig() = saved_config);
  auto& config = jit::getMutableConfig();
  config.compile_after_n_calls = 1;
  config.roi_backoff_enabled = true;
  config.roi_deopt_budget_base = 1;
  config.roi_backoff_max_rounds = 1;

  CodeExtra* extra =
      codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);
  Ci_code_extra_store_roi_deopt_count_relaxed(extra, 0);
  Ci_code_extra_store_roi_ctl_release(extra, 0);
  Ci_code_extra_store_roi_recompile_floor_release(extra, 0);

  jit::TriggerStats before = jit::triggerStatsSnapshot();
  ASSERT_TRUE(rt->armForcedDeopt(guard_site, 1, false));
  auto forced = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(forced, nullptr);
  EXPECT_EQ(PyLong_AsLong(forced), 15);
  jit::TriggerStats after_forced = jit::triggerStatsSnapshot();
  EXPECT_EQ(after_forced.forced_deopt_hits, before.forced_deopt_hits + 1);
  EXPECT_EQ(after_forced.organic_deopt_hits, before.organic_deopt_hits);
  EXPECT_EQ(extra->roi_deopt_count, 0);
  EXPECT_EQ(*rt->forcedDeoptArmedAddress(), 0);
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), compiled);

  auto fthree = Ref<>::steal(PyFloat_FromDouble(3.0));
  auto ffive = Ref<>::steal(PyFloat_FromDouble(5.0));
  auto fone = Ref<>::steal(PyFloat_FromDouble(1.0));
  auto fargs =
      Ref<>::steal(PyTuple_Pack(3, fthree.get(), ffive.get(), fone.get()));
  auto organic = Ref<>::steal(PyObject_Call(func, fargs, nullptr));
  ASSERT_NE(organic, nullptr);
  EXPECT_DOUBLE_EQ(PyFloat_AsDouble(organic), 15.0);
  jit::TriggerStats after_organic = jit::triggerStatsSnapshot();
  EXPECT_EQ(after_organic.forced_deopt_hits, after_forced.forced_deopt_hits);
  EXPECT_GT(after_organic.organic_deopt_hits, after_forced.organic_deopt_hits);
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
}

TEST_F(DeoptSite311Test, UnconditionalDeoptIsNotForceable) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def unpack(seq):
    left, right = seq
    return left + right
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "unpack"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  ASSERT_NE(compiled, nullptr);
  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  bool found = false;
  for (const jit::DeoptMetadata& meta : rt->deoptMetadatas()) {
    if (!meta.frame_meta.empty() && meta.descr != nullptr &&
        std::strcmp(meta.descr, "UNPACK_SEQUENCE") == 0) {
      EXPECT_EQ(meta.reason, jit::DeoptReason::kGuardFailure);
      EXPECT_FALSE(meta.forceable);
      EXPECT_FALSE(rt->armForcedDeopt(meta.site_id, 1, false));
      found = true;
    }
  }
  EXPECT_TRUE(found) << "UNPACK_SEQUENCE compiled with no unconditional deopt";
}

TEST_F(DeoptSite311Test, LoadGlobalReexecDoesNotDoublePushNull) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def _g():
    return 7

G = _g

def read():
    return G()
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "read"));
  ASSERT_NE(func, nullptr);
  auto args = Ref<>::steal(PyTuple_New(0));
  for (int i = 0; i < 64; ++i) {
    auto warmed = Ref<>::steal(PyObject_Call(func, args, nullptr));
    ASSERT_NE(warmed, nullptr);
  }
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  ASSERT_NE(compiled, nullptr);
  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  uint64_t site = 0;
  bool found = false;
  for (const jit::DeoptMetadata& meta : rt->deoptMetadatas()) {
    if (meta.reason == jit::DeoptReason::kGuardFailure &&
        !meta.frame_meta.empty() && meta.descr != nullptr &&
        std::strstr(meta.descr, "LOAD_GLOBAL") != nullptr) {
      site = meta.site_id;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found) << "LOAD_GLOBAL compiled with no GuardFailure site";
  ASSERT_TRUE(rt->armForcedDeopt(site, 1, false));

  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 7);
}
#endif
