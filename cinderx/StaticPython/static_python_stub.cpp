// Copyright (c) Meta Platforms, Inc. and affiliates.

// Static Python surface for builds that do not ship it (CPython 3.11).
//
// The runtime module links against a small set of Static Python symbols for
// module bookkeeping: cache-clearing hooks it calls on shutdown and type
// invalidation, the StrictModule type it registers, and the accessors that
// go with it.  Everything else in StaticPython belongs to compiled Static
// Python code, which cannot exist here, so it is not part of this build at
// all rather than being present as dead definitions.
//
// The symbol set below is exactly the one the 3.11 link requires; add to it
// only when the linker asks.

#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/strictmoduleobject.h"

namespace {

PyTypeObject disabledType(const char* name, Py_ssize_t basicsize) {
  PyTypeObject type = {PyVarObject_HEAD_INIT(nullptr, 0)};
  type.tp_name = name;
  type.tp_basicsize = basicsize;
  type.tp_flags = Py_TPFLAGS_DEFAULT;
  return type;
}

} // namespace

extern "C" {

// Types the module registers with PyType_Ready(). They are named and sized
// like the real ones so registration behaves normally; no instance is ever
// created because nothing constructs them.
PyTypeObject Ci_StrictModule_Type =
    disabledType("cinderx.StrictModule", sizeof(Ci_StrictModuleObject));
PyTypeObject _Ci_ObjectKeyType =
    disabledType("cinderx.object_key", sizeof(_Ci_ObjectKey));

int _Ci_CreateStaticModule() {
  return 0;
}

// Cache-clearing hooks. No Static Python cache exists, so these are complete
// implementations rather than stubs.
void _PyCheckedDict_ClearCaches() {}
void _PyCheckedList_ClearCaches() {}
void _PyClassLoader_ClearCache() {}
void _PyClassLoader_ClearGenericTypes() {}
void _PyClassLoader_ClearValueCache() {}

int _PyClassLoader_ClearVtables() {
  return 0;
}

int _PyClassLoader_NotifyDictChange(
    PyDictObject*,
    PyDict_WatchEvent,
    PyObject*,
    PyObject*) {
  return 0;
}

// StrictModule accessors. Reading the dict is the plain module behaviour;
// patching requires the strict-module machinery this build does not have.
PyObject* Ci_StrictModule_GetDict(PyObject* mod) {
  return PyModule_GetDict(mod);
}

PyObject* Ci_StrictModule_GetDictSetter(PyObject*) {
  return nullptr;
}

int Ci_do_strictmodule_patch(PyObject*, PyObject*, PyObject*) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "strict module patching requires Static Python, which is not built on "
      "CPython 3.11");
  return -1;
}

// Checked containers are Static Python runtime helpers.  The JIT references
// them for the checked-list/dict opcodes, which never run on 3.11 (Static
// Python is not built and machine code does not execute), so these are inert.
// The TypeCheck predicates answer "no" without setting an error, matching the
// truth that nothing is a checked container here; the constructors and
// mutators fail closed if ever reached.
static int checked_container_unavailable() {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "checked containers require Static Python, which is not built on "
      "CPython 3.11");
  return -1;
}

PyObject* Ci_CheckedDict_New(PyTypeObject*) {
  checked_container_unavailable();
  return nullptr;
}

PyObject* Ci_CheckedDict_NewPresized(PyTypeObject*, Py_ssize_t) {
  checked_container_unavailable();
  return nullptr;
}

int Ci_CheckedDict_TypeCheck(PyTypeObject*) {
  return 0;
}

int Ci_DictOrChecked_SetItem(PyObject*, PyObject*, PyObject*) {
  return checked_container_unavailable();
}

PyObject* Ci_CheckedList_New(PyTypeObject*, Py_ssize_t) {
  checked_container_unavailable();
  return nullptr;
}

int Ci_CheckedList_TypeCheck(PyTypeObject*) {
  return 0;
}

int Ci_ListOrCheckedList_Append(PyListObject*, PyObject*) {
  return checked_container_unavailable();
}

// Static Python class loader.  The JIT references the full class-loader API
// for the Static Python opcodes (INVOKE_*, LOAD_FIELD, CAST, ...); none of it
// runs on 3.11 (machine code does not execute and Static Python is not built).
// The resolvers answer "plain object / not primitive / no typed info" so any
// stray call degrades to untyped behaviour rather than fabricating a checked
// layout; the vtable invoker fails closed.
PyTypeObject* PyStaticArray_Type = nullptr;

PyObject* _PyClassLoader_Box(uint64_t value, int) {
  return PyLong_FromUnsignedLongLong(value);
}

PyObject* _PyClassLoader_GetCodeArgumentTypeDescrs(PyCodeObject*) {
  return nullptr;
}

PyObject* _PyClassLoader_GetCodeReturnTypeDescr(PyCodeObject*) {
  return nullptr;
}

PyObject* _PyClassLoader_GetReturnTypeDescr(PyFunctionObject*) {
  return nullptr;
}

int _PyClassLoader_GetTypeCode(PyTypeObject*) {
  return TYPED_OBJECT;
}

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfo(PyCodeObject*, int) {
  return nullptr;
}

_PyTypedArgsInfo*
_PyClassLoader_GetTypedArgsInfoFromThunk(PyObject*, PyObject*, int) {
  return nullptr;
}

int _PyClassLoader_HasPrimitiveArgs(PyCodeObject*) {
  return 0;
}

PyObject* _PyClassLoader_InvokeMethod(
    _PyType_VTable*,
    Py_ssize_t,
    PyObject**,
    Py_ssize_t) {
  PyErr_SetString(
      PyExc_NotImplementedError,
      "Static Python vtables are not built on CPython 3.11");
  return nullptr;
}

int _PyClassLoader_IsImmutable(PyObject*) {
  return 0;
}

int _PyClassLoader_IsPatchedThunk(PyObject*) {
  return 0;
}

Py_ssize_t _PyClassLoader_ResolveFieldOffset(PyObject*, int*) {
  return -1;
}

PyObject* _PyClassLoader_ResolveFunction(PyObject*, PyObject**) {
  Py_RETURN_NONE;
}

PyObject** _PyClassLoader_ResolveIndirectPtr(PyObject*) {
  return nullptr;
}

Py_ssize_t _PyClassLoader_ResolveMethod(PyObject*) {
  return -1;
}

int _PyClassLoader_ResolvePrimitiveType(PyObject*) {
  return TYPED_OBJECT;
}

PyObject* _PyClassLoader_ResolveReturnType(
    PyObject*,
    int* optional,
    int* exact,
    int* func_flags) {
  if (optional != nullptr) {
    *optional = 0;
  }
  if (exact != nullptr) {
    *exact = 0;
  }
  if (func_flags != nullptr) {
    *func_flags = 0;
  }
  return Py_NewRef(&PyBaseObject_Type);
}

PyTypeObject* _PyClassLoader_ResolveType(PyObject*, int* optional, int* exact) {
  if (optional != nullptr) {
    *optional = 0;
  }
  if (exact != nullptr) {
    *exact = 0;
  }
  return &PyBaseObject_Type;
}

void* _PyClassloader_LookupSymbol(PyObject*, PyObject*) {
  return nullptr;
}

} // extern "C"
