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
int Ci_InitQuickening_311(void);
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

// The last admitted compiled Python frame records a CinderX-owned logical
// boundary without borrowing CPython's overflow-recovery headroom. Any nested
// Python frame fails at that logical boundary after its arguments are bound.
int Ci_JitRecursionBoundary311_IsActive(void);
void Ci_JitRecursionBoundary311_Enter(void);
void Ci_JitRecursionBoundary311_Leave(void);
int Ci_JitRecursionBoundary311_Refuse(PyThreadState* tstate);

// The single in-process dict-keys version allocator (specialize_wrapper.c).
// Assigns from the top half of the 32-bit range -- disjoint from libpython's
// private bottom-up counter -- and returns 0 at exhaustion, which every
// consumer treats as "cannot be versioned, do not cache".  Both the vendored
// specializer and the JIT's pull-validated attribute caches must use this
// stream: a version proves identity only while no two keys objects can hold
// the same number.
struct _dictkeysobject;
uint32_t Ci_GetDictKeysVersion_311(struct _dictkeysobject* keys);

// SPIKE(ARRTC): the container record of a BINARY_SUBSCR site whose
// specialization failed, kept in the func_version entry of its inline cache.
// BINARY_SUBSCR_GETITEM writes that entry when it specializes, and a site
// that later deopts back to BINARY_SUBSCR_ADAPTIVE keeps the value.  The
// versions issued here (top half of the 32-bit range) never fit its 16 bits,
// but a function the stock interpreter versioned first (from 1 upwards)
// does, so a site can hold a leftover function version.  Every value whose
// high byte is not 0x80 therefore reads as "nothing recorded yet".  A
// leftover in 0x8000-0x80FF (after more than 32767 stock-versioned
// functions) reads as a record; the worst it can cause is a wrong typecode
// speculation, which deopts and which the per-site fuse retires.
// The specializer wrapper records every retry:
//   0 (or no 0x80 high byte)   nothing recorded yet
//   CI_SUBSCR_ARRTC_TAG | tc   only exact array.array containers of typecode tc
//   CI_SUBSCR_ARRTC_OTHER      only containers that are not exact array.array
//   CI_SUBSCR_ARRTC_MIXED      more than one of the above
#define CI_SUBSCR_ARRTC_TAG 0x8000u
#define CI_SUBSCR_ARRTC_TAG_MASK 0xFF00u
#define CI_SUBSCR_ARRTC_OTHER 0x80FEu
#define CI_SUBSCR_ARRTC_MIXED 0x80FFu

// SPIKE(ARRTC): the array.array type object the JIT resolved and validated
// against a probe instance (jit::hir::getStdlibArrayType), or NULL before
// that.  Does not initialize it; the JIT resolves it when the execute mode
// starts.  The record reads an array's descriptor only for this exact type.
PyTypeObject* Ci_StdlibArrayTypeIfResolved(void);

#ifdef __cplusplus
}
#endif
