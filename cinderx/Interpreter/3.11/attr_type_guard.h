// Qualification checks must not invoke Python key comparison callbacks.
#pragma once
#include "Python.h"
#include "internal/pycore_dict.h"

static inline int Ci_AttrHasUnicodeMro311(PyTypeObject* type) {
  PyObject* mro = type->tp_mro;
  if (mro == NULL || !PyTuple_CheckExact(mro)) return 0;
  // Only raw reads under the GIL: no materialization or user-code callbacks.
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro); i++) {
    PyObject* base = PyTuple_GET_ITEM(mro, i);
    if (!PyType_Check(base)) return 0;
    PyObject* dict = ((PyTypeObject*)base)->tp_dict;
    if (dict == NULL || !PyDict_CheckExact(dict) ||
        !DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys)) return 0;
  }
  return 1;
}