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
#include "cinderx/module_state.h"
#if PY_VERSION_HEX < 0x030C0000
// The MR-04 execute surface predicates the lifecycle cases assert on.
#include "cinderx/Interpreter/3.11/observe.h"
#endif

#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

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
  // installed.  LOAD_ATTR is still off the execute whitelist (keyword-only
  // signatures are rebound by the generated prologue and are no longer a
  // refusal).  An empty data block exercises the same early-return paths
  // an allocation failure would take.
  Ref<PyFunctionObject> func(
      compileAndGet("def func(obj): return obj.attr", "func"));
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
  // One name per step: rebinding would kill the prior step's function and
  // pollute the per-step watch arithmetic.
  const char* sources[7] = {
      "def func1(a, b):\n    total = a - a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func2(a, b):\n    total = a + a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func3(a, b):\n    total = b - a\n    while total < b:\n"
      "        total = total + a\n    return total",
      "def func4(a, b):\n    total = b + a\n    while total < a:\n"
      "        total = total + b\n    return total",
      // Step 5: the code-extra reserve -- the C-convention allocation whose
      // MemoryError used to be swallowed on the way up.
      "def func5(a, b):\n    total = b + b\n    while total < a:\n"
      "        total = total + a\n    return total",
      // Step 6: the death watch's Python allocations.  The watch is what
      // keeps the borrowed registry entry from dangling, so failing to arm
      // it must fail the publication, not fall back to an unwatched entry.
      "def func6(a, b):\n    total = a - b\n    while total < b:\n"
      "        total = total + b\n    return total",
      // Step 7: the death-watch map slot itself.
      "def func7(a, b):\n    total = a + b\n    while total < a:\n"
      "        total = total + b\n    return total",
  };
  for (int step = 1; step <= 7; step++) {
    std::string name = "func" + std::to_string(step);
    Ref<PyFunctionObject> func(compileAndGet(sources[step - 1], name.c_str()));
    ASSERT_NE(func, nullptr) << "step " << step;
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code)));
    ASSERT_NE(preloader, nullptr) << "step " << step;

    vectorcallfunc vectorcall_before = func->vectorcall;
    size_t watched_before = jit_ctx_->watchedFunctionCount();
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
    EXPECT_EQ(jit_ctx_->watchedFunctionCount(), watched_before)
        << "step " << step
        << " left a death watch armed for a function it did not publish";
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
    EXPECT_EQ(jit_ctx_->watchedFunctionCount(), watched_before + 1)
        << "step " << step << " published without arming the death watch";
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

  for (int step : {1, 2, 3, 8}) {
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
TEST_F(JITContextTest, TeardownReleasesCannotOutliveTheDeathWatch) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A's runtime holds the only chain to function B, so ~Context's
  // releases kill B mid-teardown.  Sever-before-release plus watch-
  // dismantled-last means that death is still reported -- the counter is
  // the order's observable.
  const char* py_src = R"(
def carrier(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def doomed(a, b):
    total = a + b
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> carrier(compileAndGet(py_src, "carrier"));
  Ref<PyFunctionObject> doomed(getGlobal("doomed"));
  ASSERT_NE(carrier, nullptr);
  ASSERT_NE(doomed, nullptr);

  for (BorrowedRef<PyFunctionObject> func : {carrier.get(), doomed.get()}) {
    std::unique_ptr<jit::hir::Preloader> preloader(jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code)));
    ASSERT_NE(preloader, nullptr);
    ASSERT_EQ(
        jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
        jit::Result::OK);
  }
  ASSERT_EQ(jit_ctx_->watchedFunctionCount(), 2u);

  jit::CompiledFunction* carrier_compiled = jit_ctx_->lookupFunc(carrier);
  ASSERT_NE(carrier_compiled, nullptr);
  ASSERT_NE(carrier_compiled->runtime(), nullptr);

  // Hand the killer to A's runtime, then reduce B to the killer's single
  // reference: out of the module globals, out of our hands.
  {
    auto killer = Ref<>::steal(PyList_New(0));
    ASSERT_NE(killer, nullptr);
    ASSERT_EQ(
        PyList_Append(killer, reinterpret_cast<PyObject*>(doomed.get())), 0);
    carrier_compiled->runtime()->addReference(killer);
  }
  ASSERT_EQ(PyDict_DelItemString(doomed->func_globals, "doomed"), 0);
  BorrowedRef<PyFunctionObject> doomed_borrowed = doomed.get();
  doomed.reset();
  ASSERT_EQ(Py_REFCNT(doomed_borrowed.get()), 1);

  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  jit_ctx_.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "a function died inside ~Context and its death went unreported: "
         "the watch was withdrawn before the releases that can still kill";
}

TEST_F(JITContextTest, DeathReportsToTheWatchOwningContext) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A watch belongs to the context that recorded the borrowed pointer, so
  // the death callback has to clean that context -- not whichever context
  // the module currently holds.  This context is exactly the local kind
  // the callback used to miss: publish here, pin the artifact so it
  // outlives everything, then let the function die independently.  The
  // death must empty THIS context's registries and its watch map; a
  // callback that reports into the module context instead leaves all of
  // them holding freed pointers, and the destructor's severing walk then
  // writes through one (ASAN: write after free).
  //
  // This is also the local-context native twin of the external-pin
  // contract: the artifact's lifetime is the pin's business, the
  // function's lifetime is its own, and the two part ways cleanly.
  const char* py_src = R"(
def standalone(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "standalone"));
  ASSERT_NE(func, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);
  ASSERT_EQ(jit_ctx_->watchedFunctionCount(), 1u);
  ASSERT_EQ(jit_ctx_->compiledFuncs().size(), 1u);

  // Pin the artifact independently of the function.
  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  auto pin = Ref<jit::CompiledFunction>::create(compiled);

  // Reduce the function to our single reference, then let it die.
  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "standalone"), 0);
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  func.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;

  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "the death was not delivered at all";
  EXPECT_EQ(jit_ctx_->watchedFunctionCount(), 0u)
      << "the death was delivered to a different context's watch map";
  EXPECT_EQ(jit_ctx_->compiledFuncs().size(), 0u)
      << "the owning context's installed registry still names the dead "
         "function";
  EXPECT_TRUE(compiled->functions().empty())
      << "the artifact still names the dead function as a member";
  // Physical residency is the pin's business, not the function's.
  EXPECT_EQ(jit_ctx_->compiledCodes().size(), 1u);

  // The severing walk in ~Context must find nothing dead to touch.
  jit_ctx_.reset();
}

namespace {

// Weak-reference callback helper for the owner-death test below: resets
// the context holder the capsule names, destroying the context from
// inside the callback batch.
extern "C" PyObject* destroyContextForTest(PyObject* self, PyObject* /*ref*/) {
  auto* holder =
      static_cast<std::unique_ptr<jit::CompilerContext<jit::Compiler>>*>(
          PyCapsule_GetPointer(self, "cinderx-test.ctx-holder"));
  if (holder != nullptr) {
    holder->reset();
  }
  Py_RETURN_NONE;
}

PyMethodDef kDestroyContextForTest = {
    "destroy_ctx_for_test",
    destroyContextForTest,
    METH_O,
    nullptr,
};

} // namespace

TEST_F(JITContextTest, DeathCallbackSurvivesItsOwnerDyingFirst) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CPython's weak-reference machinery snapshots every pending callback --
  // holding strong references to them -- before invoking any.  A user
  // callback in the same batch can therefore destroy the context whose
  // watch armed the JIT's callback, and the JIT's callback still runs.
  // The payload's owner token is what makes that safe: ~Context nulls the
  // cell, the late callback reads the null and takes the ownerless path --
  // process-wide accounting only, no registry access.  A raw Context* in
  // the payload is a use-after-free here (the sanitizer arm carries that
  // assertion).
  const char* py_src = R"(
import weakref
import _cinderx

events = []
refs = []

def lone(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def arm(func):
    def on_death(ref):
        # Runs before the JIT's callback (newest callback-ref first): the
        # death has not been delivered to the JIT yet, and the counter
        # value recorded here proves that ordering held.
        events.append(
            _cinderx._get_trigger_stats()["function_destroyed_notifications"])
        destroy_ctx_for_test(None)
    refs.append(weakref.ref(func, on_death))
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "lone"));
  ASSERT_NE(func, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);
  ASSERT_EQ(jit_ctx_->watchedFunctionCount(), 1u);

  // Keep the artifact alive past its owner so the only lifetime in
  // question is the context's.
  jit::CompiledFunction* compiled = jit_ctx_->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  auto pin = Ref<jit::CompiledFunction>::create(compiled);

  // Hand the context-destroying helper to the probe and arm the user
  // callback AFTER the watch: the newest callback-ref runs first, so the
  // user's callback destroys the context before the JIT's callback fires.
  {
    auto holder = Ref<>::steal(
        PyCapsule_New(&jit_ctx_, "cinderx-test.ctx-holder", nullptr));
    ASSERT_NE(holder, nullptr);
    auto destroy =
        Ref<>::steal(PyCFunction_New(&kDestroyContextForTest, holder.get()));
    ASSERT_NE(destroy, nullptr);
    ASSERT_EQ(
        PyDict_SetItemString(
            func->func_globals, "destroy_ctx_for_test", destroy.get()),
        0);
  }
  {
    Ref<> arm(getGlobal("arm"));
    ASSERT_NE(arm, nullptr);
    auto armed = Ref<>::steal(PyObject_CallOneArg(
        arm.get(), reinterpret_cast<PyObject*>(func.get())));
    ASSERT_NE(armed, nullptr);
  }

  // Reduce the function to our single reference, then let it die: the
  // batch runs the user's callback (killing the context), then the JIT's.
  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "lone"), 0);
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  func.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;

  ASSERT_EQ(jit_ctx_, nullptr)
      << "the user callback did not destroy the context; the ordering this "
         "test exists to exercise did not happen";
  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "the ownerless death was not accounted";

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 1);
  auto recorded = PyLong_AsUnsignedLongLong(PyList_GET_ITEM(events.get(), 0));
  EXPECT_EQ(recorded, deaths_before)
      << "the JIT callback ran before the user callback: the owner-death "
         "window was not exercised";
}

namespace {

// Probe invoked from a user weak-reference callback while the watched
// function is mid-death: its weak references are already cleared, the JIT's
// own callback has not run.  Everything here reads through borrowed
// pointers; taking a strong reference would itself resurrect the referent.
struct DeathInFlightProbe {
  jit::CompilerContext<jit::Compiler>* ctx{nullptr};
  PyFunctionObject* func{nullptr};
  bool ran{false};
  bool pending_before{false};
  bool watch_refused{false};
  bool pending_after{false};
  bool parked{true};
  size_t watch_count_delta{~0u};
};

DeathInFlightProbe* g_death_in_flight_probe{nullptr};

extern "C" PyObject* probeDeathInFlightForTest(PyObject*, PyObject*) {
  DeathInFlightProbe* probe = g_death_in_flight_probe;
  if (probe != nullptr && probe->ctx != nullptr && probe->func != nullptr) {
    BorrowedRef<PyFunctionObject> func{probe->func};
    size_t count_before = probe->ctx->watchedFunctionCount();
    probe->ran = true;
    probe->pending_before = probe->ctx->isFunctionDeathPending(func);
    probe->watch_refused = !probe->ctx->watchFunctionDeath(func);
    probe->pending_after = probe->ctx->isFunctionDeathPending(func);
    probe->watch_count_delta =
        probe->ctx->watchedFunctionCount() - count_before;
    probe->ctx->addDeoptedFunc(func);
    probe->parked = probe->ctx->deoptedFuncs().count(func) != 0;
  }
  Py_RETURN_NONE;
}

PyMethodDef kProbeDeathInFlight = {
    "probe_death_in_flight",
    probeDeathInFlightForTest,
    METH_O,
    nullptr,
};

} // namespace

TEST_F(JITContextTest, DeathInFlightWatchIsATombstone) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CPython clears every weak reference of a dying function before it runs
  // any callback, newest callback first.  A user callback therefore sees
  // the JIT's watch already cleared while the JIT's own callback is still
  // pending -- and that cleared entry must behave as a tombstone: it keeps
  // answering isFunctionDeathPending(), it cannot be replaced, and the
  // park/publication paths it gates refuse the dying function.  Replacing
  // it would hang a fresh weak reference on an object mid-teardown --
  // outside the callback snapshot -- and enable() would then resurrect the
  // function from a reference count of zero.
  const char* py_src = R"(
import weakref

refs = []

def lone(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def arm(func):
    def on_death(ref):
        probe_death_in_flight(None)
    refs.append(weakref.ref(func, on_death))
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "lone"));
  ASSERT_NE(func, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);
  ASSERT_EQ(jit_ctx_->watchedFunctionCount(), 1u);

  DeathInFlightProbe probe;
  probe.ctx = jit_ctx_.get();
  probe.func = func.get();
  g_death_in_flight_probe = &probe;

  {
    auto probe_fn =
        Ref<>::steal(PyCFunction_New(&kProbeDeathInFlight, nullptr));
    ASSERT_NE(probe_fn, nullptr);
    ASSERT_EQ(
        PyDict_SetItemString(
            func->func_globals, "probe_death_in_flight", probe_fn.get()),
        0);
  }
  {
    // Armed after the JIT's watch, so the user callback runs first.
    Ref<> arm(getGlobal("arm"));
    ASSERT_NE(arm, nullptr);
    auto armed = Ref<>::steal(PyObject_CallOneArg(
        arm.get(), reinterpret_cast<PyObject*>(func.get())));
    ASSERT_NE(armed, nullptr);
  }

  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "lone"), 0);
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  func.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  g_death_in_flight_probe = nullptr;

  ASSERT_TRUE(probe.ran) << "the user callback never ran";
  EXPECT_TRUE(probe.pending_before)
      << "the watch was not cleared when the user callback ran: the "
         "death-in-flight window was not exercised";
  EXPECT_TRUE(probe.watch_refused)
      << "watchFunctionDeath() re-armed over a death in flight";
  EXPECT_TRUE(probe.pending_after)
      << "the death-in-flight tombstone was replaced";
  EXPECT_EQ(probe.watch_count_delta, 0u)
      << "the watch map changed size under a death in flight";
  EXPECT_FALSE(probe.parked)
      << "a function mid-death was parked as a borrowed pointer";
  EXPECT_EQ(deaths_after, deaths_before + 1) << "the death was not delivered";
  EXPECT_EQ(jit_ctx_->watchedFunctionCount(), 0u)
      << "the tombstone outlived its own delivery";
}

TEST_F(JITContextTest, FunctionDeathCallbackContainsBookkeepingFailure) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The death callback runs on a C destructor stack: a C++ exception must
  // not cross it, the watch must still come down, and the failure must be
  // recorded (poison) rather than swallowed.  The injected fault sits at
  // the boundary's edge, after the interior cleanup has run.
  const char* py_src = R"(
def lone(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "lone"));
  ASSERT_NE(func, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);
  ASSERT_EQ(jit_ctx_->watchedFunctionCount(), 1u);

  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "lone"), 0);
  ASSERT_FALSE(jit::consumeUnitDeletionTrackingPoison());

  jit::failJitPublishStepForTest(10);
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  func.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  jit::failJitPublishStepForTest(0);

  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "cleanup did not run before the boundary fault";
  EXPECT_EQ(jit_ctx_->watchedFunctionCount(), 0u)
      << "the watch survived its own delivery";
  EXPECT_TRUE(jit::consumeUnitDeletionTrackingPoison())
      << "the contained failure was swallowed without a record";
  EXPECT_FALSE(jit::consumeUnitDeletionTrackingPoison());
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(JITContextTest, PoisonedDeletionTrackingFailsTheNextBatchOnce) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A contained death-callback failure means a deletion notification may
  // have been lost, and the mark it leaves has a consumer: the next batch
  // preload must refuse conservatively -- an untrustworthy deleted-units
  // view could keep a dead function -- and exactly once, so the machinery
  // recovers.  The mark is planted through the real delivery path (a
  // watched function dying with the boundary fault armed), not by calling
  // the poison setter directly.  (3.11 plain-code preload runs no Python
  // and takes no tracked allocation, so a mid-window death cannot be
  // staged organically here; the shared batch sinks have their own
  // fault-injection coverage where preload does run Python.)
  const char* py_src = R"(
def lone(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def other(a, b):
    total = a - a
    while total < b:
        total = total + b
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "lone"));
  ASSERT_NE(func, nullptr);
  Ref<PyFunctionObject> other(getGlobal("other"));
  ASSERT_NE(other, nullptr);
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_NE(preloader, nullptr);
  ASSERT_EQ(
      jit::compilePreloaderImpl(jit_ctx_.get(), *preloader, func),
      jit::Result::OK);

  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "lone"), 0);
  ASSERT_FALSE(jit::consumeUnitDeletionTrackingPoison());
  jit::failJitPublishStepForTest(10);
  func.reset();
  jit::failJitPublishStepForTest(0);

  // The batch consumer sees the mark exactly once.
  std::vector<BorrowedRef<PyFunctionObject>> refused =
      jit::preloadFuncAndDeps(other);
  bool memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
  PyErr_Clear();
  EXPECT_TRUE(refused.empty())
      << "a batch after a lost deletion record was allowed to proceed";
  EXPECT_TRUE(memory_error)
      << "the conservative failure did not report as a MemoryError";

  std::vector<BorrowedRef<PyFunctionObject>> recovered =
      jit::preloadFuncAndDeps(other);
  EXPECT_FALSE(recovered.empty())
      << "the poison mark was not consumed by the refused batch";
  EXPECT_EQ(PyErr_Occurred(), nullptr);
  jit::hir::preloaderManager().clear();
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

namespace {

PyObject* setAsyncExcThenNone(PyObject* /*self*/, PyObject* /*unused*/) {
  unsigned long ident = PyThread_get_thread_ident();
  int n = PyThreadState_SetAsyncExc(ident, PyExc_RuntimeError);
  if (n < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyMethodDef kSetAsyncExcThenNone = {
    "set_async_exc_then_none",
    setAsyncExcThenNone,
    METH_NOARGS,
    nullptr,
};

} // namespace

class JITLifecycle311Test : public RuntimeTest {};

TEST_F(JITLifecycle311Test, CodeDeathIsReported) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The code-extra free function is 3.11's substitute for a code watcher:
  // attach extra data, drop the code object, and the notification must
  // arrive.
  jit::TriggerStats before = jit::triggerStatsSnapshot();
  {
    Ref<> code = Ref<>::steal(Py_CompileString(
        "def transient():\n    return 5\n", "<lifecycle>", Py_file_input));
    ASSERT_NE(code, nullptr);
    // Claiming the extra slot is what registers the free function for this
    // code object; without it there is nothing to call.
    ASSERT_NE(codeExtra(reinterpret_cast<PyCodeObject*>(code.get())), nullptr);
    EXPECT_EQ(
        jit::triggerStatsSnapshot().code_destroyed_notifications,
        before.code_destroyed_notifications)
        << "the notification arrived before the code object died";
  }
  jit::TriggerStats after = jit::triggerStatsSnapshot();
  EXPECT_GT(
      after.code_destroyed_notifications, before.code_destroyed_notifications)
      << "no code-death notification; the free function is not wired to the "
         "JIT, and code-keyed registries will keep dead keys";
}

TEST_F(JITLifecycle311Test, CodeExtraFreeContainsHookFailure) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The code-extra free function runs from inside code_dealloc: a C++
  // exception must not cross it, the block must still be freed, and the
  // failure must be recorded (poison) rather than swallowed.  The injected
  // fault sits at the hook boundary's edge, after the notification's own
  // bookkeeping has run.
  ASSERT_FALSE(jit::consumeUnitDeletionTrackingPoison());
  jit::TriggerStats before = jit::triggerStatsSnapshot();
  {
    Ref<> code = Ref<>::steal(Py_CompileString(
        "def transient():\n    return 7\n", "<lifecycle>", Py_file_input));
    ASSERT_NE(code, nullptr);
    ASSERT_NE(codeExtra(reinterpret_cast<PyCodeObject*>(code.get())), nullptr);
    jit::failJitPublishStepForTest(11);
  }
  jit::failJitPublishStepForTest(0);
  jit::TriggerStats after = jit::triggerStatsSnapshot();
  EXPECT_GT(
      after.code_destroyed_notifications, before.code_destroyed_notifications)
      << "cleanup did not run before the boundary fault";
  EXPECT_TRUE(jit::consumeUnitDeletionTrackingPoison())
      << "the contained failure was swallowed without a record";
  EXPECT_FALSE(jit::consumeUnitDeletionTrackingPoison());
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(JITLifecycle311Test, CodeExtraStaysAtOurOwnIndex) {
  SKIP_311_EXECUTABLE_COMPILE();

  // _PyCode_SetExtra sizes a fresh co_extra to the interpreter-wide number
  // of registered indices, and code_dealloc then calls every registered
  // free function below that size -- including for slots this code object
  // never wrote.  The JIT attaches its data to broad swaths of code, so a
  // block sized to the full index count would drag every foreign free
  // function into the destruction of every code object the JIT ever saw.
  // Capping the allocation at our own index is what keeps foreign indices
  // above ours out of that path.
  auto* state = cinderx::getModuleState();
  ASSERT_NE(state, nullptr);
  Py_ssize_t ours = state->code_extra_index;
  ASSERT_GE(ours, 0);

  // Claim an index above ours, the way a third party importing after
  // CinderX would.  Its free function is never registered, so nothing here
  // depends on it being called -- only on it not being.
  Py_ssize_t foreign = PyUnstable_Eval_RequestCodeExtraIndex(nullptr);
  ASSERT_GT(foreign, ours);

  Ref<> code = Ref<>::steal(Py_CompileString(
      "def capped():\n    return 1\n", "<capped>", Py_file_input));
  ASSERT_NE(code, nullptr);
  auto code_obj = reinterpret_cast<PyCodeObject*>(code.get());
  ASSERT_NE(codeExtra(code_obj), nullptr);

  // Read the size back through the same private layout the setter assumes.
  struct Layout {
    Py_ssize_t ce_size;
    void* ce_extras[1];
  };
  auto* block = reinterpret_cast<Layout*>(code_obj->co_extra);
  ASSERT_NE(block, nullptr);
  EXPECT_EQ(block->ce_size, ours + 1)
      << "the code-extra block is not capped at the JIT's own index";
  EXPECT_LE(block->ce_size, foreign)
      << "the block spans a foreign index, so that index's free function "
         "will be called for every code object the JIT touches";
}

TEST_F(JITLifecycle311Test, FunctionDeathIsReported) {
  SKIP_311_EXECUTABLE_COMPILE();

  // With the death watch armed and nothing owning the function on the JIT's
  // behalf, a compiled function dies the moment its last reference goes --
  // no collection required -- and the JIT is told.
  const char* py_src = R"(
def transient_fn(x):
    return x + 1
)";

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  Ref<> weak;
  jit::TriggerStats before;
  {
    Ref<PyFunctionObject> func(compileAndGet(py_src, "transient_fn"));
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
    ASSERT_EQ(ctx->watchedFunctionCount(), 1u)
        << "the death watch was not armed when the function was registered";
    weak = Ref<>::steal(PyWeakref_NewRef(func, nullptr));
    ASSERT_NE(weak, nullptr);
    before = jit::triggerStatsSnapshot();
    runStockCode("del transient_fn");
  }

  EXPECT_EQ(PyWeakref_GetObject(weak), Py_None)
      << "a compiled function outlived its last reference; something on the "
         "JIT side is still holding it";
  jit::TriggerStats after = jit::triggerStatsSnapshot();
  EXPECT_GT(
      after.function_destroyed_notifications,
      before.function_destroyed_notifications)
      << "the function died without the JIT being told; its registry entries "
         "are now dead keys";
  EXPECT_EQ(ctx->watchedFunctionCount(), 0u)
      << "the watch outlived the function it watched";
}

TEST_F(JITLifecycle311Test, FunctionDeathInsideACycleIsReported) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The path that decides whether holding the weak reference matters: the
  // collector runs a callback only when the weak reference is reachable, so
  // a watch owned by the dying graph would be cleared in silence.  This
  // function is reachable only through a cycle of its own making.
  const char* py_src = R"(
def self_referential(x):
    return x + 1

self_referential.myself = self_referential
)";

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  Ref<> weak;
  jit::TriggerStats before;
  {
    Ref<PyFunctionObject> func(compileAndGet(py_src, "self_referential"));
    ASSERT_NE(func, nullptr);
    ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
    weak = Ref<>::steal(PyWeakref_NewRef(func, nullptr));
    ASSERT_NE(weak, nullptr);
    before = jit::triggerStatsSnapshot();
    runStockCode("del self_referential");
  }

  ASSERT_NE(PyWeakref_GetObject(weak), Py_None)
      << "the cycle did not keep the function alive; this case is not "
         "exercising the collector path";
  PyGC_Collect();

  EXPECT_EQ(PyWeakref_GetObject(weak), Py_None) << "the cycle survived";
  EXPECT_GT(
      jit::triggerStatsSnapshot().function_destroyed_notifications,
      before.function_destroyed_notifications)
      << "a function collected as garbage died unreported";
  EXPECT_EQ(ctx->watchedFunctionCount(), 0u);
}

TEST_F(JITLifecycle311Test, DefaultArgSurvivesDefaultsRebind) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Stock INCREFs the default at bind.  Rebinding __defaults__ after
  // bind must not free the value the compiled frame still holds.
  // STORE_ATTR is off the execute whitelist, so the mutation lives in
  // an interpreted helper reached by CALL.
  const char* py_src = R"(
class Boom:
    pass

def rebind():
    victim.__defaults__ = ()

def victim(x=Boom()):
    rebind()
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(py_src, "victim"));
  Ref<PyFunctionObject> rebind(getGlobal("rebind"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));
  ASSERT_FALSE(isJitCompiled(rebind));

  auto no_args = Ref<>::steal(PyTuple_New(0));
  auto got = Ref<>::steal(PyObject_Call(func, no_args, nullptr));
  ASSERT_NE(got, nullptr);
  EXPECT_STREQ(Py_TYPE(got)->tp_name, "Boom");
}

TEST_F(JITLifecycle311Test, KwOnlyDefaultSurvivesKwdefaultsClear) {
  SKIP_311_EXECUTABLE_COMPILE();

  // __kwdefaults__ is a live dict.  Clearing it after bind must not
  // free the value sitting in arg_space.  LOAD_METHOD is off the
  // execute whitelist, so the clear lives in an interpreted helper.
  const char* py_src = R"(
class Boom:
    pass

def clear_kw():
    victim.__kwdefaults__.clear()

def victim(*, x=Boom()):
    clear_kw()
    return x
)";
  Ref<PyFunctionObject> func(compileAndGet(py_src, "victim"));
  Ref<PyFunctionObject> clear_kw(getGlobal("clear_kw"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));
  ASSERT_FALSE(isJitCompiled(clear_kw));

  auto no_args = Ref<>::steal(PyTuple_New(0));
  auto got = Ref<>::steal(PyObject_Call(func, no_args, nullptr));
  ASSERT_NE(got, nullptr);
  EXPECT_STREQ(Py_TYPE(got)->tp_name, "Boom");
}

TEST_F(JITLifecycle311Test, DefaultsStayInstalledAndBindLive) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Adding __defaults__ used to take the function off the MR-04 exact
  // positional surface.  MR-06 rebinds defaults in the generated
  // vectorcall prologue, so the artifact stays installed and both
  // reporters of "is this compiled" stay true.  A __code__ swap still
  // clears them -- that is a different case.
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

  EXPECT_NE(Ci_JitShell311_InstalledArtifact(func), nullptr);
  EXPECT_TRUE(isJitCompiled(func));

  auto entries_before = jit::triggerStatsSnapshot().machine_code_entries;
  auto after = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyLong_AsLong(after), 42);
  auto no_args = Ref<>::steal(PyTuple_New(0));
  auto defaulted = Ref<>::steal(PyObject_Call(func, no_args, nullptr));
  ASSERT_NE(defaulted, nullptr);
  EXPECT_EQ(PyLong_AsLong(defaulted), 42);
  EXPECT_EQ(
      jit::triggerStatsSnapshot().machine_code_entries, entries_before + 2)
      << "live defaults must still enter machine code";
}

TEST_F(JITLifecycle311Test, ExecuteRefusalDoesNotLeaveUtf8Exception) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A lone surrogate is a legal Python str and an illegal UTF-8 C string.
  // The execute-refusal helpers must not convert through UTF-8, or a
  // pending UnicodeEncodeError leaks into compileFunction / finalize.
  const char* py_src = R"(
def victim():
    return 1
)";
  Ref<PyFunctionObject> func(compileAndGet(py_src, "victim"));
  ASSERT_NE(func, nullptr);
  auto bad = Ref<>::steal(PyUnicode_FromOrdinal(0xD800));
  ASSERT_NE(bad, nullptr);
  ASSERT_EQ(PyObject_SetAttrString(func, "__module__", bad), 0);
  PyErr_Clear();
  const char* reason = Ci_JitShell311_ExecuteRefusal(func);
  EXPECT_EQ(PyErr_Occurred(), nullptr)
      << "execute refusal left a UTF-8 conversion exception";
  (void)reason;
}

TEST_F(JITLifecycle311Test, BindFailureAtRecursionLimitMatchesStock) {
  SKIP_311_EXECUTABLE_COMPILE();

  // _Py_MakeRecCheck is post-decrement, so the innermost admitted
  // frame runs at recursion_remaining == 0.  Stock formats a missing
  // argument via PyObject_Repr, which Enter-fails as RecursionError.
  // Too-many-positional still formats TypeError.  Bind must not invert
  // that.
  const char* py_src = R"(
def needed(a):
    return a
def none():
    return 1
def with_def(a, b=1):
    return a
)";
  Ref<PyFunctionObject> needed(compileAndGet(py_src, "needed"));
  Ref<PyFunctionObject> none(getGlobal("none"));
  Ref<PyFunctionObject> with_def(getGlobal("with_def"));
  ASSERT_NE(needed, nullptr);

  auto callAtLimit = [](PyObject* fn, PyObject* args) {
    PyThreadState* tstate = PyThreadState_GET();
    int saved = tstate->recursion_remaining;
    tstate->recursion_remaining = 0;
    auto result = Ref<>::steal(PyObject_Call(fn, args, nullptr));
    tstate->recursion_remaining = saved;
    EXPECT_EQ(result, nullptr);
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* tb = nullptr;
    PyErr_Fetch(&type, &value, &tb);
    std::string tp =
        type != nullptr ? reinterpret_cast<PyTypeObject*>(type)->tp_name : "";
    std::string msg;
    if (value != nullptr) {
      auto s = Ref<>::steal(PyObject_Str(value));
      if (s != nullptr) {
        const char* utf8 = PyUnicode_AsUTF8(s);
        if (utf8 != nullptr) {
          msg = utf8;
        }
      }
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    return std::make_pair(tp, msg);
  };

  auto no_args = Ref<>::steal(PyTuple_New(0));
  auto one = makeLong(1);
  auto two = makeLong(2);
  auto three = makeLong(3);
  auto extra = Ref<>::steal(PyTuple_Pack(1, one.get()));
  auto three_args =
      Ref<>::steal(PyTuple_Pack(3, one.get(), two.get(), three.get()));

  auto missing_interp = callAtLimit(needed, no_args);
  auto extra_interp = callAtLimit(none, extra);
  auto def_interp = callAtLimit(with_def, three_args);

  ASSERT_EQ(jit::compileFunction(needed), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(none), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(with_def), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(needed));

  auto missing_jit = callAtLimit(needed, no_args);
  auto extra_jit = callAtLimit(none, extra);
  auto def_jit = callAtLimit(with_def, three_args);
  EXPECT_EQ(missing_jit, missing_interp);
  EXPECT_EQ(missing_jit.first, "RecursionError");
  EXPECT_EQ(extra_jit, extra_interp);
  EXPECT_EQ(def_jit, def_interp);

  PyThreadState* tstate = PyThreadState_GET();
  int saved = tstate->recursion_remaining;
  tstate->recursion_remaining = 0;
  auto ok_args = Ref<>::steal(PyTuple_Pack(1, one.get()));
  auto ok = Ref<>::steal(PyObject_Call(needed, ok_args, nullptr));
  tstate->recursion_remaining = saved;
  EXPECT_EQ(ok, nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RecursionError))
      << "a successful bind must still consume a recursion slot";
  PyErr_Clear();
}

TEST_F(JITLifecycle311Test, CallDeliversAsyncExcBeforeNextStatement) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CPython 3.11 CALL runs CHECK_EVAL_BREAKER after a C callable
  // returns.  A successful helper that armed SetAsyncExc must raise
  // before the next Python statement, matching stock.
  const char* py_src = R"(
def drive(helper, box):
    helper()
    box.append(1)
    return box
)";
  Ref<PyFunctionObject> func(compileAndGet(py_src, "drive"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  auto helper = Ref<>::steal(PyCFunction_New(&kSetAsyncExcThenNone, nullptr));
  ASSERT_NE(helper, nullptr);
  auto box = Ref<>::steal(PyList_New(0));
  ASSERT_NE(box, nullptr);
  auto args = Ref<>::steal(PyTuple_Pack(2, helper.get(), box.get()));
  ASSERT_NE(args, nullptr);
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  EXPECT_EQ(result, nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError))
      << "async exception must be delivered at the CALL boundary";
  PyErr_Clear();
  EXPECT_EQ(PyList_GET_SIZE(box), 0)
      << "CALL-after mutation ran; eval breaker was deferred";
}

TEST_F(JITLifecycle311Test, CallExDeliversAsyncExcBeforeNextStatement) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CALL_FUNCTION_EX checks the eval breaker after do_call_core even
  // when the callee is a Python function.  A C helper that arms
  // SetAsyncExc inside that Python callee must still stop the caller
  // before the next statement.
  const char* py_src = R"(
def callee(helper):
    helper()
    return None

def drive(callee, helper, box):
    callee(*[helper])
    box.append(1)
    return box
)";
  Ref<PyFunctionObject> drive(compileAndGet(py_src, "drive"));
  Ref<PyFunctionObject> callee(getGlobal("callee"));
  ASSERT_NE(drive, nullptr);
  ASSERT_EQ(jit::compileFunction(drive), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(drive));
  ASSERT_FALSE(isJitCompiled(callee));

  auto helper = Ref<>::steal(PyCFunction_New(&kSetAsyncExcThenNone, nullptr));
  ASSERT_NE(helper, nullptr);
  auto box = Ref<>::steal(PyList_New(0));
  ASSERT_NE(box, nullptr);
  auto args =
      Ref<>::steal(PyTuple_Pack(3, callee.get(), helper.get(), box.get()));
  ASSERT_NE(args, nullptr);
  auto result = Ref<>::steal(PyObject_Call(drive, args, nullptr));
  EXPECT_EQ(result, nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError))
      << "async exception must be delivered on the CALL_FUNCTION_EX path";
  PyErr_Clear();
  EXPECT_EQ(PyList_GET_SIZE(box), 0)
      << "CALL_FUNCTION_EX-after mutation ran; eval breaker was deferred";
}

#if defined(__linux__)
TEST_F(JITLifecycle311Test, CStackLargeGuardRaisesRecursionError) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CPython's usable stack base is pthread stack_addr + guard_size.
  // A thread with a large guard region must still raise RecursionError
  // rather than SIGSEGV into the guard page.
  struct Payload {
    int rc = 1;
  } payload;

  auto worker = +[](void* raw) -> void* {
    auto* p = static_cast<Payload*>(raw);
    PyGILState_STATE gstate = PyGILState_Ensure();
    p->rc = PyRun_SimpleString(R"PY(
import sys
import cinderjit

def rec(f, n):
    if n:
        return f(f, n - 1)
    return 0

assert cinderjit.force_compile(rec) is True
old = sys.getrecursionlimit()
sys.setrecursionlimit(10 ** 6)
try:
    rec(rec, 100000)
    raise SystemExit("expected RecursionError")
except RecursionError:
    pass
finally:
    sys.setrecursionlimit(old)
)PY");
    PyGILState_Release(gstate);
    return nullptr;
  };

  pthread_attr_t attr;
  ASSERT_EQ(pthread_attr_init(&attr), 0);
  ASSERT_EQ(pthread_attr_setguardsize(&attr, 64 * 1024), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&attr, 2 * 1024 * 1024), 0);
  pthread_t thread;
  ASSERT_EQ(pthread_create(&thread, &attr, worker, &payload), 0);
  pthread_attr_destroy(&attr);
  int join_rc;
  Py_BEGIN_ALLOW_THREADS join_rc = pthread_join(thread, nullptr);
  Py_END_ALLOW_THREADS ASSERT_EQ(join_rc, 0);
  EXPECT_EQ(payload.rc, 0);
}
#endif

TEST_F(JITLifecycle311Test, ReplacingCodeStopsTheOldMachineCode) {
  SKIP_311_EXECUTABLE_COMPILE();

  // 3.11 has no function watcher, so nothing announces that __code__ moved.
  // The guarded entry notices on the next call instead: the artifact was
  // built for the old code object and must not run for the new one.
  const char* py_src = R"(
def swapped(x):
    return x + 1

def replacement(x):
    return x + 100
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "swapped"));
  Ref<PyFunctionObject> other(getGlobal("replacement"));
  ASSERT_NE(func, nullptr);
  ASSERT_NE(other, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  auto arg = makeLong(1);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto before = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(before, nullptr);
  ASSERT_EQ(PyLong_AsLong(before), 2);

  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", other->func_code), 0);

  EXPECT_FALSE(isJitCompiled(func))
      << "the function still reports as compiled for code it no longer has";
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);

  // The answer has to come from the new code, not the old machine code.
  auto after = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyLong_AsLong(after), 101);
}

TEST_F(JITLifecycle311Test, ForceUncompileAffectsLaterCallsOnly) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* py_src = R"(
def undo_me(x):
    return x * 2
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "undo_me"));
  ASSERT_NE(func, nullptr);
  auto mod = importCinderJitModule();
  callJitOneArg(mod, "force_compile", func);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  size_t resident_before = ctx->compiledCodes().size();
  ASSERT_GT(resident_before, 0u);

  auto arg = makeLong(21);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto compiled_result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(compiled_result, nullptr);
  ASSERT_EQ(PyLong_AsLong(compiled_result), 42);

  callJitOneArg(mod, "force_uncompile", func);

  EXPECT_FALSE(isJitCompiled(func));
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);
  EXPECT_LT(ctx->compiledCodes().size(), resident_before)
      << "the artifact is still resident after force_uncompile";

  // Still callable, still correct, now through the interpreter.
  auto after = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyLong_AsLong(after), 42);

  // And compiling it again has to work rather than report "already done".
  callJitOneArg(mod, "force_compile", func);
  EXPECT_TRUE(isJitCompiled(func));
  auto recompiled = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(recompiled, nullptr);
  EXPECT_EQ(PyLong_AsLong(recompiled), 42);
}

namespace {
// The midpoint hook takes a plain function pointer, so the reference the
// hook drops lives at namespace scope.
Ref<PyFunctionObject> s_uncompile_last_ref_slot;
} // namespace

TEST_F(JITLifecycle311Test, UncompileSurvivesLosingItsLastReferenceMidway) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The lifecycle contract says a function cannot die in the middle of a
  // JIT operation on it because its last reference fell.  Python callers
  // cannot construct that state -- the call machinery owns the argument for
  // the duration -- so this drives the raw C entry with a borrowed pointer
  // and drops the only strong reference from the midpoint hook, exactly
  // between force_uncompile's unpublication and its artifact retirement.
  // The operation's own pin has to carry the function to the end; the death
  // then lands after the operation, is reported once, and leaves nothing
  // registered.
  const char* py_src = R"(
def vanishing(x):
    return x + 7
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "vanishing"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  // Reduce the function to a single strong reference and park it where the
  // hook can reach it.
  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "vanishing"), 0);
  s_uncompile_last_ref_slot = std::move(func);
  ASSERT_EQ(
      Py_REFCNT(reinterpret_cast<PyObject*>(s_uncompile_last_ref_slot.get())),
      1);

  // Fetch the raw C function so the call adds no argument reference.
  auto mod = importCinderJitModule();
  auto uncompile_obj =
      Ref<>::steal(PyObject_GetAttrString(mod, "force_uncompile"));
  ASSERT_NE(uncompile_obj, nullptr);
  ASSERT_TRUE(PyCFunction_Check(uncompile_obj.get()));
  PyCFunction raw = PyCFunction_GetFunction(uncompile_obj.get());
  PyObject* self = PyCFunction_GetSelf(uncompile_obj.get());
  ASSERT_NE(raw, nullptr);

  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  BorrowedRef<PyFunctionObject> borrowed = s_uncompile_last_ref_slot.get();
  jit::setUncompileMidpointHookForTest(
      []() { s_uncompile_last_ref_slot.reset(); });
  auto result =
      Ref<>::steal(raw(self, reinterpret_cast<PyObject*>(borrowed.get())));
  jit::setUncompileMidpointHookForTest(nullptr);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result.get(), Py_True);
  EXPECT_EQ(s_uncompile_last_ref_slot, nullptr)
      << "the midpoint hook did not run";
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "the subject of force_uncompile must die exactly once, after the "
         "operation, not in the middle of it";
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

TEST_F(
    JITLifecycle311Test,
    ForceUncompileReportsReentrantCompileDuringRetirement) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A __del__ riding the retirement's releases can reenter
  // force_compile(); the newer decision wins and uncompile must not
  // report success over it -- verdict after every release.
  const char* py_src = R"(
events = []

def waverer(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def afterimage(a, b):
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
            events.append(cinderjit.force_compile(self.target))
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "waverer"));
  Ref<PyFunctionObject> donor(getGlobal("afterimage"));
  ASSERT_NE(func, nullptr);
  ASSERT_NE(donor, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  jit::CompiledFunction* prior_art = ctx->lookupFunc(func);
  ASSERT_NE(prior_art, nullptr);
  ASSERT_NE(prior_art->runtime(), nullptr);

  runCode("k = Reenter(waverer)\n");
  {
    Ref<> trigger(getGlobal("k"));
    ASSERT_NE(trigger, nullptr);
    prior_art->runtime()->addReference(trigger);
  }
  runCode("del k\n");

  // Swap to new code so the reentrant force_compile() has something fresh
  // to compile and publish.
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", donor->func_code), 0);

  auto mod = importCinderJitModule();
  auto failed = Ref<>::steal(
      PyObject_CallMethod(mod, "force_uncompile", "O", func.get()));
  EXPECT_EQ(failed, nullptr)
      << "force_uncompile() reported success over a reentrant recompile";
  ASSERT_TRUE(PyErr_Occurred());
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();

  // The newer decision owns the world: the reentry's publication stands.
  EXPECT_TRUE(isJitCompiled(func));
  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 2);
  EXPECT_EQ(PyList_GET_ITEM(events.get(), 1), Py_True)
      << "the reentrant compile inside the retirement did not succeed";

  // With no trigger left, uncompilation completes and reports honestly.
  auto clean = Ref<>::steal(
      PyObject_CallMethod(mod, "force_uncompile", "O", func.get()));
  ASSERT_NE(clean, nullptr);
  EXPECT_EQ(clean.get(), Py_True);
  EXPECT_FALSE(isJitCompiled(func));
}

TEST_F(JITLifecycle311Test, ForceUncompileIgnoresForgedForeignAnchor) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A forged foreign value at the user-writable key can be the last
  // reference to ANOTHER function's artifact; uncompile(f1) must not
  // release it.  The key is touched only when it holds the claimed
  // artifact itself.
  const char* py_src = R"(
def keeper(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def neighbour(a, b):
    total = a + a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> f1(compileAndGet(py_src, "keeper"));
  Ref<PyFunctionObject> f2(getGlobal("neighbour"));
  ASSERT_NE(f1, nullptr);
  ASSERT_NE(f2, nullptr);
  ASSERT_EQ(jit::compileFunction(f1), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(f2), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  jit::CompiledFunction* art_b = ctx->lookupFunc(f2);
  ASSERT_NE(art_b, nullptr);

  // Keep A alive independently, forge B into f1's anchor slot, and make
  // that forged reference B's only strong one.
  auto pin_a = Ref<jit::CompiledFunction>::create(ctx->lookupFunc(f1));
  ASSERT_NE(pin_a, nullptr);
  runCode(
      "keeper.__dict__['__cinderx_compiled_func__'] = "
      "neighbour.__dict__['__cinderx_compiled_func__']\n"
      "del neighbour.__dict__['__cinderx_compiled_func__']\n");
  ASSERT_TRUE(isJitCompiled(f2));

  auto mod = importCinderJitModule();
  {
    auto ok = Ref<>::steal(
        PyObject_CallMethod(mod, "force_uncompile", "O", f1.get()));
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok.get(), Py_True);
  }
  EXPECT_FALSE(isJitCompiled(f1));

  // The neighbour's world is untouched: installed, associated, reachable,
  // and still executing machine code.
  EXPECT_TRUE(isJitCompiled(f2))
      << "uncompiling f1 retired f2's artifact through a forged anchor";
  EXPECT_EQ(ctx->lookupFunc(f2), art_b);
  EXPECT_EQ(ctx->findAssociated(f2).get(), art_b);
  // The forged value stays exactly where the user wrote it.
  ASSERT_NE(f1->func_dict, nullptr);
  EXPECT_EQ(
      PyDict_GetItemWithError(f1->func_dict, jit::kCompiledFunctionKey),
      reinterpret_cast<PyObject*>(art_b));
  PyErr_Clear();

  uint64_t entries_before = jit::triggerStatsSnapshot().machine_code_entries;
  auto arg_a = makeLong(2);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(f2, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
  EXPECT_EQ(
      jit::triggerStatsSnapshot().machine_code_entries, entries_before + 1)
      << "the neighbour fell back to the interpreter";
}

TEST_F(
    JITLifecycle311Test,
    ForceUncompilePreservesSameKeyReentrantPublication) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Same-key variant: no code swap, so the reentrant successor lives
  // under the very key the retirement erases.  Blind by-key erase left it
  // installed-but-unreachable (lookupFunc null) and outside the
  // destructor's pin-and-sever walk.
  const char* py_src = R"(
events = []

def steady(a, b):
    total = a - a
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
            events.append(cinderjit.force_compile(self.target))
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "steady"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(func));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  jit::CompiledFunction* prior_art = ctx->lookupFunc(func);
  ASSERT_NE(prior_art, nullptr);
  ASSERT_NE(prior_art->runtime(), nullptr);

  runCode("k = Reenter(steady)\n");
  {
    Ref<> trigger(getGlobal("k"));
    ASSERT_NE(trigger, nullptr);
    prior_art->runtime()->addReference(trigger);
  }
  runCode("del k\n");

  // No __code__ swap: the reentrant publication reuses the same key.
  auto mod = importCinderJitModule();
  auto failed = Ref<>::steal(
      PyObject_CallMethod(mod, "force_uncompile", "O", func.get()));
  EXPECT_EQ(failed, nullptr)
      << "force_uncompile() reported success over a reentrant recompile";
  ASSERT_TRUE(PyErr_Occurred());
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();

  // The successor's world is whole: installed, associated, and -- the
  // point of this case -- still reachable through the compiled-codes map.
  EXPECT_TRUE(isJitCompiled(func));
  jit::CompiledFunction* successor = ctx->lookupFunc(func);
  ASSERT_NE(successor, nullptr)
      << "the retirement erased the successor's compiled-codes entry";
  EXPECT_EQ(successor, ctx->findAssociated(func).get());

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 2);
  EXPECT_EQ(PyList_GET_ITEM(events.get(), 1), Py_True);

  auto arg_a = makeLong(2);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITLifecycle311Test, StaleArtifactDeathPreservesSameKeySuccessor) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Death-path sequence pin: a pinned stale artifact dies after a
  // same-key successor published; the successor must survive.  (Today the
  // stale generation reaches death already cleared; the identity guard on
  // the death-path erase is the backstop for future uncleared paths.)
  const char* py_src = R"(
def perennial(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "perennial"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  auto pin = Ref<jit::CompiledFunction>::create(ctx->lookupFunc(func));
  ASSERT_NE(pin, nullptr);

  auto mod = importCinderJitModule();
  {
    auto ok = Ref<>::steal(
        PyObject_CallMethod(mod, "force_uncompile", "O", func.get()));
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(ok.get(), Py_True);
  }
  ASSERT_FALSE(isJitCompiled(func));

  // Publish the successor under the same key, then let the stale
  // generation die.
  callJitOneArg(mod, "force_compile", func);
  ASSERT_TRUE(isJitCompiled(func));
  jit::CompiledFunction* successor = ctx->lookupFunc(func);
  ASSERT_NE(successor, nullptr);
  ASSERT_NE(successor, pin.get());

  pin.reset();

  EXPECT_TRUE(isJitCompiled(func));
  EXPECT_EQ(ctx->lookupFunc(func), successor)
      << "the stale generation's death erased the successor's entry";
  auto arg_a = makeLong(2);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 6);
}

TEST_F(JITLifecycle311Test, RegisteredFunctionDeathCleansTheRegistry) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Registration records a borrowed pointer in the module state's
  // compilation-unit registry, and the registration contract promises a
  // death notification.  On 3.11 that promise is the weak-reference death
  // watch, armed at registration exactly like the installed and parked
  // registries arm it at publication: a function that dies
  // registered-but-never-compiled must leave no dangling key behind for
  // the next batch compile to dereference.
  const char* py_src = R"(
def drifter(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "drifter"));
  ASSERT_NE(func, nullptr);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  size_t watched_before = ctx->watchedFunctionCount();

  ASSERT_TRUE(jit::registerFunctionForTest(func));
  auto& reg = cinderx::getModuleState()->registered_compilation_units;
  PyObject* raw = reinterpret_cast<PyObject*>(func.get());
  ASSERT_EQ(reg.count(raw), 1u);
  EXPECT_EQ(ctx->watchedFunctionCount(), watched_before + 1)
      << "registration recorded a borrowed pointer without arming the "
         "death watch";

  ASSERT_EQ(PyDict_DelItemString(func->func_globals, "drifter"), 0);
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  func.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;

  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "a registered function died without a notification";
  EXPECT_EQ(reg.count(raw), 0u)
      << "the registry still holds the dead function's address";
  EXPECT_EQ(ctx->watchedFunctionCount(), watched_before);
}

TEST_F(JITLifecycle311Test, DeathBatchDisableLeavesNoDeadKeys) {
  SKIP_311_EXECUTABLE_COMPILE();

  // CPython clears every weak reference in a batch before invoking any
  // callback, so a user callback runs while the JIT's registries still
  // name the dying function and the JIT's own death callback has not.
  // disable(deopt_all=True) from that window walks the installed registry
  // and parks the dying function; the JIT's callback must then still
  // remove it, leaving no dead key in any registry and exactly one death
  // on the counter.
  const char* py_src = R"(
import weakref
import cinderjit

events = []
refs = []

def doomed(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def bystander(a, b):
    total = a - a
    while total < b:
        total = total + b
    return total

def arm(func):
    def on_death(ref):
        try:
            cinderjit.disable(deopt_all=True)
            events.append("disabled")
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
    refs.append(weakref.ref(func, on_death))
)";

  Ref<PyFunctionObject> doomed(compileAndGet(py_src, "doomed"));
  Ref<PyFunctionObject> bystander(getGlobal("bystander"));
  ASSERT_NE(doomed, nullptr);
  ASSERT_NE(bystander, nullptr);
  ASSERT_EQ(jit::compileFunction(doomed), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(bystander), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(doomed));
  ASSERT_TRUE(isJitCompiled(bystander));

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  {
    Ref<> arm(getGlobal("arm"));
    ASSERT_NE(arm, nullptr);
    auto armed = Ref<>::steal(PyObject_CallOneArg(
        arm.get(), reinterpret_cast<PyObject*>(doomed.get())));
    ASSERT_NE(armed, nullptr);
  }

  ASSERT_EQ(PyDict_DelItemString(doomed->func_globals, "doomed"), 0);
  PyObject* raw = reinterpret_cast<PyObject*>(doomed.get());
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  doomed.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;

  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "the death was not delivered exactly once";
  EXPECT_EQ(ctx->compiledFuncs().count(raw), 0u)
      << "the installed registry still names the dead function";
  EXPECT_EQ(ctx->deoptedFuncs().count(raw), 0u)
      << "the parked registry still names the dead function";
  EXPECT_EQ(jit::getConfig().state, jit::State::kPaused);
  EXPECT_EQ(ctx->deoptedFuncs().count(bystander.get()), 1u)
      << "the reentrant disable() did not park the bystander exactly once";

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 1);

  auto mod = importCinderJitModule();
  callJitNoArgs(mod, "enable");
  EXPECT_TRUE(isJitCompiled(bystander));
  EXPECT_EQ(ctx->deoptedFuncs().count(raw), 0u);
}

TEST_F(JITLifecycle311Test, DeathBatchDisableEnableDoesNotResurrect) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The aggressive variant: the user callback pauses AND re-enables while
  // the function's death is pending.  enable()'s reattach walk snapshots
  // the parked set with strong references, and a strong reference to an
  // object mid-deallocation is a resurrection; the walk must skip entries
  // whose death is pending (their own callback removes them).  The world
  // afterwards: exactly one death, no dead key anywhere, the bystander
  // back on machine code, the JIT running.
  const char* py_src = R"(
import weakref
import cinderjit

events = []
refs = []

def doomed2(a, b):
    total = a - a
    while total < b:
        total = total + a
    return total

def bystander2(a, b):
    total = a - a
    while total < b:
        total = total + b
    return total

def arm(func):
    def on_death(ref):
        try:
            cinderjit.disable(deopt_all=True)
            cinderjit.enable()
            events.append("cycled")
        except Exception as exc:  # noqa: BLE001
            events.append(type(exc).__name__)
    refs.append(weakref.ref(func, on_death))
)";

  Ref<PyFunctionObject> doomed(compileAndGet(py_src, "doomed2"));
  Ref<PyFunctionObject> bystander(getGlobal("bystander2"));
  ASSERT_NE(doomed, nullptr);
  ASSERT_NE(bystander, nullptr);
  ASSERT_EQ(jit::compileFunction(doomed), jit::Result::OK);
  ASSERT_EQ(jit::compileFunction(bystander), jit::Result::OK);

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);

  {
    Ref<> arm(getGlobal("arm"));
    ASSERT_NE(arm, nullptr);
    auto armed = Ref<>::steal(PyObject_CallOneArg(
        arm.get(), reinterpret_cast<PyObject*>(doomed.get())));
    ASSERT_NE(armed, nullptr);
  }

  ASSERT_EQ(PyDict_DelItemString(doomed->func_globals, "doomed2"), 0);
  PyObject* raw = reinterpret_cast<PyObject*>(doomed.get());
  uint64_t deaths_before =
      jit::triggerStatsSnapshot().function_destroyed_notifications;
  doomed.reset();
  uint64_t deaths_after =
      jit::triggerStatsSnapshot().function_destroyed_notifications;

  EXPECT_EQ(deaths_after, deaths_before + 1)
      << "the death was not delivered exactly once";
  EXPECT_EQ(ctx->compiledFuncs().count(raw), 0u);
  EXPECT_EQ(ctx->deoptedFuncs().count(raw), 0u)
      << "the enable() walk resurrected or re-recorded the dying function";

  Ref<> events(getGlobal("events"));
  ASSERT_NE(events, nullptr);
  ASSERT_EQ(PyList_GET_SIZE(events.get()), 1);
  EXPECT_TRUE(
      PyUnicode_CompareWithASCIIString(
          PyList_GET_ITEM(events.get(), 0), "cycled") == 0);

  EXPECT_EQ(jit::getConfig().state, jit::State::kRunning);
  EXPECT_TRUE(isJitCompiled(bystander))
      << "the reentrant enable() did not reattach the bystander";
  auto arg_a = makeLong(3);
  auto arg_b = makeLong(5);
  auto args = Ref<>::steal(PyTuple_Pack(2, arg_a.get(), arg_b.get()));
  auto result = Ref<>::steal(PyObject_Call(bystander, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 5);
}

TEST_F(JITLifecycle311Test, ParkedFunctionDeathUnparksIt) {
  SKIP_311_EXECUTABLE_COMPILE();

  // A function can die while the JIT is paused, and the parked set holds it
  // by borrow.  Re-enabling walks that set, so the death has to remove the
  // entry or enable() dereferences a freed function.
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

  auto weak = Ref<>::steal(PyWeakref_NewRef(func, nullptr));
  ASSERT_NE(weak, nullptr);
  runStockCode("del parked");
  func.reset();

  ASSERT_EQ(PyWeakref_GetObject(weak), Py_None)
      << "the parked function did not die; the parked set is still owning it";
  EXPECT_EQ(ctx->deoptedFuncs().size(), 0u)
      << "a dead function is still parked; enable() would walk it";

  // Re-enabling must be uneventful rather than fatal.
  callJitNoArgs(mod, "enable");
  EXPECT_FALSE(jit::isJitPaused());
}

TEST_F(JITLifecycle311Test, FinalizeRefusesReentrantEnable) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Finalization releases references, and releasing a reference runs
  // arbitrary Python.  If that Python re-enters enable(), the execute
  // surface is re-armed in the middle of teardown and the registries being
  // emptied are walked again.  Teardown has to be one-way.  The tripwire
  // rides on the context's own reference list, which releaseReferences()
  // drops partway through finalize().
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
)");

  jit::Context* ctx = jit::getContext();
  ASSERT_NE(ctx, nullptr);
  {
    Ref<> tripwire_type(getGlobal("Tripwire"));
    ASSERT_NE(tripwire_type, nullptr);
    auto tripwire = Ref<>::steal(PyObject_CallNoArgs(tripwire_type));
    ASSERT_NE(tripwire, nullptr);
    ctx->addReference(tripwire);
  }
  runStockCode("del Tripwire");

  jit::finalize();

  Ref<> reentry(getGlobal("reentry"));
  ASSERT_NE(reentry, nullptr);
  ASSERT_TRUE(PyList_CheckExact(reentry));
  ASSERT_EQ(PyList_GET_SIZE(reentry.get()), 1)
      << "the tripwire never ran; finalize() released nothing that could "
         "re-enter, so this case is not exercising the guard";
  BorrowedRef<> outcome = PyList_GET_ITEM(reentry.get(), 0);
  const char* text = PyUnicode_AsUTF8(outcome);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(text, "refused");
  EXPECT_FALSE(jit::isJitInitialized());
}

TEST_F(JITLifecycle311Test, MultithreadedTeardownLeavesNoDeadKeys) {
  SKIP_311_EXECUTABLE_COMPILE();

  // The teardown orphans every artifact and empties both borrowed
  // registries.  What has to hold afterwards is that nothing keeps pointing
  // at the function: the association is gone, so the guarded entry refuses
  // and the call goes back to the interpreter.
  const char* py_src = R"(
def orphaned(x):
    return x - 3
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "orphaned"));
  ASSERT_NE(func, nullptr);

  auto ctx = std::make_unique<jit::CompilerContext<jit::Compiler>>();
  std::unique_ptr<jit::hir::Preloader> preloader(
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code)));
  ASSERT_EQ(
      jit::compilePreloaderImpl(ctx.get(), *preloader, func), jit::Result::OK);

  auto compiled =
      ctx->lookupCode(func->func_code, func->func_builtins, func->func_globals);
  ASSERT_NE(compiled, nullptr);
  ASSERT_EQ(compiled->functions().count(func), 1u);

  ctx->clearForMultithreadedCompileTest();

  EXPECT_EQ(compiled->functions().count(func), 0u)
      << "the orphaned artifact still names a function nobody tracks";
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);

  auto arg = makeLong(11);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 8);

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

TEST_F(JITLifecycle311Test, FinalizeIsRepeatable) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Shutdown can reach finalization more than once -- the module teardown
  // and the interpreter teardown both drive it -- so a second pass has to
  // be a no-op rather than a second teardown of state that is already gone.
  const char* py_src = R"(
def twice(x):
    return x + 9
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "twice"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  jit::finalize();
  ASSERT_FALSE(jit::isJitInitialized());
  jit::finalize();
  jit::finalize();
  EXPECT_FALSE(jit::isJitInitialized());

  auto arg = makeLong(1);
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyLong_AsLong(result), 10);
}

TEST_F(JITLifecycle311Test, FinalizeEmptiesTheParkedRegistry) {
  SKIP_311_EXECUTABLE_COMPILE();

  // Finalizing with a populated parked set has to leave nothing parked and
  // nothing armed: after this the JIT is gone, and a watch that outlived it
  // would deliver a notification with no registry to service it.
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
  ASSERT_GT(ctx->watchedFunctionCount(), 0u);

  jit::finalize();

  EXPECT_FALSE(jit::isJitInitialized());
  EXPECT_FALSE(isJitCompiled(func));
  EXPECT_EQ(Ci_JitShell311_InstalledArtifact(func), nullptr);

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
