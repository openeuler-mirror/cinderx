// Copyright (c) Meta Platforms, Inc. and affiliates.

// Use this include instead of Python.h.  This file should be imported before
// any other CPython headers, especially any CPython internal headers used by
// CinderX.  Its purpose is to address incompatibilities between CPython headers
// and our C++ code.

#pragma once

// Avoid conflicts with `min` and `max` on Windows platforms.
#ifdef WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <Python.h>

#if PY_VERSION_HEX >= 0x030E0000
#ifdef __THROW
// mi_decl_throw is defined to be __THROW and breaks in C++ files.
#undef __THROW
#define __THROW
#endif
#include "internal/pycore_mimalloc.h"
#endif

// 3.10 seems to be okay with the header ordering, but it has its own headers
// hitting symbol collisions with atomic.
#if defined(__cplusplus) && PY_VERSION_HEX >= 0x030B0000

// clang-format off

// Handle incompatibilities between atomic headers from libgcc and the
// stdatomic.h header from clang.  The headers must be imported in this specific
// order.  If not then builds will fail because of symbol collisions.

// The memory header also has atomic operations in it.
#include <atomic>
#include <memory>

// CPython 3.11's internal pycore_atomic.h expects C stdatomic names to be
// available in the global namespace, which is not true with GCC's C++ headers.
// Let it use its builtin-atomic fallback instead.
#if PY_VERSION_HEX < 0x030C0000
#undef HAVE_STD_ATOMIC
#endif

#include <stdatomic.h>

// clang-format on

#endif

// These aren't here because of C vs C++ issues, but rather because they're also
// part of the public Python API and we might as well tack them on.
#include <frameobject.h>
#include <structmember.h>

#if PY_VERSION_HEX < 0x030C0000
#ifndef _Py_TPFLAGS_STATIC_BUILTIN
#define _Py_TPFLAGS_STATIC_BUILTIN 0
#endif
#ifndef Py_TPFLAGS_PREHEADER
#define Py_TPFLAGS_PREHEADER 0
#endif
#ifndef _Py_MAKE_CODEUNIT
#define _Py_MAKE_CODEUNIT _Py_MAKECODEUNIT
#endif
#define _PyThreadState_GetCurrent _PyThreadState_UncheckedGet
#define f_funcobj f_func
#ifndef FRAME_OWNED_BY_CSTACK
#define FRAME_OWNED_BY_CSTACK 3
#endif
#ifndef FRAME_STATE_FINISHED
#define FRAME_STATE_FINISHED(S) ((S) >= FRAME_COMPLETED)
#endif
#ifndef EVAL_CALL_GENERATOR
#define EVAL_CALL_GENERATOR 0
#endif
#ifndef EVAL_CALL_STAT_INC
#define EVAL_CALL_STAT_INC(name) ((void)0)
#endif
#ifndef MAX_INTRINSIC_1
#define MAX_INTRINSIC_1 -1
#endif
#ifndef MAX_INTRINSIC_2
#define MAX_INTRINSIC_2 -1
#endif

static inline PyObject* _PyType_GetDict(PyTypeObject* type) {
  return type->tp_dict;
}

static inline void* PyObject_GetItemData(PyObject* obj) {
  return ((char*)obj) + Py_TYPE(obj)->tp_basicsize;
}

static inline int PyCode_GetFirstFree(PyCodeObject* code) {
  return code->co_nlocalsplus - PyCode_GetNumFree(code);
}

static inline int _PyLong_IsCompact(const PyLongObject* op) {
  Py_ssize_t size = Py_SIZE(op);
  return size >= -1 && size <= 1;
}

static inline Py_ssize_t _PyLong_CompactValue(const PyLongObject* op) {
  Py_ssize_t size = Py_SIZE(op);
  if (size == 0) {
    return 0;
  }
  Py_ssize_t value = op->ob_digit[0];
  return size < 0 ? -value : value;
}

static inline int _PyFrame_NumSlotsForCodeObject(PyCodeObject* code) {
  return code->co_nlocalsplus + code->co_stacksize;
}

typedef int (*gcvisitobjects_t)(PyObject*, void*);
static inline void PyUnstable_GC_VisitObjects(gcvisitobjects_t, void*) {}

typedef enum {
  PY_CODE_EVENT_CREATE,
  PY_CODE_EVENT_DESTROY,
} PyCodeEvent;
typedef int (*PyCode_WatchCallback)(PyCodeEvent, PyCodeObject*);
static inline int PyCode_AddWatcher(PyCode_WatchCallback) {
  return 0;
}
static inline int PyCode_ClearWatcher(int) {
  return 0;
}

typedef enum {
  PyDict_EVENT_ADDED,
  PyDict_EVENT_MODIFIED,
  PyDict_EVENT_DELETED,
  PyDict_EVENT_CLONED,
  PyDict_EVENT_CLEARED,
  PyDict_EVENT_DEALLOCATED,
} PyDict_WatchEvent;
typedef int (
    *PyDict_WatchCallback)(PyDict_WatchEvent, PyObject*, PyObject*, PyObject*);
static inline int PyDict_AddWatcher(PyDict_WatchCallback) {
  return 0;
}
static inline int PyDict_ClearWatcher(int) {
  return 0;
}
static inline int PyDict_Watch(int, PyObject*) {
  return 0;
}
static inline int PyDict_Unwatch(int, PyObject*) {
  return 0;
}

typedef enum {
  PyFunction_EVENT_CREATE,
  PyFunction_EVENT_DESTROY,
  PyFunction_EVENT_MODIFY_CODE,
  PyFunction_EVENT_MODIFY_DEFAULTS,
  PyFunction_EVENT_MODIFY_KWDEFAULTS,
  PyFunction_EVENT_MODIFY_QUALNAME,
} PyFunction_WatchEvent;
typedef int (*PyFunction_WatchCallback)(
    PyFunction_WatchEvent,
    PyFunctionObject*,
    PyObject*);
static inline int PyFunction_AddWatcher(PyFunction_WatchCallback) {
  return 0;
}
static inline int PyFunction_ClearWatcher(int) {
  return 0;
}

typedef int (*PyType_WatchCallback)(PyTypeObject*);
static inline int PyType_AddWatcher(PyType_WatchCallback) {
  return 0;
}
static inline int PyType_ClearWatcher(int) {
  return 0;
}
static inline int PyType_Watch(int, PyObject*) {
  return 0;
}
static inline int PyType_Unwatch(int, PyObject*) {
  return 0;
}

// 3.14 的 pyatomic 指针操作在 3.11 不存在，用编译器内建等价实现。
#define _Py_atomic_load_int_relaxed(ATOMIC) \
  __atomic_load_n((const int*)(ATOMIC), __ATOMIC_RELAXED)
#define _Py_atomic_store_int_relaxed(ATOMIC, VALUE) \
  __atomic_store_n((int*)(ATOMIC), (VALUE), __ATOMIC_RELAXED)
#define _Py_atomic_load_ptr_acquire(ATOMIC) \
  __atomic_load_n((void* const*)(ATOMIC), __ATOMIC_ACQUIRE)
#define _Py_atomic_load_ptr_relaxed(ATOMIC) \
  __atomic_load_n((void* const*)(ATOMIC), __ATOMIC_RELAXED)
#define _Py_atomic_store_ptr_release(ATOMIC, VALUE) \
  __atomic_store_n((void**)(ATOMIC), (void*)(VALUE), __ATOMIC_RELEASE)

#define PyUnstable_Long_IsCompact _PyLong_IsCompact
#define PyUnstable_Long_CompactValue _PyLong_CompactValue

// PyUnicode_EqualToUTF8 arrived in 3.13.  Shared code compares interned name
// objects against ASCII C-string literals, so the 3.11 equivalent is the
// exact-ASCII comparison, which allocates nothing and sets no exception.
static inline int PyUnicode_EqualToUTF8(PyObject* unicode, const char* str) {
  return _PyUnicode_EqualToASCIIString(unicode, str);
}

// PyErr_GetRaisedException arrived in 3.12.  The 3.11 equivalent is the
// canonical fetch/normalize/attach-traceback sequence returning the
// exception instance (matching pythoncapi_compat).
static inline PyObject* PyErr_GetRaisedException(void) {
  PyObject* type;
  PyObject* value;
  PyObject* traceback;
  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);
  if (value != NULL && traceback != NULL) {
    PyException_SetTraceback(value, traceback);
  }
  Py_XDECREF(type);
  Py_XDECREF(traceback);
  return value;
}

#define PyUnstable_Code_GetExtra _PyCode_GetExtra
#define PyUnstable_Code_SetExtra _PyCode_SetExtra
#define PyUnstable_Eval_RequestCodeExtraIndex _PyEval_RequestCodeExtraIndex

#ifndef CIX_PSEUDO_IMMORTAL_REFCNT
#define CIX_PSEUDO_IMMORTAL_REFCNT (PY_SSIZE_T_MAX / 4)
#endif
static inline int _Py_IsImmortal(const void* op) {
  return op != NULL &&
      Py_REFCNT((const PyObject*)op) >= CIX_PSEUDO_IMMORTAL_REFCNT / 2;
}
static inline void _Py_SetImmortal(PyObject* op) {
  Py_SET_REFCNT(op, CIX_PSEUDO_IMMORTAL_REFCNT);
}
static inline void _Py_SetImmortalUntracked(PyObject* op) {
  _Py_SetImmortal(op);
}
static inline int _Py_Instrument(PyCodeObject*, PyInterpreterState*) {
  return 0;
}
#endif
