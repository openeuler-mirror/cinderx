// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class PreloaderErrorPropagationTest : public RuntimeTest {
 public:
  PreloaderErrorPropagationTest()
      : RuntimeTest(static_cast<Flags>(kJit | kStaticCompiler)) {}

  Ref<> makeBadLocalIndex() {
    return Ref<>::steal(PyLong_FromUnsignedLongLong(
        std::numeric_limits<unsigned long long>::max()));
  }

  Ref<> makeDeletingBadLocalIndex() {
    runStockCode(R"(
def callback_victim():
    return None

class DeleteVictimBadLocal(int):
    def __repr__(self):
        global callback_victim
        if "callback_victim" in globals():
            del callback_victim
        return "<bad local>"

deleting_bad_local = DeleteVictimBadLocal(1 << 100)
)");
    return getGlobal("deleting_bad_local");
  }
};

std::optional<int> constIndexForOpcode(
    BorrowedRef<PyCodeObject> code,
    int opcode) {
  for (const auto& instr : jit::BytecodeInstructionBlock{code}) {
    if (instr.opcode() == opcode) {
      return instr.oparg();
    }
  }
  return std::nullopt;
}

Ref<> cloneTuple(BorrowedRef<> tuple) {
  if (!PyTuple_Check(tuple)) {
    throw std::runtime_error{"expected a tuple descriptor"};
  }

  Py_ssize_t size = PyTuple_GET_SIZE(tuple.get());
  auto clone = Ref<>::steal(PyTuple_New(size));
  if (clone == nullptr) {
    throw std::runtime_error{"failed to clone tuple descriptor"};
  }

  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* item = PyTuple_GET_ITEM(tuple.get(), i);
    Py_INCREF(item);
    PyTuple_SET_ITEM(clone.get(), i, item);
  }
  return clone;
}

void replaceTupleItem(
    BorrowedRef<> tuple,
    Py_ssize_t index,
    BorrowedRef<> replacement) {
  if (!PyTuple_Check(tuple) || index < 0 ||
      index >= PyTuple_GET_SIZE(tuple.get())) {
    throw std::runtime_error{"tuple replacement index is out of range"};
  }

  PyObject* old_item = PyTuple_GET_ITEM(tuple.get(), index);
  Py_INCREF(replacement.get());
  PyTuple_SET_ITEM(tuple.get(), index, replacement.get());
  Py_DECREF(old_item);
}

// Publish a fresh co_consts tuple after the Preloader has populated its
// identity-keyed maps, then restore the original tuple before the Preloader is
// destroyed. The guard owns the original tuple while it is detached, keeping
// the Preloader's borrowed map keys alive on every exit path.
class ScopedCodeConstsReplacement {
 public:
  ScopedCodeConstsReplacement(BorrowedRef<PyCodeObject> code, int const_index)
      : code_(code.get()), original_consts_(Ref<>::create(code->co_consts)) {
    Py_ssize_t size = PyTuple_GET_SIZE(code->co_consts);
    if (const_index < 0 || const_index >= size) {
      throw std::runtime_error{"constant index is out of range"};
    }

    auto replacement_consts = Ref<>::steal(PyTuple_New(size));
    if (replacement_consts == nullptr) {
      throw std::runtime_error{"failed to clone code constants"};
    }

    for (Py_ssize_t i = 0; i < size; i++) {
      PyObject* item = PyTuple_GET_ITEM(code->co_consts, i);
      if (i == const_index) {
        item = cloneTuple(item).release();
      } else {
        Py_INCREF(item);
      }
      PyTuple_SET_ITEM(replacement_consts.get(), i, item);
    }

    Py_SETREF(code_->co_consts, replacement_consts.release());
  }

  ~ScopedCodeConstsReplacement() {
    Py_SETREF(code_->co_consts, original_consts_.release());
  }

  ScopedCodeConstsReplacement(const ScopedCodeConstsReplacement&) = delete;
  ScopedCodeConstsReplacement& operator=(const ScopedCodeConstsReplacement&) =
      delete;

 private:
  PyCodeObject* code_;
  Ref<> original_consts_;
};

// Replace one Static Python argument-check item with a test-controlled value.
// Static Python prohibits assigning a replacement __code__, so the test must
// publish a fully-cloned metadata tree directly from C++.
class ScopedStaticArgChecksReplacement {
 public:
  ScopedStaticArgChecksReplacement(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<> replacement,
      Py_ssize_t check_index = 0)
      : code_(code.get()), original_consts_(Ref<>::create(code->co_consts)) {
    Py_ssize_t consts_size = PyTuple_GET_SIZE(code->co_consts);
    if (consts_size == 0) {
      throw std::runtime_error{"Static Python code has no constants"};
    }

    Ref<> replacement_consts = cloneTuple(code->co_consts);
    Ref<> static_type_info =
        cloneTuple(PyTuple_GET_ITEM(code->co_consts, consts_size - 1));
    if (PyTuple_GET_SIZE(static_type_info.get()) != 2) {
      throw std::runtime_error{"unexpected Static Python type metadata"};
    }

    Ref<> checks = cloneTuple(PyTuple_GET_ITEM(static_type_info.get(), 0));
    if (PyTuple_GET_SIZE(checks.get()) < 2) {
      throw std::runtime_error{"Static Python function has no argument checks"};
    }

    replaceTupleItem(checks, check_index, replacement);
    replaceTupleItem(static_type_info, 0, checks);
    replaceTupleItem(replacement_consts, consts_size - 1, static_type_info);
    Py_SETREF(code_->co_consts, replacement_consts.release());
  }

  ~ScopedStaticArgChecksReplacement() {
    Py_SETREF(code_->co_consts, original_consts_.release());
  }

  ScopedStaticArgChecksReplacement(const ScopedStaticArgChecksReplacement&) =
      delete;
  ScopedStaticArgChecksReplacement& operator=(
      const ScopedStaticArgChecksReplacement&) = delete;

 private:
  PyCodeObject* code_;
  Ref<> original_consts_;
};

struct CountingDeletionCallback {
  int* calls;

  void operator()(BorrowedRef<>) const {
    (*calls)++;
  }
};

class ScopedModuleDeletionCallback {
 public:
  explicit ScopedModuleDeletionCallback(int* calls)
      : state_(cinderx::getModuleState()),
        previous_(state_->unit_deleted_during_preload),
        replacement_(CountingDeletionCallback{calls}),
        calls_(calls) {
    state_->unit_deleted_during_preload = replacement_;
  }

  ~ScopedModuleDeletionCallback() {
    state_->unit_deleted_during_preload = std::move(previous_);
  }

  bool isReplacementInstalled() const {
    const auto* callback =
        state_->unit_deleted_during_preload.target<CountingDeletionCallback>();
    return callback != nullptr && callback->calls == calls_;
  }

  // Old code can leave a callback that captures an unwound stack frame. Put
  // the sentinel back before RED-test cleanup destroys a function or code
  // object, so the regression test itself never dereferences the stale lambda.
  void ensureReplacementInstalled() {
    if (!isReplacementInstalled()) {
      state_->unit_deleted_during_preload = replacement_;
    }
  }

  ScopedModuleDeletionCallback(const ScopedModuleDeletionCallback&) = delete;
  ScopedModuleDeletionCallback& operator=(const ScopedModuleDeletionCallback&) =
      delete;

 private:
  cinderx::ModuleState* state_;
  std::function<void(BorrowedRef<>)> previous_;
  std::function<void(BorrowedRef<>)> replacement_;
  int* calls_;
};

class ScopedPreloadTestCleanup {
 public:
  ~ScopedPreloadTestCleanup() {
    jit::hir::preloaderManager().clear();
    cinderx::getModuleState()->registered_compilation_units.clear();
    PyErr_Clear();
  }
};

Ref<> importCinderJitModule() {
  return Ref<>::steal(PyImport_ImportModule("cinderx.jit"));
}

Ref<> callJitOneArg(BorrowedRef<> module, const char* name, BorrowedRef<> arg) {
  return Ref<>::steal(PyObject_CallMethod(module.get(), name, "O", arg.get()));
}

Ref<> callPrecompileAll(BorrowedRef<> module) {
  return Ref<>::steal(
      PyObject_CallMethod(module.get(), "precompile_all", "i", 1));
}

std::string takeRaisedExceptionMessage() {
  Ref<> raised = Ref<>::steal(PyErr_GetRaisedException());
  if (raised == nullptr) {
    return "<no exception raised>";
  }
  Ref<> str = Ref<>::steal(PyObject_Str(raised));
  if (str == nullptr) {
    PyErr_Clear();
    return "<failed to stringify exception>";
  }
  const char* message = PyUnicode_AsUTF8(str);
  if (message == nullptr) {
    PyErr_Clear();
    return "<failed to decode exception message>";
  }
  return message;
}

void expectContextualBuilderError(
    jit::hir::Preloader& preloader,
    std::string_view expected_error) {
  try {
    auto hir = jit::hir::buildHIR(preloader);
    FAIL() << "expected HIR builder failure, built " << hir->fullname;
  } catch (const std::runtime_error& error) {
    std::string message{error.what()};
    EXPECT_NE(message.find(expected_error), std::string::npos) << message;
    EXPECT_NE(message.find("jittestmodule:test"), std::string::npos) << message;
    EXPECT_NE(message.find("at offset"), std::string::npos) << message;
  }
}

TEST_F(
    PreloaderErrorPropagationTest,
    MissingPreloadedTypeThrowsContextualBuilderException) {
  const char* source = R"(
def test(value) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  auto preloader =
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code));
  ASSERT_NE(preloader, nullptr);

  auto const_index = constIndexForOpcode(func->func_code, CAST);
  ASSERT_TRUE(const_index.has_value());
  ScopedCodeConstsReplacement replacement(func->func_code, *const_index);

  expectContextualBuilderError(
      *preloader, "CAST: Can't find type for type descr");
}

TEST_F(
    PreloaderErrorPropagationTest,
    MissingPreloadedFieldThrowsContextualBuilderException) {
  const char* source = R"(
class C:
    value: int

def test(instance: C):
    return instance.value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  auto preloader =
      jit::hir::Preloader::make(func, jit::makeFrameReifier(func->func_code));
  ASSERT_NE(preloader, nullptr);

  auto const_index = constIndexForOpcode(func->func_code, LOAD_FIELD);
  ASSERT_TRUE(const_index.has_value());
  ScopedCodeConstsReplacement replacement(func->func_code, *const_index);

  expectContextualBuilderError(
      *preloader, "LOAD_FIELD: Can't find field for descr");
}

TEST_F(PreloaderErrorPropagationTest, UnknownStaticArgTypeThrowsDuringPreload) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  ScopedPreloadTestCleanup cleanup;
  Ref<> invalid_descr = Ref<>::create(Py_None);
  ScopedStaticArgChecksReplacement replacement{
      func->func_code, invalid_descr, 1};

  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  try {
    (void)jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code));
  } catch (const std::runtime_error& exn) {
    cpp_exception_escaped = true;
    cpp_exception_message = exn.what();
  }

  EXPECT_TRUE(cpp_exception_escaped);
  EXPECT_NE(cpp_exception_message.find("Unknown type descr"), std::string::npos)
      << cpp_exception_message;
}

TEST_F(
    PreloaderErrorPropagationTest,
    FailedPrimitiveTargetArgTypeLoadThrowsDuringPreload) {
  const char* source = R"(
from __static__ import int64

def target(value: int64) -> int64:
    return value

def test(value: int64) -> int64:
    return target(value)
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<PyFunctionObject> target(getGlobal("target"));
  ASSERT_NE(target, nullptr);
  ScopedPreloadTestCleanup cleanup;
  Ref<> invalid_descr = Ref<>::create(Py_None);
  ScopedStaticArgChecksReplacement replacement{
      target->func_code, invalid_descr, 1};

  try {
    (void)jit::hir::Preloader::make(
        func, jit::makeFrameReifier(func->func_code));
    FAIL() << "expected primitive argument type loading to fail";
  } catch (const std::runtime_error& exn) {
    std::string message{exn.what()};
    EXPECT_NE(
        message.find(
            "Failed to load primitive argument type information for function"),
        std::string::npos)
        << message;
    EXPECT_NE(message.find("jittestmodule:target"), std::string::npos)
        << message;
  }
}

TEST_F(
    PreloaderErrorPropagationTest,
    RestoresUnitDeletionCallbackAfterSuccessfulPreload) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  std::vector<BorrowedRef<PyFunctionObject>> targets =
      jit::preloadFuncAndDeps(func);

  EXPECT_FALSE(targets.empty());
  EXPECT_TRUE(callback.isReplacementInstalled());

  // Destroy the preloader and the compilation unit while the restored sentinel
  // remains published. Under ASAN this also exercises the lifetime edge that
  // previously retained a stack-capturing lambda.
  int calls_before_destroy = deletion_calls;
  callback.ensureReplacementInstalled();
  jit::hir::preloaderManager().clear();
  runStockCode("del test");
  func.reset();
  EXPECT_GT(deletion_calls, calls_before_destroy);
}

TEST_F(
    PreloaderErrorPropagationTest,
    RestoresUnitDeletionCallbackAfterPreloadException) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<> bad_local = makeDeletingBadLocalIndex();
  ASSERT_NE(bad_local, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  {
    ScopedStaticArgChecksReplacement replacement{func->func_code, bad_local};
    // The deletion this test stages lands mid-preload, inside a death
    // callback where nothing may throw.  Arming the deleted-units record
    // fault here pins the containment: the record-taking failure must not
    // escape the callback, must not disturb the preload's own error, and
    // must not stop the chained ambient callback from seeing the event.
    jit::failJitPublishStepForTest(9);
    try {
      (void)jit::preloadFuncAndDeps(func);
    } catch (const std::runtime_error& exn) {
      cpp_exception_escaped = true;
      cpp_exception_message = exn.what();
    }
    jit::failJitPublishStepForTest(0);

    EXPECT_TRUE(cpp_exception_escaped);
    EXPECT_NE(cpp_exception_message.find("hit bad local"), std::string::npos);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_OverflowError));
    EXPECT_GT(deletion_calls, 0);
    PyErr_Clear();
    EXPECT_TRUE(callback.isReplacementInstalled());
  }

  callback.ensureReplacementInstalled();
  jit::hir::preloaderManager().clear();
  runStockCode("del test");
  func.reset();
  EXPECT_GT(deletion_calls, 0);
}

TEST_F(
    PreloaderErrorPropagationTest,
    ForceCompileConvertsPreloadExceptionToRuntimeError) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<> bad_local = makeBadLocalIndex();
  ASSERT_NE(bad_local, nullptr);
  Ref<> jit_module = importCinderJitModule();
  ASSERT_NE(jit_module, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  Ref<> result;
  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  {
    ScopedStaticArgChecksReplacement replacement{func->func_code, bad_local};
    try {
      result = callJitOneArg(jit_module, "force_compile", func.getObj());
    } catch (const std::exception& exn) {
      cpp_exception_escaped = true;
      cpp_exception_message = exn.what();
    }

    EXPECT_FALSE(cpp_exception_escaped) << cpp_exception_message;
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    std::string python_message = takeRaisedExceptionMessage();
    EXPECT_NE(python_message.find("hit bad local"), std::string::npos)
        << python_message;
    EXPECT_TRUE(callback.isReplacementInstalled());
  }
  callback.ensureReplacementInstalled();
}

TEST_F(
    PreloaderErrorPropagationTest,
    LazyCompileFallsBackToInterpreterAfterPreloadException) {
  const char* static_source = R"(
def bad_target(value: int) -> int:
    return value
)";
  const char* wrapper_source = R"(
def test(value):
    if value < 0:
        return bad_target(value)
    return value
)";

  Ref<PyFunctionObject> bad_target(
      compileStaticAndGet(static_source, "bad_target"));
  ASSERT_NE(bad_target, nullptr);
  runStockCode(wrapper_source);
  Ref<PyFunctionObject> func(getGlobal("test"));
  ASSERT_NE(func, nullptr);
  Ref<> bad_local = makeBadLocalIndex();
  ASSERT_NE(bad_local, nullptr);
  Ref<> jit_module = importCinderJitModule();
  ASSERT_NE(jit_module, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  Ref<> lazy_result = callJitOneArg(jit_module, "lazy_compile", func.getObj());
  ASSERT_NE(lazy_result, nullptr);
  ASSERT_EQ(lazy_result.get(), Py_True);

  Ref<> arg = Ref<>::steal(PyLong_FromLong(42));
  ASSERT_NE(arg, nullptr);
  Ref<> result;
  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  {
    ScopedStaticArgChecksReplacement replacement{
        bad_target->func_code, bad_local};
    try {
      result = Ref<>::steal(PyObject_CallOneArg(func.getObj(), arg.get()));
    } catch (const std::exception& exn) {
      cpp_exception_escaped = true;
      cpp_exception_message = exn.what();
    }

    EXPECT_FALSE(cpp_exception_escaped) << cpp_exception_message;
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(PyLong_AsLong(result), 42);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    EXPECT_FALSE(isJitCompiled(func.get()));
    EXPECT_TRUE(callback.isReplacementInstalled());
  }
  callback.ensureReplacementInstalled();
}

TEST_F(
    PreloaderErrorPropagationTest,
    PrecompileAllRestoresCallbackAfterSuccessfulPreload) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<> jit_module = importCinderJitModule();
  ASSERT_NE(jit_module, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  Ref<> lazy_result = callJitOneArg(jit_module, "lazy_compile", func.getObj());
  ASSERT_NE(lazy_result, nullptr);
  ASSERT_EQ(lazy_result.get(), Py_True);

  Ref<> result;
  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  try {
    result = callPrecompileAll(jit_module);
  } catch (const std::exception& exn) {
    cpp_exception_escaped = true;
    cpp_exception_message = exn.what();
  }

  EXPECT_FALSE(cpp_exception_escaped) << cpp_exception_message;
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result.get(), Py_True);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
  EXPECT_TRUE(callback.isReplacementInstalled());
  EXPECT_TRUE(jit::hir::preloaderManager().empty());
  EXPECT_TRUE(isJitCompiled(func.get()));
  callback.ensureReplacementInstalled();
}

TEST_F(
    PreloaderErrorPropagationTest,
    PrecompileAllConvertsPreloadExceptionAndRestoresState) {
  const char* source = R"(
def test(value: int) -> int:
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<> bad_local = makeBadLocalIndex();
  ASSERT_NE(bad_local, nullptr);
  Ref<> jit_module = importCinderJitModule();
  ASSERT_NE(jit_module, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  Ref<> lazy_result = callJitOneArg(jit_module, "lazy_compile", func.getObj());
  ASSERT_NE(lazy_result, nullptr);
  ASSERT_EQ(lazy_result.get(), Py_True);

  Ref<> result;
  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  {
    ScopedStaticArgChecksReplacement replacement{func->func_code, bad_local};
    try {
      result = callPrecompileAll(jit_module);
    } catch (const std::exception& exn) {
      cpp_exception_escaped = true;
      cpp_exception_message = exn.what();
    }

    EXPECT_FALSE(cpp_exception_escaped) << cpp_exception_message;
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    std::string python_message = takeRaisedExceptionMessage();
    EXPECT_NE(python_message.find("hit bad local"), std::string::npos)
        << python_message;
    EXPECT_TRUE(jit::hir::preloaderManager().empty());
    EXPECT_TRUE(callback.isReplacementInstalled());
    EXPECT_FALSE(isJitCompiled(func.get()));
  }
  callback.ensureReplacementInstalled();
}

TEST_F(
    PreloaderErrorPropagationTest,
    OSRCompileFallsBackAfterPreloadException) {
  const char* source = R"(
def test(value: int) -> int:
    while value > 0:
        value -= 1
    return value
)";

  Ref<PyFunctionObject> func(compileStaticAndGet(source, "test"));
  ASSERT_NE(func, nullptr);
  Ref<> bad_local = makeBadLocalIndex();
  ASSERT_NE(bad_local, nullptr);
  ScopedPreloadTestCleanup cleanup;
  int deletion_calls = 0;
  ScopedModuleDeletionCallback callback{&deletion_calls};

  jit::Result result = jit::Result::OK;
  bool cpp_exception_escaped = false;
  std::string cpp_exception_message;
  {
    ScopedStaticArgChecksReplacement replacement{func->func_code, bad_local};
    try {
      result = jit::compileFunctionWithOSR(func);
    } catch (const std::exception& exn) {
      cpp_exception_escaped = true;
      cpp_exception_message = exn.what();
    }
  }

  EXPECT_FALSE(cpp_exception_escaped) << cpp_exception_message;
  EXPECT_EQ(result, jit::Result::UNKNOWN_ERROR);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
  EXPECT_TRUE(jit::hir::preloaderManager().empty());
  EXPECT_TRUE(callback.isReplacementInstalled());
  callback.ensureReplacementInstalled();
}

} // namespace
