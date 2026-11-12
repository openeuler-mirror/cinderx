// Copyright (c) Meta Platforms, Inc. and affiliates.
// CPython 3.11 interpreter attribute adaptation. No framework-specific lookup.
// Included once by ceval_wrapper.c, before its evaluator-local API mappings.

#include "cinderx/UpstreamBorrow/borrowed.h"
#include "internal/pycore_object.h"
#include "attr_type_guard.h"
#include "internal/pycore_typeobject.h"
#include "internal/pycore_pystate.h"

static int ci_interp_getattr_enabled;
static int ci_interp_getattr_counting;
// Borrowed from the immutable, process-lifetime built-in object type.
static PyObject* ci_interp_default_getattribute;
static unsigned long long ci_interp_getattr_eligible;
static unsigned long long ci_interp_getattr_hits;
static unsigned long long ci_interp_getattr_misses;
static unsigned long long ci_interp_getattr_errors;
static unsigned long long ci_interp_getattr_fallbacks;
static unsigned long long ci_interp_getattr_methods;

static int ci_mro_cache_enabled_311;
static unsigned long long ci_mro_calls_311, ci_mro_hits_311, ci_mro_scans_311;
static unsigned long long ci_mro_rejects_311, ci_mro_base_visits_311;
static unsigned long long ci_mro_cached_fallbacks_311;

// Mirrors CPython 3.11.6 _PyType_Lookup's cache-hit conditions. This reads
// the current interpreter's cache; it neither fills it nor runs Python.
static PyObject* Ci_AttrCachedHandler311(
    PyTypeObject* type, PyObject* name, int* original) {
  unsigned int version = type->tp_version_tag;
  if (version == 0 || !(type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG)) return NULL;
  struct type_cache* cache = &_PyInterpreterState_GET()->type_cache;
  PyObject* hname = &_Py_ID(__getattr__);
  PyObject* aname = &_Py_ID(__getattribute__);
  size_t mask = (1U << MCACHE_SIZE_EXP) - 1;
  struct type_cache_entry* a = &cache->hashtable[(version ^ ((uintptr_t)aname >> 3)) & mask];
  if (a->version != version || a->name != aname) return NULL;
  if (a->value != ci_interp_default_getattribute) {
    // No lookup or callback has run. Delegating the whole request to the
    // original API is always safe and avoids redundant qualification work.
    *original = 1;
    return NULL;
  }
  struct type_cache_entry* h = &cache->hashtable[(version ^ ((uintptr_t)hname >> 3)) & mask];
  if (h->version != version || h->name != hname || h->value == NULL) return NULL;
  // GenericGetAttr looks up the target on the type before reading instance
  // storage. A cold lookup in a general-key namespace could run __eq__ and
  // replace a previously checked instance dict. Require this lookup to be a
  // cache hit too; a cached NULL value proves a callback-free miss. No code
  // between this probe and GenericGetAttr's type lookup may run Python.
  struct type_cache_entry* target = &cache->hashtable[(version ^ ((uintptr_t)name >> 3)) & mask];
  if (target->version != version || target->name != name) return NULL;
  PyObject* handler = h->value;
  Py_INCREF(handler);
  return handler;
}

PyObject* Ci_AttrMroStats311(void) {
  return Py_BuildValue("{s:i,s:i,s:K,s:K,s:K,s:K,s:K,s:K}",
      "cache_enabled", ci_mro_cache_enabled_311, "counting", ci_interp_getattr_counting,
      "calls", ci_mro_calls_311, "hits", ci_mro_hits_311, "scans", ci_mro_scans_311,
      "rejects", ci_mro_rejects_311, "accepted_base_visits", ci_mro_base_visits_311,
      "cached_fallbacks", ci_mro_cached_fallbacks_311);
}

int Ci_InterpGetattr311_Init(void) {
  Ci_AttrPathConfigSnapshot311();
  const char* cache = getenv("ATTR_PATH_MRO_CACHE");
  if (cache != NULL && strcmp(cache, "0") && strcmp(cache, "1")) {
    PyErr_SetString(PyExc_ValueError, "ATTR_PATH_MRO_CACHE must be 0 or 1");
    return -1;
  }
  ci_mro_cache_enabled_311 = cache == NULL || strcmp(cache, "1") == 0;
  ci_interp_getattr_enabled = ci_attr_noexc_config_311;
  ci_interp_getattr_counting = ci_attr_diagnostics_config_311;
  if (ci_interp_getattr_enabled && Ci_tp_getattr_hook == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "CPython getattr hook is not initialized");
    return -1;
  }
  if (ci_interp_getattr_enabled) {
    ci_interp_default_getattribute =
        _PyType_Lookup(&PyBaseObject_Type, &_Py_ID(__getattribute__));
    if (ci_interp_default_getattribute == NULL) {
      PyErr_SetString(PyExc_RuntimeError, "object.__getattribute__ is unavailable");
      return -1;
    }
  }
  return 0;
}

// Configuration is separate from optional diagnostic counters.
PyObject* Ci_AttrPathStatus311(void) {
  return Py_BuildValue("{s:i,s:i,s:i,s:i,s:i,s:i}",
      "no_exception", ci_interp_getattr_enabled,
      "specialization", Ci_SpecGaEnabled_311,
      "split_dict", Ci_SplitHintEnabled_311,
      "global_cache", ci_global_cache_enabled_311,
      "inline_call", ci_inline_enabled_311,
      "diagnostics", ci_interp_getattr_counting);
}

PyObject* Ci_AttrPathCounters311(void) {
  if (!ci_interp_getattr_counting) Py_RETURN_NONE;
  return Py_BuildValue("{s:K,s:K,s:K,s:K,s:K,s:K}",
      "attribute_hits", ci_interp_getattr_hits,
      "attribute_misses", ci_interp_getattr_misses,
      "attribute_fallbacks", ci_interp_getattr_fallbacks,
      "attribute_errors", ci_interp_getattr_errors,
      "global_hits", ci_global_cache_hits_311,
      "global_misses", ci_global_cache_misses_311);
}

static PyObject* Ci_InterpGetattrHook311(PyObject* obj, PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (ci_interp_getattr_counting) ci_mro_calls_311++;
  int original = 0;
  PyObject* handler = ci_mro_cache_enabled_311 ? Ci_AttrCachedHandler311(tp, name, &original) : NULL;
  if (original) {
    if (ci_interp_getattr_counting) {
      ci_mro_cached_fallbacks_311++;
      ci_interp_getattr_fallbacks++;
    }
    return PyObject_GetAttr(obj, name);
  }
  if (handler != NULL) {
    if (ci_interp_getattr_counting) ci_mro_hits_311++;
  } else {
    // Type lookup can execute Python equality on general-key namespaces.
    // Reject the whole request before doing any additional type lookup.
    if (ci_interp_getattr_counting) ci_mro_scans_311++;
    if (!Ci_AttrHasUnicodeMro311(tp)) {
      if (ci_interp_getattr_counting) ci_mro_rejects_311++;
      if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
      return PyObject_GetAttr(obj, name);
    }
    if (ci_interp_getattr_counting) ci_mro_base_visits_311 += PyTuple_GET_SIZE(tp->tp_mro);
    handler = _PyType_Lookup(tp, &_Py_ID(__getattr__));
    if (handler == NULL) {
      if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
      return PyObject_GetAttr(obj, name);
    }
    // Match the slot's lifetime ordering: own handler before the next lookup.
    Py_INCREF(handler);
    PyObject* access = _PyType_Lookup(tp, &_Py_ID(__getattribute__));
    if (access != ci_interp_default_getattribute) {
      if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
      Py_DECREF(handler);
      return PyObject_GetAttr(obj, name);
    }

  }

  // Inspect storage without materializing a dict or invoking user code.
  // suppress=1 may continue to a class value after dict equality raises
  // AttributeError. The original slot instead calls __getattr__ immediately.
  if (tp->tp_flags & Py_TPFLAGS_MANAGED_DICT) {
    PyObject* dict = *_PyObject_ManagedDictPointer(obj);
    if (dict != NULL &&
        (!PyDict_CheckExact(dict) || !DK_IS_UNICODE(((PyDictObject*)dict)->ma_keys))) {
      if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
      Py_DECREF(handler);
      return PyObject_GetAttr(obj, name);
    }
  } else if (tp->tp_dictoffset != 0) {
    // Conservatively retain the original protocol for other dict layouts.
    if (ci_interp_getattr_counting) ci_interp_getattr_fallbacks++;
    Py_DECREF(handler);
    return PyObject_GetAttr(obj, name);
  }

  // The owned handler also survives mutations during the generic lookup.
  if (ci_interp_getattr_counting) {
    ci_interp_getattr_eligible++;
  }
  PyObject* result = _PyObject_GenericGetAttrWithDict(obj, name, NULL, 1);
  if (result != NULL) {
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_hits++;
    }
  } else if (PyErr_Occurred()) {
    // suppress=1 only suppresses AttributeError. Other descriptor failures
    // must propagate without retrying a descriptor with side effects.
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_errors++;
    }
  } else {
    if (ci_interp_getattr_counting) {
      ci_interp_getattr_misses++;
    }
    // Preserve CPython 3.11 typeobject.c:call_attribute descriptor binding
    // and invoke the original handler; do not implement its logic in C.
    descrgetfunc bind = Py_TYPE(handler)->tp_descr_get;
    PyObject* bound = bind == NULL
        ? NULL
        : bind(handler, obj, (PyObject*)Py_TYPE(obj));
    PyObject* callable = bind == NULL ? handler : bound;
    if (callable != NULL) {
      result = PyObject_CallOneArg(callable, name);
    }
    Py_XDECREF(bound);
  }
  Py_DECREF(handler);
  if (result == NULL) {
    // The outer PyObject_GetAttr normally adds this context. Reuse the
    // versioned borrowed implementation, including AttributeError subclasses.
    Cix_set_attribute_error_context(obj, name);
  }
  return result;
}

static inline PyObject* Ci_InterpGetAttr311(PyObject* obj, PyObject* name) {
  if (!ci_interp_getattr_enabled || !PyUnicode_CheckExact(name) ||
      Py_TYPE(obj)->tp_getattro != Ci_tp_getattr_hook) {
    return PyObject_GetAttr(obj, name);
  }
  return Ci_InterpGetattrHook311(obj, name);
}

static inline int Ci_InterpGetMethod311(
    PyObject* obj, PyObject* name, PyObject** method) {
  if (!ci_interp_getattr_enabled || !PyUnicode_CheckExact(name) ||
      Py_TYPE(obj)->tp_getattro != Ci_tp_getattr_hook) {
    return _PyObject_GetMethod(obj, name, method);
  }
  if (ci_interp_getattr_counting) {
    ci_interp_getattr_methods++;
  }
  // Stock _PyObject_GetMethod delegates to PyObject_GetAttr and returns 0
  // for these hook types too. The result is already bound, including errors.
  *method = Ci_InterpGetattrHook311(obj, name);
  return 0;
}
