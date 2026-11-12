#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pycore_dict.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_typeobject.h"
static PyObject* snapshot(PyObject* self, PyObject* obj) {
    if (!PyDict_CheckExact(obj)) {
        PyErr_SetString(PyExc_TypeError, "exact dict required"); return NULL;
    }
    PyDictObject* d = (PyDictObject*)obj;
    return Py_BuildValue("{s:k,s:K,s:K}", "version", (unsigned long)d->ma_keys->dk_version,
        "keys_address", (unsigned long long)(uintptr_t)d->ma_keys,
        "dict_version", (unsigned long long)d->ma_version_tag);
}
static PyObject* call_n(PyObject* self, PyObject* args) {
    PyObject* fn; PyObject* result = NULL; int count;
    if (!PyArg_ParseTuple(args, "Oi", &fn, &count)) return NULL;
    for (int i = 0; i < count; i++) {
        Py_XDECREF(result); result = PyObject_CallNoArgs(fn);
        if (result == NULL) return NULL;
    }
    if (result == NULL) Py_RETURN_NONE;
    return result;
}
static PyObject* clear_type_cache(PyObject* self, PyObject* args) {
    return PyLong_FromUnsignedLong(PyType_ClearCache());
}
static PyObject* type_info(PyObject* self, PyObject* type) {
    if (!PyType_Check(type)) { PyErr_SetString(PyExc_TypeError, "type required"); return NULL; }
    PyTypeObject* t = (PyTypeObject*)type;
    return Py_BuildValue("{s:k,s:i}", "version", (unsigned long)t->tp_version_tag,
        "valid", (t->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG) != 0);
}
// Read-only test probe: do not fill entries or execute a type lookup.
static PyObject* type_cache_info(PyObject* self, PyObject* args) {
    PyObject *type, *name;
    if (!PyArg_ParseTuple(args, "OO", &type, &name)) return NULL;
    if (!PyType_Check(type) || !PyUnicode_CheckExact(name)) {
        PyErr_SetString(PyExc_TypeError, "type and exact str required"); return NULL;
    }
    PyTypeObject* t = (PyTypeObject*)type;
    unsigned int version = t->tp_version_tag;
    struct type_cache* cache = &PyThreadState_Get()->interp->type_cache;
    size_t slot = (version ^ ((uintptr_t)name >> 3)) & ((1U << MCACHE_SIZE_EXP) - 1);
    struct type_cache_entry* entry = &cache->hashtable[slot];
    int hit = version != 0 && (t->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG) &&
        entry->version == version && entry->name == name;
    return Py_BuildValue("{s:i,s:i}", "hit", hit,
        "has_value", hit && entry->value != NULL);
}
static PyMethodDef methods[] = {
    {"type_cache_info", type_cache_info, METH_VARARGS, NULL},
    {"clear_type_cache", clear_type_cache, METH_NOARGS, NULL},
    {"type_info", type_info, METH_O, NULL},
    {"snapshot", snapshot, METH_O, NULL}, {"call_n", call_n, METH_VARARGS, NULL}, {NULL}
};
static struct PyModuleDef module = {PyModuleDef_HEAD_INIT, "_key_probe", NULL, -1, methods};
PyMODINIT_FUNC PyInit__key_probe(void) { return PyModule_Create(&module); }
