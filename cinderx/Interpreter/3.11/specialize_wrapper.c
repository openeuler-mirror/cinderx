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

#include "Python.h"
#include "internal/pycore_ceval.h"
#include "opcode.h"

#include "cinderx/Common/spike_env.h"
#include "cinderx/Interpreter/3.11/interpreter_contract.h"
#include "cinderx/Interpreter/3.11/interpreter_dependencies.h"
#include "cinderx/Interpreter/interpreter.h"

#include <string.h>

// CPython owns code_dealloc and its hidden _Py_QuickenedCount. Credit that
// SAME counter when our evaluator quickens code. A private, fixed three-word
// frame asks the stock RESUME handler to perform the increment; its credit
// belongs to the real code object, whose eventual stock deallocator balances
// it. The private code is reset to unquickened before releasing any reference.
// No sys getter or code destructor is replaced, including cached aliases.
#define CI_QUICKEN_CLOCK_KEY "_cinderx_quickening_clock_311"
#define CI_QUICKEN_CLOCK_CAPSULE "cinderx.quickening_clock_311"

extern void Ci_PyFrame_Clear_311(_PyInterpreterFrame* frame);

static void Ci_QuickeningClockFree_311(PyObject* capsule) {
  PyObject *type, *value, *tb;
  PyErr_Fetch(&type, &value, &tb);
  PyObject* function = PyCapsule_GetPointer(capsule, CI_QUICKEN_CLOCK_CAPSULE);
  // func_dealloc uses the unchecked GC unlink macro on 3.11.
  if (function != NULL && !PyObject_GC_IsTracked(function)) {
    PyObject_GC_Track(function);
  }
  Py_XDECREF(function);
  PyErr_Clear();
  PyErr_Restore(type, value, tb);
}

int Ci_InitQuickening_311(void) {
  PyObject* dict = PyInterpreterState_GetDict(PyInterpreterState_Get());
  if (dict == NULL) {
    if (!PyErr_Occurred()) {
      PyErr_NoMemory();
    }
    return -1;
  }
  PyObject* existing = PyDict_GetItemString(dict, CI_QUICKEN_CLOCK_KEY);
  if (existing != NULL) {
    if (PyCapsule_IsValid(existing, CI_QUICKEN_CLOCK_CAPSULE)) {
      return 0;
    }
    PyErr_SetString(PyExc_RuntimeError, "invalid quickening accounting state");
    return -1;
  }
  if (PyErr_Occurred()) {
    return -1;
  }
  // RESUME 2 skips the eval breaker: this bookkeeping must not execute user
  // callbacks, service signals or dispatch through a foreign PEP 523 hook.
  const char instructions[] = {RESUME, 2, LOAD_CONST, 0, RETURN_VALUE, 0};
  PyObject* bytes =
      PyBytes_FromStringAndSize(instructions, sizeof(instructions));
  PyObject* empty_bytes = PyBytes_FromStringAndSize("", 0);
  PyObject* empty = PyTuple_New(0);
  PyObject* consts = PyTuple_Pack(1, Py_None);
  PyObject* name = PyUnicode_FromString("<cinderx quickening accounting>");
  PyObject* globals = PyDict_New();
  PyObject* builtins = PyDict_New();
  PyCodeObject* code = NULL;
  PyObject* function = NULL;
  PyObject* capsule = NULL;
  int result = -1;
  if (bytes == NULL || empty_bytes == NULL || empty == NULL || consts == NULL ||
      name == NULL || globals == NULL || builtins == NULL) {
    goto done;
  }
  if (PyDict_SetItemString(globals, "__builtins__", builtins) < 0) {
    goto done;
  }
  code = PyCode_NewWithPosOnlyArgs(
      0,
      0,
      0,
      0,
      1,
      CO_OPTIMIZED | CO_NEWLOCALS,
      bytes,
      consts,
      empty,
      empty,
      empty,
      empty,
      name,
      name,
      name,
      1,
      empty_bytes,
      empty_bytes);
  if (code == NULL) {
    goto done;
  }
  function = PyFunction_New((PyObject*)code, globals);
  if (function == NULL) {
    goto done;
  }
  // This private function has no closure/defaults and its two private
  // dictionaries contain no back-reference. Keep it opaque to Python GC
  // introspection; the capsule owns its acyclic graph until teardown.
  capsule = PyCapsule_New(
      function, CI_QUICKEN_CLOCK_CAPSULE, Ci_QuickeningClockFree_311);
  if (capsule == NULL) {
    goto done;
  }
  PyObject_GC_UnTrack(function);
  function = NULL; // Capsule owns it through interpreter teardown.
  result = PyDict_SetItemString(dict, CI_QUICKEN_CLOCK_KEY, capsule);
done:
  Py_XDECREF(capsule);
  Py_XDECREF(function);
  Py_XDECREF(code);
  Py_XDECREF(builtins);
  Py_XDECREF(globals);
  Py_XDECREF(name);
  Py_XDECREF(consts);
  Py_XDECREF(empty);
  Py_XDECREF(empty_bytes);
  Py_XDECREF(bytes);
  return result;
}

static int Ci_CreditQuickening_311(void) {
  PyThreadState* tstate = PyThreadState_Get();
  PyObject *type, *value, *tb;
  PyErr_Fetch(&type, &value, &tb);
  int credited = 0;
  PyObject* dict = PyInterpreterState_GetDict(tstate->interp);
  PyObject* capsule =
      dict == NULL ? NULL : PyDict_GetItemString(dict, CI_QUICKEN_CLOCK_KEY);
  if (capsule == NULL ||
      !PyCapsule_IsValid(capsule, CI_QUICKEN_CLOCK_CAPSULE)) {
    goto done;
  }
  PyFunctionObject* function =
      PyCapsule_GetPointer(capsule, CI_QUICKEN_CLOCK_CAPSULE);
  PyCodeObject* code = (PyCodeObject*)function->func_code;
  // Fail closed if another native component has modified this private state.
  if (code->co_warmup == 0 || code->co_nlocalsplus != 0 ||
      code->co_stacksize != 1 || Py_SIZE(code) != 3 ||
      code->co_flags != (CO_OPTIMIZED | CO_NEWLOCALS) ||
      PyTuple_GET_SIZE(code->co_consts) != 1 ||
      PyTuple_GET_ITEM(code->co_consts, 0) != Py_None ||
      _Py_OPCODE(_PyCode_CODE(code)[1]) != LOAD_CONST ||
      _Py_OPARG(_PyCode_CODE(code)[1]) != 0 ||
      _Py_OPCODE(_PyCode_CODE(code)[2]) != RETURN_VALUE ||
      PyDict_GET_SIZE(function->func_globals) != 1) {
    goto done;
  }
  struct {
    _PyInterpreterFrame frame;
    PyObject* spare_stack_slot;
  } storage;
  memset(&storage, 0, sizeof(storage));
  _PyFrame_InitializeSpecials(
      &storage.frame, (PyFunctionObject*)Py_NewRef(function), NULL, 0);
  _PyCode_CODE(code)[0] = _Py_MAKECODEUNIT(RESUME, 2);
  code->co_warmup = -1;
  PyThreadState_EnterTracing(tstate);
  // This is an internal accounting operation, not another logical Python
  // call. Restore both tracing and recursion state before touching real code.
  tstate->recursion_remaining++;
  PyObject* result = _PyEval_EvalFrameDefault(tstate, &storage.frame, 0);
  tstate->recursion_remaining--;
  PyThreadState_LeaveTracing(tstate);
  credited = code->co_warmup == 0;
  code->co_warmup = QUICKENING_INITIAL_WARMUP_VALUE;
  _PyCode_CODE(code)[0] = _Py_MAKECODEUNIT(RESUME, 2);
  Py_XDECREF(result);
  Ci_PyFrame_Clear_311(&storage.frame);
done:
  // Quickening is optional. If bookkeeping cannot complete, the caller
  // leaves the real code unquickened rather than publishing an unowned count.
  PyErr_Clear();
  PyErr_Restore(type, value, tb);
  return credited;
}

#define CI_VERSION_RANGE_BASE (UINT32_C(1) << 31)

static uint32_t ci_next_dict_keys_version_311 = CI_VERSION_RANGE_BASE;
static uint32_t ci_next_func_version_311 = CI_VERSION_RANGE_BASE;

// Non-static: the JIT's pull-validated attribute caches (MR-09) must draw
// keys versions from THIS stream.  A second allocator would eventually hand
// the same number to two different keys objects, and a version's only job
// is to make that impossible.  Declared in interpreter_contract.h.
// UINT32_MAX is never issued: the JIT's LOAD_GLOBAL cells record it as the
// "unpinned" version (jit::kGlobalLoadCellUnpinned311), which must compare
// unequal to every live keys version.  Reaching it exhausts the stream.
uint32_t Ci_GetDictKeysVersion_311(PyDictKeysObject* keys) {
  if (keys->dk_version != 0) {
    return keys->dk_version;
  }
  if (ci_next_dict_keys_version_311 == 0 ||
      ci_next_dict_keys_version_311 == UINT32_MAX) {
    ci_next_dict_keys_version_311 = 0;
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

#define _PyCode_Quicken Ci_QuickenBytecode_311
#define _Py_QuickenedCount ci_quickening_scratch_311

#include "upstream/specialize.c"

#undef _PyCode_Quicken
#undef _Py_QuickenedCount

void _PyCode_Quicken(PyCodeObject* code) {
  if (!Ci_CreditQuickening_311()) {
    code->co_warmup = -1;
    return;
  }
  Ci_QuickenBytecode_311(code);
  // Keep the pristine source's increment local and bounded; ownership is
  // recorded by CPython's real counter above, never by a shadow counter.
  ci_quickening_scratch_311 = 0;
}

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

// SPIKE(ARRTC): record what a BINARY_SUBSCR site whose specialization failed
// subscripts (the CI_SUBSCR_ARRTC_* record in interpreter_contract.h).  The
// JIT reads the record at compile time and speculates the element type of a
// site that has only seen one array.array typecode.  Retries come at
// exponentially spaced executions (the adaptive backoff saturates at 4096 and
// keeps retrying), so the record is a sample rather than a census; a wrong
// speculation costs a deopt, and a site that keeps deopting is recompiled
// without it.
//
// arraymodule.c's arrayobject is private.  This mirror matches the JIT's
// StdlibArrayObject (Jit/hir/array_specialize.h), which the JIT validates
// against a probe instance.  ob_descr is read only when the container's type
// IS that validated type object: a type's name and instance size say nothing
// about its layout.  Before the JIT has resolved the type nothing is
// recorded.
typedef struct {
  char typecode;
  int itemsize;
} Ci_ArrayDescrMirror_311;

typedef struct {
  PyObject_VAR_HEAD char* ob_item;
  Py_ssize_t allocated;
  const Ci_ArrayDescrMirror_311* ob_descr;
  PyObject* weakreflist;
  Py_ssize_t ob_exports;
} Ci_ArrayObjectMirror_311;

static void Ci_RecordSubscrContainer_311(
    PyObject* container,
    _Py_CODEUNIT* instr) {
  if (_Py_OPCODE(*instr) != BINARY_SUBSCR_ADAPTIVE) {
    // Specialized: only a failed site's record means anything.
    return;
  }
  PyTypeObject* array_type = Ci_StdlibArrayTypeIfResolved();
  if (array_type == NULL) {
    return;
  }
  uint16_t mark = CI_SUBSCR_ARRTC_OTHER;
  if (Py_TYPE(container) == array_type) {
    const Ci_ArrayDescrMirror_311* descr =
        ((Ci_ArrayObjectMirror_311*)container)->ob_descr;
    unsigned char tc = descr != NULL ? (unsigned char)descr->typecode : 0;
    if (tc != 0 && tc < 0x7F) {
      mark = (uint16_t)(CI_SUBSCR_ARRTC_TAG | tc);
    }
  }
  _PyBinarySubscrCache* cache = (_PyBinarySubscrCache*)(instr + 1);
  uint16_t prev = cache->func_version;
  if ((prev & CI_SUBSCR_ARRTC_TAG_MASK) != CI_SUBSCR_ARRTC_TAG) {
    // Empty, or a function version BINARY_SUBSCR_GETITEM left behind.
    prev = 0;
  }
  if (prev == 0) {
    cache->func_version = mark;
  } else if (prev != mark) {
    cache->func_version = CI_SUBSCR_ARRTC_MIXED;
  }
}

int _Py_Specialize_BinarySubscr(
    PyObject* container,
    PyObject* sub,
    _Py_CODEUNIT* instr) {
  PyInterpreterState* interp = _PyInterpreterState_GET();
  _PyFrameEvalFunction saved = Ci_HideTrustedEvaluator_311(interp);
  int result = Ci_SpecializeBinarySubscrInner_311(container, sub, instr);
  interp->eval_frame = saved;
  static int record = -1;
  if (record < 0) {
    record = Ci_SpikeGetenv("SPIKE_ARRTC") != NULL;
  }
  if (record && result == 0) {
    Ci_RecordSubscrContainer_311(container, instr);
  }
  return result;
}
