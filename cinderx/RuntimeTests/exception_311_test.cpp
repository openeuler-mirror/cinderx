// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstring>

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

// Byte offset (tb_lasti units) of the first occurrence of `opcode` in
// `code`, or -1 when the opcode does not appear.  The opcode unit is the
// position stock 3.11 leaves in prev_instr when the instruction's body
// reaches `goto error`, so it is the traceback position the exception
// resume has to reproduce.
int firstOpcodeLasti(BorrowedRef<PyCodeObject> code, int opcode) {
  for (const auto& instr : jit::BytecodeInstructionBlock{code}) {
    if (instr.opcode() == opcode) {
      return instr.opcodeIndex().value() *
          static_cast<int>(sizeof(_Py_CODEUNIT));
    }
  }
  return -1;
}

// The (tb_lasti, tb_lineno) of the innermost traceback entry of the
// currently-set exception, which is restored afterwards so the caller can
// keep inspecting it.  Returns false when no traceback is attached.
bool innermostTracebackPosition(int* lasti, int* lineno) {
  PyObject* exc = nullptr;
  PyObject* val = nullptr;
  PyObject* tb = nullptr;
  PyErr_Fetch(&exc, &val, &tb);
  bool ok = false;
  if (tb != nullptr) {
    Ref<> cursor = Ref<>::create(tb);
    while (true) {
      auto next = Ref<>::steal(PyObject_GetAttrString(cursor, "tb_next"));
      if (next == nullptr || next == Py_None) {
        PyErr_Clear();
        break;
      }
      cursor = std::move(next);
    }
    auto lasti_obj = Ref<>::steal(PyObject_GetAttrString(cursor, "tb_lasti"));
    auto lineno_obj = Ref<>::steal(PyObject_GetAttrString(cursor, "tb_lineno"));
    if (lasti_obj != nullptr && lineno_obj != nullptr) {
      *lasti = static_cast<int>(PyLong_AsLong(lasti_obj));
      *lineno = static_cast<int>(PyLong_AsLong(lineno_obj));
      ok = !PyErr_Occurred();
    }
    PyErr_Clear();
  }
  PyErr_Restore(exc, val, tb);
  return ok;
}

} // namespace

class Exception311Test : public RuntimeTest {};

TEST_F(Exception311Test, HandledExceptionMatchesTheInterpreter) {
  SKIP_311_EXECUTABLE_COMPILE();

  const char* src = R"(
def guard(a, b):
    try:
        return a // b
    except ZeroDivisionError:
        return -1
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "guard"));
  ASSERT_NE(func, nullptr);
  auto six = Ref<>::steal(PyLong_FromLong(6));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto zero = Ref<>::steal(PyLong_FromLong(0));
  auto ok_args = Ref<>::steal(PyTuple_Pack(2, six.get(), two.get()));
  auto raising_args = Ref<>::steal(PyTuple_Pack(2, six.get(), zero.get()));

  // Interpreter answers, before any machine code exists.
  auto interp_ok = Ref<>::steal(PyObject_Call(func, ok_args, nullptr));
  ASSERT_NE(interp_ok, nullptr);
  auto interp_handled =
      Ref<>::steal(PyObject_Call(func, raising_args, nullptr));
  ASSERT_NE(interp_handled, nullptr);
  for (int i = 0; i < 32; ++i) {
    auto warmed = Ref<>::steal(PyObject_Call(func, ok_args, nullptr));
    ASSERT_NE(warmed, nullptr);
  }

  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  ASSERT_NE(compiled, nullptr);
  jit::CodeRuntime* rt = compiled->runtime();
  ASSERT_NE(rt, nullptr);

  // The open exception surface does not make exception sites forceable:
  // an armed UnhandledException site with no live error would abort in
  // prepareForDeopt.
  bool saw_exception_site = false;
  for (const jit::DeoptMetadata& meta : rt->deoptMetadatas()) {
    if (meta.reason == jit::DeoptReason::kUnhandledException &&
        !meta.frame_meta.empty()) {
      saw_exception_site = true;
      EXPECT_FALSE(rt->armForcedDeopt(meta.site_id, 1, false));
    }
  }
  EXPECT_TRUE(saw_exception_site)
      << "try/except compiled without an UnhandledException site";

  jit::TriggerStats before = jit::triggerStatsSnapshot();
  auto jit_ok = Ref<>::steal(PyObject_Call(func, ok_args, nullptr));
  ASSERT_NE(jit_ok, nullptr);
  EXPECT_EQ(PyLong_AsLong(jit_ok), PyLong_AsLong(interp_ok));
  jit::TriggerStats after_ok = jit::triggerStatsSnapshot();
  EXPECT_EQ(after_ok.organic_deopt_hits, before.organic_deopt_hits)
      << "the non-raising path must stay in machine code";

  // The raising call deopts organically and the anchored evaluator runs
  // the handler; the answer must match the interpreter's.
  auto jit_handled = Ref<>::steal(PyObject_Call(func, raising_args, nullptr));
  ASSERT_NE(jit_handled, nullptr);
  EXPECT_EQ(PyLong_AsLong(jit_handled), PyLong_AsLong(interp_handled));
  EXPECT_EQ(PyLong_AsLong(jit_handled), -1);
  jit::TriggerStats after_raise = jit::triggerStatsSnapshot();
  EXPECT_GT(after_raise.organic_deopt_hits, after_ok.organic_deopt_hits);
  EXPECT_EQ(after_raise.forced_deopt_hits, after_ok.forced_deopt_hits);

  // The deopt was per-activation: the next raising call re-enters machine
  // code and deopts again.
  auto again = Ref<>::steal(PyObject_Call(func, raising_args, nullptr));
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(PyLong_AsLong(again), -1);
  jit::TriggerStats after_again = jit::triggerStatsSnapshot();
  EXPECT_GT(after_again.organic_deopt_hits, after_raise.organic_deopt_hits);
}

TEST_F(Exception311Test, UnhandledTracebackStopsAtTheFaultingUnit) {
  SKIP_311_EXECUTABLE_COMPILE();

  // BINARY_OP carries an inline cache: the error resume must park
  // prev_instr on the opcode unit, where stock leaves it, not past the
  // cache where the default advance would land.
  const char* src = R"(
def div(a, b):
    return a // b
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "div"));
  ASSERT_NE(func, nullptr);
  auto six = Ref<>::steal(PyLong_FromLong(6));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto zero = Ref<>::steal(PyLong_FromLong(0));
  auto ok_args = Ref<>::steal(PyTuple_Pack(2, six.get(), two.get()));
  auto raising_args = Ref<>::steal(PyTuple_Pack(2, six.get(), zero.get()));
  for (int i = 0; i < 32; ++i) {
    auto warmed = Ref<>::steal(PyObject_Call(func, ok_args, nullptr));
    ASSERT_NE(warmed, nullptr);
  }
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  BorrowedRef<PyCodeObject> code{func->func_code};
  int expected_lasti = firstOpcodeLasti(code, BINARY_OP);
  ASSERT_GE(expected_lasti, 0);

  auto result = Ref<>::steal(PyObject_Call(func, raising_args, nullptr));
  ASSERT_EQ(result, nullptr);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ZeroDivisionError));
  int lasti = -1;
  int lineno = -1;
  ASSERT_TRUE(innermostTracebackPosition(&lasti, &lineno));
  PyErr_Clear();
  EXPECT_EQ(lasti, expected_lasti);
  EXPECT_EQ(lineno, 3);
}

TEST_F(Exception311Test, RaiseStatementReexecutesInTheInterpreter) {
  SKIP_311_EXECUTABLE_COMPILE();

  // RAISE_VARARGS compiles to an unconditional deopt with no exception
  // set; the interpreter re-executes the raise itself, so do_raise
  // semantics (argument use, traceback position) are stock by
  // construction.
  const char* src = R"(
def boom(x):
    raise ValueError(x)
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "boom"));
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);

  BorrowedRef<PyCodeObject> code{func->func_code};
  int expected_lasti = firstOpcodeLasti(code, RAISE_VARARGS);
  ASSERT_GE(expected_lasti, 0);

  auto arg = Ref<>::steal(PyUnicode_FromString("boom-payload"));
  auto args = Ref<>::steal(PyTuple_Pack(1, arg.get()));
  auto result = Ref<>::steal(PyObject_Call(func, args, nullptr));
  ASSERT_EQ(result, nullptr);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));

  int lasti = -1;
  int lineno = -1;
  ASSERT_TRUE(innermostTracebackPosition(&lasti, &lineno));

  PyObject* exc = nullptr;
  PyObject* val = nullptr;
  PyObject* tb = nullptr;
  PyErr_Fetch(&exc, &val, &tb);
  PyErr_NormalizeException(&exc, &val, &tb);
  ASSERT_NE(val, nullptr);
  auto exc_args = Ref<>::steal(PyObject_GetAttrString(val, "args"));
  ASSERT_NE(exc_args, nullptr);
  ASSERT_EQ(PyTuple_GET_SIZE(exc_args.get()), 1);
  EXPECT_TRUE(_PyUnicode_EqualToASCIIString(
      PyTuple_GET_ITEM(exc_args.get(), 0), "boom-payload"));
  Py_XDECREF(exc);
  Py_XDECREF(val);
  Py_XDECREF(tb);

  EXPECT_EQ(lasti, expected_lasti);
  EXPECT_EQ(lineno, 3);
}
#endif
