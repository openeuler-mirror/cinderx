// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/log.h"

#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace {

class LogDiagnosticsTest : public testing::Test {
 protected:
  void SetUp() override {
    Py_Initialize();
  }

  void TearDown() override {
    EXPECT_EQ(Py_FinalizeEx(), 0);
  }
};

} // namespace

TEST_F(LogDiagnosticsTest, UnattachedThreadReturnsWithoutPythonErrorAPIs) {
  ASSERT_EXIT(
      {
        std::thread worker([] { jit::printPythonException(); });
        worker.join();
        std::exit(0);
      },
      testing::ExitedWithCode(0),
      "");
}

TEST_F(LogDiagnosticsTest, UnattachedThreadDoesNotConsumeOwnerException) {
  ASSERT_EXIT(
      {
        PyErr_SetString(PyExc_ValueError, "owner Python error");
        std::thread worker([] { jit::printPythonException(); });
        worker.join();
        bool preserved = PyErr_ExceptionMatches(PyExc_ValueError);
        PyErr_Clear();
        std::exit(preserved ? 0 : 1);
      },
      testing::ExitedWithCode(0),
      "");
}

TEST_F(
    LogDiagnosticsTest,
    UnattachedThreadAfterSubinterpreterPreservesException) {
  ASSERT_EXIT(
      {
        PyThreadState* main_state = PyThreadState_Get();
        PyThreadState* sub_state = Py_NewInterpreter();
        if (sub_state == nullptr) {
          std::exit(2);
        }
        Py_EndInterpreter(sub_state);
        PyThreadState_Swap(main_state);
        PyErr_SetString(PyExc_ValueError, "owner after subinterpreter");
        std::thread worker([] { jit::printPythonException(); });
        worker.join();
        bool preserved = PyErr_ExceptionMatches(PyExc_ValueError);
        PyErr_Clear();
        std::exit(preserved ? 0 : 1);
      },
      testing::ExitedWithCode(0),
      "");
}

TEST_F(LogDiagnosticsTest, AttachedSubinterpreterDisplaysException) {
  PyThreadState* main_state = PyThreadState_Get();
  PyThreadState* sub_state = Py_NewInterpreter();
  ASSERT_NE(sub_state, nullptr);
  PyErr_SetString(PyExc_ValueError, "subinterpreter Python error");
  testing::internal::CaptureStderr();
  jit::printPythonException();
  std::string output = testing::internal::GetCapturedStderr();
  bool consumed = PyErr_Occurred() == nullptr;
  PyErr_Clear();
  Py_EndInterpreter(sub_state);
  PyThreadState_Swap(main_state);
  EXPECT_TRUE(consumed);
  EXPECT_NE(
      output.find("ValueError: subinterpreter Python error"),
      std::string::npos);
}

TEST_F(LogDiagnosticsTest, DetachedThreadPreservesPendingException) {
  ASSERT_EXIT(
      {
        PyErr_SetString(PyExc_ValueError, "preserved while detached");
        PyThreadState* state = PyEval_SaveThread();
        jit::printPythonException();
        PyEval_RestoreThread(state);
        bool preserved = PyErr_ExceptionMatches(PyExc_ValueError);
        PyErr_Clear();
        std::exit(preserved ? 0 : 1);
      },
      testing::ExitedWithCode(0),
      "");
}

TEST_F(LogDiagnosticsTest, AttachedThreadWithoutExceptionIsSilent) {
  ASSERT_EQ(PyErr_Occurred(), nullptr);
  testing::internal::CaptureStderr();
  jit::printPythonException();
  EXPECT_EQ(testing::internal::GetCapturedStderr(), "");
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(LogDiagnosticsTest, AttachedThreadDisplaysAndConsumesException) {
  PyErr_SetString(PyExc_ValueError, "diagnostic Python error");
  testing::internal::CaptureStderr();
  jit::printPythonException();
  std::string output = testing::internal::GetCapturedStderr();
  EXPECT_NE(
      output.find("ValueError: diagnostic Python error"), std::string::npos);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(LogDiagnosticsTest, UnattachedCheckRetainsMessageAndAbortSignal) {
  ASSERT_EXIT(
      {
        std::thread worker([] { JIT_CHECK(false, "unattached check marker"); });
        worker.join();
      },
      testing::KilledBySignal(SIGABRT),
      "Assertion failed: false\nunattached check marker");
}

TEST_F(LogDiagnosticsTest, UnattachedAbortRetainsMessageAndAbortSignal) {
  ASSERT_EXIT(
      {
        std::thread worker([] { JIT_ABORT("unattached abort marker"); });
        worker.join();
      },
      testing::KilledBySignal(SIGABRT),
      "Abort\nunattached abort marker");
}

TEST_F(LogDiagnosticsTest, AttachedAbortDisplaysPendingPythonException) {
  ASSERT_EXIT(
      {
        PyErr_SetString(PyExc_ValueError, "fatal Python error");
        JIT_ABORT("attached abort marker");
      },
      testing::KilledBySignal(SIGABRT),
      "attached abort marker\nValueError: fatal Python error");
}

TEST_F(LogDiagnosticsTest, ShadowCheckStillThrowsWithoutConsumingException) {
  PyErr_SetString(PyExc_ValueError, "shadow Python error");
  {
    jit::ShadowCompileScope scope;
    EXPECT_THROW(JIT_CHECK(false, "shadow check marker"), std::runtime_error);
  }
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
  PyErr_Clear();
}
