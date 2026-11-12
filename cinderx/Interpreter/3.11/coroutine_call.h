// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "Python.h"
#include "internal/pycore_code.h"
#include "internal/pycore_pystate.h"
#include "opcode.h"

#include "cinderx/Interpreter/interpreter.h"

// Native coroutine bodies cannot be JIT compiled on 3.11. Their exact,
// positional factory calls can use the upstream inlined frame protocol
// while our evaluator is installed, without losing an observation/dispatch
// opportunity. Resumption still enters the installed evaluator normally.
static inline int Ci_CanInlineCoroutineCall311(
    PyThreadState* tstate,
    PyObject* callable,
    int nargs,
    PyObject* kwnames) {
  if (tstate->interp->eval_frame == NULL) {
    return 1;
  }
  if (tstate->interp->eval_frame != Ci_EvalFrame ||
      tstate->c_tracefunc != NULL || tstate->c_profilefunc != NULL ||
      kwnames != NULL || !PyFunction_Check(callable)) {
    return 0;
  }
  PyFunctionObject* func = (PyFunctionObject*)callable;
  if (func->vectorcall != _PyFunction_Vectorcall &&
      func->vectorcall != Ci_PyFunction_Vectorcall) {
    return 0;
  }
  PyCodeObject* code = (PyCodeObject*)func->func_code;
  const int required = CO_COROUTINE | CO_OPTIMIZED | CO_NEWLOCALS;
  const int forbidden = CO_GENERATOR | CO_ASYNC_GENERATOR |
      CO_ITERABLE_COROUTINE | CO_VARARGS | CO_VARKEYWORDS;
  return (code->co_flags & required) == required &&
      (code->co_flags & forbidden) == 0 && code->co_kwonlyargcount == 0 &&
      code->co_argcount == nargs && Py_SIZE(code) > 0 &&
      _Py_OPCODE(*_PyCode_CODE(code)) == RETURN_GENERATOR;
}
