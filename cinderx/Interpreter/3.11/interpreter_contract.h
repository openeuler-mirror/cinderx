// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "Python.h"

// Stable entry point for the later CPython 3.11 runtime integration.
#define CINDERX_PY311_VERSION_HEX 0x030B06F0

#ifdef __cplusplus
extern "C" {
#endif

// Must be called after Python initialization and before evaluating frames.
int Ci_InitInterpreter_311(void);
PyObject* _Py_HOT_FUNCTION Ci_EvalFrameDefault_311(
    PyThreadState* tstate,
    struct _PyInterpreterFrame* frame,
    int throwflag);

// The vendored evaluator's own eval-breaker service, exposed for the JIT's
// loop back edges.  Py_MakePendingCalls covers only signals and pending
// calls; the anchored 3.11.6 eval_frame_handle_pending() additionally
// honours a GIL drop request and delivers asynchronous exceptions, and a
// compiled loop owes its thread exactly those semantics.  Returns 0, or -1
// with an exception set.
int Ci_EvalFrameHandlePending_311(PyThreadState* tstate);

// The single in-process dict-keys version allocator (specialize_wrapper.c).
// Assigns from the top half of the 32-bit range -- disjoint from libpython's
// private bottom-up counter -- and returns 0 at exhaustion, which every
// consumer treats as "cannot be versioned, do not cache".  Both the vendored
// specializer and the JIT's pull-validated attribute caches must use this
// stream: a version proves identity only while no two keys objects can hold
// the same number.
struct _dictkeysobject;
uint32_t Ci_GetDictKeysVersion_311(struct _dictkeysobject* keys);

#ifdef __cplusplus
}
#endif
