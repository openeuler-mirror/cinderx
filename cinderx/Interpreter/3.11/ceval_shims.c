// Copyright (c) Meta Platforms, Inc. and affiliates.

// Minimal behavior-equivalent adapters for private helpers whose upstream
// implementation is a thin call into inaccessible file-static machinery.

#define Py_BUILD_CORE

#include "Python.h"
#include "internal/pycore_frame.h"
#include "internal/pycore_pystate.h"

#include "upstream/condvar.h"

#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

PyObject* _PyNumber_PowerNoMod(PyObject* lhs, PyObject* rhs) {
  return PyNumber_Power(lhs, rhs, Py_None);
}

PyObject* _PyNumber_InPlacePowerNoMod(PyObject* lhs, PyObject* rhs) {
  return PyNumber_InPlacePower(lhs, rhs, Py_None);
}

PyObject* _PyDict_FromItems(
    PyObject* const* keys,
    Py_ssize_t keys_offset,
    PyObject* const* values,
    Py_ssize_t values_offset,
    Py_ssize_t length) {
  PyObject* dict = PyDict_New();
  if (dict == NULL) {
    return NULL;
  }
  for (Py_ssize_t index = 0; index < length; index++) {
    if (PyDict_SetItem(
            dict, keys[index * keys_offset], values[index * values_offset]) <
        0) {
      Py_DECREF(dict);
      return NULL;
    }
  }
  return dict;
}

PyObject* _PyLong_Add(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_add((PyObject*)left, (PyObject*)right);
}

PyObject* _PyLong_Subtract(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_subtract(
      (PyObject*)left, (PyObject*)right);
}

PyObject* _PyLong_Multiply(PyLongObject* left, PyLongObject* right) {
  return PyLong_Type.tp_as_number->nb_multiply(
      (PyObject*)left, (PyObject*)right);
}

void _PyFloat_ExactDealloc(PyObject* object) {
  assert(PyFloat_CheckExact(object));
  Py_TYPE(object)->tp_dealloc(object);
}

void _PyUnicode_ExactDealloc(PyObject* object) {
  assert(PyUnicode_CheckExact(object));
  Py_TYPE(object)->tp_dealloc(object);
}

PyObject* _PyTuple_FromArraySteal(PyObject* const* items, Py_ssize_t length) {
  PyObject* tuple = PyTuple_New(length);
  if (tuple == NULL) {
    for (Py_ssize_t index = 0; index < length; index++) {
      Py_DECREF(items[index]);
    }
    return NULL;
  }
  for (Py_ssize_t index = 0; index < length; index++) {
    PyTuple_SET_ITEM(tuple, index, items[index]);
  }
  return tuple;
}

int _PySys_Audit(
    PyThreadState* tstate,
    const char* event,
    const char* format,
    ...) {
  assert(tstate == _PyThreadState_GET());
  if (format == NULL) {
    return PySys_Audit(event, NULL);
  }
  va_list arguments;
  va_start(arguments, format);
  PyObject* values = Py_VaBuildValue(format, arguments);
  va_end(arguments);
  if (values == NULL) {
    return -1;
  }
  int result = PySys_Audit(event, "O", values);
  Py_DECREF(values);
  return result;
}

// The common Release 34 Borrow cache already owns the exact datastack growth
// implementation, emitted as Cix_PyThreadState_PushFrame (the stock
// _PyThreadState_BumpFramePointerSlow body; stock 3.11 has no
// _PyThreadState_PushFrame symbol).  Provide the out-of-line symbol CPython's
// inline frame push expects by forwarding to that copy.  This translation
// unit does not include the Borrow header, so the reference is declared
// directly rather than via the Cix_ rename.
extern _PyInterpreterFrame* Cix_PyThreadState_PushFrame(
    PyThreadState* tstate,
    size_t size);

_PyInterpreterFrame* _PyThreadState_BumpFramePointerSlow(
    PyThreadState* tstate,
    size_t size) {
  return Cix_PyThreadState_PushFrame(tstate, size);
}

#if defined(HAVE_PTHREAD_CONDATTR_SETCLOCK) && defined(HAVE_CLOCK_GETTIME) && \
    defined(CLOCK_MONOTONIC)
#define CI_CONDATTR_MONOTONIC 1
#else
#define CI_CONDATTR_MONOTONIC 0
#endif

static pthread_condattr_t* Ci_CondAttr_311(void) {
#if CI_CONDATTR_MONOTONIC
  static pthread_condattr_t attributes;
  static int initialized;
  if (initialized == 0) {
    initialized = pthread_condattr_init(&attributes) == 0 &&
            pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC) == 0
        ? 1
        : -1;
  }
  return initialized == 1 ? &attributes : NULL;
#else
  return NULL;
#endif
}

int _PyThread_cond_init(PyCOND_T* condition) {
  return pthread_cond_init(condition, Ci_CondAttr_311());
}

void _PyThread_cond_after(long long microseconds, struct timespec* deadline) {
#if CI_CONDATTR_MONOTONIC
  if (Ci_CondAttr_311() != NULL) {
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += microseconds / 1000000;
    deadline->tv_nsec += (microseconds % 1000000) * 1000;
    if (deadline->tv_nsec >= 1000000000) {
      deadline->tv_sec++;
      deadline->tv_nsec -= 1000000000;
    }
    return;
  }
#endif
  struct timeval now;
  gettimeofday(&now, NULL);
  deadline->tv_sec = now.tv_sec + (now.tv_usec + microseconds) / 1000000;
  deadline->tv_nsec = ((now.tv_usec + microseconds) % 1000000) * 1000;
}
