// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030B0000 && PY_VERSION_HEX < 0x030C0000

#include "internal/pycore_code.h"
#include "opcode.h"

#include "cinderx/RuntimeTests/fixtures.h"

#include <array>

namespace {
class CallSpecialization311Test : public RuntimeTest {
 public:
  CallSpecialization311Test() : RuntimeTest(static_cast<Flags>(0)) {}

  int specialize(PyObject* function, int nargs, uint16_t* counter = nullptr) {
    std::array<_Py_CODEUNIT, 1 + INLINE_CACHE_ENTRIES_CALL> instructions{};
    instructions[0] = _Py_MAKECODEUNIT(CALL_ADAPTIVE, 0);
    EXPECT_EQ(
        _Py_Specialize_Call(function, instructions.data(), nargs, nullptr), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    if (counter != nullptr) {
      *counter =
          reinterpret_cast<_PyCallCache*>(instructions.data() + 1)->counter;
    }
    return _Py_OPCODE(instructions[0]);
  }
};

class ScopedCallEvaluator {
 public:
  ScopedCallEvaluator()
      : tstate{PyThreadState_Get()},
        evaluator{tstate->interp->eval_frame},
        trace{tstate->c_tracefunc},
        profile{tstate->c_profilefunc} {
    tstate->interp->eval_frame = Ci_EvalFrame;
    tstate->c_tracefunc = nullptr;
    tstate->c_profilefunc = nullptr;
  }
  ~ScopedCallEvaluator() {
    tstate->interp->eval_frame = evaluator;
    tstate->c_tracefunc = trace;
    tstate->c_profilefunc = profile;
  }
  PyThreadState* tstate;

 private:
  _PyFrameEvalFunction evaluator;
  Py_tracefunc trace;
  Py_tracefunc profile;
};

int observeCall(PyObject*, PyFrameObject*, int, PyObject*) {
  return 0;
}
PyObject* otherEvaluator(PyThreadState*, _PyInterpreterFrame*, int) {
  return nullptr;
}
} // namespace

TEST_F(CallSpecialization311Test, OrdinaryCallsKeepAdaptiveBackoff) {
  auto function =
      compileAndGet("def ordinary(a, b=3): return a + b", "ordinary");
  ScopedCallEvaluator hooks;
  for (int nargs : {1, 2}) {
    uint16_t counter = 0;
    EXPECT_EQ(specialize(function, nargs, &counter), CALL_ADAPTIVE);
    EXPECT_GT(counter, 0);
    EXPECT_EQ(hooks.tstate->interp->eval_frame, Ci_EvalFrame);
  }
}

TEST_F(CallSpecialization311Test, ExactCoroutineKeepsInlineSpecialization) {
  auto function = compileAndGet("async def coro(a, b=3): return a + b", "coro");
  ScopedCallEvaluator hooks;
  EXPECT_EQ(specialize(function, 2), CALL_PY_EXACT_ARGS);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, Ci_EvalFrame);
  EXPECT_EQ(specialize(function, 1), CALL_ADAPTIVE);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, Ci_EvalFrame);
}

TEST_F(CallSpecialization311Test, ObservationAndForeignEvaluatorStayInstalled) {
  auto function = compileAndGet("async def coro(a): return a", "coro");
  ScopedCallEvaluator hooks;
  hooks.tstate->c_tracefunc = observeCall;
  EXPECT_EQ(specialize(function, 1), CALL_ADAPTIVE);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, Ci_EvalFrame);
  hooks.tstate->c_tracefunc = nullptr;
  hooks.tstate->c_profilefunc = observeCall;
  EXPECT_EQ(specialize(function, 1), CALL_ADAPTIVE);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, Ci_EvalFrame);
  hooks.tstate->c_profilefunc = nullptr;
  hooks.tstate->interp->eval_frame = otherEvaluator;
  EXPECT_EQ(specialize(function, 1), CALL_ADAPTIVE);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, otherEvaluator);
}

TEST_F(CallSpecialization311Test, StockEvaluatorKeepsOrdinarySpecialization) {
  auto function =
      compileAndGet("def ordinary(a, b=3): return a + b", "ordinary");
  ScopedCallEvaluator hooks;
  hooks.tstate->interp->eval_frame = nullptr;
  EXPECT_EQ(specialize(function, 2), CALL_PY_EXACT_ARGS);
  EXPECT_EQ(specialize(function, 1), CALL_PY_WITH_DEFAULTS);
  EXPECT_EQ(hooks.tstate->interp->eval_frame, nullptr);
}

#endif
