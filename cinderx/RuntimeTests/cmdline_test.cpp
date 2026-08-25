// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/behavior_classifier.h"
#include "cinderx/Jit/generators_rt.h"
#if PY_VERSION_HEX >= 0x030D0000
#include "cinderx/Jit/osr_capi.h"
#else
// The OSR C-API header needs 3.13+ atomics.  The three state ints the
// fixture saves and restores come from Jit/osr_stub_311.cpp on 3.11.
extern "C" {
extern int cinderx_osr_enabled;
extern int cinderx_osr_capable;
extern int cinderx_osr_state;
}
#endif
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/testutil.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

// Here we make sure that the JIT specific command line arguments
// are being processed correctly to have the required effect
// on the JIT config

using namespace jit;
using namespace jit::lir;

class CmdLineTest : public RuntimeTest {
 public:
  CmdLineTest() : RuntimeTest{RuntimeTest::Flags{}} {}
};

namespace {

class ScopedEnvVar {
 public:
  explicit ScopedEnvVar(const char* name) : name_{name} {
    if (const char* value = getenv(name)) {
      old_value_ = value;
    }
  }

  ~ScopedEnvVar() {
    if (old_value_.has_value()) {
      setenv(name_, old_value_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

  void set(const char* value) {
    setenv(name_, value, 1);
  }

 private:
  const char* name_;
  std::optional<std::string> old_value_;
};

class ScopedJitConfigState {
 public:
  ScopedJitConfigState()
      : frame_mode_{getMutableConfig().frame_mode},
        osr_enabled_{getMutableConfig().osr_enabled},
        osr_capable_{getMutableConfig().osr_capable},
        cinderx_osr_enabled_{cinderx_osr_enabled},
        cinderx_osr_capable_{cinderx_osr_capable},
        cinderx_osr_state_{cinderx_osr_state} {}

  ~ScopedJitConfigState() {
    getMutableConfig().frame_mode = frame_mode_;
    getMutableConfig().osr_enabled = osr_enabled_;
    getMutableConfig().osr_capable = osr_capable_;
    cinderx_osr_enabled = cinderx_osr_enabled_;
    cinderx_osr_capable = cinderx_osr_capable_;
    cinderx_osr_state = cinderx_osr_state_;
  }

 private:
  FrameMode frame_mode_;
  bool osr_enabled_;
  bool osr_capable_;
  int cinderx_osr_enabled_;
  int cinderx_osr_capable_;
  int cinderx_osr_state_;
};

class ScopedAutoJitConfigState {
 public:
  ScopedAutoJitConfigState()
      : compile_after_n_calls_{getMutableConfig().compile_after_n_calls},
        auto_classify_{getMutableConfig().auto_classify},
        enable_startup_init_policy_{
            getMutableConfig().enable_startup_init_policy} {}

  ~ScopedAutoJitConfigState() {
    getMutableConfig().compile_after_n_calls = compile_after_n_calls_;
    getMutableConfig().auto_classify = auto_classify_;
    getMutableConfig().enable_startup_init_policy = enable_startup_init_policy_;
  }

 private:
  std::optional<uint32_t> compile_after_n_calls_;
  bool auto_classify_;
  bool enable_startup_init_policy_;
};

class ScopedXOption {
 public:
  explicit ScopedXOption(const wchar_t* flag) : key_{addToXargsDict(flag)} {}

  ~ScopedXOption() {
    if (key_ != nullptr) {
      PyDict_DelItem(PySys_GetXOptions(), key_);
      Py_DECREF(key_);
    }
  }

 private:
  PyObject* key_;
};

void resetFrameModeAndOSRConfig() {
  getMutableConfig().frame_mode = FrameMode::kNormal;
  getMutableConfig().osr_enabled = false;
  getMutableConfig().osr_capable = false;
  cinderx_osr_enabled = 0;
  cinderx_osr_capable = 0;
  cinderx_osr_state = 0;
}

void resetJitForAutoJitEntryTest() {
  jit::finalize();
  jit::shutdown_jit_genobject_type();
  Ci_FiniFrameEvalFunc();
  getMutableConfig().compile_after_n_calls.reset();
  getMutableConfig().auto_classify = false;
  getMutableConfig().enable_startup_init_policy = false;
}

// Keep these snippets inside RuntimeTest's embedded interpreter: they validate
// JIT config mutations made in this process, which an external Python script
// would not observe.
void assertNewFunctionCountsAndCompiles(RuntimeTest& test) {
  test.runStockCode(R"(
import cinderx
import cinderx.jit as jit

def target(n):
    total = 0
    for value in range(n):
        total += value
    return total

assert jit.get_compile_after_n_calls() == 2
assert cinderx.is_frame_evaluator_installed()
assert jit.count_interpreted_calls(target) == 0
target(5)
target(5)
assert not jit.is_jit_compiled(target)
assert jit.count_interpreted_calls(target) == 2
target(5)
assert jit.is_jit_compiled(target)
)");
}

void assertNewFunctionCountsWithoutFrameEvaluator(RuntimeTest& test) {
  // Start from an unwarmed process so the trivial target sits in the
  // held-call regime deterministically, regardless of test order.
  jit::resetLowRoiReleaseState();
  test.runStockCode(R"(
import cinderx
import cinderx.jit as jit

def target(value):
    return value

assert jit.get_compile_after_n_calls() == 2
assert not cinderx.is_frame_evaluator_installed()
assert jit.count_interpreted_calls(target) == 0
target(1)
target(2)
assert not jit.is_jit_compiled(target)
assert jit.count_interpreted_calls(target) == 2
target(3)
target(4)
target(5)
assert not jit.is_jit_compiled(target)
assert jit.count_interpreted_calls(target) == 5
)");
  jit::resetLowRoiReleaseState();
}

} // namespace

int try_flag_and_envvar_effect(
    const wchar_t* flag,
    const char* env_name,
    std::function<void(void)> reset_vars,
    std::function<void(void)> conditions_to_check,
    bool capture_stderr = false,
    bool capture_stdout = false) {
  // Shutdown the JIT so we can start it up again under different conditions.
  jit::finalize();

  jit::shutdown_jit_genobject_type();

  reset_vars(); // reset variable state before and
  // between flag and cmd line param runs

  int init_status = 0;

  // as env var
  if (nullptr != env_name) {
    if (capture_stderr) {
      testing::internal::CaptureStderr();
    }
    if (capture_stdout) {
      testing::internal::CaptureStdout();
    }

    std::string key = parseAndSetEnvVar(env_name);
    init_status = jit::initialize();
    conditions_to_check();
    unsetenv(key.c_str());
    jit::finalize();
    jit::shutdown_jit_genobject_type();
    reset_vars();
  }

  if (capture_stderr) {
    testing::internal::CaptureStderr();
  }
  if (capture_stdout) {
    testing::internal::CaptureStdout();
  }
  // sneak in a command line argument
  PyObject* to_remove = addToXargsDict(flag);
  init_status += jit::initialize();
  conditions_to_check();
  PyDict_DelItem(PySys_GetXOptions(), to_remove);
  Py_DECREF(to_remove);

  jit::finalize();
  reset_vars();

  return init_status;
}

TEST_F(CmdLineTest, BasicFlags) {
  // easy flags that don't interact with one another in tricky ways
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug",
          "PYTHONJITDEBUG",
          []() { getMutableConfig().log.debug = false; },
          []() { ASSERT_TRUE(getConfig().log.debug); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-refcount",
          "PYTHONJITDEBUGREFCOUNT",
          []() { getMutableConfig().log.debug_refcount = false; },
          []() { ASSERT_TRUE(getConfig().log.debug_refcount); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-inliner",
          "PYTHONJITDEBUGINLINER",
          []() { getMutableConfig().log.debug_inliner = false; },
          []() { ASSERT_TRUE(getConfig().log.debug_inliner); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir",
          "PYTHONJITDUMPHIR",
          []() { getMutableConfig().log.dump_hir_initial = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_initial); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir-passes",
          "PYTHONJITDUMPHIRPASSES",
          []() { getMutableConfig().log.dump_hir_passes = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_passes); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-final-hir",
          "PYTHONJITDUMPFINALHIR",
          []() { getMutableConfig().log.dump_hir_final = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_final); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir",
          "PYTHONJITDUMPLIR",
          []() { getMutableConfig().log.dump_lir = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_lir); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir-origin",
          "PYTHONJITDUMPLIRORIGIN",
          []() {
            getMutableConfig().log.dump_lir = false;
            getMutableConfig().log.lir_origin = false;
          },
          []() {
            ASSERT_TRUE(getConfig().log.dump_lir);
            ASSERT_TRUE(getConfig().log.lir_origin);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-asm",
          "PYTHONJITDUMPASM",
          []() { getMutableConfig().log.dump_asm = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_asm); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-support",
          "PYTHONJITGDBSUPPORT",
          []() {
            getMutableConfig().log.debug = false;
            getMutableConfig().gdb.supported = false;
          },
          []() {
            ASSERT_TRUE(getMutableConfig().log.debug);
            ASSERT_TRUE(getConfig().gdb.supported);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-write-elf",
          "PYTHONJITGDBWRITEELF",
          []() {
            getMutableConfig().log.debug = false;
            getMutableConfig().gdb.supported = false;
            getMutableConfig().gdb.write_elf_objects = false;
          },
          []() {
            ASSERT_TRUE(getConfig().log.debug);
            ASSERT_TRUE(getConfig().gdb.supported);
            ASSERT_TRUE(getConfig().gdb.write_elf_objects);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-stats",
          "PYTHONJITDUMPSTATS",
          []() { getMutableConfig().log.dump_stats = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_stats); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable-lir-inliner",
          "PYTHONJITDISABLELIRINLINER",
          []() { getMutableConfig().lir_opts.inliner = 0; },
          []() { ASSERT_EQ(getConfig().lir_opts.inliner, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-prefix-reverse-assign",
          "PYTHONJITLISTPREFIXREVERSEASSIGN",
          []() {
            getMutableConfig().hir_opts.list_prefix_reverse_assign = false;
          },
          []() { ASSERT_TRUE(getConfig().hir_opts.list_prefix_reverse_assign); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-prefix-reverse-assign=0",
          "PYTHONJITLISTPREFIXREVERSEASSIGN=0",
          []() {
            getMutableConfig().hir_opts.list_prefix_reverse_assign = true;
          },
          []() {
            ASSERT_FALSE(getConfig().hir_opts.list_prefix_reverse_assign);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-huge-pages=0",
          "PYTHONJITHUGEPAGES=0",
          []() {},
          []() { ASSERT_FALSE(getConfig().use_huge_pages); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-huge-pages=1",
          "PYTHONJITHUGEPAGES=1",
          []() {},
          []() {
#if PY_VERSION_HEX < 0x030C0000
            // The 3.11 surface answers an explicit huge-page request with
            // the reclaiming allocator, and the configuration must read
            // the effective policy rather than the request.
            ASSERT_FALSE(getConfig().use_huge_pages);
#else
            ASSERT_TRUE(getConfig().use_huge_pages);
#endif
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-enable-jit-list-wildcards",
          "PYTHONJITENABLEJITLISTWILDCARDS",
          []() {},
          []() { ASSERT_TRUE(getConfig().allow_jit_list_wildcards); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-all-static-functions",
          "PYTHONJITALLSTATICFUNCTIONS",
          []() {},
          []() { ASSERT_TRUE(getConfig().compile_all_static_functions); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-perfmap",
          "JIT_PERFMAP",
          []() { perf::jit_perfmap = 0; },
          []() { ASSERT_EQ(perf::jit_perfmap, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-perf-dumpdir=/tmp/",
          "JIT_DUMPDIR=/tmp/",
          []() { perf::perf_jitdump_dir = ""; },
          []() { ASSERT_EQ(perf::perf_jitdump_dir, "/tmp/"); }),
      0);
}

TEST_F(CmdLineTest, JITEnable) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-all",
          "PYTHONJITALL",
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = true;
#if PY_VERSION_HEX < 0x030C0000
            // Traditional execution flags do not select shadow in production;
            // force initialization only for this config parser test.
            getMutableConfig().force_init = true;
#endif
          },
          []() {
#if PY_VERSION_HEX < 0x030C0000
            ASSERT_TRUE(isJitShadow());
            ASSERT_FALSE(isJitUsable());
#else
            ASSERT_TRUE(isJitUsable());
#endif
            ASSERT_EQ(getConfig().compile_after_n_calls, 0);
            ASSERT_FALSE(getConfig().auto_classify);
            ASSERT_EQ(
                getConfig().asm_syntax,
                AsmSyntax::ATT); // default to AT&T syntax
          }),
      0);
}

TEST_F(CmdLineTest, JITAutoNumericKeepsClassificationOff) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-auto=2",
          "PYTHONJITAUTO=2",
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = true;
          },
          []() {
            ASSERT_EQ(getConfig().compile_after_n_calls, 2);
            ASSERT_FALSE(getConfig().auto_classify);
          }),
      0);
}

TEST_F(CmdLineTest, JITAutoEmptyXOptionKeepsLegacyThresholdOne) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-auto",
          nullptr,
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = true;
          },
          []() {
            ASSERT_EQ(getConfig().compile_after_n_calls, 1);
            ASSERT_FALSE(getConfig().auto_classify);
          }),
      0);
}

TEST_F(CmdLineTest, JITAutoKeywordEnablesClassification) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-auto=auto:7",
          "PYTHONJITAUTO=auto:7",
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = false;
          },
          []() {
            ASSERT_EQ(getConfig().compile_after_n_calls, 7);
            ASSERT_TRUE(getConfig().auto_classify);
          }),
      0);
}

TEST_F(CmdLineTest, JITAutoKeywordUsesDefaultThreshold) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-auto=auto",
          "PYTHONJITAUTO=auto",
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = false;
          },
          []() {
            ASSERT_EQ(getConfig().compile_after_n_calls, 2);
            ASSERT_TRUE(getConfig().auto_classify);
          }),
      0);
}

TEST_F(CmdLineTest, JITAutoNumericEnvInstallsFrameEvaluator) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar env{"PYTHONJITAUTO"};
  resetJitForAutoJitEntryTest();
  env.set("2");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_FALSE(getConfig().auto_classify);

  assertNewFunctionCountsAndCompiles(*this);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoNumericEnvKeepsStartupInitPolicyDisabled) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar env{"PYTHONJITAUTO"};
  resetJitForAutoJitEntryTest();
  env.set("2");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_FALSE(getConfig().auto_classify);
  EXPECT_FALSE(getConfig().enable_startup_init_policy);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoEnvAutoModeCountsWithoutFrameEvaluator) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar env{"PYTHONJITAUTO"};
  resetJitForAutoJitEntryTest();
  env.set("auto:2");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);

  assertNewFunctionCountsWithoutFrameEvaluator(*this);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoEnvAutoModeDoesNotScheduleExistingFunctions) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar env{"PYTHONJITAUTO"};
  resetJitForAutoJitEntryTest();
  runStockCode(R"(
def existing_function(value):
    return value
)");
  Ref<> existing_function = getGlobal("existing_function");
  ASSERT_TRUE(PyFunction_Check(existing_function));
  BorrowedRef<PyFunctionObject> func{existing_function};
  vectorcallfunc original_vectorcall = func->vectorcall;

  env.set("auto:2");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);
  EXPECT_EQ(func->vectorcall, original_vectorcall);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoXOptionAutoModeCountsWithoutFrameEvaluator) {
  ScopedAutoJitConfigState config_guard;
  ScopedXOption xoption{L"jit-auto=auto:2"};
  resetJitForAutoJitEntryTest();
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);

  assertNewFunctionCountsWithoutFrameEvaluator(*this);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoImportProviderEnablesStartupInitPolicy) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar auto_env{"PYTHONJITAUTO"};
  ScopedEnvVar provider_env{"CINDERX_AUTOJIT_IMPORT_PROVIDER"};
  resetJitForAutoJitEntryTest();
  auto_env.set("auto:2");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);

  jit::finalize();
  jit::shutdown_jit_genobject_type();

  resetJitForAutoJitEntryTest();
  auto_env.set("auto:2");
  provider_env.set("builtins");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);

  jit::finalize();
  jit::shutdown_jit_genobject_type();

  resetJitForAutoJitEntryTest();
  auto_env.set("auto:2");
  provider_env.set("find_and_load");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_TRUE(getConfig().enable_startup_init_policy);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoImportProviderOffKeepsStartupInitPolicyDisabled) {
  ScopedAutoJitConfigState config_guard;
  ScopedEnvVar auto_env{"PYTHONJITAUTO"};
  ScopedEnvVar provider_env{"CINDERX_AUTOJIT_IMPORT_PROVIDER"};
  resetJitForAutoJitEntryTest();
  auto_env.set("auto:2");
  provider_env.set("off");
  ASSERT_EQ(jit::initialize(), 0);
  EXPECT_EQ(getConfig().compile_after_n_calls, 2);
  EXPECT_TRUE(getConfig().auto_classify);
  EXPECT_FALSE(getConfig().enable_startup_init_policy);

  jit::finalize();
  jit::shutdown_jit_genobject_type();
}

TEST_F(CmdLineTest, JITAutoMalformedInputPreservesExistingConfig) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-auto=auto:not-a-number",
          "PYTHONJITAUTO=auto:not-a-number",
          []() {
            getMutableConfig().compile_after_n_calls.reset();
            getMutableConfig().auto_classify = false;
          },
          []() {
            ASSERT_FALSE(getConfig().compile_after_n_calls.has_value());
            ASSERT_FALSE(getConfig().auto_classify);
          }),
      0);
}

TEST_F(CmdLineTest, OSREnabledFlag) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"osr-enabled",
          "CINDERX_OSR_ENABLED",
          []() { getMutableConfig().osr_enabled = false; },
          []() { ASSERT_TRUE(getConfig().osr_enabled); }),
      0);
}

TEST_F(CmdLineTest, OSREnabledFlagSyncsRuntimeGate) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"osr-enabled",
          "CINDERX_OSR_ENABLED",
          []() {
            getMutableConfig().osr_enabled = false;
            getMutableConfig().osr_capable = false;
            cinderx_osr_enabled = 0;
            cinderx_osr_capable = 0;
            cinderx_osr_state = 0;
          },
          []() {
            ASSERT_TRUE(getConfig().osr_enabled);
            ASSERT_TRUE(getConfig().osr_capable);
            ASSERT_EQ(cinderx_osr_enabled, 1);
            ASSERT_EQ(cinderx_osr_capable, 1);
            ASSERT_EQ(cinderx_osr_state, 1);
          }),
      0);
}

TEST_F(CmdLineTest, LightweightFrameFlagRequiresCompileSupport) {
  ScopedEnvVar lightweight_env{"PYTHONJITLIGHTWEIGHTFRAME"};
  ScopedJitConfigState config_guard;
  jit::finalize();
  jit::shutdown_jit_genobject_type();
  resetFrameModeAndOSRConfig();
  lightweight_env.set("1");
  int init_status = jit::initialize();
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  ASSERT_EQ(init_status, 0);
  EXPECT_EQ(getConfig().frame_mode, FrameMode::kLightweight);
  jit::finalize();
#else
  ASSERT_EQ(init_status, -1);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();
#endif
  jit::shutdown_jit_genobject_type();
}

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
TEST_F(CmdLineTest, LightweightFrameRejectsOSRConflict) {
  ScopedEnvVar lightweight_env{"PYTHONJITLIGHTWEIGHTFRAME"};
  ScopedEnvVar osr_env{"CINDERX_OSR_ENABLED"};
  ScopedJitConfigState config_guard;
  jit::finalize();
  jit::shutdown_jit_genobject_type();
  resetFrameModeAndOSRConfig();
  lightweight_env.set("1");
  osr_env.set("1");

  int init_status = jit::initialize();
  ASSERT_EQ(init_status, -1);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();

  jit::shutdown_jit_genobject_type();
}
#endif

TEST_F(CmdLineTest, OSRBackedgeThresholdFlag) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"osr-backedge-threshold=7",
          "CINDERX_OSR_BACKEDGE_THRESHOLD=7",
          []() { getMutableConfig().osr_backedge_threshold = 2000; },
          []() { ASSERT_EQ(getConfig().osr_backedge_threshold, 7); }),
      0);
}

TEST_F(CmdLineTest, OSRCompileBudgetFlag) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"osr-compile-budget=17",
          "CINDERX_OSR_COMPILE_BUDGET=17",
          []() { getMutableConfig().osr_compile_budget_code_units = 1024; },
          []() { ASSERT_EQ(getConfig().osr_compile_budget_code_units, 17); }),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MultithreadCompile) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          []() {},
          []() { ASSERT_TRUE(getConfig().multithreaded_compile_test); }),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MatchLineNumbers) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-match-line-numbers",
          "PYTHONJITLISTMATCHLINENUMBERS",
          []() { getMutableConfig().jit_list.match_line_numbers = false; },
          []() { ASSERT_TRUE(getConfig().jit_list.match_line_numbers); }),
      0);
}

// end of tests associated with flags the setting of which is dependent upon if
// jit is enabled

TEST_F(CmdLineTest, JITEnabledFlags_BatchCompileWorkers) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-batch-compile-workers=21",
          "PYTHONJITBATCHCOMPILEWORKERS=21",
          []() {},
          []() { ASSERT_EQ(getConfig().batch_compile_workers, 21); }),
      0);
}

TEST_F(CmdLineTest, ASMSyntax) {
  // default when nothing defined is AT&T, covered in prvious test
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=intel",
          "PYTHONJITASMSYNTAX=intel",
          []() { getMutableConfig().asm_syntax = AsmSyntax::ATT; },
          []() { ASSERT_EQ(getConfig().asm_syntax, AsmSyntax::Intel); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=att",
          "PYTHONJITASMSYNTAX=att",
          []() { getMutableConfig().asm_syntax = AsmSyntax::ATT; },
          []() { ASSERT_EQ(getConfig().asm_syntax, AsmSyntax::ATT); }),
      0);
}

const wchar_t* makeWideChar(const char* to_convert) {
  const size_t cSize = strlen(to_convert) + 1;
  wchar_t* wide = new wchar_t[cSize];
  mbstowcs(wide, to_convert, cSize);

  return wide;
}

TEST_F(CmdLineTest, JITList) {
  std::string list_file = tmpnam(nullptr);
  std::ofstream list_file_handle(list_file);
  list_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-list-file=" + list_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLISTFILE=" + list_file).c_str()),
          []() {
            getMutableConfig().asm_syntax = AsmSyntax::ATT;
#if PY_VERSION_HEX < 0x030C0000
            getMutableConfig().force_init = true;
#endif
          },
          []() {
#if PY_VERSION_HEX < 0x030C0000
            ASSERT_TRUE(isJitShadow());
            ASSERT_FALSE(isJitUsable());
#else
            ASSERT_TRUE(isJitUsable());
#endif
          }),
      0);

  delete[] xarg;
  std::filesystem::remove(list_file);
}

TEST_F(CmdLineTest, JITLogFile) {
  std::string log_file = tmpnam(nullptr);
  std::ofstream log_file_handle(log_file);
  log_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-log-file=" + log_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLOGFILE=" + log_file).c_str()),
          []() { getMutableConfig().log.output_file = stderr; },
          []() { ASSERT_NE(getConfig().log.output_file, stderr); }),
      0);

  delete[] xarg;
  std::filesystem::remove(log_file);
}

TEST_F(CmdLineTest, ExplicitJITDisable) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable",
          "PYTHONJITDISABLE",
          []() {},
          []() { ASSERT_FALSE(isJitUsable()); }),
      0);
}

TEST_F(CmdLineTest, DisplayHelpMessage) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-help",
          nullptr,
          []() {},
          []() {
            ASSERT_TRUE(
                testing::internal::GetCapturedStdout().find(
                    "-X opt : set Cinder JIT-specific option.") !=
                std::string::npos);
          },
          false /* capture_stderr */,
          true /* capture_stdout */),
      -2);
}
