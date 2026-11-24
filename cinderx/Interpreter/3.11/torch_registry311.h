// Experimental substitute for a verified torch.nn.Module.__getattr__.
// Included from getattr_noexc.h. No type slots or registry contents are
// changed.

static int ci_torch_registry_requested_311;
static unsigned long long ci_torch_registry_hits_311;
static PyObject* ci_torch_handler_311;
static PyObject* ci_torch_handler_code_311;
static PyObject* ci_torch_dict_descriptor_311;
static PyObject* ci_torch_registry_names_311[3];

PyObject* Ci_RegisterTorchRegistry311(PyObject* self, PyObject* args) {
  PyObject* handler;
  PyObject* code;
  PyObject* descriptor;
  if (!PyArg_ParseTuple(args, "OOO", &handler, &code, &descriptor)) {
    return NULL;
  }
  if (!ci_torch_registry_requested_311 || !Ci_OffEntryEnabled311 ||
      _PyInterpreterState_GET() != PyInterpreterState_Main() ||
      !PyFunction_Check(handler) ||
      ((PyFunctionObject*)handler)->func_code != code ||
      !Py_IS_TYPE(descriptor, &PyGetSetDescr_Type)) {
    PyErr_SetString(
        PyExc_RuntimeError, "Unsupported torch registry registration");
    return NULL;
  }
  if (ci_torch_handler_311 != NULL) {
    if (handler == ci_torch_handler_311 && code == ci_torch_handler_code_311 &&
        descriptor == ci_torch_dict_descriptor_311) {
      Py_RETURN_NONE;
    }
    PyErr_SetString(
        PyExc_RuntimeError, "Torch registry handler already registered");
    return NULL;
  }
  const char* keys[] = {"_parameters", "_buffers", "_modules"};
  PyObject* names[3] = {NULL, NULL, NULL};
  for (int i = 0; i < 3; i++) {
    names[i] = PyUnicode_InternFromString(keys[i]);
    if (names[i] == NULL) {
      for (int k = 0; k < i; k++) {
        Py_DECREF(names[k]);
      }
      return NULL;
    }
  }
  if (ci_torch_handler_311 != NULL ||
      ((PyFunctionObject*)handler)->func_code != code) {
    for (int i = 0; i < 3; i++) {
      Py_DECREF(names[i]);
    }
    PyErr_SetString(PyExc_RuntimeError, "Torch registry registration changed");
    return NULL;
  }
  for (int i = 0; i < 3; i++) {
    ci_torch_registry_names_311[i] = names[i];
  }
  ci_torch_handler_code_311 = Py_NewRef(code);
  ci_torch_dict_descriptor_311 = Py_NewRef(descriptor);
  ci_torch_handler_311 = Py_NewRef(handler);
  Py_RETURN_NONE;
}

static inline int Ci_IsExactUnicodeDict311(PyObject* obj) {
  return PyDict_CheckExact(obj) && DK_IS_UNICODE(((PyDictObject*)obj)->ma_keys);
}

static int Ci_TorchDictProtocol311(PyTypeObject* type, PyThreadState* tstate) {
  PyObject* getter = &_Py_ID(__getattribute__);
  PyObject* name = &_Py_ID(__dict__);
  unsigned int version = type->tp_version_tag;
  if (version != 0 && (type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG)) {
    struct type_cache* cache = &tstate->interp->type_cache;
    size_t mask = (1U << MCACHE_SIZE_EXP) - 1;
    struct type_cache_entry* access =
        &cache->hashtable[(version ^ ((uintptr_t)getter >> 3)) & mask];
    struct type_cache_entry* dict =
        &cache->hashtable[(version ^ ((uintptr_t)name >> 3)) & mask];
    if (access->version == version && access->name == getter &&
        dict->version == version && dict->name == name) {
      return access->value == ci_interp_default_getattribute &&
          dict->value == ci_torch_dict_descriptor_311;
    }
  }
  // Avoid adding an observable key comparison if a custom class dictionary
  // could execute Python. A host cache clear must recheck the namespace.
  return Ci_AttrHasUnicodeMro311(type) &&
      _PyType_Lookup(type, getter) == ci_interp_default_getattribute &&
      _PyType_Lookup(type, name) == ci_torch_dict_descriptor_311;
}

static PyObject*
Ci_TryTorchRegistry311(PyObject* obj, PyObject* name, PyObject* handler) {
  PyFunctionObject* fn = (PyFunctionObject*)handler;
  PyThreadState* tstate = _PyThreadState_GET();
  PyTypeObject* type = Py_TYPE(obj);
  if (fn->func_code != ci_torch_handler_code_311 ||
      (fn->vectorcall != Ci_PyFunction_Vectorcall &&
       fn->vectorcall != _PyFunction_Vectorcall) ||
      !Ci_OffEntryEnabled311 || tstate->interp->eval_frame != Ci_EvalFrame ||
      tstate->c_tracefunc != NULL || tstate->c_profilefunc != NULL ||
      type->tp_getattro != Ci_tp_getattr_hook ||
      !(type->tp_flags & Py_TPFLAGS_MANAGED_DICT) ||
      !PyType_IsSubtype(type, PyDescr_TYPE(ci_torch_dict_descriptor_311)) ||
      !PyUnicode_IS_READY(name) || !Ci_TorchDictProtocol311(type, tstate)) {
    return NULL;
  }
  PyObject* dict = *_PyObject_ManagedDictPointer(obj);
  if (dict == NULL || !Ci_IsExactUnicodeDict311(dict)) {
    return NULL;
  }
  for (int i = 0; i < 3; i++) {
    PyObject* registry =
        PyDict_GetItemWithError(dict, ci_torch_registry_names_311[i]);
    if (registry == NULL) {
      if (PyErr_Occurred()) {
        return NULL;
      }
      continue;
    }
    if (!Ci_IsExactUnicodeDict311(registry)) {
      return NULL;
    }
    PyObject* value = PyDict_GetItemWithError(registry, name);
    if (value != NULL) {
      if (ci_interp_getattr_counting) {
        ci_torch_registry_hits_311++;
      }
      return Py_NewRef(value);
    }
    if (PyErr_Occurred()) {
      return NULL;
    }
  }
  return NULL;
}
