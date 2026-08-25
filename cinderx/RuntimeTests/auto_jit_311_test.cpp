// Copyright (c) Meta Platforms, Inc. and affiliates.

// The CPython 3.11 execute mode's scheduling machinery (MR-11): the mode
// resolver, the one-attempt-per-code verdict recorded on the code object,
// fresh function objects attaching to a published artifact within the
// per-code budget, and the outer function that anchors a nested artifact.

#include "cinderx/python.h"

#include <gtest/gtest.h>

#if PY_VERSION_HEX < 0x030B0000 || PY_VERSION_HEX >= 0x030C0000
// The execute-mode scheduler under test exists only on CPython 3.11.
#else

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#define SKIP_311_EXECUTABLE_COMPILE()                                     \
  do {                                                                    \
    if (jit::getConfig().state != jit::State::kRunning) {                 \
      GTEST_SKIP() << "3.11 executes machine code only in execute mode; " \
                      "set CINDERX_JIT_MODE=canary to run this";          \
    }                                                                     \
  } while (0)

namespace {

constexpr const char* kFactorySource = R"(
def factory(k):
    def adder(x, y):
        total = x - x
        i = total
        while i < y:
            total = total + x + k
            i = i + 1
        return total
    return adder
)";

// Restores one environment variable on scope exit.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_{name} {
    const char* old = std::getenv(name);
    if (old != nullptr) {
      old_ = old;
    }
    if (value != nullptr) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnv() {
    if (old_.has_value()) {
      setenv(name_, old_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

 private:
  const char* name_;
  std::optional<std::string> old_;
};

jit::Context* context() {
  return static_cast<jit::Context*>(
      cinderx::getModuleState()->jit_context.get());
}

jit::CompiledFunction* installedArtifact(BorrowedRef<PyFunctionObject> func) {
  return reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
}

} // namespace

// The configuration half runs in every mode (green family); the scheduling
// half needs machine code and runs in the execute-mode leg.
class AutoJit311ConfigTest : public RuntimeTest {};

class AutoJit311Test : public RuntimeTest {
 protected:
  // Evaluate `call` in the test globals and hand back the function object
  // it produced, leaving no reference to it behind in the namespace.
  Ref<PyFunctionObject> makeInstance(const char* call) {
    std::string src = std::string("__instance = ") + call + "\n";
    runCode(src.c_str());
    Ref<> obj = getGlobal("__instance");
    runCode("del __instance\n");
    EXPECT_TRUE(PyFunction_Check(obj.get()));
    return Ref<PyFunctionObject>::create(
        reinterpret_cast<PyFunctionObject*>(obj.get()));
  }

  long callInstance(BorrowedRef<PyFunctionObject> func, long x, long y) {
    Ref<> a = Ref<>::steal(PyLong_FromLong(x));
    Ref<> b = Ref<>::steal(PyLong_FromLong(y));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), a.get(), b.get(), nullptr));
    EXPECT_NE(result, nullptr);
    if (result == nullptr) {
      PyErr_Print();
      return -1;
    }
    return PyLong_AsLong(result);
  }
};

TEST_F(AutoJit311ConfigTest, ModeResolverAcceptsTheProductSpellings) {
  struct Case {
    const char* spelling;
    Ci_JitMode311 mode;
    const char* reported;
  };
  const Case cases[] = {
      {"off", CI_JIT_MODE_311_OFF, "off"},
      {"observe", CI_JIT_MODE_311_OBSERVE, "observe"},
      {"shadow", CI_JIT_MODE_311_SHADOW, "shadow"},
      {"execute", CI_JIT_MODE_311_EXECUTE, "execute"},
      {"canary", CI_JIT_MODE_311_EXECUTE, "canary"},
  };
  ScopedEnv disable_a("PYTHONJITDISABLE", nullptr);
  ScopedEnv disable_b("CINDERX_JIT_DISABLE", nullptr);
  for (const Case& c : cases) {
    ScopedEnv env("CINDERX_JIT_MODE", c.spelling);
    Ci_JitMode311 mode = CI_JIT_MODE_311_OFF;
    const char* spelling = nullptr;
    ASSERT_EQ(Ci_Observe311_ResolveMode(&mode, &spelling), 0) << c.spelling;
    EXPECT_EQ(mode, c.mode) << c.spelling;
    EXPECT_STREQ(spelling, c.reported);
  }
  {
    // Unset and empty both mean off.
    ScopedEnv env("CINDERX_JIT_MODE", nullptr);
    Ci_JitMode311 mode = CI_JIT_MODE_311_EXECUTE;
    ASSERT_EQ(Ci_Observe311_ResolveMode(&mode, nullptr), 0);
    EXPECT_EQ(mode, CI_JIT_MODE_311_OFF);
  }
  {
    ScopedEnv env("CINDERX_JIT_MODE", "turbo");
    Ci_JitMode311 mode = CI_JIT_MODE_311_OFF;
    const char* spelling = nullptr;
    EXPECT_EQ(Ci_Observe311_ResolveMode(&mode, &spelling), -1);
    EXPECT_NE(PyErr_Occurred(), nullptr);
    PyErr_Clear();
    EXPECT_STREQ(spelling, "unknown");
  }
}

TEST_F(AutoJit311ConfigTest, DisableSwitchesOutrankTheExecuteMode) {
  ScopedEnv env("CINDERX_JIT_MODE", "execute");
  for (const char* name : {"PYTHONJITDISABLE", "CINDERX_JIT_DISABLE"}) {
    for (const char* value : {"1", "true", "YES", "on"}) {
      ScopedEnv sw(name, value);
      Ci_JitMode311 mode = CI_JIT_MODE_311_EXECUTE;
      const char* spelling = nullptr;
      ASSERT_EQ(Ci_Observe311_ResolveMode(&mode, &spelling), 0);
      EXPECT_EQ(mode, CI_JIT_MODE_311_OFF) << name << "=" << value;
      // The spelling still reports what was asked for.
      EXPECT_STREQ(spelling, "execute");
    }
    // "0" is not a disable.
    ScopedEnv sw(name, "0");
    Ci_JitMode311 mode = CI_JIT_MODE_311_OFF;
    ASSERT_EQ(Ci_Observe311_ResolveMode(&mode, nullptr), 0);
    EXPECT_EQ(mode, CI_JIT_MODE_311_EXECUTE);
  }
  // The switches do not touch the diagnostic modes.
  ScopedEnv shadow("CINDERX_JIT_MODE", "shadow");
  ScopedEnv sw("PYTHONJITDISABLE", "1");
  Ci_JitMode311 mode = CI_JIT_MODE_311_OFF;
  ASSERT_EQ(Ci_Observe311_ResolveMode(&mode, nullptr), 0);
  EXPECT_EQ(mode, CI_JIT_MODE_311_SHADOW);
}

TEST_F(AutoJit311Test, FreshInstanceAttachesToThePublishedArtifact) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(first));
  jit::CompiledFunction* artifact = installedArtifact(first);
  ASSERT_NE(artifact, nullptr);
  uint64_t made = jit::triggerStatsSnapshot().compiled_function_creations;

  Ref<PyFunctionObject> second = makeInstance("factory(2)");
  ASSERT_NE(second, nullptr);
  EXPECT_FALSE(isJitCompiled(second));
  EXPECT_EQ(Ci_JitShell311_AttachFresh(second), 1);
  EXPECT_TRUE(isJitCompiled(second));
  EXPECT_EQ(installedArtifact(second), artifact)
      << "the fresh instance did not attach to the code's own artifact";
  EXPECT_EQ(jit::triggerStatsSnapshot().compiled_function_creations, made)
      << "attachment compiled the code again";
  EXPECT_TRUE(artifact->functions().contains(second.get()));

  // A member is not fresh: asking again does nothing and costs nothing.
  EXPECT_EQ(Ci_JitShell311_AttachFresh(second), 0);
  EXPECT_EQ(Ci_JitShell311_AttachFresh(first), 0);

  CodeExtra* extra =
      codeExtraIfExists(reinterpret_cast<PyCodeObject*>(second->func_code));
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(Ci_code_extra_jit311_attach_count(extra), 1u);

  uint64_t before = jit::triggerStatsSnapshot().machine_code_entries;
  EXPECT_EQ(callInstance(second, 2, 3), 3 * (2 + 2));
  EXPECT_EQ(callInstance(first, 2, 3), 3 * (2 + 1));
  EXPECT_EQ(jit::triggerStatsSnapshot().machine_code_entries, before + 2);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, CodeStateDiagnosticReportsInstalledArtifactPolicy) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  runCode(R"(
import cinderjit
state_function = factory(1)
assert cinderjit.force_compile(state_function) is True
state = cinderjit._jit311_code_state(state_function)
assert state["installed"] is True
assert state["code_has_artifact"] is True
assert state["artifact_member"] is True
assert state["auto_jit_disabled"] is False
assert state["policy_reason"] == "installed"
assert state["code_id"] == id(state_function.__code__)
assert state["function_id"] == id(state_function)
assert state["fresh_attach_count"] == 0
assert state["fresh_attach_budget"] >= 0
)");
}

TEST_F(AutoJit311Test, AttachmentBudgetIsPerCodeAndFinal) {
  SKIP_311_EXECUTABLE_COMPILE();

  uint32_t saved = jit::getConfig().fresh_attach_budget;
  jit::getMutableConfig().fresh_attach_budget = 2;
  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);

  Ref<PyFunctionObject> a = makeInstance("factory(2)");
  Ref<PyFunctionObject> b = makeInstance("factory(3)");
  Ref<PyFunctionObject> c = makeInstance("factory(4)");
  EXPECT_EQ(Ci_JitShell311_AttachFresh(a), 1);
  EXPECT_EQ(Ci_JitShell311_AttachFresh(b), 1);
  // Exhausted: the answer is "never again for this code object".
  EXPECT_EQ(Ci_JitShell311_AttachFresh(c), -1);
  EXPECT_FALSE(isJitCompiled(c));
  // Dead members do not refund the budget: the count is a lifetime count.
  a.reset();
  b.reset();
  PyGC_Collect();
  EXPECT_EQ(Ci_JitShell311_AttachFresh(c), -1);

  // Explicit compilation is not budgeted, and still attaches.
  uint64_t made = jit::triggerStatsSnapshot().compiled_function_creations;
  EXPECT_EQ(jit::compileFunction(c), jit::Result::OK);
  EXPECT_TRUE(isJitCompiled(c));
  EXPECT_EQ(jit::triggerStatsSnapshot().compiled_function_creations, made);

  // A budget of zero turns automatic attachment off entirely.
  jit::getMutableConfig().fresh_attach_budget = 0;
  Ref<PyFunctionObject> d = makeInstance("factory(5)");
  EXPECT_EQ(Ci_JitShell311_AttachFresh(d), -1);
  jit::getMutableConfig().fresh_attach_budget = saved;
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, NamespaceTwinNeverAttaches) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);

  Ref<> other_globals = Ref<>::steal(PyDict_New());
  ASSERT_NE(other_globals, nullptr);
  ASSERT_EQ(
      PyDict_SetItemString(other_globals, "__builtins__", PyEval_GetBuiltins()),
      0);
  Ref<PyFunctionObject> twin = Ref<PyFunctionObject>::steal(
      PyFunction_New(first->func_code, other_globals));
  ASSERT_NE(twin, nullptr);
  EXPECT_EQ(Ci_JitShell311_AttachFresh(twin), 0);
  EXPECT_FALSE(isJitCompiled(twin));
  EXPECT_STREQ(
      Ci_JitShell311_ExecuteRefusal(twin),
      "REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED");
  EXPECT_EQ(jit::compileFunction(twin), jit::Result::CANNOT_SPECIALIZE);
  // The first owner is untouched.
  EXPECT_TRUE(isJitCompiled(first));
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, FailedAutomaticAttemptDisablesTheCodeObject) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  auto code = reinterpret_cast<PyCodeObject*>(first->func_code);
  ASSERT_FALSE(codeAutoJitDisabled311(code));

  // A suppressed function is refused at its automatic attempt, and the
  // refusal is the verdict for the code object.
  code->co_flags |= CI_CO_SUPPRESS_JIT;
  EXPECT_STREQ(
      Ci_JitShell311_RequestCompile(
          first, reinterpret_cast<PyCodeObject*>(first->func_code)),
      "REFUSE_SHAPE_JIT_SUPPRESSED");
  EXPECT_TRUE(codeAutoJitDisabled311(code));
  EXPECT_TRUE(Ci_JitShell311_CodeAutoJitDisabled(code));
  EXPECT_FALSE(isJitCompiled(first));

  // Lifting the suppression does not reopen automatic compilation: the
  // scheduler asks once, and the verdict stands for every door.
  code->co_flags &= ~CI_CO_SUPPRESS_JIT;
  EXPECT_STREQ(
      Ci_JitShell311_RequestCompile(
          first, reinterpret_cast<PyCodeObject*>(first->func_code)),
      "REFUSE_SHAPE_AUTO_JIT_DISABLED");
  Ref<PyFunctionObject> second = makeInstance("factory(2)");
  EXPECT_EQ(Ci_JitShell311_AttachFresh(second), -1);

  // The explicit path is not bound by the verdict.
  EXPECT_EQ(jit::compileFunction(first), jit::Result::OK);
  EXPECT_TRUE(isJitCompiled(first));
  EXPECT_TRUE(codeAutoJitDisabled311(code));
  // ...and once compiled by hand, the request reports what it finds.
  EXPECT_STREQ(
      Ci_JitShell311_RequestCompile(
          first, reinterpret_cast<PyCodeObject*>(first->func_code)),
      "REFUSE_SHAPE_AUTO_JIT_DISABLED");
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, OuterFunctionAnchorsTheNestedArtifact) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_NE(first, nullptr);
  auto code = reinterpret_cast<PyCodeObject*>(first->func_code);
  ASSERT_NE(code->co_flags & CO_NESTED, 0);
  Ref<> factory_obj = getGlobal("factory");
  BorrowedRef<PyFunctionObject> factory{factory_obj.get()};
  ASSERT_NE(factory, nullptr);

  // The outer is found through the namespace binding, without a frame.
  size_t watched = context()->watchedFunctionCount();
  Ci_JitShell311_TrackOuterFromFrame(first, nullptr);
  auto& outer_rows = context()->codeOuterFunctions();
  auto row = outer_rows.find(BorrowedRef<PyCodeObject>{code});
  ASSERT_NE(row, outer_rows.end()) << "the nested code was not mapped";
  EXPECT_EQ(row->second, factory);
  EXPECT_GT(context()->watchedFunctionCount(), watched)
      << "the outer function was registered without a death watch";

  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);
  jit::CompiledFunction* artifact = installedArtifact(first);
  ASSERT_NE(artifact, nullptr);
  Ref<> nested = Ref<>::steal(PyObject_GetAttrString(
      reinterpret_cast<PyObject*>(factory.get()),
      "__cinderx_nested_compiled_funcs__"));
  ASSERT_NE(nested, nullptr) << "the artifact was not anchored on the outer";
  ASSERT_TRUE(PyList_CheckExact(nested.get()));
  ASSERT_EQ(PyList_GET_SIZE(nested.get()), 1);
  EXPECT_EQ(
      PyList_GET_ITEM(nested.get(), 0), reinterpret_cast<PyObject*>(artifact));
  nested.reset();

  // The compiled instance dies; the artifact stays resident for the next
  // instance, which attaches to it.
  uint64_t resident = jit::triggerStatsSnapshot().resident_code_buffers;
  first.reset();
  PyGC_Collect();
  EXPECT_EQ(jit::triggerStatsSnapshot().resident_code_buffers, resident)
      << "the artifact died with the instance that was compiled";
  Ref<PyFunctionObject> second = makeInstance("factory(2)");
  EXPECT_EQ(Ci_JitShell311_AttachFresh(second), 1);
  EXPECT_EQ(installedArtifact(second), artifact);
  second.reset();
  PyGC_Collect();

  // Dropping the outer function (the namespace binding) releases the
  // machine code, and its death erases the outer row.
  factory_obj.reset();
  runCode("del factory\n");
  PyGC_Collect();
  EXPECT_EQ(jit::triggerStatsSnapshot().resident_code_buffers, resident - 1)
      << "the outer function's death did not release the artifact";
  EXPECT_EQ(
      context()->codeOuterFunctions().count(BorrowedRef<PyCodeObject>{code}),
      0u)
      << "a dead outer function is still registered";
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, OuterRowsAreErasedAcrossACodeSwap) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The outer map holds borrowed pointers kept honest by the death
  // notification; a function whose __code__ moved after registration must
  // still have its rows erased, or a row names freed memory.
  runCode(kFactorySource);
  Ref<> factory_obj = getGlobal("factory");
  BorrowedRef<PyFunctionObject> factory{factory_obj.get()};
  ASSERT_NE(factory, nullptr);
  Ref<PyFunctionObject> instance = makeInstance("factory(1)");
  ASSERT_NE(instance, nullptr);
  auto nested_code = reinterpret_cast<PyCodeObject*>(instance->func_code);

  Ci_JitShell311_TrackOuterFromFrame(instance, nullptr);
  auto& rows = context()->codeOuterFunctions();
  ASSERT_NE(rows.find(BorrowedRef<PyCodeObject>{nested_code}), rows.end())
      << "the nested code was not registered";

  // Replace the outer's code, keeping the original alive so its own death
  // notification cannot do the cleaning.
  Ref<> old_code = Ref<>::create(factory->func_code);
  const char* other = R"(
def other(k):
    return k
)";
  Ref<PyFunctionObject> replacement(compileAndGet(other, "other"));
  ASSERT_NE(replacement, nullptr);
  ASSERT_EQ(
      PyObject_SetAttrString(
          reinterpret_cast<PyObject*>(factory.get()),
          "__code__",
          replacement->func_code),
      0);

  // Drop the outer.  Every row it created must go with it.
  auto* dead = factory.get();
  factory_obj.reset();
  runCode("del factory\n");
  instance.reset();
  PyGC_Collect();

  for (auto& row : context()->codeOuterFunctions()) {
    EXPECT_NE(row.second.get(), dead)
        << "an outer-function row survived the function it names";
  }
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, MemberDeathLeavesTheOthersCompiled) {
  SKIP_311_EXECUTABLE_COMPILE();

  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);
  Ref<PyFunctionObject> second = makeInstance("factory(2)");
  ASSERT_EQ(Ci_JitShell311_AttachFresh(second), 1);
  jit::CompiledFunction* artifact = installedArtifact(first);

  first.reset();
  PyGC_Collect();
  EXPECT_TRUE(isJitCompiled(second));
  EXPECT_EQ(installedArtifact(second), artifact);
  EXPECT_EQ(artifact->functions().size(), 1u);
  EXPECT_EQ(callInstance(second, 2, 3), 3 * (2 + 2));
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, UncompilingOneMemberRetiresTheArtifactForAll) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Uncompile takes the CODE off machine code, as on 3.12+: every member
  // returns to the interpreter, and nothing attaches to the retired
  // artifact afterwards.
  runCode(kFactorySource);
  Ref<PyFunctionObject> first = makeInstance("factory(1)");
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);
  Ref<PyFunctionObject> second = makeInstance("factory(2)");
  ASSERT_EQ(Ci_JitShell311_AttachFresh(second), 1);

  jit::uncompile(first);
  EXPECT_FALSE(isJitCompiled(first));
  EXPECT_FALSE(isJitCompiled(second));
  Ref<PyFunctionObject> third = makeInstance("factory(3)");
  EXPECT_EQ(Ci_JitShell311_AttachFresh(third), 0);
  EXPECT_FALSE(isJitCompiled(third));
  EXPECT_EQ(callInstance(second, 2, 3), 3 * (2 + 2));
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(AutoJit311Test, AFailedCodeExtraAllocationLeavesTheGaugeBalanced) {
  // The residency gauge must be balanced on every path, including the one
  // where storing into co_extra fails and gives the block back.  (The gate
  // is needed because AutoJit311Test only runs in the canary arm.)
  SKIP_311_EXECUTABLE_COMPILE();
  const char* source =
      "def gauge_probe(a, b):\n"
      "    total = a + b\n"
      "    return total";
  Ref<PyFunctionObject> func(compileAndGet(source, "gauge_probe"));
  ASSERT_NE(func, nullptr);
  BorrowedRef<PyCodeObject> code{func->func_code};

  // A code object with no block yet: the failing path only runs on the
  // allocating call.
  ASSERT_EQ(codeExtraIfExists(code), nullptr);

  size_t before = liveCodeExtraBlocks();
  jit::failJitPublishStepForTest(5);
  CodeExtra* extra = codeExtra(code);
  jit::failJitPublishStepForTest(0);

  EXPECT_EQ(extra, nullptr);
  EXPECT_EQ(liveCodeExtraBlocks(), before);
  PyErr_Clear();

  // And the successful call that follows still accounts for itself.
  ASSERT_NE(codeExtra(code), nullptr);
  EXPECT_EQ(liveCodeExtraBlocks(), before + 1);
}

#endif
