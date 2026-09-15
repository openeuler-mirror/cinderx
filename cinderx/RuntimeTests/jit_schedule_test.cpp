// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/code.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX >= 0x030C0000 && defined(CINDERX_RUNTIME_TESTS_CMAKE)

class JITScheduleTest : public RuntimeTest {};

namespace {

PyCodeObject* firstNestedCode(PyFunctionObject* func) {
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  PyObject* consts = code->co_consts;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(consts); i++) {
    PyObject* item = PyTuple_GET_ITEM(consts, i);
    if (PyCode_Check(item)) {
      return reinterpret_cast<PyCodeObject*>(item);
    }
  }
  return nullptr;
}

} // namespace

TEST_F(JITScheduleTest, ResolverMissThenCreateUsesEachRealBoundaryOnce) {
  Ref<PyFunctionObject> factory(compileAndGet(
      R"(
def factory():
    def inner(value):
        return value + 1
    return inner
)",
      "factory"));
  ASSERT_NE(factory, nullptr);

  PyCodeObject* inner_code = firstNestedCode(factory);
  ASSERT_NE(inner_code, nullptr);
  ASSERT_EQ(codeExtraIfExists(inner_code), nullptr);

  bool saved_roi_backoff = jit::getConfig().roi_backoff_enabled;
  jit::getMutableConfig().roi_backoff_enabled = true;
  jit::resetCodeExtraResolverCountsForTest(inner_code);

  Ref<PyFunctionObject> inner = Ref<PyFunctionObject>::steal(
      reinterpret_cast<PyFunctionObject*>(PyFunction_New(
          reinterpret_cast<PyObject*>(inner_code), factory->func_globals)));

  size_t get_or_create = jit::codeExtraGetOrCreateCountForTest();
  size_t if_exists = jit::codeExtraIfExistsCountForTest();
  jit::disableCodeExtraResolverCountingForTest();
  jit::getMutableConfig().roi_backoff_enabled = saved_roi_backoff;

  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(if_exists, 1);
  EXPECT_EQ(get_or_create, 1);
  EXPECT_NE(codeExtraIfExists(inner_code), nullptr);
}

TEST_F(JITScheduleTest, CachedAttachUsesRealResolverBoundariesOnce) {
  Ref<PyFunctionObject> factory(compileAndGet(
      R"(
def factory():
    def inner(value):
        return value + 1
    return inner
)",
      "factory"));
  ASSERT_NE(factory, nullptr);

  Ref<PyFunctionObject> first = Ref<PyFunctionObject>::steal(
      reinterpret_cast<PyFunctionObject*>(PyObject_CallNoArgs(factory)));
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(jit::compileFunction(first), jit::Result::OK);
  ASSERT_TRUE(isJitCompiled(first));

  auto* code = reinterpret_cast<PyCodeObject*>(first->func_code);
  jit::resetCodeExtraResolverCountsForTest(code);
  jit::resetJitContextLookupCountForTest();

  // PyFunction_New emits the function-create watcher synchronously. The new
  // function therefore exercises the recreated compiled-function fast path,
  // rather than merely calling the scheduling wrapper on an arbitrary factory.
  Ref<PyFunctionObject> second =
      Ref<PyFunctionObject>::steal(reinterpret_cast<PyFunctionObject*>(
          PyFunction_New(first->func_code, first->func_globals)));

  size_t create_get_or_create = jit::codeExtraGetOrCreateCountForTest();
  size_t create_if_exists = jit::codeExtraIfExistsCountForTest();
  size_t create_context = jit::jitContextLookupCountForTest();
  jit::disableCodeExtraResolverCountingForTest();
  jit::disableJitContextLookupCountingForTest();

  ASSERT_NE(second, nullptr);
  EXPECT_TRUE(isJitCompiled(second));
  EXPECT_EQ(create_get_or_create + create_if_exists, 1);
  EXPECT_EQ(create_context, 1);

  jit::resetCodeExtraResolverCountsForTest(code);
  jit::resetJitContextLookupCountForTest();
  bool scheduled = jit::scheduleJitCompile(second);
  size_t schedule_get_or_create = jit::codeExtraGetOrCreateCountForTest();
  size_t schedule_if_exists = jit::codeExtraIfExistsCountForTest();
  size_t schedule_context = jit::jitContextLookupCountForTest();
  jit::disableCodeExtraResolverCountingForTest();
  jit::disableJitContextLookupCountingForTest();

  EXPECT_TRUE(scheduled);
  EXPECT_TRUE(isJitCompiled(second));
  EXPECT_EQ(schedule_get_or_create + schedule_if_exists, 1);
  EXPECT_EQ(schedule_context, 1);

  Ref<> arg = Ref<>::steal(PyLong_FromLong(41));
  ASSERT_NE(arg, nullptr);
  Ref<> result = Ref<>::steal(PyObject_CallOneArg(second, arg));
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(isIntEquals(result, 42));
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

#endif
