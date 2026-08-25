// Copyright (c) Meta Platforms, Inc. and affiliates.

// The pristine 3.11 specializer is retained. Its two version allocators live
// behind file-static counters in libpython that the target does not export,
// so this wrapper supplies them.
//
// A version number's only job is to prove that a keys object or a function
// has not changed since a cache entry recorded it.  That proof breaks only
// if two different objects hold the same number, so this allocator issues
// from the top half of the 32-bit range.  The stock counters issue upward
// from 1 and would need 2^31 first-time allocations to reach it -- far
// beyond any real process, though not an architectural impossibility -- and
// they do not advance while CinderX holds the entry point, since all
// specialization then runs in the vendored loop.  Both allocators stop at
// wraparound and report 0, which upstream reads as "out of versions" and
// declines to specialize.
//
// Numbers this large deliberately fail the specializations whose caches
// store a version in 16 bits (BINARY_SUBSCR_GETITEM, LOAD_GLOBAL_BUILTIN):
// for objects versioned here those paths decline rather than cache a
// truncated number.  That is fail-closed -- the generic path stays correct.
// Lifting it belongs to the performance milestone, together with the
// execution-side trust the README describes.

#define Py_BUILD_CORE
#define NEED_OPCODE_TABLES

#include "cinderx/Interpreter/3.11/interpreter_dependencies.h"
#include "cinderx/Interpreter/interpreter.h"

#define CI_VERSION_RANGE_BASE (UINT32_C(1) << 31)

static uint32_t ci_next_dict_keys_version_311 = CI_VERSION_RANGE_BASE;
static uint32_t ci_next_func_version_311 = CI_VERSION_RANGE_BASE;

// Non-static: the JIT's pull-validated attribute caches (MR-09) must draw
// keys versions from THIS stream.  A second allocator would eventually hand
// the same number to two different keys objects, and a version's only job
// is to make that impossible.  Declared in interpreter_contract.h.
uint32_t Ci_GetDictKeysVersion_311(PyDictKeysObject* keys) {
  if (keys->dk_version != 0) {
    return keys->dk_version;
  }
  if (ci_next_dict_keys_version_311 == 0) {
    return 0;
  }
  uint32_t version = ci_next_dict_keys_version_311++;
  keys->dk_version = version;
  return version;
}

static uint32_t Ci_GetFunctionVersion_311(PyFunctionObject* function) {
  if (function->func_version != 0) {
    return function->func_version;
  }
  if (ci_next_func_version_311 == 0) {
    return 0;
  }
  uint32_t version = ci_next_func_version_311++;
  function->func_version = version;
  return version;
}

#define _PyDictKeys_GetVersionForCurrentState Ci_GetDictKeysVersion_311
#define _PyFunction_GetVersionForCurrentState Ci_GetFunctionVersion_311
#define _PyDictKeys_StringLookup Cix_PyDictKeys_StringLookup
#define _PyDict_GetItemHint Cix_PyDict_GetItemHint

// CALL and BINARY_SUBSCR specialization bakes a callee entry point into the
// cache, so upstream declines both whenever a PEP 523 evaluator is installed:
// it cannot know what that evaluator does with the frame. Ours is the stock
// 3.11.6 loop, hash-locked against the target's own sources and incapable of
// executing machine code in this build, so those caches stay valid under it
// and the checks are answered as if no hook were installed. Any other
// evaluator -- a third party's, or a future one that runs compiled code --
// keeps upstream's refusal.
#define _Py_Specialize_Call Ci_SpecializeCallInner_311
#define _Py_Specialize_BinarySubscr Ci_SpecializeBinarySubscrInner_311

#include "upstream/specialize.c"

#undef _Py_Specialize_Call
#undef _Py_Specialize_BinarySubscr

// Hiding the entry point for the duration of the call is safe: specialization
// only inspects types and dictionary shapes, never runs Python code, and runs
// under the GIL, so no frame can be evaluated while it is cleared.
static _PyFrameEvalFunction Ci_HideTrustedEvaluator_311(
    PyInterpreterState* interp) {
  _PyFrameEvalFunction current = interp->eval_frame;
  if (current == Ci_EvalFrame) {
    interp->eval_frame = NULL;
  }
  return current;
}

// pycore_code.h declares both entry points as returning int and the vendored
// ceval.c takes its error path on a negative return, so the wrappers must
// keep that ABI and propagate the inner specializer's result.
int _Py_Specialize_Call(
    PyObject* callable,
    _Py_CODEUNIT* instr,
    int nargs,
    PyObject* kwnames) {
  PyInterpreterState* interp = _PyInterpreterState_GET();
  _PyFrameEvalFunction saved = Ci_HideTrustedEvaluator_311(interp);
  int result = Ci_SpecializeCallInner_311(callable, instr, nargs, kwnames);
  interp->eval_frame = saved;
  return result;
}

int _Py_Specialize_BinarySubscr(
    PyObject* container,
    PyObject* sub,
    _Py_CODEUNIT* instr) {
  PyInterpreterState* interp = _PyInterpreterState_GET();
  _PyFrameEvalFunction saved = Ci_HideTrustedEvaluator_311(interp);
  int result = Ci_SpecializeBinarySubscrInner_311(container, sub, instr);
  interp->eval_frame = saved;
  return result;
}
