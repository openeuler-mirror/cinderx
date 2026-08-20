// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"
#if PY_VERSION_HEX < 0x030C0000
// The MR-04 execute surface predicates the lifecycle cases assert on.
#include "cinderx/Interpreter/3.11/observe.h"
#endif

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

#if PY_VERSION_HEX < 0x030C0000
// A mode gate, not a version gate.  These cases compile and install
// machine code, which on 3.11 the executing (canary) mode does and the
// shadow mode does not -- so what decides is the mode the binary was
// started in, not the version it was built for.  Left as a version gate
// they skipped on every 3.11 build, including the sanitized one, which is
// the only place a use-after-free in the install and lifecycle paths would
// actually be caught.  Run the binary with CINDERX_JIT_MODE=canary to
// execute them.
#define SKIP_311_EXECUTABLE_COMPILE()                                    \
  do {                                                                   \
    if (jit::getConfig().state != jit::State::kRunning) {                \
      GTEST_SKIP() << "3.11 executes machine code only in canary mode; " \
                      "set CINDERX_JIT_MODE=canary to run this";         \
    }                                                                    \
  } while (0)
#else
#define SKIP_311_EXECUTABLE_COMPILE() static_cast<void>(0)
#endif

#if PY_VERSION_HEX < 0x030C0000
// A milestone gate, not a mode gate.  These cases assert a surface the 3.11
// port has not opened in either mode -- generators and coroutines, the
// opcodes outside the MR-04 execute whitelist, the Static Python runtime
// cache, or control-plane entry points the canary does not publish -- so
// the executing mode refuses to compile them by design.  Running them there
// would only assert that a deliberate refusal is a failure.  The reason
// names the surface the case waits on, so that widening the surface makes
// the skip visible instead of leaving it inert.
#define SKIP_311_UNTIL_SURFACE(reason)                                    \
  do {                                                                    \
    GTEST_SKIP() << "3.11 has not opened this surface yet: " << (reason); \
  } while (0)
#else
#define SKIP_311_UNTIL_SURFACE(reason) static_cast<void>(0)
#endif

class JITContextTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

#if PY_VERSION_HEX < 0x030C0000
TEST_F(JITContextTest, RejectsExecutableCompileInShadowMode) {
#if PY_VERSION_HEX < 0x030C0000
  // The mirror image of SKIP_311_EXECUTABLE_COMPILE: this case asserts
  // that shadow refuses to install, so it is the executing mode that has
  // nothing to say here.
  if (jit::getConfig().state == jit::State::kRunning) {
    GTEST_SKIP() << "shadow-mode assertion; the canary mode installs by "
                    "design";
  }
#endif
  Ref<PyFunctionObject> func(compileAndGet("def func(): return 42", "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);

  auto* module_state = cinderx::getModuleState();
  ASSERT_NE(module_state, nullptr);
  ASSERT_NE(module_state->code_allocator, nullptr);
  size_t used_before = module_state->code_allocator->usedBytes();
  vectorcallfunc vectorcall_before = func->vectorcall;
  jit::TriggerStats trigger_before = jit::triggerStatsSnapshot();

  EXPECT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::CANNOT_SPECIALIZE);

  jit::TriggerStats trigger_after = jit::triggerStatsSnapshot();
  EXPECT_EQ(module_state->code_allocator->usedBytes(), used_before);
  EXPECT_EQ(jit_ctx_->lookupFunc(func), nullptr);
  EXPECT_FALSE(jit_ctx_->didCompile(func));
  EXPECT_EQ(func->vectorcall, vectorcall_before);
  EXPECT_EQ(
      trigger_after.executable_alloc_calls,
      trigger_before.executable_alloc_calls);
  EXPECT_EQ(
      trigger_after.executable_alloc_bytes,
      trigger_before.executable_alloc_bytes);
  EXPECT_EQ(
      trigger_after.compiled_function_creations,
      trigger_before.compiled_function_creations);
  EXPECT_EQ(
      trigger_after.machine_code_entries, trigger_before.machine_code_entries);
  EXPECT_EQ(
      trigger_after.shadow_compile_success,
      trigger_before.shadow_compile_success);
  EXPECT_EQ(
      trigger_after.shadow_codegen_bytes, trigger_before.shadow_codegen_bytes);
}
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(JITContextTest, CanaryExecutesUnspecializedForms) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The contract behind force_compile_warm(): "warm" is warmed interpreter
  // state, not specialized input.  Consuming a specialized instruction is
  // what creates a speculative guard, and guard and deopt metadata belong
  // to MR-07 -- so the executing mode must compile the unspecialized
  // forms, and the attribute-cache arm stays closed until MR-09.  If
  // either flag is found open here, the warm entry has silently started
  // claiming a dimension this milestone does not deliver.
  EXPECT_FALSE(jit::getConfig().specialized_opcodes);
  EXPECT_FALSE(jit::getConfig().attr_caches);
}
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(JITContextTest, CodeCompiledReportsPublicationRefusal) {
  SKIP_311_EXECUTABLE_COMPILE();

  // codeCompiled() sits between "the compiler produced machine code" and
  // "the function is installed", and publication is fallible there: the
  // artifact allocation, the code-extra reservation, and the post-compile
  // execute refusal all live below it.  Its answer is what lets the
  // compile entry stop reporting OK for a function that was never
  // installed.  A keyword-only signature is a deterministic publication
  // refusal, and an empty data block exercises the same early-return
  // paths an allocation failure would take.
  Ref<PyFunctionObject> func(
      compileAndGet("def func(*, flag=None): return flag", "func"));
  ASSERT_NE(func, nullptr);

  vectorcallfunc vectorcall_before = func->vectorcall;
  jit::CompilationKey key{func};
  EXPECT_FALSE(jit_ctx_->codeCompiled(func, key, {}));
  EXPECT_FALSE(jit_ctx_->didCompile(func));
  EXPECT_EQ(jit_ctx_->lookupFunc(func), nullptr);
  EXPECT_EQ(func->vectorcall, vectorcall_before);
  EXPECT_FALSE(PyErr_Occurred());
}
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(JITContextTest, PublicationFailureIsNotReportedAsCompiled) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The pre-compile choke and the install run the same refusal predicate,
  // so every failure a caller can predict is answered before the compiler
  // runs; what remains are the failures only publication can see.  This
  // manufactures one deterministically: a second function sharing the
  // published code object under a fresh namespace forms a new compilation
  // key, so the compile itself succeeds -- and publication then hits the
  // one-artifact-per-code refusal.  The result must say so; reporting OK
  // here was how force_compile() returned True for a function whose every
  // call runs interpreted.
  Ref<PyFunctionObject> owner(compileAndGet(
      "def func(a, b, one):\n"
      "    total = a - a\n"
      "    i = total\n"
      "    while i < b:\n"
      "        total = total + a\n"
      "        i = i + one\n"
      "    return total",
      "func"));
  ASSERT_NE(owner, nullptr);

  std::unique_ptr<jit::hir::Preloader> owner_preloader(
      jit::hir::Preloader::make(
          owner, jit::makeFrameReifier(owner->func_code)));
  ASSERT_NE(owner_preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *owner_preloader, owner),
      jit::Result::OK);
  ASSERT_TRUE(jit_ctx_->didCompile(owner));

  auto globals = Ref<>::steal(PyDict_New());
  ASSERT_NE(globals, nullptr);
  auto second =
      Ref<PyFunctionObject>::steal(reinterpret_cast<PyFunctionObject*>(
          PyFunction_New(owner->func_code, globals)));
  ASSERT_NE(second, nullptr);

  std::unique_ptr<jit::hir::Preloader> second_preloader(
      jit::hir::Preloader::make(
          second, jit::makeFrameReifier(second->func_code)));
  ASSERT_NE(second_preloader, nullptr);

  vectorcallfunc vectorcall_before = second->vectorcall;
  EXPECT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *second_preloader, second),
      jit::Result::CANNOT_SPECIALIZE);
  EXPECT_FALSE(jit_ctx_->didCompile(second));
  EXPECT_EQ(jit_ctx_->lookupFunc(second), nullptr);
  EXPECT_EQ(second->vectorcall, vectorcall_before);
  // The owner's installation is untouched by the refused publication.
  EXPECT_TRUE(jit_ctx_->didCompile(owner));
}
#endif

#if PY_VERSION_HEX < 0x030C0000
TEST_F(JITContextTest, DecrefsPrecedeTheNextBoundaryPoll) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The refcount pass runs after the instrumentation-poll insertion and
  // generates arbitrary execution of its own: the last DECREF of a value
  // can enter tp_dealloc and run __del__.  The boundary polls only close
  // that door if every Decref-family instruction is followed by another
  // poll before the frame can return.  Assert that ordering on the final
  // HIR -- the exact pipeline codegen consumes -- for the shape that
  // exercises it: a discarded operator result released by POP_TOP.
  Ref<PyFunctionObject> func(compileAndGet(
      "def func(a, b):\n"
      "    a + b\n"
      "    return 42",
      "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Function> irfunc =
      jit::compileToFinalHIRForTest(func);
  ASSERT_NE(irfunc, nullptr);

  int polls = 0;
  for (auto& block : irfunc->cfg.blocks) {
    bool decref_since_poll = false;
    const jit::hir::Instr* offender = nullptr;
    for (const jit::hir::Instr& instr : block) {
      switch (instr.opcode()) {
        case jit::hir::Opcode::kDecref:
        case jit::hir::Opcode::kXDecref:
        case jit::hir::Opcode::kBatchDecref:
          decref_since_poll = true;
          offender = &instr;
          break;
        case jit::hir::Opcode::kCheckInstrumentation:
          polls++;
          decref_since_poll = false;
          break;
        case jit::hir::Opcode::kReturn:
          EXPECT_FALSE(decref_since_poll)
              << "a Decref (last: "
              << (offender != nullptr
                      ? jit::hir::hirOpcodeName(offender->opcode())
                      : "?")
              << " at bytecode offset "
              << (offender != nullptr ? offender->bytecodeOffset().value() : -1)
              << ") can run __del__ after the last poll and before the "
                 "return";
          break;
        default:
          break;
      }
    }
    (void)offender;
  }
  EXPECT_GT(polls, 0);
}

TEST_F(JITContextTest, PublicationUnwindsOnAllocationFailure) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The association writes the dictionary anchor first, and the container
  // inserts that follow can throw std::bad_alloc.  Escaping mid-transaction
  // used to strand the artifact: anchored by the function, connected to the
  // context through its owner pointer, and in no registry -- so context
  // teardown could not find it, and its eventual destructor called back
  // into a context that no longer existed.  Fail each allocation point in
  // turn and require the same story every time: a MemoryError, no
  // observable installation, no hidden artifact -- and a clean publish of
  // the same function afterwards, which is only possible if the unwind
  // left no residue.
  const char* sources[5] = {
      "def func(a, b):\n    total = a - a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func(a, b):\n    total = a + a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func(a, b):\n    total = b - a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func(a, b):\n    total = b + a\n    while total < a:\n"
      "        total = total + b\n    return total",
      // Step 5: the code-extra reserve -- the C-convention allocation whose
      // MemoryError used to be swallowed on the way up.
      "def func(a, b):\n    total = b + b\n    while total < a:\n"
      "        total = total + a\n    return total",
  };
  for (int step = 1; step <= 5; step++) {
    Ref<PyFunctionObject> func(compileAndGet(sources[step - 1], "func"));
    ASSERT_NE(func, nullptr) << "step " << step;
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code)));
    ASSERT_NE(preloader, nullptr) << "step " << step;

    vectorcallfunc vectorcall_before = func->vectorcall;
    jit::failJitPublishStepForTest(step);
    EXPECT_EQ(
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
        jit::Result::PYTHON_EXCEPTION)
        << "step " << step;
    jit::failJitPublishStepForTest(0);
    ASSERT_TRUE(PyErr_Occurred()) << "step " << step;
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_MemoryError)) << "step " << step;
    PyErr_Clear();

    EXPECT_FALSE(jit_ctx_->didCompile(func)) << "step " << step;
    EXPECT_EQ(jit_ctx_->lookupFunc(func), nullptr) << "step " << step;
    EXPECT_EQ(func->vectorcall, vectorcall_before) << "step " << step;
    if (func->func_dict != nullptr) {
      EXPECT_EQ(
          PyDict_GetItemWithError(func->func_dict, jit::kCompiledFunctionKey),
          nullptr)
          << "step " << step << " left a hidden artifact anchored";
      PyErr_Clear();
    }

    EXPECT_EQ(
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
        jit::Result::OK)
        << "step " << step << " left residue behind";
    EXPECT_TRUE(jit_ctx_->didCompile(func)) << "step " << step;
  }
}

TEST_F(JITContextTest, FailedRepublicationRestoresThePriorClaim) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Publishing a successor for a function whose __code__ moved is a
  // takeover, and a takeover that fails must leave the prior claim exactly
  // as it found it.  Severing the prior claim first was irrecoverable:
  // with the prior artifact pinned, the ownership oracle -- membership --
  // refused the function forever after a failed publication, and the
  // code-extra ledger blocked any fresh compile of the old code.  Walk
  // every allocation failpoint of the publication and require the prior
  // association, membership, installation and dictionary anchor to
  // survive each failure -- then require both directions to still work:
  // the old code re-attaches, and a clean republication takes over.
  const char* py_src = R"(
def shifty(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def replacement(a, b):
    total = a + a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "shifty"));
  Ref<PyFunctionObject> donor(getGlobal("replacement"));
  ASSERT_NE(func, nullptr);
  ASSERT_NE(donor, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);

  jit::CompiledFunction* prior_art = jit_ctx_->lookupFunc(func);
  ASSERT_NE(prior_art, nullptr);
  auto prior_pin = Ref<jit::CompiledFunction>::create(prior_art);
  auto old_code = Ref<>::create(func->func_code);

  // Move the function to the replacement's code; on 3.11 nothing announces
  // the swap, so the prior claim stays exactly as published.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", donor->func_code), 0);

  for (int step = 1; step <= 3; step++) {
    std::unique_ptr<jit::hir::Preloader> takeover(jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code)));
    ASSERT_NE(takeover, nullptr) << "step " << step;
    jit::failJitPublishStepForTest(step);
    EXPECT_EQ(
        jit::compilePreloaderImpl(jit_ctx_.get(), *takeover, func),
        jit::Result::PYTHON_EXCEPTION)
        << "step " << step;
    jit::failJitPublishStepForTest(0);
    ASSERT_TRUE(PyErr_Occurred()) << "step " << step;
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_MemoryError)) << "step " << step;
    PyErr_Clear();

    EXPECT_TRUE(prior_art->functions().contains(func.get()))
        << "step " << step << " severed the prior membership";
    auto installed = jit_ctx_->compiledFuncs().find(func.get());
    ASSERT_TRUE(installed != jit_ctx_->compiledFuncs().end())
        << "step " << step << " dropped the prior installation";
    EXPECT_EQ(installed->second.get(), prior_art)
        << "step " << step << " re-pointed the installation";
    ASSERT_NE(func->func_dict, nullptr) << "step " << step;
    EXPECT_EQ(
        PyDict_GetItemWithError(func->func_dict, jit::kCompiledFunctionKey),
        reinterpret_cast<PyObject*>(prior_art))
        << "step " << step << " lost the prior anchor";
    PyErr_Clear();
  }

  // The old code still re-attaches: the oracle accepts its own function.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", old_code), 0);
  EXPECT_EQ(Ci_JitShell311_ExecuteRefusal(func), nullptr)
      << "the prior claim no longer answers for its own function";
  EXPECT_TRUE(jit_ctx_->finalizeFunc(func, prior_art));

  // And a clean takeover still works end to end.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", donor->func_code), 0);
  std::unique_ptr<jit::hir::Preloader> clean(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(clean, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *clean, func), jit::Result::OK);
  jit::CompiledFunction* new_art = jit_ctx_->lookupFunc(func);
  ASSERT_NE(new_art, nullptr);
  EXPECT_NE(new_art, prior_art);
  EXPECT_TRUE(new_art->functions().contains(func.get()));
  EXPECT_FALSE(prior_art->functions().contains(func.get()))
      << "the settled takeover left the prior claim standing";
  EXPECT_EQ(
      PyDict_GetItemWithError(func->func_dict, jit::kCompiledFunctionKey),
      reinterpret_cast<PyObject*>(new_art));
  PyErr_Clear();
}
#endif

TEST_F(JITContextTest, UnwatchableBuiltins) {
  SKIP_311_EXECUTABLE_COMPILE();

  // This is a C++ test rather than in test_cinderjit so we can guarantee a
  // fresh runtime state with a watchable builtins dict when the test begins.
  const char* py_src = R"(
import builtins

def del_foo():
    global foo
    del foo

def func():
    foo
    builtins.__dict__[42] = 42
    del_foo()

foo = "hello"
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
#if PY_VERSION_HEX < 0x030C0000
  // The body reads globals, loads an attribute and makes calls -- opcodes
  // the canary execute surface has not opened (MR-06 and later), so
  // publication refuses this compile.  While the publication verdict was
  // swallowed this case reported OK, ran the call through the interpreter,
  // and was green without asserting anything; the honest verdict is what
  // it can assert until the surface opens, at which point the OK branch
  // below takes over and the builtins-watchability contract it was
  // written for becomes checkable again.
  ASSERT_EQ(comp_result, jit::Result::CANNOT_SPECIALIZE);
  ASSERT_FALSE(jit_ctx_->didCompile(func));
#else
  ASSERT_EQ(comp_result, jit::Result::OK);
#endif

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_EQ(result, Py_None);
}

class JITConfigTest : public RuntimeTest {};


TEST_F(JITConfigTest, IsJitUsableAfterInit) {
#if PY_VERSION_HEX < 0x030C0000
  EXPECT_TRUE(jit::isJitShadow());
  EXPECT_FALSE(jit::isJitUsable());
#else
  EXPECT_TRUE(jit::isJitUsable());
#endif
}

TEST_F(JITConfigTest, IsJitInitialized) {
  EXPECT_TRUE(jit::isJitInitialized());
}

TEST_F(JITConfigTest, GlobalModuleStateAccessorMatchesModuleObjectState) {
  Ref<> mod = Ref<>::steal(PyImport_ImportModule("_cinderx"));
  ASSERT_NE(mod, nullptr);

  EXPECT_EQ(cinderx::getModuleState(), cinderx::getModuleState(mod));
}

TEST_F(JITConfigTest, IsJitNotPaused) {
  EXPECT_FALSE(jit::isJitPaused());
}

TEST_F(JITConfigTest, GetMutableConfigModifiesState) {
  auto& cfg = jit::getMutableConfig();
  auto orig_attr_cache_size = cfg.attr_cache_size;
  cfg.attr_cache_size = 8;
  EXPECT_EQ(jit::getConfig().attr_cache_size, 8);
  cfg.attr_cache_size = orig_attr_cache_size;
}

TEST_F(JITConfigTest, DefaultFrameMode) {
  auto mode = jit::getConfig().frame_mode;
  EXPECT_TRUE(
      mode == jit::FrameMode::kNormal || mode == jit::FrameMode::kLightweight);
}

TEST_F(JITConfigTest, DefaultAttrCachesEnabled) {
  bool attr_caches = jit::getConfig().attr_caches;
  EXPECT_TRUE(attr_caches || !attr_caches);
}

TEST_F(JITConfigTest, DefaultSpecializedOpcodes) {
  EXPECT_TRUE(jit::getConfig().specialized_opcodes);
}

TEST_F(JITConfigTest, DefaultStableFrame) {
  EXPECT_TRUE(jit::getConfig().stable_frame);
}

TEST_F(JITConfigTest, DefaultInlinerCostLimit) {
  EXPECT_GT(jit::getConfig().inliner_cost_limit, 0u);
}

TEST_F(JITConfigTest, DefaultSimplifierIterationLimit) {
  EXPECT_GT(jit::getConfig().simplifier.iteration_limit, 0u);
}

TEST_F(JITConfigTest, DefaultHIROptsEnabled) {
  const auto& opts = jit::getConfig().hir_opts;
  EXPECT_TRUE(opts.simplify);
  EXPECT_TRUE(opts.clean_cfg);
  EXPECT_TRUE(opts.dead_code_elim);
  EXPECT_TRUE(opts.list_prefix_reverse_assign);
  EXPECT_TRUE(opts.phi_elim);
}

TEST_F(JITConfigTest, DefaultLIROptsEnabled) {
  EXPECT_TRUE(jit::getConfig().lir_opts.inliner);
}

TEST_F(JITConfigTest, LogOptionsDefaults) {
  const auto& log = jit::getConfig().log;
  EXPECT_FALSE(log.debug);
  EXPECT_FALSE(log.dump_hir_initial);
  EXPECT_FALSE(log.dump_hir_passes);
  EXPECT_FALSE(log.dump_hir_final);
  EXPECT_FALSE(log.dump_lir);
  EXPECT_FALSE(log.dump_asm);
  EXPECT_EQ(log.output_file, stderr);
}

TEST_F(JITConfigTest, JitListOptionsDefaults) {
  const auto& jl = jit::getConfig().jit_list;
  EXPECT_TRUE(jl.filename.empty());
  EXPECT_FALSE(jl.error_on_parse);
  EXPECT_FALSE(jl.match_line_numbers);
}

TEST_F(JITConfigTest, GdbOptionsDefaults) {
  const auto& gdb = jit::getConfig().gdb;
  EXPECT_FALSE(gdb.supported);
  EXPECT_FALSE(gdb.write_elf_objects);
}


class JITPyjitTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITPyjitTest, IsJitUsable) {
  bool usable = jit::isJitUsable();
#if PY_VERSION_HEX < 0x030C0000
  EXPECT_TRUE(jit::isJitShadow());
  EXPECT_FALSE(usable);
#else
  EXPECT_TRUE(usable);
#endif
}

TEST_F(JITPyjitTest, CompileAndCheckVectorcall) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func(a: int) -> int:
    return a + 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  EXPECT_FALSE(compiled->codeBuffer().empty());
  EXPECT_NE(compiled->runtime(), nullptr);
}

TEST_F(JITPyjitTest, CompileTwoFunctions) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def add(a: int, b: int) -> int:
    return a + b

def mul(a: int, b: int) -> int:
    return a * b
)";

  Ref<PyFunctionObject> add_func(compileAndGet(py_src, "add"));
  Ref<PyFunctionObject> mul_func(compileAndGet(py_src, "mul"));
  ASSERT_NE(add_func, nullptr);
  ASSERT_NE(mul_func, nullptr);

  {
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        add_func, jit::makeFrameReifier(add_func->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, add_func);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  {
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        mul_func, jit::makeFrameReifier(mul_func->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, mul_func);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  EXPECT_TRUE(jit_ctx_->didCompile(add_func));
  EXPECT_TRUE(jit_ctx_->didCompile(mul_func));
}

TEST_F(JITPyjitTest, CompiledFunctionAddrNonNull) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func() -> int:
    return 100
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  std::span<const std::byte> code_buf = compiled->codeBuffer();
  EXPECT_FALSE(code_buf.empty());
}

TEST_F(JITPyjitTest, ForgetCodeAndRecompile) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func() -> int:
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit_ctx_->forgetCode(func);

  compiled = jit_ctx_->lookupFunc(func);
  EXPECT_EQ(compiled, nullptr);
}

TEST_F(JITPyjitTest, AddRemoveCompiledFunc) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func() -> int:
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
  EXPECT_TRUE(jit_ctx_->removeCompiledFunc(func));
  EXPECT_FALSE(jit_ctx_->removeCompiledFunc(func));
}
class JITFrameTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITFrameTest, MakeFrameReifier) {
  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_TRUE(PyObject_TypeCheck(reifier, (PyTypeObject*)Py_TYPE(reifier)));
  }
}

TEST_F(JITFrameTest, RuntimeFrameStateFromThreadState) {
  PyThreadState* tstate = PyThreadState_Get();
  ASSERT_NE(tstate, nullptr);

  EXPECT_EQ(tstate, PyThreadState_Get());
}

TEST_F(JITFrameTest, CompileAndGetHeader) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_GT(rt->frameSize(), 0);
}

TEST_F(JITFrameTest, ClearExceptCodeOnJitFrame) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
}

TEST_F(JITFrameTest, AfterCompileDidCompile) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  EXPECT_FALSE(jit_ctx_->didCompile(func));

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
}

TEST_F(JITFrameTest, LookupCodeRuntimeBeforeCompile) {
  const char* py_src = R"(
def func():
    return 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  EXPECT_EQ(rt, nullptr);
}

TEST_F(JITFrameTest, FrameSizeAfterCompile) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func(a: int, b: int) -> int:
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  EXPECT_GE(compiled->runtime()->frameSize(), 0);
}

TEST_F(JITFrameTest, CompiledCodesNotEmptyAfterCompile) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  const auto before_size = jit_ctx_->compiledCodes().size();

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  const auto& codes_after = jit_ctx_->compiledCodes();
  EXPECT_GT(codes_after.size(), before_size);
}

TEST_F(JITFrameTest, MakeFrameReifierForGenerator) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_TRUE(PyObject_TypeCheck(reifier, (PyTypeObject*)Py_TYPE(reifier)));
  }
}

TEST_F(JITFrameTest, MakeFrameReifierWithArgs) {
  const char* py_src = R"(
def func(a, b, c):
    return a + b + c
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto reifier = jit::makeFrameReifier(func->func_code);
  if (reifier != nullptr) {
    ASSERT_NE(reifier, nullptr);
  }
}

TEST_F(JITFrameTest, CompileAndCheckCodeRuntimeFrameState) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func(x):
    return x * 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  EXPECT_EQ(rt->code(), func->func_code);
  EXPECT_NE(rt->builtins(), nullptr);
  EXPECT_NE(rt->globals(), nullptr);
}

TEST_F(JITFrameTest, CompileAndRunSimpleFunc) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 42);
}

TEST_F(JITFrameTest, CompileAndRunFuncWithArgs) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto a = Ref<>::steal(PyLong_FromLong(10));
  auto b = Ref<>::steal(PyLong_FromLong(20));
  auto args = Ref<>::steal(PyTuple_Pack(2, a.get(), b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 30);
}

TEST_F(JITFrameTest, CompileAndRunGeneratorFunc) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 10
    yield 20
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 10);

  auto second = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(second.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(second), 20);
}

TEST_F(JITFrameTest, MultipleCompiles) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func_a():
    return 1

def func_b():
    return 2
)";

  Ref<PyFunctionObject> func_a(compileAndGet(py_src, "func_a"));
  Ref<PyFunctionObject> func_b(compileAndGet(py_src, "func_b"));
  ASSERT_NE(func_a, nullptr);
  ASSERT_NE(func_b, nullptr);

  {
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func_a, jit::makeFrameReifier(func_a->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func_a);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  {
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func_b, jit::makeFrameReifier(func_b->func_code)));
    auto comp =
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func_b);
    ASSERT_EQ(comp, jit::Result::OK);
  }

  EXPECT_TRUE(jit_ctx_->didCompile(func_a));
  EXPECT_TRUE(jit_ctx_->didCompile(func_b));
}

class JITPyjitApiTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};


TEST_F(JITPyjitApiTest, CompileFunction) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1 + 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileFunctionWithArgs) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileFunctionTwice) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 99
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result1 = jit::compileFunction(func);
  EXPECT_EQ(result1, jit::Result::OK);

  auto result2 = jit::compileFunction(func);
  EXPECT_EQ(result2, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, CompileGenerator) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);
}

TEST_F(JITPyjitApiTest, PreloadFuncAndDeps) {
  const char* py_src = R"(
def helper():
    return 10

def func():
    return helper()
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto deps = jit::preloadFuncAndDeps(func, false);
  EXPECT_GE(deps.size(), 1);
}

TEST_F(JITPyjitApiTest, TypeModified) {
  const char* py_src = R"(
class MyClass:
    x = 1
)";

  Ref<> obj(compileAndGet(py_src, "MyClass"));
  ASSERT_NE(obj, nullptr);

  auto type = reinterpret_cast<PyTypeObject*>(obj.get());
  ASSERT_NE(type, nullptr);

  jit::typeModified(type);
}

TEST_F(JITPyjitApiTest, TypeNameModified) {
  const char* py_src = R"(
class MyClass:
    pass
)";

  Ref<> obj(compileAndGet(py_src, "MyClass"));
  ASSERT_NE(obj, nullptr);

  auto type = reinterpret_cast<PyTypeObject*>(obj.get());
  ASSERT_NE(type, nullptr);

  jit::typeNameModified(type);
}

TEST_F(JITPyjitApiTest, FuncModified) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::funcModified(func);
}

TEST_F(JITPyjitApiTest, CompileAndCall) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto call_result =
      Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 42);
}

TEST_F(JITPyjitApiTest, CompileAndCallWithArgs) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def add(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "add"));
  ASSERT_NE(func, nullptr);

  auto result = jit::compileFunction(func);
  EXPECT_EQ(result, jit::Result::OK);

  auto a = Ref<>::steal(PyLong_FromLong(3));
  auto b = Ref<>::steal(PyLong_FromLong(4));
  auto args = Ref<>::steal(PyTuple_Pack(2, a.get(), b.get()));
  auto call_result =
      Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 7);
}

class JITContextExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITContextExtendedTest, CompiledFuncsAfterCompile) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  const auto& compiled_funcs = jit_ctx_->compiledFuncs();
  EXPECT_FALSE(compiled_funcs.empty());
}

TEST_F(JITContextExtendedTest, LookupCode) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  BorrowedRef<PyCodeObject> code = func->func_code;
  BorrowedRef<PyDictObject> builtins = func->func_builtins;
  BorrowedRef<PyDictObject> globals = func->func_globals;

  auto compiled = jit_ctx_->lookupCode(code, builtins, globals);
  ASSERT_NE(compiled, nullptr);
}

TEST_F(JITContextExtendedTest, CompilePublishesCodeExtraCompiledEntry) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());
  EXPECT_EQ(extra->jit_globals, func->func_globals);
  EXPECT_EQ(extra->jit_builtins, func->func_builtins);
}

TEST_F(JITContextExtendedTest, ForgetCodeClearsCodeExtraCompiledEntry) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());

  jit_ctx_->forgetCode(func);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
}

TEST_F(
    JITContextExtendedTest,
    ClearForMultithreadedCompileTestClearsCodeExtraCompiledEntry) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());

  jit_ctx_->clearForMultithreadedCompileTest();

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
}

TEST_F(
    JITContextExtendedTest,
    ForgetCompiledFunctionClearsCodeExtraCompiledEntry) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(func->func_code));
  ASSERT_NE(extra, nullptr);

  auto compiled = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), compiled.get());
  ASSERT_NE(func->func_dict, nullptr);

  ASSERT_EQ(
      PyDict_DelItemString(func->func_dict, "__cinderx_compiled_func__"), 0);

  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);
  EXPECT_EQ(extra->jit_globals, nullptr);
  EXPECT_EQ(extra->jit_builtins, nullptr);
  auto cached = jit_ctx_->lookupCode(
      func->func_code, func->func_builtins, func->func_globals);
  EXPECT_EQ(cached, nullptr);
}

TEST_F(JITContextExtendedTest, AddCompileTime) {
  auto before = jit_ctx_->totalCompileTime();
  jit_ctx_->addCompileTime(std::chrono::nanoseconds(1000000));
  auto after = jit_ctx_->totalCompileTime();
  EXPECT_GE(after.count(), before.count());
}

TEST_F(JITContextExtendedTest, IfDeoptStatNotFound) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  ASSERT_NE(rt, nullptr);

  bool called = jit_ctx_->ifDeoptStat(
      rt, 9999, [](const jit::DeoptStat&) { FAIL() << "Should not be called"; });
  EXPECT_FALSE(called);
}

TEST_F(JITContextExtendedTest, ReleaseReferencesAfterAdd) {
  auto obj = Ref<>::steal(PyLong_FromLong(999));
  ASSERT_NE(obj, nullptr);

  jit_ctx_->addReference(obj);
  jit_ctx_->releaseReferences();
}

TEST_F(JITContextExtendedTest, GetAndClearLoadMethodCacheStats) {
  auto stats = jit_ctx_->getAndClearLoadMethodCacheStats();
  EXPECT_TRUE(stats.empty());
}

TEST_F(JITContextExtendedTest, GetAndClearLoadTypeMethodCacheStats) {
  auto stats = jit_ctx_->getAndClearLoadTypeMethodCacheStats();
  EXPECT_TRUE(stats.empty());
}

TEST_F(JITContextExtendedTest, CodeOuterFunctions) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  auto& outer_funcs = jit_ctx_->codeOuterFunctions();
  EXPECT_GE(outer_funcs.size(), 0u);
}

TEST_F(JITContextExtendedTest, BuiltinsFindByName) {
  const jit::Builtins& builtins = jit_ctx_->builtins();
  auto result = builtins.find("print");
  if (result.has_value()) {
    EXPECT_NE(*result, nullptr);
  }
}

TEST_F(JITContextExtendedTest, BuiltinsFindByMethod) {
  const jit::Builtins& builtins = jit_ctx_->builtins();
  auto meth = builtins.find("len");
  if (meth.has_value() && *meth != nullptr) {
    auto name_result = builtins.find(*meth);
    EXPECT_TRUE(name_result.has_value());
  }
}

TEST_F(JITContextExtendedTest, AllocateAllCaches) {
  auto* la = jit_ctx_->allocateLoadAttrCache();
  auto* lta = jit_ctx_->allocateLoadTypeAttrCache();
  auto* lm = jit_ctx_->allocateLoadMethodCache();
  auto* lma = jit_ctx_->allocateLoadModuleAttrCache();
  auto* lmm = jit_ctx_->allocateLoadModuleMethodCache();
  auto* ltm = jit_ctx_->allocateLoadTypeMethodCache();
  auto* sa = jit_ctx_->allocateStoreAttrCache();

  EXPECT_NE(la, nullptr);
  EXPECT_NE(lta, nullptr);
  EXPECT_NE(lm, nullptr);
  EXPECT_NE(lma, nullptr);
  EXPECT_NE(lmm, nullptr);
  EXPECT_NE(ltm, nullptr);
  EXPECT_NE(sa, nullptr);
}

class JITGenDataFooterTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITGenDataFooterTest, YieldFromValueNotYieldFrom) {
  jit::GenYieldPoint yp(0, jit::kInvalidYieldFromOffset);
  EXPECT_FALSE(yp.isYieldFrom());

  jit::GenDataFooter footer;
  footer.yieldPoint = &yp;

  PyObject* result = jit::yieldFromValue(&footer, &yp);
  EXPECT_EQ(result, nullptr);
}

TEST_F(JITGenDataFooterTest, GenDataFooterDefaults) {
  jit::GenDataFooter footer;
  EXPECT_EQ(footer.linkAddress, 0u);
  EXPECT_EQ(footer.returnAddress, 0u);
  EXPECT_EQ(footer.originalFramePointer, 0u);
  EXPECT_EQ(footer.yieldPoint, nullptr);
  EXPECT_EQ(footer.spillWords, 0u);
  EXPECT_EQ(footer.resumeEntry, nullptr);
  EXPECT_EQ(footer.gen, nullptr);
  EXPECT_EQ(footer.code_rt, nullptr);
}

TEST_F(JITGenDataFooterTest, CompileGeneratorAndCheckRuntime) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CodeRuntime* rt = jit_ctx_->lookupCodeRuntime(func);
  ASSERT_NE(rt, nullptr);
  EXPECT_TRUE(rt->isGen());
}

class JITCodeRuntimeExtendedTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr);
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITCodeRuntimeExtendedTest, AllocateRuntimeFrameState) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  EXPECT_EQ(rt.code(), func->func_code);
  EXPECT_NE(rt.builtins(), nullptr);
  EXPECT_NE(rt.globals(), nullptr);
}

TEST_F(JITCodeRuntimeExtendedTest, MultipleDeoptMetadatas) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta1;
  jit::DeoptMetadata meta2;
  std::size_t idx1 = rt.addDeoptMetadata(std::move(meta1));
  std::size_t idx2 = rt.addDeoptMetadata(std::move(meta2));

  EXPECT_EQ(idx1, 0);
  EXPECT_EQ(idx2, 1);
  EXPECT_EQ(rt.deoptMetadatas().size(), 2);
}

TEST_F(JITCodeRuntimeExtendedTest, MultipleGenYieldPoints) {
  const char* py_src = R"(
def gen():
    yield 1
    yield 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  auto* yp1 = rt.addGenYieldPoint(jit::GenYieldPoint(0, jit::kInvalidYieldFromOffset));
  auto* yp2 = rt.addGenYieldPoint(jit::GenYieldPoint(1, 10));
  ASSERT_NE(yp1, nullptr);
  ASSERT_NE(yp2, nullptr);

  EXPECT_FALSE(yp1->isYieldFrom());
  EXPECT_TRUE(yp2->isYieldFrom());
}

TEST_F(JITCodeRuntimeExtendedTest, RuntimeFrameStateFields) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  EXPECT_EQ(rt.code(), func->func_code);
  EXPECT_NE(rt.builtins(), nullptr);
  EXPECT_NE(rt.globals(), nullptr);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionStackSizes) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func(a, b):
    return a + b
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  EXPECT_GE(compiled->stackSize(), 0);
  EXPECT_GE(compiled->spillStackSize(), 0);
  EXPECT_GT(compiled->codeSize(), 0u);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionCompileTime) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  auto compile_time = compiled->compileTime();
  EXPECT_GE(compile_time.count(), 0);
}

TEST_F(JITCodeRuntimeExtendedTest, CompiledFunctionAddRemoveFunction) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  const char* py_src2 = R"(
def func2():
    return 2
)";

  Ref<PyFunctionObject> func2(compileAndGet(py_src2, "func2"));
  ASSERT_NE(func2, nullptr);

  compiled->addFunction(func2);
  EXPECT_TRUE(compiled->functions().count(func2) > 0);

  compiled->removeFunction(func2);
  EXPECT_TRUE(compiled->functions().count(func2) == 0);
}



class JITCodeRuntimeTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITCodeRuntimeTest, GenYieldPointDeoptIdx) {
  jit::GenYieldPoint yp(42, jit::kInvalidYieldFromOffset);
  EXPECT_EQ(yp.deoptIdx(), 42);
  EXPECT_FALSE(yp.isYieldFrom());
}

TEST_F(JITCodeRuntimeTest, GenYieldPointIsYieldFrom) {
  jit::GenYieldPoint yp(5, 10);
  EXPECT_TRUE(yp.isYieldFrom());
  EXPECT_EQ(yp.yieldFromOffset(), 10);
}

TEST_F(JITCodeRuntimeTest, GenYieldPointSetResumeTarget) {
  jit::GenYieldPoint yp(1, jit::kInvalidYieldFromOffset);
  EXPECT_EQ(yp.resumeTarget(), 0);
  yp.setResumeTarget(0xDEADBEEF);
  EXPECT_EQ(yp.resumeTarget(), 0xDEADBEEF);
}

TEST_F(JITCodeRuntimeTest, RuntimeFrameStateIsGen) {
  const char* py_src = R"(
def normal():
    return 1

def gen():
    yield 1
)";

  Ref<PyFunctionObject> normal_func(compileAndGet(py_src, "normal"));
  Ref<PyFunctionObject> gen_func(compileAndGet(py_src, "gen"));
  ASSERT_NE(normal_func, nullptr);
  ASSERT_NE(gen_func, nullptr);

  {
    jit::CodeRuntime rt{normal_func};
    EXPECT_FALSE(rt.isGen());
  }

  {
    jit::CodeRuntime rt{gen_func};
    EXPECT_TRUE(rt.isGen());
  }
}


TEST_F(JITCodeRuntimeTest, CodeRuntimeSetFrameSize) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  rt.setFrameSize(128);
  EXPECT_EQ(rt.frameSize(), 128);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeSpillWords) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  EXPECT_EQ(rt.spillWords(), 0);

  rt.setSpillWords(32);
  EXPECT_EQ(rt.spillWords(), 32);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeAddGenYieldPoint) {
  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::GenYieldPoint* yp = rt.addGenYieldPoint(
      jit::GenYieldPoint(0, jit::kInvalidYieldFromOffset));
  ASSERT_NE(yp, nullptr);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeAddDeoptMetadata) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta;
  std::size_t idx = rt.addDeoptMetadata(std::move(meta));
  EXPECT_EQ(idx, 0);

  const jit::DeoptMetadata& stored = rt.getDeoptMetadata(idx);
  EXPECT_EQ(stored.live_values.size(), 0);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeDeoptMetadatas) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};

  jit::DeoptMetadata meta;
  rt.addDeoptMetadata(std::move(meta));

  const auto& metadatas = rt.deoptMetadatas();
  EXPECT_EQ(metadatas.size(), 1);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeDebugInfo) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  jit::DebugInfo* info = rt.debugInfo();
  ASSERT_NE(info, nullptr);
}

TEST_F(JITCodeRuntimeTest, CodeRuntimeGetUnitCallStackInvalidIdx) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  auto result = rt.getUnitCallStackFromDeoptIdx(999);
  EXPECT_FALSE(result.has_value());
}


TEST_F(JITCodeRuntimeTest, CodeRuntimeIsCleared) {
  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  jit::CodeRuntime rt{func};
  EXPECT_FALSE(rt.isCleared());
}

TEST_F(JITCodeRuntimeTest, CompileAndVerifyCodeRuntime) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_GE(rt->frameSize(), 0);
  EXPECT_FALSE(rt->isCleared());

  ASSERT_NE(rt->code(), nullptr);
  ASSERT_NE(rt->builtins(), nullptr);
  ASSERT_NE(rt->globals(), nullptr);
}

class JITGeneratorTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<jit::CompilerContext<jit::Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<jit::CompilerContext<jit::Compiler>> jit_ctx_;
};

TEST_F(JITGeneratorTest, CompileGenerator) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
    yield 2
    yield 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
}

TEST_F(JITGeneratorTest, CompileGeneratorAndCheck) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  EXPECT_FALSE(jit_ctx_->didCompile(func));

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  EXPECT_TRUE(jit_ctx_->didCompile(func));
}

TEST_F(JITGeneratorTest, CompileAndRunGenerator) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 10
    yield 20
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 10);
}

TEST_F(JITGeneratorTest, CompileGeneratorWithArg) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen(n: int):
    for i in range(n):
        yield i
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto arg = Ref<>::steal(PyLong_FromLong(3));
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto gen_obj = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  for (int expected = 0; expected < 3; expected++) {
    auto val = Ref<>::steal(PyIter_Next(iter));
    ASSERT_NE(val.get(), nullptr) << "Failed at iteration " << expected;
    EXPECT_EQ(PyLong_AsLong(val), expected);
  }

  auto val = Ref<>::steal(PyIter_Next(iter));
  EXPECT_EQ(val.get(), nullptr);
}



TEST_F(JITGeneratorTest, CompileGeneratorForgetCode) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
  EXPECT_TRUE(jit_ctx_->didCompile(func));

  jit_ctx_->forgetCode(func);
  EXPECT_FALSE(jit_ctx_->didCompile(func));
}

TEST_F(JITGeneratorTest, CompileAndIterateGenerator) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
    yield 2
    yield 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  long expected[] = {1, 2, 3};
  for (int i = 0; i < 3; i++) {
    auto val = Ref<>::steal(PyIter_Next(iter));
    ASSERT_NE(val.get(), nullptr) << "Failed at iteration " << i;
    EXPECT_EQ(PyLong_AsLong(val), expected[i]);
  }

  auto val = Ref<>::steal(PyIter_Next(iter));
  EXPECT_EQ(val.get(), nullptr);
}


TEST_F(JITGeneratorTest, CompileCoroutine) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
async def coro():
    return 42
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "coro"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);
}

TEST_F(JITGeneratorTest, GeneratorClose) {
  const char* py_src = R"(
def gen():
    try:
        yield 1
        yield 2
    finally:
        pass
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  auto gen_obj = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen_obj, nullptr);

  auto iter = Ref<>::steal(PyObject_GetIter(gen_obj));
  ASSERT_NE(iter, nullptr);

  auto first = Ref<>::steal(PyIter_Next(iter));
  ASSERT_NE(first.get(), nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 1);

  auto close_result = PyObject_CallMethod(gen_obj, "close", nullptr);
  EXPECT_NE(close_result, nullptr);
  Py_XDECREF(close_result);
}


TEST_F(JITGeneratorTest, GeneratorRuntimeIsGen) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def gen():
    yield 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "gen"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_TRUE(rt->isGen());
}

TEST_F(JITGeneratorTest, NormalFuncRuntimeIsNotGen) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def func():
    return 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  ASSERT_NE(func, nullptr);

  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));

  auto comp_result =
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func);
  ASSERT_EQ(comp_result, jit::Result::OK);

  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);

  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);
  EXPECT_FALSE(rt->isGen());
}

namespace {

Ref<> importCinderJitModule() {
  auto mod = Ref<>::steal(PyImport_ImportModule("cinderx.jit"));
  if (mod == nullptr) {
    PyErr_Print();
    throw std::runtime_error("Failed to import cinderx.jit");
  }
  return mod;
}

Ref<> callJitNoArgs(BorrowedRef<> mod, const char* name) {
  auto result = Ref<>::steal(PyObject_CallMethod(mod, name, nullptr));
  if (result == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("cinderx.jit.") + name + " failed");
  }
  return result;
}

Ref<> callJitOneArg(BorrowedRef<> mod, const char* name, PyObject* arg) {
  auto result = Ref<>::steal(PyObject_CallMethod(mod, name, "O", arg));
  if (result == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("cinderx.jit.") + name + " failed");
  }
  return result;
}

Ref<> makeLong(long value) {
  return Ref<>::steal(PyLong_FromLong(value));
}

Ref<> makeList(std::initializer_list<long> values) {
  auto list =
      Ref<>::steal(PyList_New(static_cast<Py_ssize_t>(values.size())));
  if (list == nullptr) {
    return list;
  }
  Py_ssize_t idx = 0;
  for (long value : values) {
    PyObject* item = PyLong_FromLong(value);
    if (item == nullptr) {
      return Ref<>(nullptr);
    }
    PyList_SET_ITEM(list.get(), idx++, item);
  }
  return list;
}

Ref<> call2(PyObject* func, PyObject* arg0, PyObject* arg1) {
  return Ref<>::steal(PyObject_CallFunctionObjArgs(
      func, arg0, arg1, nullptr));
}

std::string pyRepr(BorrowedRef<> obj) {
  auto repr = Ref<>::steal(PyObject_Repr(obj));
  if (repr == nullptr) {
    PyErr_Print();
    throw std::runtime_error("PyObject_Repr failed");
  }
  const char* utf8 = PyUnicode_AsUTF8(repr);
  if (utf8 == nullptr) {
    PyErr_Print();
    throw std::runtime_error("PyUnicode_AsUTF8 failed");
  }
  return utf8;
}

long getLongAttr(BorrowedRef<> obj, const char* name) {
  auto attr = Ref<>::steal(PyObject_GetAttrString(obj, name));
  if (attr == nullptr) {
    PyErr_Print();
    throw std::runtime_error(std::string("missing attribute ") + name);
  }
  return PyLong_AsLong(attr);
}

void expectPyEqual(BorrowedRef<> actual, BorrowedRef<> expected) {
  int equal = PyObject_RichCompareBool(actual, expected, Py_EQ);
  ASSERT_NE(equal, -1) << "comparison failed";
  EXPECT_EQ(equal, 1) << pyRepr(actual) << " != " << pyRepr(expected);
}

void expectPrefixReverseHelperMatchesPython(
    BorrowedRef<> original_func,
    BorrowedRef<> actual,
    BorrowedRef<> expected,
    BorrowedRef<> actual_index,
    BorrowedRef<> expected_index) {
  auto original_result =
      call2(original_func.get(), expected.get(), expected_index.get());
  ASSERT_NE(original_result, nullptr) << "reference Python expression failed";

  ASSERT_EQ(JITRT_ListPrefixReverseAssign(actual.get(), actual_index.get()), 0)
      << "helper failed: " << (PyErr_Occurred() ? "exception set" : "no exception");
  expectPyEqual(actual, original_result);
}

} // namespace

class JITJitRtCoverageTest : public RuntimeTest {};

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperFastPathSemantics) {
  const char* py_src = R"(
def original(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq
)";
  Ref<> original(compileAndGet(py_src, "original"));

  {
    Ref<> index = makeLong(3);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original, makeList({}), makeList({}), index, index);
  }
  {
    Ref<> index = makeLong(10);
    expectPrefixReverseHelperMatchesPython(
        original, makeList({7}), makeList({7}), index, index);
  }
}

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperFallbackSemantics) {
  const char* py_src = R"(
def original(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq

class CountingList(list):
    def __init__(self, value):
        super().__init__(value)
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        return super().__getitem__(key)
    def __setitem__(self, key, value):
        self.set_count += 1
        return super().__setitem__(key, value)

class CustomIndex:
    def __init__(self, value):
        self.value = value
        self.index_count = 0
        self.add_count = 0
    def __index__(self):
        self.index_count += 1
        return self.value
    def __add__(self, other):
        self.add_count += 1
        return self.value + other

class CustomSequence:
    def __init__(self, value):
        self.data = list(value)
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        return self.data[key]
    def __setitem__(self, key, value):
        self.set_count += 1
        self.data[key] = value
    def __eq__(self, other):
        return isinstance(other, CustomSequence) and self.data == other.data
)";
  Ref<> original(compileAndGet(py_src, "original"));
  Ref<> counting_list_type(getGlobal("CountingList"));
  Ref<> custom_index_type(getGlobal("CustomIndex"));
  Ref<> custom_sequence_type(getGlobal("CustomSequence"));

  {
    Ref<> index = makeLong(-1);
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2, 3, 4}),
        makeList({0, 1, 2, 3, 4}),
        index,
        index);
  }
  {
    expectPrefixReverseHelperMatchesPython(
        original,
        makeList({0, 1, 2}),
        makeList({0, 1, 2}),
        Ref<>::create(Py_True),
        Ref<>::create(Py_True));
  }
  {
    Ref<> source = makeList({0, 1, 2, 3});
    Ref<> actual = Ref<>::steal(
        PyObject_CallFunctionObjArgs(counting_list_type.get(), source.get(), nullptr));
    Ref<> expected = Ref<>::steal(
        PyObject_CallFunctionObjArgs(counting_list_type.get(), source.get(), nullptr));
    Ref<> index = makeLong(2);
    expectPrefixReverseHelperMatchesPython(original, actual, expected, index, index);
    EXPECT_EQ(getLongAttr(actual, "get_count"), 1);
    EXPECT_EQ(getLongAttr(actual, "set_count"), 1);
  }
  {
    Ref<> actual_index = Ref<>::steal(
        PyObject_CallFunction(custom_index_type.get(), "i", 2));
    Ref<> expected_index = Ref<>::steal(
        PyObject_CallFunction(custom_index_type.get(), "i", 2));
    Ref<> actual = makeList({0, 1, 2, 3});
    Ref<> expected = makeList({0, 1, 2, 3});
    auto original_result =
        call2(original.get(), expected.get(), expected_index.get());
    ASSERT_NE(original_result, nullptr);

    ASSERT_EQ(
        JITRT_ListPrefixReverseAssign(actual.get(), actual_index.get()), 0);
    expectPyEqual(actual, original_result);
    EXPECT_GE(getLongAttr(actual_index, "index_count"), 1);
    EXPECT_EQ(getLongAttr(actual_index, "add_count"), 1);
  }
  {
    Ref<> source = makeList({0, 1, 2, 3});
    Ref<> actual = Ref<>::steal(
        PyObject_CallFunctionObjArgs(custom_sequence_type.get(), source.get(), nullptr));
    Ref<> expected = Ref<>::steal(
        PyObject_CallFunctionObjArgs(custom_sequence_type.get(), source.get(), nullptr));
    Ref<> index = makeLong(2);
    expectPrefixReverseHelperMatchesPython(original, actual, expected, index, index);
    EXPECT_EQ(getLongAttr(actual, "get_count"), 1);
    EXPECT_EQ(getLongAttr(actual, "set_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, ListPrefixReverseAssignHelperExceptionSideEffects) {
  const char* py_src = R"(
class GetError(Exception):
    pass
class AddError(Exception):
    pass
class SetError(Exception):
    pass

class RaisingSeq:
    def __init__(self, phase):
        self.phase = phase
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        if self.phase == "get":
            raise GetError("get")
        return [2, 1, 0]
    def __setitem__(self, key, value):
        self.set_count += 1
        if self.phase == "set":
            raise SetError("set")

class RaisingIndex:
    def __init__(self, phase):
        self.phase = phase
        self.add_count = 0
    def __index__(self):
        return 2
    def __add__(self, other):
        self.add_count += 1
        if self.phase == "add":
            raise AddError("add")
        return 3
)";
  Ref<> raising_seq_type(compileAndGet(py_src, "RaisingSeq"));
  Ref<> raising_index_type(getGlobal("RaisingIndex"));
  Ref<> get_error(getGlobal("GetError"));
  Ref<> add_error(getGlobal("AddError"));
  Ref<> set_error(getGlobal("SetError"));

  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "get"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(get_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 0);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", ""));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", "add"));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(add_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "set"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    ASSERT_EQ(JITRT_ListPrefixReverseAssign(seq.get(), index.get()), -1);
    EXPECT_TRUE(PyErr_ExceptionMatches(set_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 1);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, CompiledListPrefixReverseAssignFastPathSemantics) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
def target(seq, k):
    seq[: k + 1] = seq[k::-1]
    return seq
)";
  Ref<PyFunctionObject> target(compileAndGet(py_src, "target"));
  ASSERT_EQ(jit::compileFunction(target), jit::Result::OK);

  {
    Ref<> seq = makeList({0, 1, 2, 3, 4});
    Ref<> index = makeLong(3);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({3, 2, 1, 0, 4}));
  }
  {
    Ref<> seq = makeList({0, 1, 2, 3, 4});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({4, 3, 2, 1, 0}));
  }
  {
    Ref<> seq = makeList({});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({}));
  }
  {
    Ref<> seq = makeList({7});
    Ref<> index = makeLong(10);
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_NE(result, nullptr);
    expectPyEqual(result, makeList({7}));
  }
}

TEST_F(JITJitRtCoverageTest, CompiledListPrefixReverseAssignExceptionSideEffects) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
class GetError(Exception):
    pass
class AddError(Exception):
    pass
class SetError(Exception):
    pass

class RaisingSeq:
    def __init__(self, phase):
        self.phase = phase
        self.get_count = 0
        self.set_count = 0
    def __getitem__(self, key):
        self.get_count += 1
        if self.phase == "get":
            raise GetError("get")
        return [2, 1, 0]
    def __setitem__(self, key, value):
        self.set_count += 1
        if self.phase == "set":
            raise SetError("set")

class RaisingIndex:
    def __init__(self, phase):
        self.phase = phase
        self.add_count = 0
    def __index__(self):
        return 2
    def __add__(self, other):
        self.add_count += 1
        if self.phase == "add":
            raise AddError("add")
        return 3

def target(seq, k):
    seq[: k + 1] = seq[k::-1]
)";
  Ref<PyFunctionObject> target(compileAndGet(py_src, "target"));
  Ref<> raising_seq_type(getGlobal("RaisingSeq"));
  Ref<> raising_index_type(getGlobal("RaisingIndex"));
  Ref<> get_error(getGlobal("GetError"));
  Ref<> add_error(getGlobal("AddError"));
  Ref<> set_error(getGlobal("SetError"));
  ASSERT_EQ(jit::compileFunction(target), jit::Result::OK);

  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "get"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(get_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 0);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", ""));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", "add"));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(add_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 0);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
  {
    Ref<> seq = Ref<>::steal(PyObject_CallFunction(raising_seq_type.get(), "s", "set"));
    Ref<> index = Ref<>::steal(PyObject_CallFunction(raising_index_type.get(), "s", ""));
    Ref<> result = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(target.get()),
        seq.get(),
        index.get(),
        nullptr));
    ASSERT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(set_error.get()));
    PyErr_Clear();
    EXPECT_EQ(getLongAttr(seq, "get_count"), 1);
    EXPECT_EQ(getLongAttr(seq, "set_count"), 1);
    EXPECT_EQ(getLongAttr(index, "add_count"), 1);
  }
}

TEST_F(JITJitRtCoverageTest, CompiledArithmeticUnaryModAndPower) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
def kernel(a, b):
    return (not bool(a), a % b, a ** b, pow(a, b))

def driver():
    return kernel(17, 5)
)";

  Ref<PyFunctionObject> kernel(compileAndGet(py_src, "kernel"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(kernel, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(kernel), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyTuple_Check(result));
  EXPECT_EQ(PyTuple_GET_SIZE(result), 4);
}

TEST_F(JITJitRtCoverageTest, CompiledGlobalNameLoad) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
ANSWER = 321

def get_answer():
    return ANSWER

def run():
    return get_answer()
)";

  Ref<PyFunctionObject> get_answer(compileAndGet(py_src, "get_answer"));
  Ref<PyFunctionObject> run(compileAndGet(py_src, "run"));
  ASSERT_EQ(jit::compileFunction(get_answer), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(run), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 321);
}

TEST_F(JITJitRtCoverageTest, CompiledGeneratorSendAndYieldFrom) {
  SKIP_311_UNTIL_SURFACE("JIT generators and coroutines");

  const char* py_src = R"(
def inner():
    received = yield 1
    yield received + 10

def outer():
    return (yield from inner())

def drive():
    gen = outer()
    first = next(gen)
    second = gen.send(5)
    return first, second
)";

  Ref<PyFunctionObject> inner(compileAndGet(py_src, "inner"));
  Ref<PyFunctionObject> outer(compileAndGet(py_src, "outer"));
  Ref<PyFunctionObject> drive(compileAndGet(py_src, "drive"));
  ASSERT_EQ(jit::compileFunction(inner), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(outer), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(drive), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(drive, args, nullptr));
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyTuple_Check(result));
  EXPECT_EQ(PyTuple_GET_SIZE(result), 2);
  EXPECT_EQ(PyLong_AsLong(PyTuple_GET_ITEM(result.get(), 0)), 1);
  EXPECT_EQ(PyLong_AsLong(PyTuple_GET_ITEM(result.get(), 1)), 15);
}

TEST_F(JITJitRtCoverageTest, CompiledVectorcallEntry) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
def callee(a, b, c):
    return a + b + c

def caller():
    return callee(1, 2, 3)
)";

  Ref<PyFunctionObject> callee(compileAndGet(py_src, "callee"));
  Ref<PyFunctionObject> caller(compileAndGet(py_src, "caller"));
  ASSERT_EQ(jit::compileFunction(callee), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(caller), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(caller, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITJitRtCoverageTest, CompiledContainersComparisonsAndBitOps) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
def mix(a, b):
    bits = ((a & b) | (a ^ b)) << 1
    values = [a, b, bits, a + b]
    values[1] += 3
    data = {"a": values[0], "b": values[1], "bits": bits}
    part = values[1:4]
    if data["a"] < data["b"] and bits != 0:
        return part[0] + part[1] + part[2] + data.get("bits", 0)
    return -1

def driver():
    return mix(3, 6)
)";

  Ref<PyFunctionObject> mix(compileAndGet(py_src, "mix"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(mix, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(mix), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

TEST_F(JITJitRtCoverageTest, CompiledAttributesMethodsAndLoops) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
class Box:
    def __init__(self, value):
        self.value = value

    def add(self, other):
        self.value += other
        return self.value

def work(limit):
    box = Box(1)
    total = 0
    i = 0
    while i < limit:
        total += box.add(i)
        i += 1
    return total + box.value

def driver():
    return work(8)
)";

  Ref<PyFunctionObject> work(compileAndGet(py_src, "work"));
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  ASSERT_NE(work, nullptr);
  ASSERT_NE(driver, nullptr);
  ASSERT_EQ(jit::compileFunction(work), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(driver), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

TEST_F(JITJitRtCoverageTest, CompiledBroadOpcodeShapes) {
  SKIP_311_UNTIL_SURFACE(
      "calls, attribute loads and global loads in the execute whitelist");

  const char* py_src = R"(
class Base:
    def value(self):
        return 3

class Child(Base):
    def value(self):
        return super().value() + 4

def unicode_ops(text, n):
    return (text + "!" * n)[1:4] + str(n)

def dict_set_ops(a, b):
    d = {"a": a, **{"b": b}}
    e = {"c": a + b}
    d.update(e)
    s = {a, b}
    s.update({a + b})
    return d["a"] + d["b"] + d["c"] + len(s)

def match_ops(obj):
    match obj:
        case {"kind": "pair", "left": left, "right": right}:
            return left + right
        case [first, second, *rest]:
            return first + second + len(rest)
        case _:
            return 0

def unpack_ops(seq):
    first, *middle, last = seq
    return first + last + len(middle)

def format_ops(a, b):
    return f"{a}:{b!r}:{a + b}"

def driver():
    child = Child()
    return (
        len(unicode_ops("abcdef", 3))
        + dict_set_ops(2, 5)
        + match_ops({"kind": "pair", "left": 4, "right": 6})
        + match_ops([1, 2, 3, 4])
        + unpack_ops((1, 2, 3, 4))
        + len(format_ops(3, 8))
        + child.value()
    )
)";

  const char* names[] = {
      "unicode_ops",
      "dict_set_ops",
      "match_ops",
      "unpack_ops",
      "format_ops",
      "driver",
  };
  for (const char* name : names) {
    Ref<PyFunctionObject> func(compileAndGet(py_src, name));
    ASSERT_NE(func, nullptr) << name;
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK) << name;
  }
  Ref<PyFunctionObject> driver(compileAndGet(py_src, "driver"));
  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(driver, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}

class JITPyjitApiCoverageTest : public RuntimeTest {};

TEST_F(JITPyjitApiCoverageTest, EnableDisableAndQueryState) {
  SKIP_311_EXECUTABLE_COMPILE();

  auto mod = importCinderJitModule();
  callJitNoArgs(mod, "enable");
  auto enabled = callJitNoArgs(mod, "is_enabled");
  EXPECT_TRUE(PyObject_IsTrue(enabled));

  callJitNoArgs(mod, "disable");
  auto disabled = callJitNoArgs(mod, "is_enabled");
  EXPECT_FALSE(PyObject_IsTrue(disabled));

  callJitNoArgs(mod, "enable");
}

TEST_F(JITPyjitApiCoverageTest, CompileLazyForceAndUncompile) {
  SKIP_311_UNTIL_SURFACE("lazy compilation and force_uncompile");

  const char* py_src = R"(
def sample(x):
    return x + 7
)";

  Ref<PyFunctionObject> sample(compileAndGet(py_src, "sample"));
  auto mod = importCinderJitModule();

  callJitOneArg(mod, "lazy_compile", sample);
  auto not_compiled = callJitOneArg(mod, "is_jit_compiled", sample);
  EXPECT_FALSE(PyObject_IsTrue(not_compiled));

  auto arg = Ref<>::steal(PyLong_FromLong(10));
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto call_result = Ref<>::steal(PyObject_Call(sample, args, nullptr));
  ASSERT_NE(call_result, nullptr);
  EXPECT_EQ(PyLong_AsLong(call_result), 17);

  callJitOneArg(mod, "force_compile", sample);
  auto compiled = callJitOneArg(mod, "is_jit_compiled", sample);
  EXPECT_TRUE(PyObject_IsTrue(compiled));

  callJitOneArg(mod, "force_uncompile", sample);
}

TEST_F(JITPyjitApiCoverageTest, JitListDisassembleAndSuppress) {
  SKIP_311_UNTIL_SURFACE("the JIT list and disassembly control plane");

  runStockCode(R"(
import cinderx.jit as jit
import os
import tempfile

def listed_fn(x):
    return x * 3

jit.enable()
jit.append_jit_list("jittestmodule:listed_fn")
jit.get_jit_list()

with tempfile.NamedTemporaryFile("w", delete=False) as f:
    f.write("jittestmodule:listed_fn\n")
    path = f.name
try:
    jit.read_jit_list(path)
finally:
    os.unlink(path)

jit.force_compile(listed_fn)
jit.disassemble(listed_fn)

@jit.jit_suppress
def suppressed(y):
    return y + 1

jit.jit_unsuppress(suppressed)
jit.force_uncompile(listed_fn)
)");
}

TEST_F(JITPyjitApiCoverageTest, AutoCompileAfterNCallsAndStats) {
  SKIP_311_UNTIL_SURFACE("automatic compilation and its statistics");

  runStockCode(R"(
import cinderx.jit as jit

def counted(x):
    return x + 2

jit.enable()
jit.auto()
jit.compile_after_n_calls(1)
assert counted(4) == 6
calls = jit.count_interpreted_calls(counted)
assert calls >= 0
funcs = jit.get_compiled_functions()
assert isinstance(funcs, list)
jit.clear_runtime_stats()
stats = jit.get_and_clear_runtime_stats()
assert stats is not None
)");
}

class JITContextP0CoverageTest : public RuntimeTest {};

TEST_F(JITContextP0CoverageTest, GlobalContextStrBuildClassAndDeoptedFuncs) {
  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  BorrowedRef<> build_class = ctx->strBuildClass();
  ASSERT_NE(build_class, nullptr);
  EXPECT_TRUE(PyUnicode_Check(build_class));

  const auto& deopted = ctx->deoptedFuncs();
  EXPECT_GE(deopted.size(), 0u);
}

TEST_F(JITContextP0CoverageTest, GlobalContextForgetCodeAndReferences) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def forget_me():
    return 88
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "forget_me"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  EXPECT_TRUE(ctx->didCompile(func));

  auto holder = Ref<>::steal(PyLong_FromLong(42));
  ctx->addReference(holder);
  ctx->releaseReferences();

  ctx->forgetCode(func);
  EXPECT_FALSE(ctx->didCompile(func));
}

#if PY_VERSION_HEX < 0x030C0000
// The MR-04 lifecycle contracts, asserted natively rather than only from
// Python.  Each of these was reported against a build where the Python-level
// suite was already green: the Python surface can only see what the canary
// control plane publishes, and the registries these cases read are not on it.
class JITLifecycle311Test : public RuntimeTest {};

TEST_F(JITLifecycle311Test, DefaultsUninstallTheEntry) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Adding __defaults__ moves the function out of the argument shape the
  // artifact was compiled for.  3.11 has no function watcher to notice, so
  // the guarded entry has to notice on the next call -- and every reporter
  // of "is this compiled" has to agree with it, or the state is compiled to
  // one caller and interpreted to another.
  const char* py_src = R"(
def defaulted(x):
    return x + 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "defaulted"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));
  ASSERT_NE(Ci_JitShell311_InstalledArtifact(func), nullptr);

  auto arg = makeLong(41);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto before = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(PyLong_AsLong(before), 42);

  auto defaults = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  ASSERT_NE(defaults, nullptr);
  ASSERT_EQ(PyObject_SetAttrString(func, "__defaults__", defaults), 0);

  // Both reporters must move together with the entry.
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
  EXPECT_FALSE(isJitCompiled(func));

  // And the call still has to produce the right answer, through the
  // interpreter.
  auto after = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyLong_AsLong(after), 42);
  auto no_args = Ref<>::steal(PyTuple_New(0));
  auto defaulted = Ref<>::steal(PyObject_Call(func, no_args, nullptr));
  ASSERT_NE(defaulted, nullptr);
  EXPECT_EQ(PyLong_AsLong(defaulted), 42);
}

TEST_F(JITLifecycle311Test, PausedArtifactStaysResidentAndReattaches) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Pausing deopts every function, which empties the installed registry.
  // The artifact itself is still resident -- it holds code memory -- so a
  // lifecycle report that counts only installed functions reads zero while
  // the machine code is still allocated.  The two counts are separate
  // measurements and this asserts they stay separate.
  const char* py_src = R"(
def paused(x):
    return x * 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "paused"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  size_t resident_before = ctx->compiledCodes().size();
  ASSERT_GT(resident_before, 0u);

  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);

  EXPECT_TRUE(jit::isJitPaused());
  EXPECT_FALSE(isJitCompiled(func));
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
  // Resident, not installed: the artifact outlives the entry.
  EXPECT_EQ(ctx->compiledCodes().size(), resident_before);
  EXPECT_EQ(ctx->deoptedFuncs().count(func), 1u);

  auto arg = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto while_paused = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(while_paused, nullptr);
  EXPECT_EQ(PyLong_AsLong(while_paused), 15);

  callJitNoArgs(mod, "enable");
  EXPECT_FALSE(jit::isJitPaused());
  EXPECT_TRUE(isJitCompiled(func));
  EXPECT_NE(Ci_JitShell311_InstalledArtifact(func), nullptr);
  EXPECT_EQ(ctx->deoptedFuncs().count(func), 0u);

  auto after = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyLong_AsLong(after), 15);
}

TEST_F(JITLifecycle311Test, EnableKeepsParkedFunctionsAcrossFailures) {
  SKIP_311_EXECUTABLE_COMPILE();

  // enable() walks the parked set and republishes each function.  A
  // publication can fail (MemoryError), and the walk used to swallow the
  // failure: the parked entry was already removed before the attempt, the
  // error stayed pending while the walk kept calling into the C API, and
  // enable() reported success.  Now the failure propagates, and whatever
  // was not reattached -- the failed function included -- stays parked for
  // the next enable() to retry.
  const char* py_src = R"(
def resilient(x):
    total = x - x
    while total < x:
        total = total + x
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "resilient"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);
  ASSERT_FALSE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(ctx->deoptedFuncs().count(func), 1u);

  // Fail the first republication the walk attempts.  Which parked function
  // consumes the failpoint depends on set order; the contract holds either
  // way: enable() reports the failure, and nothing is lost.
  jit::failJitPublishStepForTest(2);
  auto failed = Ref<>::steal(PyObject_CallMethod(mod, "enable", nullptr));
  jit::failJitPublishStepForTest(0);
  EXPECT_EQ(failed, nullptr) << "enable() swallowed a failed republication";
  ASSERT_TRUE(PyErr_Occurred());
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_MemoryError));
  PyErr_Clear();

  // A failed enable() hands back the flip it performed: the caller that
  // saw the exception must not find the JIT reporting enabled, globally or
  // by state.
  EXPECT_EQ(jit::getConfig().state, jit::State::kPaused)
      << "the failed enable() left the execute state armed";
  {
    auto enabled =
        Ref<>::steal(PyObject_CallMethod(mod, "is_enabled", nullptr));
    ASSERT_NE(enabled, nullptr);
    EXPECT_FALSE(PyObject_IsTrue(enabled))
        << "is_enabled() says true after enable() raised";
  }

  // The failed function is still parked or already reattached -- never
  // silently gone -- so a clean enable() finishes the job.
  callJitNoArgs(mod, "enable");
  EXPECT_TRUE(isJitCompiled(func))
      << "the failed enable() dropped the parked entry";
  EXPECT_EQ(ctx->deoptedFuncs().count(func), 0u);
  EXPECT_EQ(jit::getConfig().state, jit::State::kRunning);
  {
    auto enabled =
        Ref<>::steal(PyObject_CallMethod(mod, "is_enabled", nullptr));
    ASSERT_NE(enabled, nullptr);
    EXPECT_TRUE(PyObject_IsTrue(enabled));
  }

  auto arg = makeLong(4);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 4);
}

TEST_F(JITLifecycle311Test, RepublicationSurvivesReentrantAnchorRelease) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The dictionary anchor written by a takeover displaces the prior
  // artifact's owning reference.  Released in place, that reference's
  // destructor can run arbitrary Python -- here an object held only by the
  // prior artifact's runtime, whose __del__ reenters force_compile() for
  // the same function -- in the middle of the publication.  The inner call
  // used to complete a full installation that the outer transaction then
  // dismantled while reporting success.  The displaced anchor is now
  // detained until the transaction settles: the reentry runs after
  // publication completes and finds a consistent, finished world.
  const char* py_src = R"(
events = []

def chameleon(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def replacement(a, b):
    total = a + a
    while total < b:
        total = total + a
    return total

class Reenter:
    def __init__(self, target):
        self.target = target
    def __del__(self):
        events.append("del")
        import cinderjit
        try:
            events.append(cinderjit.is_jit_compiled(self.target))
            events.append(cinderjit.force_compile(self.target))
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "chameleon"));
  Ref<PyFunctionObject> donor(getGlobal("replacement"));
  ASSERT_NE(func, nullptr);
  ASSERT_NE(donor, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  jit::CompiledFunction* prior_art = ctx->lookupFunc(func);
  ASSERT_NE(prior_art, nullptr);
  ASSERT_NE(prior_art->runtime(), nullptr);

  // Hand the reentry trigger to the prior artifact's runtime, keep no
  // other reference to it, and make the anchor the artifact's only owner.
  runCode("k = Reenter(chameleon)\n");
  {
    Ref<> trigger(getGlobal("k"));
    ASSERT_NE(trigger, nullptr);
    prior_art->runtime()->addReference(trigger);
  }
  runCode("del k\n");

  // Take over with new code.  On the unfixed order the prior artifact died
  // inside the anchor write, __del__ reentered force_compile() and the
  // outer transaction dismantled the inner installation.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", donor->func_code), 0);
  auto mod = importCinderJitModule();
  callJitOneArg(mod, "force_compile", func);

  // The published world is coherent: installed, listed, anchored, and the
  // reentry observed it exactly once, after settlement.
  EXPECT_TRUE(isJitCompiled(func));
  jit::CompiledFunction* new_art = ctx->lookupFunc(func);
  ASSERT_NE(new_art, nullptr);
  EXPECT_NE(new_art, prior_art);
  EXPECT_TRUE(new_art->functions().contains(func.get()));
  ASSERT_NE(func->func_dict, nullptr);
  EXPECT_EQ(
      PyDict_GetItemWithError(func->func_dict, jit::kCompiledFunctionKey),
      reinterpret_cast<PyObject*>(new_art));
  PyErr_Clear();

  // The reentry fired exactly once and observed one finished world: the
  // predicate answers compiled, and a forced recompile is refused as
  // already-compiled -- the established answer for a settled publication.
  // (Before the displaced anchor moved to the deferred-release queue, the
  // release fired inside the publication call stack, where the code-extra
  // ledger was not yet written: the same reentry then saw not-compiled and
  // ran a full nested publication against the half-written world.)
  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_TRUE(PyList_Check(events.get()));
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 3)
      << "the reentry trigger did not fire exactly once";
  EXPECT_EQ(PyList_GET_ITEM(events.get(), 1), Py_True)
      << "the post-settlement reentry did not see a finished publication";
  EXPECT_EQ(PyList_GET_ITEM(events.get(), 2), Py_False)
      << "the reentry re-ran a publication instead of being refused as "
         "already-compiled";

  auto arg_a = makeLong(2);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITLifecycle311Test, RepublicationSurvivesReentrantDisable) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The displaced anchor's release can run a __del__ that calls
  // disable(deopt_all=True): everything just published is unloaded again,
  // parked, and the JIT is paused -- before the compile that performed the
  // takeover reports its verdict.  The verdict must describe the world it
  // returns into, not the one it built: the drain runs before the answer
  // is computed, the answer re-verifies the installation, and a caller
  // never sees force_compile() succeed for a function whose every call
  // runs interpreted.
  const char* py_src = R"(
events = []

def swapper(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def successor(a, b):
    total = a + a
    while total < b:
        total = total + a
    return total

class Downer:
    def __del__(self):
        events.append("del")
        import cinderjit
        try:
            cinderjit.disable(deopt_all=True)
            events.append("disabled")
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "swapper"));
  Ref<PyFunctionObject> donor(getGlobal("successor"));
  ASSERT_NE(func, nullptr);
  ASSERT_NE(donor, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  jit::CompiledFunction* prior_art = ctx->lookupFunc(func);
  ASSERT_NE(prior_art, nullptr);
  ASSERT_NE(prior_art->runtime(), nullptr);

  runCode("k = Downer()\n");
  {
    Ref<> trigger(getGlobal("k"));
    ASSERT_NE(trigger, nullptr);
    prior_art->runtime()->addReference(trigger);
  }
  runCode("del k\n");

  // Take over with new code; the prior artifact dies in the post-verdict
  // drain and its __del__ pauses the JIT.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", donor->func_code), 0);
  auto mod = importCinderJitModule();
  auto failed =
      Ref<>::steal(PyObject_CallMethod(mod, "force_compile", "O", func.get()));
  EXPECT_EQ(failed, nullptr)
      << "force_compile() reported success for a function the reentrant "
         "disable() already unloaded";
  ASSERT_TRUE(PyErr_Occurred());
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();

  // The reentrant decision owns the final state: paused, function parked,
  // nothing installed -- and nothing lost.
  EXPECT_FALSE(isJitCompiled(func));
  EXPECT_EQ(jit::getConfig().state, jit::State::kPaused);
  EXPECT_EQ(ctx->deoptedFuncs().count(func), 1u);

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 2);
  EXPECT_TRUE(
      PyUnicode_CompareWithASCIIString(
          PyList_GET_ITEM(events.get(), 1), "disabled") == 0);

  // The next enable() reattaches what the reentry parked.
  callJitNoArgs(mod, "enable");
  EXPECT_TRUE(isJitCompiled(func));
  EXPECT_EQ(ctx->deoptedFuncs().count(func), 0u);
  auto arg_a = makeLong(2);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITLifecycle311Test, EnableReattachSurvivesReentrantDisable) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The enable() walk can displace a value planted at the anchor key of a
  // parked function -- a forged entry carries no ownership, but its
  // release still runs arbitrary Python.  The release is deferred to the
  // walk's end, so it cannot mutate the set mid-iteration; a __del__ that
  // calls disable(deopt_all=True) there re-parks everything, and the
  // decision is newer than the enable(): the state it wrote survives, the
  // caller is told the JIT did not come up enabled, and no entry is
  // duplicated or lost.
  const char* py_src = R"(
events = []

def alpha(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def bravo(a, b):
    total = a - a
    while total < b:
        total = total + b
    return total

class Downer:
    def __del__(self):
        events.append("del")
        import cinderjit
        try:
            cinderjit.disable(deopt_all=True)
            events.append("disabled")
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
)";

  Ref<PyFunctionObject> alpha(compileAndGet(py_src, "alpha"));
  Ref<PyFunctionObject> bravo(getGlobal("bravo"));
  ASSERT_NE(alpha, nullptr);
  ASSERT_NE(bravo, nullptr);
  ASSERT_EQ(jit::compileFunction(alpha), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(bravo), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  // Keep bravo's artifact alive across the forged-anchor overwrite below:
  // the anchor is its only owning reference while parked.
  Ref<jit::CompiledFunction> pin(
      Ref<jit::CompiledFunction>::create(ctx->lookupFunc(bravo)));
  ASSERT_NE(pin, nullptr);

  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);
  ASSERT_FALSE(isJitCompiled(alpha));
  ASSERT_FALSE(isJitCompiled(bravo));
  ASSERT_EQ(ctx->deoptedFuncs().size(), 2u);

  // Plant the trigger at bravo's anchor key; the reattachment displaces
  // it, and the deferred release fires after the walk.
  runCode("bravo.__dict__['__cinderx_compiled_func__'] = Downer()\n");

  auto failed = Ref<>::steal(PyObject_CallMethod(mod, "enable", nullptr));
  EXPECT_EQ(failed, nullptr)
      << "enable() reported an enabled JIT after a reentrant disable()";
  ASSERT_TRUE(PyErr_Occurred());
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();

  // The reentrant decision owns the state; both functions are parked
  // exactly once and nothing is installed.
  EXPECT_EQ(jit::getConfig().state, jit::State::kPaused);
  EXPECT_FALSE(isJitCompiled(alpha));
  EXPECT_FALSE(isJitCompiled(bravo));
  EXPECT_EQ(ctx->deoptedFuncs().size(), 2u);
  EXPECT_EQ(ctx->deoptedFuncs().count(alpha), 1u);
  EXPECT_EQ(ctx->deoptedFuncs().count(bravo), 1u);

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 2);

  // A clean enable() finishes the job for both.
  callJitNoArgs(mod, "enable");
  EXPECT_TRUE(isJitCompiled(alpha));
  EXPECT_TRUE(isJitCompiled(bravo));
  EXPECT_EQ(ctx->deoptedFuncs().size(), 0u);
  auto arg_a = makeLong(3);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(alpha, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITLifecycle311Test, ParkedFunctionOutlivesEveryOtherReference) {
  SKIP_311_EXECUTABLE_COMPILE();

  // While paused, the parked set is the only thing keeping the function
  // reachable: 3.11 raises no destroy notification, so a borrowed entry
  // would be walked as a dangling pointer the moment the JIT is re-enabled.
  // Drop every reference this test holds and re-enable.
  const char* py_src = R"(
def parked(x):
    return x - 1
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "parked"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);
  ASSERT_EQ(ctx->deoptedFuncs().count(func), 1u);

  // The module still names it, so drop that too: after this the parked set
  // is the last owner.
  runStockCode("del parked");
  BorrowedRef<PyFunctionObject> observed = func;
  // Assert ownership before dropping: if the parked set only borrowed, the
  // reset below would free the function and every read after it would be a
  // use-after-free rather than a failed expectation.
  ASSERT_GT(Py_REFCNT(func.get()), 1);
  func.reset();
  // Nothing outside the parked set names it now, and the cycle it sits in is
  // collectable, so a borrowing set would lose it right here.
  PyGC_Collect();
  ASSERT_EQ(ctx->deoptedFuncs().count(observed), 1u);

  // Take a strong reference back before re-enabling.  enable() drops the
  // parked set's ownership, and what remains is the collectable
  // function -> dict -> artifact -> function cycle; reading through a
  // borrowed pointer afterwards would be at the mercy of whichever
  // allocation trips the next collection.
  Ref<PyFunctionObject> revived{Ref<PyFunctionObject>::create(observed)};

  // Re-enabling walks the parked set.  A borrowed entry crashes here.
  callJitNoArgs(mod, "enable");
  EXPECT_EQ(ctx->deoptedFuncs().count(revived), 0u);
  EXPECT_TRUE(isJitCompiled(revived));

  // The function is still callable and still correct after the round trip.
  auto arg = makeLong(9);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto* callee = reinterpret_cast<PyObject*>(revived.get());
  auto result = Ref<>::steal(PyObject_Call(callee, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 8);
}

TEST_F(JITLifecycle311Test, MultithreadedTeardownSurvivesSelfFree) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The sharp case for the teardown above: leave the function and its
  // artifact holding only each other.  Releasing the artifact's owned
  // reference then destroys the function, whose dictionary holds the
  // artifact's last reference -- so the artifact can be freed inside the
  // very call that is walking it, and everything the teardown does
  // afterwards would touch freed memory.
  const char* py_src = R"(
def solo(x):
    return x + 6
)";

  auto ctx = std::make_unique<jit::CompilerContext<jit::Compiler>>();
  Ref<> weak;
  {
    Ref<PyFunctionObject> func(compileAndGet(py_src, "solo"));
    ASSERT_NE(func, nullptr);
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code)));
    ASSERT_EQ(
        jit::compilePreloaderImpl(ctx.get(), *preloader, func),
        jit::Result::OK);
    weak = Ref<>::steal(PyWeakref_NewRef(func, nullptr));
    ASSERT_NE(weak, nullptr);
  }
  // Drop the module binding too; after this the cycle is all that is left.
  runStockCode("del solo");
  ASSERT_NE(PyWeakref_GetObject(weak), Py_None)
      << "the artifact's owned reference should still be holding the function";

  ctx->clearForMultithreadedCompileTest();

  // The owned reference was the last one, so the function died inside the
  // call -- which is the condition this case exists to put the teardown in.
  EXPECT_EQ(PyWeakref_GetObject(weak), Py_None);

  ctx.reset();
}

TEST_F(JITLifecycle311Test, FinalizeRefusesReentrantEnable) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Releasing the parked set runs destructors, and a destructor runs
  // arbitrary Python.  If that Python re-enters enable(), the execute
  // surface is re-armed in the middle of teardown and the same set is walked
  // again while it is being emptied.  Teardown has to be one-way, and the
  // tripwire below is what a user object in the function's dictionary can
  // actually do.
  runStockCode(R"(
import cinderjit

reentry = []

class Tripwire:
    def __del__(self):
        try:
            cinderjit.enable()
            reentry.append("enabled")
        except RuntimeError:
            reentry.append("refused")
        except BaseException as exc:
            reentry.append("other:" + type(exc).__name__)

def tripwired(x):
    return x + 2

tripwired.tripwire = Tripwire()
)");

  Ref<PyFunctionObject> func(getGlobal("tripwired"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);
  ASSERT_EQ(ctx->deoptedFuncs().count(func), 1u);

  // Leave the parked set as the only owner, so finalize() is what destroys
  // the function and therefore what runs the tripwire.
  runStockCode("del tripwired");
  ASSERT_GT(Py_REFCNT(func.get()), 1);
  func.reset();

  jit::finalize();

  Ref<> reentry(getGlobal("reentry"));
  ASSERT_NE(reentry, nullptr);
  ASSERT_TRUE(PyList_CheckExact(reentry));
  ASSERT_EQ(PyList_GET_SIZE(reentry.get()), 1)
      << "the tripwire did not run; finalize() never released the parked set";
  BorrowedRef<> outcome = PyList_GET_ITEM(reentry.get(), 0);
  const char* text = PyUnicode_AsUTF8(outcome);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "refused");
  EXPECT_FALSE(jit::isJitInitialized());
}

TEST_F(JITLifecycle311Test, MultithreadedCompileTeardownReleasesFunctions) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The multithreaded-compile teardown detaches every artifact from the
  // context before dropping the maps.  clear() only releases the functions an
  // artifact owns while that owner is still set, so a detached artifact would
  // carry its references to the grave -- silently, forever, once per run of
  // that path.  Nothing crashes, so the assertion has to be on the count.
  const char* py_src = R"(
def orphaned(x):
    return x - 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "orphaned"));
  ASSERT_NE(func, nullptr);

  // A private context, as the existing clearForMultithreadedCompileTest case
  // uses.  That helper orphans artifacts while their functions keep owning
  // them, and an orphaned artifact holds a raw CodeRuntime pointer into the
  // context's slab.  Run against the process-wide context, such an artifact
  // is collected at interpreter shutdown -- after the slab is gone.
  auto ctx = std::make_unique<jit::CompilerContext<jit::Compiler>>();
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_EQ(
      jit::compilePreloaderImpl(ctx.get(), *preloader, func), jit::Result::OK);

  auto compiled =
      ctx->lookupCode(func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(compiled->functions().count(func), 1u);

  Py_ssize_t before = Py_REFCNT(func.get());
  ctx->clearForMultithreadedCompileTest();
  Py_ssize_t after = Py_REFCNT(func.get());

  EXPECT_LT(after, before) << "the orphaned artifact kept its owned reference";
  EXPECT_EQ(compiled->functions().count(func), 0u);

  // The association is gone, so the guarded entry has to send the call back
  // to the interpreter rather than into an artifact nobody tracks.
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
  auto arg = makeLong(11);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 8);

  // Release the artifact before the context whose slab its CodeRuntime lives
  // in: the orphan is not in compiled_codes_ any more, so ~Context() does not
  // clear it and it would be cleared later against freed memory.
  compiled.reset();
  preloader.reset();
  func.reset();
  ctx.reset();
}

TEST_F(JITLifecycle311Test, FinalizeEmptiesTheInstalledRegistry) {
  SKIP_311_EXECUTABLE_COMPILE();

  // finalize() enforces "nothing is still compiled" with a JIT_CHECK that
  // aborts the process, so reaching the end of this case is part of the
  // assertion.  What is observable afterwards is the function itself: the
  // entry must be back on the interpreter and the code-extra cache must no
  // longer name an artifact, or a later call would jump into freed code.
  const char* py_src = R"(
def finalized(x):
    return x + 4
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "finalized"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));
  ASSERT_NE(Ci_JitShell311_InstalledArtifact(func), nullptr);

  jit::finalize();

  EXPECT_FALSE(jit::isJitInitialized());
  EXPECT_FALSE(isJitCompiled(func));
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);

  // Assert the cache directly, not just the predicate built on it: the
  // predicate also fails when the artifact merely forgot this function, so
  // on its own it would pass over a code object still naming an artifact
  // that is about to be freed.
  BorrowedRef<PyCodeObject> code{func->func_code};
  CodeExtra* extra = codeExtraIfExists(code);
  ASSERT_NE(extra, nullptr);
  EXPECT_EQ(_Py_atomic_load_ptr_acquire(&extra->jit_compiled), nullptr);

  // Now outlive the artifact.  Its last strong reference is the entry in the
  // function's dictionary; drop that, then query again.  A stale cache turns
  // this into a use-after-free under the sanitizer instead of a null read.
  auto artifact_key =
      Ref<>::steal(PyUnicode_FromString("__cinderx_compiled_func__"));
  ASSERT_NE(artifact_key, nullptr);
  ASSERT_NE(func->func_dict, nullptr);
  ASSERT_EQ(PyDict_Contains(func->func_dict, artifact_key), 1);
  ASSERT_EQ(PyDict_DelItem(func->func_dict, artifact_key), 0);
  PyGC_Collect();
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);

  auto arg = makeLong(6);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 10);
}

TEST_F(JITLifecycle311Test, FinalizeEmptiesTheParkedRegistry) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The parked set owns its entries on this branch, so finalizing while it
  // is populated has to release them.  Skipping that release would not
  // crash -- it would leak every parked function for the life of the
  // process -- which is why the assertion is on the reference count and not
  // on survival.
  const char* py_src = R"(
def parked_at_exit(x):
    return x * 5
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "parked_at_exit"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  auto mod = importCinderJitModule();
  auto deopt_all = Ref<>::steal(PyBool_FromLong(1));
  callJitOneArg(mod, "disable", deopt_all);
  ASSERT_EQ(ctx->deoptedFuncs().count(func), 1u);

  Py_ssize_t before = Py_REFCNT(func.get());
  jit::finalize();
  Py_ssize_t after = Py_REFCNT(func.get());

  EXPECT_FALSE(jit::isJitInitialized());
  EXPECT_LT(after, before) << "the parked set kept its owned reference";

  auto arg = makeLong(7);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 35);
}

#endif // PY_VERSION_HEX < 0x030C0000

class JITStaticJitRtCoverageTest : public RuntimeTest {
 public:
  JITStaticJitRtCoverageTest()
#if PY_VERSION_HEX < 0x030C0000
      // Static Python is not built on 3.11; skip in the test body instead of
      // requesting strict globals that MakeGlobalsStrict() rejects.
      : RuntimeTest(kJit) {
  }
#else
      : RuntimeTest(static_cast<Flags>(kJit | kStaticCompiler)) {
  }
#endif
};

TEST_F(JITStaticJitRtCoverageTest, StaticPrimitiveBoxUnboxModPow) {
  SKIP_311_UNTIL_SURFACE("the Static Python runtime cache");

  const char* py_src = R"(
from __static__ import int32, int64, uint32, uint64

def work() -> int64:
    a: int32 = int32(17)
    b: int32 = int32(5)
    m: int32 = a % b
    u: uint32 = uint32(9) % uint32(4)
    p: int64 = int64(m * m)
    q: uint64 = uint64(u * u)
    return p + int64(q)

def run() -> int64:
    return work()
)";

  Ref<PyFunctionObject> work(compileStaticAndGet(py_src, "work"));
  Ref<PyFunctionObject> run(compileStaticAndGet(py_src, "run"));
  ASSERT_NE(work, nullptr);
  ASSERT_NE(run, nullptr);
  ASSERT_EQ(jit::compileFunction(work), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(run), jit::Result::OK);

  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_NE(PyLong_AsLong(result), 0);
}

TEST_F(JITStaticJitRtCoverageTest, StaticArrayFieldsAndInvoke) {
  SKIP_311_UNTIL_SURFACE("the Static Python runtime cache");

  const char* py_src = R"(
from __static__ import Array, box, clen, int64

class Accum:
    value: int64

    def __init__(self, value: int64):
        self.value = value

    def add(self, other: int64) -> int64:
        self.value = self.value + other
        return self.value

def array_total(size: int) -> int:
    arr: Array[int64] = Array[int64](size)
    i: int64 = 0
    while i < clen(arr):
        arr[i] = i + int64(1)
        i += 1
    arr[int64(0)] = int64(7)
    total: int64 = 0
    for value in arr:
        total += value
    total += arr[int64(0)]
    total += arr[1]
    return box(total)

def invoke_total() -> int:
    acc = Accum(int64(5))
    first: int64 = acc.add(int64(3))
    second: int64 = acc.add(int64(4))
    return box(first + second + acc.value)

def run() -> int:
    return array_total(5) + invoke_total()
)";

  const char* names[] = {"array_total", "invoke_total", "run"};
  for (const char* name : names) {
    Ref<PyFunctionObject> func(compileStaticAndGet(py_src, name));
    ASSERT_NE(func, nullptr) << name;
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK) << name;
  }

  Ref<PyFunctionObject> run(compileStaticAndGet(py_src, "run"));
  auto args = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(run, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_GT(PyLong_AsLong(result), 0);
}
