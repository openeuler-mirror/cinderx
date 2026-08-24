// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/inline_cache.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#if PY_VERSION_HEX < 0x030C0000
// PY_AUDIT_READ for the 3.11 member-descriptor fill gate (the modern
// Py_AUDIT_READ spelling only exists from 3.12).
#include <structmember.h>
#endif

#include <algorithm>
#include <memory>

namespace jit {

namespace {

template <class T>
struct TypeWatcher {
  jit::UnorderedMap<BorrowedRef<PyTypeObject>, jit::UnorderedSet<T*>> caches;

  void watch(BorrowedRef<PyTypeObject> type, T* cache) {
#if PY_VERSION_HEX < 0x030C0000
    // 3.11 has no type watchers: the compatibility shim's PyType_Watch
    // fakes success and no callback ever fires, so a registry here would
    // be dead weight pretending to be a safety mechanism.  Validity is
    // pull-based instead -- every hit re-checks the captured
    // tp_version_tag (typeVersionMatches / Entry::type_version).
    (void)type;
    (void)cache;
    return;
#else
    if (PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE) &&
        !PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
      return;
    }
    JIT_CHECK(
        cinderx::getModuleState()->watcher_state.watchType(type) == 0,
        "Failed to watch type {} for attribute cache",
        type->tp_name);
    caches[type].emplace(cache);
#endif
  }

  void unwatch(BorrowedRef<PyTypeObject> type, T* cache) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    it->second.erase(cache);
    // don't unwatch type; other watchers may still be watching it
  }

  template <typename Callback>
  void typeChanged(BorrowedRef<PyTypeObject> type, Callback cb) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    jit::UnorderedSet<T*> to_notify = std::move(it->second);
    caches.erase(it);
    for (T* cache : to_notify) {
      cb(cache, type);
    }
  }

  void typeChanged(BorrowedRef<PyTypeObject> type) {
    typeChanged(type, [](T* cache, BorrowedRef<PyTypeObject> tp) {
      cache->typeChanged(tp);
    });
  }
};

TypeWatcher<AttributeCache> ac_watcher;
TypeWatcher<AttributeCache> ac_descr_watcher;
TypeWatcher<LoadTypeAttrCache> ltac_watcher;
TypeWatcher<LoadMethodCache> lm_watcher;
TypeWatcher<LoadTypeMethodCache> ltm_watcher;

// Sentinel PyTypeObject that must never escape into user code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject s_empty_type_attr_cache = {
    PyVarObject_HEAD_INIT(NULL, 0) "EmptyLoadTypeAttrCache",
};
#pragma clang diagnostic pop

inline Ref<PyDictObject> get_dict(PyObject* obj, Py_ssize_t dictoffset) {
  PyObject** dictptr = (PyObject**)((char*)obj + dictoffset);
  return Ref<PyDictObject>::create((PyDictObject*)*dictptr);
}

#if PY_VERSION_HEX < 0x030C0000
// Look up `name` in the instance's attribute storage without materializing
// anything: live inline values are scanned through the type's cached keys,
// a materialized managed dict and a plain tp_dictoffset dict are read
// directly.  _PyObject_GetDictPtr is unusable for reads on 3.11 -- for a
// managed-dict object with live values it converts them into a real dict.
//
// Returns 1 with *out set to a NEW reference when the attribute is found,
// 0 when it is absent, and -1 with the exception left in place when the
// lookup itself raised (a non-unicode key's __eq__ runs arbitrary code;
// stock GenericGetAttr propagates it via PyDict_GetItemWithError, and so
// must every cached path).  The found value is strengthened BEFORE the
// storage's temporary reference is released: an __eq__ that swaps
// obj.__dict__ mid-lookup leaves the old dict alive only through that
// temporary, and stock's ordering (incref the value, then drop the dict)
// is the only one that cannot dangle.
int peekInstanceAttr311(PyObject* obj, PyObject* name, PyObject** out) {
  *out = nullptr;
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictValues* values = *_PyObject_ValuesPointer(obj);
    if (values != nullptr) {
      PyDictKeysObject* keys =
          reinterpret_cast<PyHeapTypeObject*>(tp)->ht_cached_keys;
      if (keys == nullptr) {
        return 0;
      }
      Py_ssize_t index = getDictKeysIndex(keys, name);
      if (index < 0 || index >= keys->dk_nentries) {
        return 0;
      }
      PyObject* value = values->values[index];
      if (value == nullptr) {
        return 0;
      }
      // The cached-keys scan compares interned unicode keys only -- no
      // user code ran, the value is strengthened in place.
      *out = Py_NewRef(value);
      return 1;
    }
    PyDictObject* dict =
        reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
    if (dict == nullptr) {
      return 0;
    }
    auto strong_ref = Ref<>::create(reinterpret_cast<PyObject*>(dict));
    PyObject* value =
        PyDict_GetItemWithError(reinterpret_cast<PyObject*>(dict), name);
    if (value == nullptr) {
      return PyErr_Occurred() ? -1 : 0;
    }
    *out = Py_NewRef(value);
    return 1;
  }
  // Non-managed receivers: _PyObject_GetDictPtr only materializes for
  // managed-dict objects with live values, so it is side-effect free here
  // and handles negative tp_dictoffset correctly.
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == nullptr || *dictptr == nullptr) {
    return 0;
  }
  auto strong_ref = Ref<>::create(*dictptr);
  PyObject* value = PyDict_GetItemWithError(strong_ref, name);
  if (value == nullptr) {
    return PyErr_Occurred() ? -1 : 0;
  }
  *out = Py_NewRef(value);
  return 1;
}
#endif

inline bool is_dict_unmaterialized(PyDictObject* dict) {
  return
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030E0000
      // On 3.12 if we have values then it means we all of our dict indxes
      // are in the array. We won't have a combined mutator that has
      // this index
      _PyDictOrValues_IsValues(
          PyDictOrValues{reinterpret_cast<PyObject*>(dict)}) ||
#endif
      dict == nullptr;
}

PyObject* __attribute__((noinline)) raise_attribute_error(
    PyObject* obj,
    PyObject* name) {
  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      Py_TYPE(obj)->tp_name,
      name);
  Cix_set_attribute_error_context(obj, name);
  return nullptr;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<PyModuleObject> mod) {
  if (mod->md_dict) {
    BorrowedRef<PyDictObject> md_dict = mod->md_dict;
    return Ci_DictVersionTag(md_dict.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<Ci_StrictModuleObject> mod) {
  if (mod->globals) {
    BorrowedRef<PyDictObject> globals = mod->globals;
    return Ci_DictVersionTag(globals.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else {
    return 0;
  }
}

// No-op on 3.12+: the MR-09 tallies observe only the pull-validated 3.11
// arms.
inline void countAttrCacheFill311([[maybe_unused]] bool is_set) {
#if PY_VERSION_HEX < 0x030C0000
  auto& stats = attrCacheStats311();
  (is_set ? stats.store_attr : stats.load_attr).fills++;
#endif
}

void maybeCollectCacheStats(
    std::unique_ptr<CacheStats>& stat,
    BorrowedRef<PyTypeObject> tp,
    BorrowedRef<> name,
    CacheMissReason reason) {
  if (!getConfig().collect_attr_cache_stats) {
    return;
  }
  std::string key =
      fmt::format("{}.{}", typeFullname(tp), PyUnicode_AsUTF8(name));
  stat->misses.insert({key, CacheMiss{0, reason}}).first->second.count++;
}

// Check whether a type's __getattr__ hook wraps PyObject_GenericGetAttr.
//
// When tp_getattro == Ci_tp_getattr_hook, the hook first calls the type's
// __getattribute__, then falls back to __getattr__ on AttributeError. Our IC
// assumes the attribute lookup follows PyObject_GenericGetAttr
// semantics (instance dict + type dict lookup). This is only correct when
// __getattribute__ resolves to object.__getattribute__.
//
// For metaclasses (subclasses of type), __getattribute__ resolves to
// type.__getattribute__ (type_getattro), which does MRO search on the class
// object. The IC's instance dict lookup cannot replicate this, so we must
// reject caching for such types.
bool hookUsesGenericGetAttr(BorrowedRef<PyTypeObject> type) {
  BorrowedRef<> getattribute = _PyType_Lookup(type, &_Py_ID(__getattribute__));
  return getattribute == cinderx::getModuleState()->object_getattribute;
}

// Check whether a type uses the __getattr__ hook as its tp_getattro.
// CPython installs _Py_slot_tp_getattr_hook (captured as Ci_tp_getattr_hook)
// when a class defines __getattr__ with the default __getattribute__. This
// is the fastest and most precise check for __getattr__-eligible types.
//
// Returns the __getattr__ method if the type is eligible, or nullptr if not.
// Returns nullptr for types where the hook wraps something other than
// PyObject_GenericGetAttr (e.g., metaclasses where it wraps type_getattro).
BorrowedRef<> getGetAttrForCaching(BorrowedRef<PyTypeObject> type) {
  if (type->tp_getattro != Ci_tp_getattr_hook) {
    return nullptr;
  }

  // Look up the __getattr__ method on the type. This should always succeed
  // when tp_getattro == Ci_tp_getattr_hook, but the type could have been
  // modified between the slot being set and this check.
  return _PyType_Lookup(type, &_Py_ID(__getattr__));
}

// Call a __getattr__ method with the given object and attribute name.
// Replicates CPython's call_attribute() logic from typeobject.c.
//
// The hook is strengthened for the duration of the call: stock
// slot_tp_getattr_hook holds its Py_INCREF across call_attribute(), and
// the code running here -- a native hook's tp_descr_get, or the call
// itself -- can delete the owner class's __getattr__ entry, which may be
// the hook's last reference, while the hook's own C frame is still
// executing.  Several callers hand in a borrowed pointer (a type-dict
// lookup, a fill-time cached hook), so the pin lives at this choke point
// and covers every dispatch arm at once; no caller runs user code
// between resolving the hook and calling in here.
PyObject* callGetAttr(
    BorrowedRef<> getattr_method,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  auto hook_guard = Ref<>::create(getattr_method);
  BorrowedRef<PyTypeObject> attr_type = Py_TYPE(getattr_method);
  if (PyType_HasFeature(attr_type, Py_TPFLAGS_METHOD_DESCRIPTOR)) {
    PyObject* args[] = {obj, name};
    return PyObject_Vectorcall(getattr_method, args, 2, nullptr);
  }

  descrgetfunc f = attr_type->tp_descr_get;
  if (f != nullptr) {
    auto descr = Ref<>::steal(f(getattr_method, obj, (PyObject*)Py_TYPE(obj)));
    if (descr == nullptr) {
      return nullptr;
    }
    return PyObject_CallOneArg(descr, name);
  }

  return PyObject_CallOneArg(getattr_method, name);
}

#if PY_VERSION_HEX < 0x030C0000
// The slot_tp_getattr_hook protocol, replicated for cached lookups on
// 3.11 hook receivers (tp_getattro == Ci_tp_getattr_hook wrapping the
// generic lookup).  Stock pins the __getattr__ this call will use with a
// Py_INCREF BEFORE the generic part runs any user code, executes the
// generic part with AttributeError suppression, and invokes the pinned
// hook when the generic part comes up empty.  The pin is load-bearing:
// user code inside the lookup (a dict key's __eq__, a descriptor slot)
// can unpublish the hook, and the borrowed cached pointer would dangle.
// Receivers without the hook run unsuppressed: every error propagates,
// AttributeError included.
class GetAttrHookSnapshot311 {
 public:
  // Snapshot from the receiver (kinds that do not cache the hook).
  explicit GetAttrHookSnapshot311(PyObject* obj)
      : hook_{Ref<>::create(getGetAttrForCaching(Py_TYPE(obj)).get())} {}
  // Snapshot from a hook cached at fill time; the receiver-version hit
  // check already proved it equals the entry-time lookup.
  explicit GetAttrHookSnapshot311(BorrowedRef<> known_hook)
      : hook_{Ref<>::create(known_hook.get())} {}

  bool active() const {
    return hook_ != nullptr;
  }

  // Handle an exception raised inside the generic part: under the hook
  // an AttributeError is suppressed (the caller then continues or
  // finishes the generic part); anything else -- or any error without
  // the hook -- propagates.  Returns true when the error was cleared.
  bool suppressAttributeError() const {
    if (hook_ == nullptr || !PyErr_ExceptionMatches(PyExc_AttributeError)) {
      return false;
    }
    PyErr_Clear();
    return true;
  }

  PyObject* call(PyObject* obj, PyObject* name) const {
    return callGetAttr(hook_, obj, name);
  }

 private:
  Ref<> hook_;
};
#endif

// Try __getattr__ fallback for split/combined dict lookups where the value is
// missing. If the type has __getattr__ (tp_getattro == Ci_tp_getattr_hook),
// look it up and call it. Otherwise, raise AttributeError as usual.
PyObject* getAttrFallback(PyObject* obj, PyObject* name) {
  BorrowedRef<PyTypeObject> type = Py_TYPE(obj);
  if (type->tp_getattro == Ci_tp_getattr_hook) {
    BorrowedRef<> getattr = _PyType_Lookup(type, &_Py_ID(__getattr__));
    if (getattr != nullptr) {
      return callGetAttr(getattr, obj, name);
    }
  }
  return raise_attribute_error(obj, name);
}

// Checks to see if the cached keys version allows a lookup w/o looking in
// the dictionary. This could be either that we have a match of the keys version
// or that we have a non-heap type w/ no dictionary.
//
// Avoid using _PyObject_GetDictPtr here as it can materialize the dictionary
// on 3.12+. Instead use version-specific APIs to check the dict state without
// side effects.
bool isValidKeysVersion(uint32_t keys_version, BorrowedRef<> obj) {
  if (keys_version == 0) {
    // 0 is an invalid keys version and a sentinel value that we'll never
    // generate a cache for a heap type with. We may have a non-heap type
    // that is cached w/ a keys_version of 0 that has no dictionary in which
    // case the cache is always valid.
    return true;
  }

#if PY_VERSION_HEX >= 0x030E0000
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    if (PyType_HasFeature(tp, Py_TPFLAGS_INLINE_VALUES)) {
      PyDictValues* values = _PyObject_InlineValues(obj);
      if (values->valid) {
        // Inline values are still active but the shared keys may have changed
        // (e.g., a new instance attribute was added). Check the type's shared
        // keys version.
        PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
        return ht->ht_cached_keys->dk_version == keys_version;
      }
    }
    // Check the managed dict directly.
    PyDictObject* dict = _PyObject_GetManagedDict(obj);
    if (dict == nullptr) {
      return true;
    }
    return dict->ma_keys->dk_version == keys_version;
  }
#elif PY_VERSION_HEX >= 0x030C0000
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
    if (_PyDictOrValues_IsValues(dorv)) {
      // Values are still inline but the shared keys may have changed
      // (e.g., a new instance attribute was added). Check the type's shared
      // keys version.
      PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
      return ht->ht_cached_keys->dk_version == keys_version;
    }
    PyDictObject* dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
    if (dict == nullptr) {
      return true;
    }
    return dict->ma_keys->dk_version == keys_version;
  }
#else
  PyTypeObject* tp = Py_TYPE(obj);
  if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictValues* values = *_PyObject_ValuesPointer(obj);
    if (values != nullptr) {
      PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(tp);
      return ht->ht_cached_keys->dk_version == keys_version;
    }
    PyDictObject* dict =
        reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
    if (dict == nullptr) {
      return true;
    }
    return dict->ma_keys->dk_version == keys_version;
  }
#endif

  // Non-managed-dict fallback (non-heap types with tp_dictoffset).
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  JIT_DCHECK(
      dictptr != nullptr, "should have dict ptr {}", Py_TYPE(obj)->tp_name);

  PyDictObject* dict = reinterpret_cast<PyDictObject*>(*dictptr);
  if (dict == nullptr) {
    return true;
  }

  return dict->ma_keys->dk_version == keys_version;
}

// If the current exception is AttributeError and the type has __getattr__,
// suppress the error and call __getattr__ as a fallback. Returns the
// __getattr__ result, or nullptr if __getattr__ was not applicable (in
// which case the original error remains set).
PyObject* tryGetAttrFallback(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> cached_getattr,
    PyObject* obj,
    PyObject* name) {
  if (type->tp_getattro != Ci_tp_getattr_hook) {
    return nullptr;
  }
  if (!PyErr_ExceptionMatches(PyExc_AttributeError)) {
    return nullptr;
  }
  PyErr_Clear();
  BorrowedRef<> getattr = cached_getattr;
  if (getattr == nullptr) {
    getattr = _PyType_Lookup(type, &_Py_ID(__getattr__));
  }
  if (getattr == nullptr) {
    return nullptr;
  }
  return callGetAttr(getattr, obj, name);
}

} // namespace

void AttributeMutator::changeKindFromSplitInline(
    SplitMutator* split,
    Kind new_kind) {
  AttributeMutator* mutator = from(split);
  mutator->type_ = reinterpret_cast<uintptr_t>(mutator->type()) |
      static_cast<uintptr_t>(new_kind);
}

PyDictKeysObject* getSplitKeys(BorrowedRef<PyTypeObject> type) {
  JIT_DCHECK(
      PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE),
      "expected heap type {}",
      type->tp_name);
  PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(type.get());
  return ht->ht_cached_keys;
}

bool SplitMutator::canInsertToSplitDict(
    BorrowedRef<PyDictObject> dict,
    BorrowedRef<> name) {
  if (dict->ma_keys != keys) {
    return false;
  }
  // In 3.12 we can insert in any order, we just need to update the insertion
  // order
  return (
      val_offset != -1 || (val_offset = getDictKeysIndex(keys, name)) != -1);
}

#if PY_VERSION_HEX >= 0x030E0000
PyObject* SplitMutator::getAttrInline(PyObject* obj, PyObject* name) {
  JIT_DCHECK(val_offset != -1, "Should have value offset");
  PyDictValues* values = _PyObject_InlineValues(obj);
  if (!values->valid) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplit);
    return getAttr(obj, name);
  }
  PyObject* result = values->values[val_offset];
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
  return Py_NewRef(result);
}

PyObject* SplitMutator::getAttrSlowPath(
    PyObject* obj,
    PyObject* name,
    BorrowedRef<PyDictObject> dict) {
  PyObject* attr_o;
  int res = [&] {
    auto strong_ref = Ref<>::create(dict);
    return PyDict_GetItemRef(dict, name, &attr_o);
  }();
  if (res == 0) {
    return getAttrFallback(obj, name);
  }
  if (res == -1) {
    return nullptr;
  }
  return attr_o;
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);
  JIT_DCHECK(val_offset != -1, "Should have value offset");

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);
  if (dict->ma_keys != keys) {
    return getAttrSlowPath(obj, name, dict);
  }
  JIT_DCHECK(
      DK_IS_UNICODE(keys) && val_offset < keys->dk_nentries,
      "Expected dictionary keys object to change");
  PyObject* attr_o = dict->ma_values->values[val_offset];
  if (attr_o == nullptr) {
    return getAttrFallback(obj, name);
  }
  return Py_NewRef(attr_o);
}

int SplitMutator::setAttrInline(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  JIT_DCHECK(val_offset != -1, "Should have value offset");
  PyDictValues* values = _PyObject_InlineValues(obj);
  PyDictObject* dict = _PyObject_GetManagedDict(obj);
  if (!values->valid || dict) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplit);
    return setAttr(obj, name, value);
  }
  auto old_value = Ref<>::steal(values->values[val_offset]);
  values->values[val_offset] = Py_NewRef(value);
  if (!old_value) {
    _PyDictValues_AddToInsertionOrder(values, val_offset);
  }
  return 0;
}

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  JIT_DCHECK(val_offset != -1, "Should have value offset");
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);
  if (dict == nullptr) {
    return PyObject_SetAttr(obj, name, value);
  }
  if (keys != dict->ma_keys) {
    // Slow path
    auto strong_ref = Ref<>::create(dict);
    return PyDict_SetItem(dict, name, value);
  }
  _PyDict_InsertSplitValue(dict, name, value, val_offset);
  return 0;
}

#elif PY_VERSION_HEX >= 0x030C0000

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    // Values are stored in a values array not attached to a dictionary.
    JIT_DCHECK(val_offset != -1, "Should have value offset");

    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* old_value = values->values[val_offset];
    values->values[val_offset] = value;
    Py_INCREF(value);
    if (old_value == nullptr) {
      _PyDictValues_AddToInsertionOrder(values, val_offset);
    } else {
      Py_DECREF(old_value);
    }
    return 0;
  }

  // Dictionary has been materialized but may still be using shared keys.
  BorrowedRef<PyDictObject> dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    dict =
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr));
    if (dict == nullptr) {
      return -1;
    }
    Py_DECREF(dict);
  }

  if (dict == nullptr) {
    return -1;
  }

  if (canInsertToSplitDict(dict, name)) {
    PyObject* old_value = DICT_VALUES(dict.get())[val_offset];
    if (old_value == nullptr) {
      // Track insertion order on 3.12.
      _PyDictValues_AddToInsertionOrder(dict->ma_values, val_offset);
    }
    if (!_PyObject_GC_IS_TRACKED(dict.getObj())) {
      if (_PyObject_GC_MAY_BE_TRACKED(value)) {
        PyObject_GC_Track(dict.getObj());
      }
    }

    uint64_t new_version =
        _PyDict_NotifyEvent(PyDict_EVENT_MODIFIED, dict, name, value);

    Py_INCREF(value);
    DICT_VALUES(dict.get())[val_offset] = value;
    dict->ma_version_tag = new_version;

    if (old_value == nullptr) {
      dict->ma_used++;
    } else {
      Py_DECREF(old_value);
    }

    return 0;
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    JIT_DCHECK(val_offset != -1, "Should have value offset");

    // Values are stored in values w/o materialized dictionary
    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* result = values->values[val_offset];
    if (result == nullptr) {
      return getAttrFallback(obj, name);
    }
    Py_INCREF(result);
    return result;
  }

  PyDictObject* dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    return getAttrFallback(obj, name);
  }
  PyObject* result = nullptr;
  if (dict->ma_keys == keys) {
    JIT_DCHECK(val_offset != -1, "Should have value offset");
    // We are still sharing keys with the inline object.
    result = DICT_VALUES(dict)[val_offset];
  } else {
    auto dictobj = reinterpret_cast<PyObject*>(dict);
    Py_INCREF(dictobj);
    result = PyDict_GetItem(dictobj, name);
    Py_DECREF(dictobj);
  }
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
  Py_INCREF(result);
  return result;
}
#else
int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  PyDictValues* values = *_PyObject_ValuesPointer(obj);
  if (values == nullptr) {
    return PyObject_SetAttr(obj, name, value);
  }

  JIT_DCHECK(val_offset != -1, "Should have value offset");
  PyObject* old_value = values->values[val_offset];
  values->values[val_offset] = Py_NewRef(value);
  if (old_value == nullptr) {
    _PyDictValues_AddToInsertionOrder(values, val_offset);
  } else {
    Py_DECREF(old_value);
  }
  return 0;
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  PyDictValues* values = *_PyObject_ValuesPointer(obj);
  if (values == nullptr) {
    return PyObject_GetAttr(obj, name);
  }

  JIT_DCHECK(val_offset != -1, "Should have value offset");
  PyObject* result = values->values[val_offset];
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
  return Py_NewRef(result);
}
#endif // PY_VERSION_HEX < 0x030E0000

int CombinedMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  Ref<PyDictObject> dict = get_dict(obj, dict_offset);
  if (dict == nullptr) {
    dict = Ref<PyDictObject>::steal(
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr)));
    if (dict == nullptr) {
      return -1;
    }
  }
  return PyDict_SetItem(dict, name, value);
}

PyObject* CombinedMutator::getAttr(PyObject* obj, PyObject* name) {
  Ref<PyDictObject> dict = get_dict(obj, dict_offset);
  if (dict == nullptr) {
    return getAttrFallback(obj, name);
  }
#if PY_VERSION_HEX < 0x030C0000
  // slot_tp_getattr_hook protocol: the hook this call will use is
  // pinned BEFORE the lookup runs user code (a non-unicode key's __eq__
  // can unpublish it), a raised AttributeError is suppressed under the
  // hook and routes to it (no descriptor exists for this kind, so
  // continuing the generic lookup is "not found"), and any other error
  // -- or any error without the hook -- propagates.  The result is
  // strengthened while `dict` still holds its strong reference.
  GetAttrHookSnapshot311 hook(getattr_method);
  PyObject* result =
      PyDict_GetItemWithError(reinterpret_cast<PyObject*>(dict.get()), name);
  if (result == nullptr) {
    if (PyErr_Occurred()) {
      if (!hook.suppressAttributeError()) {
        return nullptr;
      }
      return hook.call(obj, name);
    }
    if (hook.active()) {
      return hook.call(obj, name);
    }
    return raise_attribute_error(obj, name);
  }
#else
  PyObject* result = PyDict_GetItem(dict, name);
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
#endif
  Py_INCREF(result);
  return result;
}

#if PY_VERSION_HEX >= 0x030E0000
int DictMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  Ref<PyDictObject> dict =
      Ref<PyDictObject>::create(_PyObject_GetManagedDict(obj));
  if (dict == nullptr) {
    dict = Ref<PyDictObject>::steal(
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr)));
    if (dict == nullptr) {
      return -1;
    }
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* DictMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);
  if (dict == nullptr) {
    return getAttrFallback(obj, name);
  }
  auto strong_ref = Ref<>::create(dict);
  PyObject* result = PyDict_GetItem(dict, name);
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
  return Py_NewRef(result);
}
#elif PY_VERSION_HEX < 0x030C0000
// 3.11 managed-dict arm (kDict fills only for managed types whose shared
// keys are full).  Stores may materialize the dict -- stock's generic
// STORE_ATTR does exactly that when the value cannot live in the shared
// keys -- but loads must not: _PyObject_GetDictPtr converts live inline
// values into a real dict on 3.11, so reads go through the managed-dict
// slot directly.
int DictMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  auto dict = Ref<>::steal(PyObject_GenericGetDict(obj, nullptr));
  if (dict == nullptr) {
    return -1;
  }
  return PyDict_SetItem(dict, name, value);
}

PyObject* DictMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict =
      reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
  if (dict == nullptr) {
    // Live inline values cannot hold this attribute: kDict was chosen
    // because the name is absent from the (full) shared keys, and the
    // pull-validated type version pins that keys era.
    return getAttrFallback(obj, name);
  }
  auto strong_ref = Ref<>::create(dict);
  // Same pinned-hook protocol as CombinedMutator: error-preserving
  // lookup, AttributeError suppressed under the hook and routed to it,
  // and the result strengthened while strong_ref still pins the dict (a
  // raising or dict-swapping __eq__ runs inside this lookup).
  GetAttrHookSnapshot311 hook(getattr_method);
  PyObject* result =
      PyDict_GetItemWithError(reinterpret_cast<PyObject*>(dict.get()), name);
  if (result == nullptr) {
    if (PyErr_Occurred()) {
      if (!hook.suppressAttributeError()) {
        return nullptr;
      }
      return hook.call(obj, name);
    }
    if (hook.active()) {
      return hook.call(obj, name);
    }
    return raise_attribute_error(obj, name);
  }
  return Py_NewRef(result);
}
#else
int DictMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  // Materialize the dict if needed (handles DictOrValues on 3.12).
  auto dict = Ref<>::steal(PyObject_GenericGetDict(obj, nullptr));
  if (dict == nullptr) {
    return -1;
  }
  return PyDict_SetItem(dict, name, value);
}

PyObject* DictMutator::getAttr(PyObject* obj, PyObject* name) {
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    // Inline values are still active. The attribute is not in the shared keys
    // (we wouldn't be using DictMutator otherwise), so it cannot be in the
    // inline values array.
    return getAttrFallback(obj, name);
  }
  BorrowedRef<PyDictObject> dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    return getAttrFallback(obj, name);
  }
  auto strong_ref = Ref<>::create(dict);
  PyObject* result = PyDict_GetItem(dict, name);
  if (result == nullptr) {
    return getAttrFallback(obj, name);
  }
  return Py_NewRef(result);
}
#endif

int DataDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  // Stock GenericSetAttr strengthens the descriptor across the slot call:
  // the slot runs arbitrary code and can drop the descriptor's last
  // reference (e.g. delete itself from the owner class) while its own
  // C frame is still executing.  The caller's strong reference is what
  // keeps `self` alive in stock; mirror it.
  auto descr_guard = Ref<>::create(descr);
  return Py_TYPE(descr)->tp_descr_set(descr, obj, value);
}

PyObject* DataDescrMutator::getAttr(PyObject* obj) {
  // Same ownership as stock GenericGetAttr: hold the descriptor across
  // the slot call.  The __getattr__ routing for a raising slot lives in
  // the dispatch layer, which pins the hook before this runs.
  auto descr_guard = Ref<>::create(descr);
  return Py_TYPE(descr)->tp_descr_get(descr, obj, (PyObject*)Py_TYPE(obj));
}

int MemberDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  return PyMember_SetOne((char*)obj, memberdef, value);
}

PyObject* MemberDescrMutator::getAttr(PyObject* obj) {
  return PyMember_GetOne((char*)obj, memberdef);
}

int DescrOrClassVarMutator::setAttr(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  descrsetfunc setter = Py_TYPE(descr)->tp_descr_set;
  if (setter != nullptr) {
    auto descr_guard = Ref<>::create(descr);
    return setter(descr, obj, value);
  }
#if PY_VERSION_HEX < 0x030C0000
  // Route the instance store through the stock generic path: it stores
  // into live inline values without materializing the dict, exactly as
  // the interpreter's STORE_ATTR does.  The dictptr-based store below
  // would materialize first (3.11's _PyObject_GetDictPtr converts live
  // values into a dict).
  return PyObject_GenericSetAttr(obj, name, value);
#else
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == nullptr) {
    PyErr_Format(
        PyExc_AttributeError,
        "'%.50s' object attribute '%U' is read-only",
        Py_TYPE(obj)->tp_name,
        name);
    return -1;
  }
  BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
  int st = Cix_PyObjectDict_SetItem(type, obj, dictptr, name, value);
  if (st < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
    PyErr_SetObject(PyExc_AttributeError, name);
  }
  return st;
#endif
}

PyObject* DescrOrClassVarMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
  descrsetfunc setter = descr_type->tp_descr_set;
  descrgetfunc getter = descr_type->tp_descr_get;

  auto descr_guard = Ref<>::create(descr);
  if (setter != nullptr && getter != nullptr) {
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

#if PY_VERSION_HEX < 0x030C0000
  // Check the instance storage for a shadowing attribute without
  // materializing: a matching nonzero keys version proves the name is
  // absent, anything else is peeked in place.  A raising lookup (a
  // non-unicode key's __eq__) aborts the generic part with the error in
  // place, exactly like stock's unsuppressed PyObject_GenericGetAttr
  // inside slot_tp_getattr_hook: on a hook receiver the DISPATCH layer
  // then clears an AttributeError and calls the pinned __getattr__; on
  // a plain receiver it propagates.
  if (keys_version == 0 || !isValidKeysVersion(keys_version, obj)) {
    PyObject* shadow = nullptr;
    int found = peekInstanceAttr311(obj, name, &shadow);
    if (found < 0) {
      return nullptr;
    }
    if (found != 0) {
      return shadow;
    }
  }
#else
  Ref<> dict;
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr) {
    dict.reset(*dictptr);
  }

  // Check instance dict.
  if (dict != nullptr) {
    if (keys_version == 0 ||
        reinterpret_cast<PyDictObject*>(dict.get())->ma_keys->dk_version !=
            keys_version) {
      auto res = Ref<>::create(PyDict_GetItem(dict, name));
      if (res != nullptr) {
        return res.release();
      }
    }
  }
#endif

  if (getter != nullptr) {
    // Non-data descriptor
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

  // Class var
  return descr_guard.release();
}

AttributeMutator::AttributeMutator() {
  reset();
}

void AttributeMutator::reset() {
  type_ = 0;
}

void AttributeMutator::set_combined(PyTypeObject* type) {
  set_type(type, Kind::kCombined);
  combined_.dict_offset = type->tp_dictoffset;
  combined_.getattr_method = getGetAttrForCaching(type);
}

void AttributeMutator::set_dict(PyTypeObject* type) {
  set_type(type, Kind::kDict);
  dict_.getattr_method = getGetAttrForCaching(type);
}

void AttributeMutator::set_data_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kDataDescr);
  data_descr_.descr = descr;
  data_descr_.descr_type = Py_TYPE(descr);
#if PY_VERSION_HEX < 0x030C0000
  // fill() ran ensureVersionTag(descr_type) before selecting this kind,
  // so the tag is valid here; a hit revalidates it (descrVersionMatches).
  data_descr_.descr_type_version = Py_TYPE(descr)->tp_version_tag;
#endif
}

void AttributeMutator::set_member_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kMemberDescr);
  member_descr_.memberdef = ((PyMemberDescrObject*)descr)->d_member;
  member_descr_.getattr_method = getGetAttrForCaching(type);
}

void AttributeMutator::set_descr_or_classvar(
    PyTypeObject* type,
    PyObject* descr,
    uint32_t keys_version) {
  set_type(type, Kind::kDescrOrClassVar);
  descr_or_cvar_.descr = descr;
  descr_or_cvar_.keys_version = keys_version;
}

void AttributeMutator::set_split(
    PyTypeObject* type,
    Py_ssize_t val_offset,
    [[maybe_unused]] PyDictKeysObject* keys,
    bool inline_values) {
  set_type(type, inline_values ? Kind::kSplitInline : Kind::kSplit);
  split_.val_offset = val_offset;
  split_.keys = keys;
}

void AttributeMutator::set_getattr(
    PyTypeObject* type,
    PyObject* getattr_method,
    uint32_t keys_version) {
  set_type(type, Kind::kGetAttr);
  getattr_.getattr_method = getattr_method;
  getattr_.keys_version = keys_version;
}

BorrowedRef<PyTypeObject> AttributeMutator::watchedDescrType() const {
  if (get_kind() == Kind::kDataDescr) {
    return data_descr_.descr_type;
  }
  return nullptr;
}

inline int
AttributeMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator setting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.setAttr(obj, name, value);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitInline:
      return split_.setAttrInline(obj, name, value);
#endif
    case AttributeMutator::Kind::kCombined:
      return combined_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kDict:
      return dict_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kDataDescr:
      return data_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kMemberDescr:
      return member_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kDescrOrClassVar:
      return descr_or_cvar_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kGetAttr:
      // __getattr__ only applies to loads, we shouldn't ever populate it for
      // a set attr cache.
    default:
      JIT_ABORT(
          "Cannot invoke setAttr for attr of kind {}", static_cast<int>(kind));
  }
}

inline PyObject* AttributeMutator::getAttr(PyObject* obj, PyObject* name) {
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator getting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.getAttr(obj, name);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitInline:
      return split_.getAttrInline(obj, name);
#endif
    case AttributeMutator::Kind::kCombined:
      return combined_.getAttr(obj, name);
    case AttributeMutator::Kind::kDict:
      return dict_.getAttr(obj, name);
    case AttributeMutator::Kind::kDataDescr: {
#if PY_VERSION_HEX < 0x030C0000
      // slot_tp_getattr_hook pins the __getattr__ it will use BEFORE the
      // generic part runs any user code; the descriptor slot below can
      // unpublish the hook, so a post-hoc live lookup is the wrong hook
      // -- or a dangling one.
      GetAttrHookSnapshot311 hook(obj);
      PyObject* result = data_descr_.getAttr(obj);
      if (result == nullptr && hook.suppressAttributeError()) {
        result = hook.call(obj, name);
      }
      return result;
#else
      PyObject* result = data_descr_.getAttr(obj);
      if (result == nullptr) {
        result = tryGetAttrFallback(type(), nullptr, obj, name);
      }
      return result;
#endif
    }
    case AttributeMutator::Kind::kMemberDescr: {
#if PY_VERSION_HEX < 0x030C0000
      // Same pinned-hook protocol as the other dispatch kinds: stock
      // slot_tp_getattr_hook pins the __getattr__ BEFORE the generic
      // part runs.  The member read itself runs no user code, but the
      // hook's own call can delete its owner entry mid-flight, so the
      // fill-time cached hook (its identity is proven by the
      // receiver-version hit check) is strengthened up front.  The live
      // tp_getattro re-check tryGetAttrFallback performs is subsumed by
      // that same version proof.
      GetAttrHookSnapshot311 hook(member_descr_.getattr_method);
      PyObject* result = member_descr_.getAttr(obj);
      if (result == nullptr && hook.suppressAttributeError()) {
        result = hook.call(obj, name);
      }
      return result;
#else
      PyObject* result = member_descr_.getAttr(obj);
      if (result == nullptr && member_descr_.getattr_method != nullptr) {
        result =
            tryGetAttrFallback(type(), member_descr_.getattr_method, obj, name);
      }
      return result;
#endif
    }
    case AttributeMutator::Kind::kDescrOrClassVar: {
#if PY_VERSION_HEX < 0x030C0000
      // Same pinned-hook protocol as kDataDescr: the mutator's probe and
      // descriptor slots run user code, so the hook this call uses is
      // captured first.  Stock's hook runs the generic part UNSUPPRESSED
      // and treats any escaping AttributeError -- probe __eq__,
      // descriptor slot, or not-found -- the same way: clear it and call
      // the pinned __getattr__.
      GetAttrHookSnapshot311 hook(obj);
      PyObject* result = descr_or_cvar_.getAttr(obj, name);
      if (result == nullptr && hook.suppressAttributeError()) {
        result = hook.call(obj, name);
      }
      return result;
#else
      PyObject* result = descr_or_cvar_.getAttr(obj, name);
      if (result == nullptr) {
        result = tryGetAttrFallback(type(), nullptr, obj, name);
      }
      return result;
#endif
    }
    case AttributeMutator::Kind::kGetAttr:
      return getattr_.getAttr(obj, name);
    default:
      JIT_ABORT(
          "Cannot invoke getAttr for attr of kind {}", static_cast<int>(kind));
  }
}

void AttributeMutator::set_type(PyTypeObject* type, Kind kind) {
  auto raw = reinterpret_cast<uintptr_t>(type);
  JIT_CHECK((raw & kindMask()) == 0, "PyTypeObject* expected to be aligned");
  auto mask = static_cast<uintptr_t>(kind);
  type_ = raw | mask;
#if PY_VERSION_HEX < 0x030C0000
  // Every fill path proved Ci_Type_HasValidVersionTag first, so this is a
  // live, nonzero tag; hits pull-validate against it.
  JIT_DCHECK(type->tp_version_tag != 0, "fill requires a valid version tag");
  type_version_ = type->tp_version_tag;
#endif
}

AttributeCache::AttributeCache() {
  for (auto& entry : entries()) {
    entry.reset();
  }
}

AttributeCache::~AttributeCache() {
  for (auto& entry : entries()) {
    if (entry.type() != nullptr) {
      ac_watcher.unwatch(entry.type(), this);
      BorrowedRef<PyTypeObject> descr_tp = entry.watchedDescrType();
      if (descr_tp != nullptr) {
        ac_descr_watcher.unwatch(descr_tp, this);
      }
      entry.reset();
    }
  }
}

void AttributeCache::typeChanged(PyTypeObject* tp) {
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
      BorrowedRef<PyTypeObject> descr_tp = entry.watchedDescrType();
      entry.reset();
      if (descr_tp != nullptr) {
        bool found = false;
        for (auto& other : entries()) {
          if (other.watchedDescrType() == descr_tp) {
            found = true;
            break;
          }
        }
        if (!found) {
          ac_descr_watcher.unwatch(descr_tp, this);
        }
      }
    }
  }
}

void AttributeCache::descrTypeChanged(PyTypeObject* tp) {
  bool found = false;
  for (auto& entry : entries()) {
    if (entry.watchedDescrType() == tp) {
      ac_watcher.unwatch(entry.type(), this);
      entry.reset();
      if (!found) {
        ac_descr_watcher.unwatch(tp, this);
        found = true;
      }
    }
  }
}

AttributeMutator* AttributeCache::findEmptyEntry() {
  auto it = std::ranges::find_if(
      entries(), [](const AttributeMutator& e) { return e.isEmpty(); });
  return it == entries().end() ? nullptr : &*it;
}

void AttributeCache::fill(BorrowedRef<> obj, BorrowedRef<> name, bool is_set) {
  BorrowedRef<> descr = _PyType_Lookup(Py_TYPE(obj), name);
  fill(obj, name, descr, is_set);
}

bool canCacheType(PyTypeObject* type) {
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    // We can cache values for types which have managed dictionaries on 3.12 or
    // later.
    return true;
  }

  // We only support the common case for objects - fixed-size instances
  // (tp_dictoffset > 0) of heap types (Py_TPFLAGS_HEAPTYPE).
  // tp_dictoffset == 0 means no instance dict, so nothing to cache.
  return type->tp_dictoffset > 0 &&
      PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE);
}

bool canCacheAttribute(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    uint32_t& keys_version) {
  if (type->tp_dictoffset == 0) {
    return true;
  }

  if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
    return false;
  }

  PyDictKeysObject* keys = getSplitKeys(type);
  if (keys == nullptr) {
    return false;
  }

  // If we don't have a valid keys version or the key exists in the shared
  // keys then we can't cache the value as we need to check and see if it's
  // been overridden.
  if (dictGetKeysVersion(PyInterpreterState_Get(), keys) == 0 ||
      getDictKeysIndex(keys, name) != -1) {
    return false;
  }
  keys_version = keys->dk_version;
  return true;
}

void AttributeCache::fill(
    BorrowedRef<> obj,
    BorrowedRef<> name,
    BorrowedRef<> descr,
    bool is_set) {
  BorrowedRef<PyTypeObject> type{Py_TYPE(obj)};
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  if ((is_set && type->tp_setattro != PyObject_GenericSetAttr) ||
      (!is_set && type->tp_getattro != PyObject_GenericGetAttr &&
       (type->tp_getattro != Ci_tp_getattr_hook ||
        !hookUsesGenericGetAttr(type)))) {
    // tp_ slot takes precedence. When tp_getattro is the __getattr__ hook,
    // we can only cache if the hook wraps PyObject_GenericGetAttr. For
    // metaclasses, the hook wraps type_getattro which does MRO search,
    // and our IC cannot replicate that.
    return;
  }

  AttributeMutator* mut = findEmptyEntry();
  if (mut == nullptr) {
    return;
  }

  if (descr != nullptr) {
    BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
    if (descr_type->tp_descr_get != nullptr &&
        descr_type->tp_descr_set != nullptr) {
      // Data descriptor
      if (descr_type == &PyMemberDescr_Type) {
#if PY_VERSION_HEX < 0x030C0000
        // Stock member_get() raises the object.__getattr__ audit event
        // for PY_AUDIT_READ members (traceback.tb_frame, frame.f_locals
        // era fields) BEFORE PyMember_GetOne, which itself never audits.
        // A cached hit would silently drop that user-visible side effect,
        // and replicating member_get() here would import the audit
        // hook's own reentrancy windows -- so reads of audited members
        // are fail-closed: never filled, every access runs the stock
        // path that owns the audit semantics.  Stores have no audit
        // (member_set is a plain PyMember_SetOne).
        auto* md = reinterpret_cast<PyMemberDescrObject*>(descr.get());
        if (!is_set && (md->d_member->flags & PY_AUDIT_READ)) {
          return;
        }
#endif
        mut->set_member_descr(type, descr);
      } else {
        if (!ensureVersionTag(descr_type)) {
          return;
        }
        // If someone modifies descr_type (e.g., deletes __set__), it may no
        // longer be a data descriptor, and the cache kind has to change.
        // On 3.12+ this watcher retires the entry; on 3.11 watch() is a
        // no-op and the hit path pull-validates the tag captured by
        // set_data_descr instead (descrVersionMatches).
        ac_descr_watcher.watch(descr_type, this);
        mut->set_data_descr(type, descr);
      }
    } else {
      // Non-data descriptor or class var.
      // We do NOT watch descr_type here: DescrOrClassVarMutator::getAttr
      // dynamically checks tp_descr_set/tp_descr_get at runtime, correctly
      // handling the transition if __set__ is added or removed.
      uint32_t keys_version = 0;
      canCacheAttribute(type, name, keys_version);
      mut->set_descr_or_classvar(type, descr, keys_version);
    }
    ac_watcher.watch(type, this);
    countAttrCacheFill311(is_set);
    return;
  }

  if (!canCacheType(type)) {
    return;
  }

  // Instance attribute with no shadowing. Specialize the lookup based on
  // whether or not the type is using split dictionaries.
  PyDictKeysObject* keys = getSplitKeys(type);
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
#if PY_VERSION_HEX < 0x030C0000
    // A 3.11 managed-dict heap type can be caught before its cached keys
    // exist; there is nothing to specialize against yet.
    if (keys == nullptr) {
      return;
    }
#else
    JIT_DCHECK(keys != nullptr, "Managed dict should have a split dict");
#endif
    bool inline_values = false;
#if PY_VERSION_HEX >= 0x030E0000
    inline_values = type->tp_flags & Py_TPFLAGS_INLINE_VALUES;
#endif

    Py_ssize_t index = getDictKeysIndex(keys, name);
    if (index == -1) {
      // no index exists, it could have not been set yet, the _shared
      // keys could be full, or we may have a type with __getattr__
      // where the attribute will never show up.
      if (!is_set && type->tp_getattro == Ci_tp_getattr_hook) {
        // unknown attribute and __getattr__ exists, dispatch to it
        BorrowedRef<> getattr_method = getGetAttrForCaching(type);
        if (getattr_method != nullptr) {
          uint32_t keys_version = keys->dk_version;
          if (!isValidKeysVersion(keys_version, obj)) {
            // The instance dict is not the shared dict. Set the keys version
            // to zero and the GetAttrMutator will just check for the
            // lack of the attribute.
            keys_version = 0;
          }
          mut->set_getattr(type, getattr_method, keys_version);
          ac_watcher.watch(type, this);
          countAttrCacheFill311(is_set);
          return;
        }
      }

      if (keys->dk_nentries == SHARED_KEYS_MAX_SIZE) {
        // The shared dict is full, fallback to dict access via managed
        // dict APIs.
        mut->set_dict(type);
        ac_watcher.watch(type, this);
        countAttrCacheFill311(is_set);
      }

      return;
    }

    // set_split handles __getattr__ fallback too
    mut->set_split(type, index, keys, inline_values);
  } else {
    // combined handles __getattr__ fallback too
    mut->set_combined(type);
  }
  ac_watcher.watch(type, this);
  countAttrCacheFill311(is_set);
}

int StoreAttrCache::invoke(
    StoreAttrCache* cache,
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  return cache->doInvoke(obj, name, value);
}

int StoreAttrCache::doInvoke(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
#if PY_VERSION_HEX < 0x030C0000
      // Pull-based invalidation: no watcher retires entries on 3.11, so a
      // hit must prove the receiver type is unchanged since fill -- and,
      // for kDataDescr, that the descriptor's own type still carries the
      // protocol the kind selection baked in (del D.__set__ would
      // otherwise reach a stale or NULL tp_descr_set).
      if (!entry.typeVersionMatches(tp) || !entry.descrVersionMatches()) {
        entry.reset();
        attrCacheStats311().store_attr.invalidations++;
        break;
      }
      attrCacheStats311().store_attr.hits++;
#endif
      return entry.setAttr(obj, name, value);
    }
  }
  return invokeSlowPath(obj, name, value);
}

int __attribute__((noinline))
StoreAttrCache::invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value) {
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().store_attr.misses++;
#endif
  int result = PyObject_SetAttr(obj, name, value);
  if (result < 0) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_SetAttr failed so there should be a Python error");
    return result;
  }

  fill(obj, name, /* is_set */ true);
  return result;
}

PyObject*
LoadAttrCache::invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name) {
  return cache->doInvoke(obj, name);
}

PyObject* LoadAttrCache::doInvoke(PyObject* obj, PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
#if PY_VERSION_HEX < 0x030C0000
      // Pull-based invalidation: no watcher retires entries on 3.11, so a
      // hit must prove the receiver type is unchanged since fill -- and,
      // for kDataDescr, that the descriptor's own type still carries the
      // protocol the kind selection baked in (a descriptor demoted to
      // non-data must yield to an instance shadow, which requires the
      // full lookup, not this dispatch).
      if (!entry.typeVersionMatches(tp) || !entry.descrVersionMatches()) {
        entry.reset();
        attrCacheStats311().load_attr.invalidations++;
        break;
      }
      attrCacheStats311().load_attr.hits++;
#endif
      return entry.getAttr(obj, name);
    }
  }
  return invokeSlowPath(obj, name);
}

PyObject* __attribute__((noinline)) LoadAttrCache::invokeSlowPath(
    PyObject* obj,
    PyObject* name) {
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().load_attr.misses++;
#endif
  auto result = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (result == nullptr) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_GetAttr failed so there should be a Python error");
    return nullptr;
  }
  fill(obj, name, /* is_set */ false);

  return result.release();
}

LoadTypeAttrCache::LoadTypeAttrCache() {
  reset();
}

LoadTypeAttrCache::~LoadTypeAttrCache() {
  ltac_watcher.unwatch(type_, this);
}

#if PY_VERSION_HEX < 0x030C0000
bool LoadTypeAttrCache::hitIsValid311(
    BorrowedRef<PyTypeObject> receiver) const {
  if (!pull_valid_ || type_ != receiver) {
    return false;
  }
  // Owner MRO era.  A stale-and-reallocated type cannot revalidate: the
  // version stream is global and monotonic, so a fresh type at a recycled
  // address never carries the captured number.
  if (receiver->tp_version_tag != type_version_) {
    return false;
  }
  // Metaclass era.  Read through the LIVE metatype: the captured pointer
  // is only compared, never dereferenced, so a dead metatype cannot be
  // touched here.
  BorrowedRef<PyTypeObject> live_metatype{Py_TYPE(receiver.get())};
  if (live_metatype != metatype_ ||
      live_metatype->tp_version_tag != metatype_version_) {
    return false;
  }
  // Descriptor era, when the cachability decision depended on one.
  if (value_guard_type_ != nullptr) {
    if (value_ == nullptr || Py_TYPE(value_) != value_guard_type_ ||
        value_guard_type_->tp_version_tag != value_guard_version_) {
      return false;
    }
  }
  return true;
}
#endif

PyObject* LoadTypeAttrCache::invoke(
    LoadTypeAttrCache* cache,
    PyObject* obj,
    PyObject* name) {
#if PY_VERSION_HEX < 0x030C0000
  // 3.11 has no inline fast path for this cache (simplify routes every
  // type-receiver load through this helper), so the pull check lives here
  // and a hit is a plain owned return of the cached value.
  if (PyType_Check(obj)) {
    BorrowedRef<PyTypeObject> receiver{obj};
    if (cache->hitIsValid311(receiver)) {
      attrCacheStats311().load_type_attr.hits++;
      return Py_NewRef(cache->value_);
    }
    if (cache->pull_valid_ && cache->type_ == receiver) {
      cache->reset();
      attrCacheStats311().load_type_attr.invalidations++;
    }
  }
  attrCacheStats311().load_type_attr.misses++;
#endif
  // The 3.12+ fast path is handled by direct memory access via valueAddr().
  return cache->invokeSlowPath(obj, name);
}

PyTypeObject** LoadTypeAttrCache::typeAddr() {
  return &type_;
}

PyObject** LoadTypeAttrCache::valueAddr() {
  return &value_;
}

// NB: This function needs to be kept in sync with PyType_Type.tp_getattro.
PyObject* LoadTypeAttrCache::invokeSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> metatype{Py_TYPE(obj)};
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    return PyObject_GetAttr(obj, name);
  }

  BorrowedRef<PyTypeObject> type{obj};
  if (PyType_Ready(type) < 0) {
    return nullptr;
  }

  descrgetfunc meta_get = nullptr;
  auto meta_attribute = Ref<>::create(_PyType_Lookup(metatype, name));
  if (meta_attribute != nullptr) {
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;
    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      // Data descriptors implement tp_descr_set to intercept writes. Assume the
      // attribute is not overridden in type's tp_dict (and bases): call the
      // descriptor now.
      return meta_get(meta_attribute, type, metatype);
    }
  }

  // No data descriptor found on metatype. Look in tp_dict of this type and its
  // bases.
  auto attribute = Ref<>::create(_PyType_Lookup(type, name));
  if (attribute != nullptr) {
    // Implement descriptor functionality, if any.
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;

    meta_attribute.reset();

    bool is_cachable = local_get == nullptr;
    // The type whose descriptor protocol the cachability decision rested
    // on, when that type is user-mutable.  A plain value is cachable only
    // because ITS type has no tp_descr_get today; functions and
    // staticmethods are decided by identity against static builtin types,
    // which cannot gain __get__ from Python, so they need no guard.
    BorrowedRef<PyTypeObject> value_guard_type{Py_TYPE(attribute)};
    if (PyFunction_Check(attribute)) {
      // Loading a function from a type returns the type
      is_cachable = true;
      value_guard_type = nullptr;
    } else if (Py_TYPE(attribute) == &PyStaticMethod_Type) {
      // static method returns the underlying object
      attribute = Ref<>::create(Ci_PyStaticMethod_GetFunc(attribute));
      is_cachable = true;
      value_guard_type = nullptr;
    }
    if (!is_cachable) {
      // nullptr 2nd argument indicates the descriptor was found on the target
      // object itself (or a base).
      return local_get(attribute, nullptr, type);
    }

    fill(type, attribute, metatype, value_guard_type);
    return attribute.release();
  }

  // No attribute found in local __dict__ (or bases): use the descriptor from
  // the metatype, if any.
  if (meta_get != nullptr) {
    return meta_get(meta_attribute, type, metatype);
  }

  // If an ordinary attribute was found on the metatype, return it now.
  if (meta_attribute != nullptr) {
    return meta_attribute.release();
  }

  // Give up.
  raise_attribute_error(obj, name);
  return nullptr;
}

void LoadTypeAttrCache::typeChanged(
    [[maybe_unused]] BorrowedRef<PyTypeObject> arg) {
  JIT_DCHECK(arg == type_, "Type watcher notified the wrong LoadTypeAttrCache");
  reset();
}

void LoadTypeAttrCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    [[maybe_unused]] BorrowedRef<PyTypeObject> metatype,
    [[maybe_unused]] BorrowedRef<PyTypeObject> value_guard_type) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }
#if PY_VERSION_HEX < 0x030C0000
  // Every fact the hit re-proves must be pinnable, or the entry is not
  // filled at all: a half-guarded entry is worse than the generic path.
  // Version tags are assigned lazily, so ask for one (as the instance
  // caches do for descriptor types) instead of walking away from a type
  // that simply has not been looked up yet.
  if (!ensureVersionTag(metatype) ||
      (value_guard_type != nullptr && !ensureVersionTag(value_guard_type))) {
    return;
  }
#endif

  ltac_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
#if PY_VERSION_HEX < 0x030C0000
  type_version_ = type->tp_version_tag;
  metatype_ = metatype;
  metatype_version_ = metatype->tp_version_tag;
  value_guard_type_ = value_guard_type;
  value_guard_version_ =
      value_guard_type != nullptr ? value_guard_type->tp_version_tag : 0;
  pull_valid_ = true;
  attrCacheStats311().load_type_attr.fills++;
#endif
  ltac_watcher.watch(type_, this);
}

void LoadTypeAttrCache::reset() {
  // We need to return a PyTypeObject* even in the empty case so that subsequent
  // refcounting operations work correctly.
  type_ = &s_empty_type_attr_cache;
  value_ = nullptr;
#if PY_VERSION_HEX < 0x030C0000
  pull_valid_ = false;
  type_version_ = 0;
  metatype_ = nullptr;
  metatype_version_ = 0;
  value_guard_type_ = nullptr;
  value_guard_version_ = 0;
#endif
}

std::string_view kCacheMissReasons[] = {
#define NAME_REASON(reason) #reason,
    FOREACH_CACHE_MISS_REASON(NAME_REASON)
#undef NAME_REASON
};

std::string_view cacheMissReason(CacheMissReason reason) {
  return kCacheMissReasons[static_cast<size_t>(reason)];
}

LoadMethodCache::~LoadMethodCache() {
  for (auto& entry : entries_) {
    if (entry.type != nullptr) {
      lm_watcher.unwatch(entry.type, this);
      entry.type.reset();
      entry.value.reset();
    }
  }
}

LoadMethodResult LoadMethodCache::lookupHelper(
    LoadMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

PyObject* GetAttrMutator::getAttr(PyObject* obj, PyObject* name) {
  // Make sure the attribute we're cached against isn't overridden. We can
  // either have cached against a dictionary which doesn't have split keys
  // (keys_version == 0) or a dictionary with split keys w/ a valid version
  // which doesn't include the key.

  if (keys_version == 0) {
    // dictionary w/o split keys, see if the value has been overridden, if not
    // call __getattr__.
#if PY_VERSION_HEX >= 0x030E0000
    Ref<PyDictObject> dict = get_dict(obj, MANAGED_DICT_OFFSET);
    if (dict == nullptr) {
      return callGetAttr(getattr_method, obj, name);
    }
    PyObject* res =
        PyDict_GetItem(reinterpret_cast<PyObject*>(dict.get()), name);
    if (res != nullptr) {
      return Py_NewRef(res);
    }
    return callGetAttr(getattr_method, obj, name);
#elif PY_VERSION_HEX < 0x030C0000
    // 3.11: peek the instance storage without materializing -- live inline
    // values are scanned through the cached keys, a real dict is read in
    // place.  _PyObject_GetDictPtr would convert values into a dict here.
    // The hook is pinned before the peek (its __eq__ window can
    // unpublish it); a peek-raised AttributeError is suppressed and --
    // the name being absent from the pinned type -- lands on the hook,
    // like every other empty-handed outcome.  Other errors propagate.
    GetAttrHookSnapshot311 hook(getattr_method);
    PyObject* res = nullptr;
    int found = peekInstanceAttr311(obj, name, &res);
    if (found < 0) {
      if (!hook.suppressAttributeError()) {
        return nullptr;
      }
      return hook.call(obj, name);
    }
    if (found != 0) {
      return res;
    }
    return hook.call(obj, name);
#else
    // On 3.12 the managed-dict slot may hold a tagged inline-values pointer
    // rather than a real dict. We must check for inline values *before*
    // treating the slot as a PyObject*, otherwise get_dict() would incref a
    // tagged (misaligned) pointer and corrupt the heap.
    PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
    if (!_PyDictOrValues_IsValues(dorv)) {
      BorrowedRef<PyDictObject> dict =
          reinterpret_cast<PyDictObject*>(_PyDictOrValues_GetDict(dorv));
      if (dict == nullptr) {
        return callGetAttr(getattr_method, obj, name);
      }
      Ref<PyDictObject> dict_ref = Ref<PyDictObject>::create(dict);
      PyObject* res =
          PyDict_GetItem(reinterpret_cast<PyObject*>(dict.get()), name);
      if (res != nullptr) {
        return Py_NewRef(res);
      }
      return callGetAttr(getattr_method, obj, name);
    }
    // we have inline values, try and reset again.
#endif
  } else if (isValidKeysVersion(keys_version, obj)) {
    return callGetAttr(getattr_method, obj, name);
  }

  AttributeMutator::from(this)->reset();
  return PyObject_GetAttr(obj, name);
}

LoadMethodResult LoadMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);

  for (auto& entry : entries_) {
    if (entry.type == tp) {
#if PY_VERSION_HEX < 0x030C0000
      // Pull-based invalidation: no watcher retires entries on 3.11.
      if (tp->tp_version_tag != entry.type_version) {
        entry.type.reset();
        entry.value.reset();
        attrCacheStats311().load_method.invalidations++;
        continue;
      }
#endif
      if (!isValidKeysVersion(entry.keys_version, obj)) {
        continue;
      }

#if PY_VERSION_HEX < 0x030C0000
      attrCacheStats311().load_method.hits++;
#endif
      PyObject* result = entry.value;
      Py_INCREF(result);
      Py_INCREF(obj);
      return {result, obj};
    }
  }

  return lookupSlowPath(obj, name);
}

void LoadMethodCache::typeChanged(PyTypeObject* type) {
  for (auto& entry : entries_) {
    if (entry.type == type) {
      entry.type.reset();
      entry.value.reset();
    }
  }
}

void LoadMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadMethodCache::cacheStats() {
  return cache_stats_.get();
}

LoadMethodResult __attribute__((noinline)) LoadMethodCache::lookupSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  PyTypeObject* tp = Py_TYPE(obj);
  PyObject* descr;
  descrgetfunc f = nullptr;
  PyObject* attr;
  bool is_method = false;
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().load_method.misses++;
#endif

  if ((tp->tp_getattro != PyObject_GenericGetAttr)) {
    PyObject* res = PyObject_GetAttr(obj, name);
    if (res != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kWrongTpGetAttro);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    return {nullptr, nullptr};
  } else if (_PyType_GetDict(tp) == nullptr && PyType_Ready(tp) < 0) {
    return {nullptr, nullptr};
  }

  descr = _PyType_Lookup(tp, name);
  if (descr != nullptr) {
    Py_INCREF(descr);
    if (PyFunction_Check(descr) || Py_TYPE(descr) == &PyMethodDescr_Type ||
        PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
      is_method = true;
    } else {
      f = descr->ob_type->tp_descr_get;
      if (f != nullptr && PyDescr_IsData(descr)) {
        maybeCollectCacheStats(
            cache_stats_, tp, name, CacheMissReason::kPyDescrIsData);
        PyObject* result = f(descr, obj, (PyObject*)obj->ob_type);
        Py_DECREF(descr);
        if (result == nullptr) {
          return {nullptr, nullptr};
        }
        Py_INCREF(Py_None);
        return {Py_None, result};
      }
    }
  }

#if PY_VERSION_HEX < 0x030C0000
  // Do not use _PyObject_GetDictPtr here: a cache miss must not
  // materialize the instance dict (it converts live inline values on
  // 3.11).  The peek reads values or the dict in place; a raising lookup
  // propagates as the pair-of-nullptrs error shape.
  attr = nullptr;
  {
    int found = peekInstanceAttr311(obj, name, &attr);
    if (found < 0) {
      Py_XDECREF(descr);
      return {nullptr, nullptr};
    }
    if (found != 0) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kUncategorized);
      Py_XDECREF(descr);
      Py_INCREF(Py_None);
      return {Py_None, attr};
    }
  }
#else
  PyObject **dictptr, *dict;
  dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr && (dict = *dictptr) != nullptr) {
    Py_INCREF(dict);
    attr = PyDict_GetItem(dict, name);
    if (attr != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kUncategorized);
      Py_INCREF(attr);
      Py_DECREF(dict);
      Py_XDECREF(descr);
      Py_INCREF(Py_None);
      return {Py_None, attr};
    }
    Py_DECREF(dict);
  }
#endif

  if (is_method) {
    fill(tp, descr, name);
    Py_INCREF(obj);
    return {descr, obj};
  }

  if (f != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    PyObject* result = f(descr, obj, (PyObject*)Py_TYPE(obj));
    Py_DECREF(descr);
    if (result == nullptr) {
      return {nullptr, nullptr};
    }
    Py_INCREF(Py_None);
    return {Py_None, result};
  }

  if (descr != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, descr};
  }

  raise_attribute_error(obj, name);
  return {nullptr, nullptr};
}

void LoadMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    BorrowedRef<> name) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  for (auto& entry : entries_) {
    if (entry.type == nullptr) {
      uint32_t keys_version = 0;
      if (!canCacheAttribute(type, name, keys_version)) {
        break;
      }

      lm_watcher.watch(type, this);
      entry.type = type;
      entry.value = value;
      entry.keys_version = keys_version;
#if PY_VERSION_HEX < 0x030C0000
      entry.type_version = type->tp_version_tag;
      attrCacheStats311().load_method.fills++;
#endif
      return;
    }
  }
}

LoadTypeMethodCache::~LoadTypeMethodCache() {
  if (type_ != nullptr) {
    ltm_watcher.unwatch(type_, this);
  }
}

LoadMethodResult LoadTypeMethodCache::lookupHelper(
    LoadTypeMethodCache* cache,
    PyTypeObject* obj,
    PyObject* name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadTypeMethodCache::getValueHelper(
    LoadTypeMethodCache* cache,
    PyObject* obj) {
#if PY_VERSION_HEX < 0x030C0000
  // Pull-based invalidation: the inline fast path only proved pointer
  // equality with the cached receiver type; the entry is usable only if
  // that type is unchanged since fill.  getValueHelper does not receive
  // the name, so the stale path re-runs the full lookup with the per-site
  // name captured at fill.
  BorrowedRef<PyTypeObject> receiver{obj};
  // Owner MRO era, then metaclass era: the fill below only happened
  // because the metatype routes through type_getattro and holds no data
  // descriptor of this name, and mutating the metaclass never bumps the
  // owner's own version tag.  The captured metatype pointer is compared,
  // never dereferenced, so a dead metaclass cannot be touched here.
  BorrowedRef<PyTypeObject> live_metatype{Py_TYPE(receiver.get())};
  if (receiver->tp_version_tag != cache->type_version_ ||
      live_metatype != cache->metatype_ ||
      live_metatype->tp_version_tag != cache->metatype_version_) {
    cache->type_ = nullptr;
    cache->value_.reset();
    cache->metatype_ = nullptr;
    cache->metatype_version_ = 0;
    attrCacheStats311().load_type_method.invalidations++;
    return cache->lookup(receiver, cache->name_);
  }
  attrCacheStats311().load_type_method.hits++;
#endif
  PyObject* result = cache->value_;
  Py_INCREF(result);
  if (cache->is_unbound_meth_) {
    Py_INCREF(obj);
    return {result, obj};
  }
  Py_INCREF(Py_None);
  return {Py_None, result};
}

// This needs to be kept in sync with PyType_Type.tp_getattro.
LoadMethodResult LoadTypeMethodCache::lookup(
    BorrowedRef<PyTypeObject> obj,
    BorrowedRef<> name) {
#if PY_VERSION_HEX < 0x030C0000
  // One cache per LoadMethod site, so the name is a per-site constant;
  // capture it for getValueHelper's stale re-lookup.
  name_ = name;
  attrCacheStats311().load_type_method.misses++;
#endif
  PyTypeObject* metatype = Py_TYPE(obj);
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kWrongTpGetAttro);
    PyObject* res = PyObject_GetAttr(obj, name);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }
  if (_PyType_GetDict(obj) == nullptr) {
    if (PyType_Ready(obj) < 0) {
      return {nullptr, nullptr};
    }
  }

  descrgetfunc meta_get = nullptr;
  PyObject* meta_attribute = _PyType_Lookup(metatype, name);
  if (meta_attribute != nullptr) {
    Py_INCREF(meta_attribute);
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;

    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      /* Data descriptors implement tp_descr_set to intercept
       * writes. Assume the attribute is not overridden in
       * type's tp_dict (and bases): call the descriptor now.
       */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kPyDescrIsData);
      PyObject* res =
          meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
      Py_DECREF(meta_attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
  }

  /* No data descriptor found on metatype. Look in tp_dict of this
   * type and its bases */
  PyObject* attribute = _PyType_Lookup(obj, name);
  if (attribute != nullptr) {
    Py_XDECREF(meta_attribute);
    BorrowedRef<PyTypeObject> attribute_type = Py_TYPE(attribute);
    if (attribute_type == &PyClassMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyClassMethod_GetFunc(attribute);
      if (Py_TYPE(cm_callable) == &PyFunction_Type) {
        Py_INCREF(obj);
        Py_INCREF(cm_callable);

        // Get the underlying callable from classmethod and return the
        // callable alongside the class object, allowing the runtime to call
        // the method as an unbound method.
        fill(obj, cm_callable, true);
        return {cm_callable, obj};
      } else if (Py_TYPE(cm_callable)->tp_descr_get != nullptr) {
        // cm_callable has custom tp_descr_get that can run arbitrary
        // user code. Do not cache in this instance.
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        Py_INCREF(Py_None);
        return {
            Py_None, Py_TYPE(cm_callable)->tp_descr_get(cm_callable, obj, obj)};
      } else {
        // It is not safe to cache custom objects decorated with classmethod
        // as they can be modified later
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        BorrowedRef<> py_meth = PyMethod_New(cm_callable, obj);
        Py_INCREF(Py_None);
        return {Py_None, py_meth};
      }
    }
    if (attribute_type == &PyStaticMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyStaticMethod_GetFunc(attribute);
      Py_INCREF(cm_callable);
      Py_INCREF(Py_None);
      fill(obj, cm_callable, false);
      return {Py_None, cm_callable};
    }
    if (PyFunction_Check(attribute)) {
      Py_INCREF(attribute);
      Py_INCREF(Py_None);
      fill(obj, attribute, false);
      return {Py_None, attribute};
    }
    Py_INCREF(attribute);
    /* Implement descriptor functionality, if any */
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;
    if (local_get != nullptr) {
      /* nullptr 2nd argument indicates the descriptor was
       * found on the target object itself (or a base)  */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kUncategorized);
      PyObject* res = local_get(attribute, nullptr, obj);
      Py_DECREF(attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, attribute};
  }

  /* No attribute found in local __dict__ (or bases): use the
   * descriptor from the metatype, if any */
  if (meta_get != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    PyObject* res;
    res = meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
    Py_DECREF(meta_attribute);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }

  /* If an ordinary attribute was found on the metatype, return it now */
  if (meta_attribute != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, meta_attribute};
  }

  raise_attribute_error(obj, name);
  return {nullptr, nullptr};
}

PyTypeObject** LoadTypeMethodCache::typeAddr() {
  return &type_;
}

BorrowedRef<> LoadTypeMethodCache::value() {
  return value_;
}

void LoadTypeMethodCache::typeChanged(BorrowedRef<PyTypeObject> /* type */) {
  type_ = nullptr;
  value_.reset();
}

void LoadTypeMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadTypeMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadTypeMethodCache::cacheStats() {
  return cache_stats_.get();
}

void LoadTypeMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    bool is_unbound_meth) {
#if PY_VERSION_HEX < 0x030C0000
  // The metaclass fact must be pinnable too, or the entry is not filled:
  // a half-guarded entry is worse than the generic path.  Tags are lazy,
  // so ask for one rather than refusing a merely-unvisited metaclass.
  if (!ensureVersionTag(Py_TYPE(type.get()))) {
    return;
  }
#endif
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  ltm_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
  is_unbound_meth_ = is_unbound_meth;
#if PY_VERSION_HEX < 0x030C0000
  type_version_ = type->tp_version_tag;
  metatype_ = Py_TYPE(type.get());
  metatype_version_ = metatype_->tp_version_tag;
  attrCacheStats311().load_type_method.fills++;
#endif
  ltm_watcher.watch(type_, this);
}

PyObject* LoadModuleAttrCache::lookupHelper(
    LoadModuleAttrCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

static BorrowedRef<PyDictObject> getModuleDict(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return mod->md_dict;
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return mod->globals;
  }
  return nullptr;
}

PyObject* LoadModuleAttrCache::lookup(
    BorrowedRef<> object,
    BorrowedRef<> name) {
  // First, check if we can use the cached value. If we can, we will return a
  // new reference to it.
#if PY_VERSION_HEX >= 0x030E0000
  if (module_ == object && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return Py_NewRef(res);
    }
  }
#else
  if (module_ == object && value_ != nullptr) {
    if (version_ == getModuleVersion(object)) {
#if PY_VERSION_HEX < 0x030C0000
      attrCacheStats311().load_module_attr.hits++;
#endif
      return Py_NewRef(value_);
    }
#if PY_VERSION_HEX < 0x030C0000
    // The module dict moved on; the entry is retired by the refill below.
    attrCacheStats311().load_module_attr.invalidations++;
#endif
  }
#endif

  // Otherwise, we will fall back to the slow path.
  return lookupSlowPath(object, name);
}

static std::pair<ci_dict_version_tag_t, PyObject*> getModuleAttribute(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  BorrowedRef<PyDictObject> dict = getModuleDict(obj);

  if (dict != nullptr &&
      (tp->tp_getattro == PyModule_Type.tp_getattro ||
       tp->tp_getattro == Ci_StrictModule_Type.tp_getattro) &&
      _PyType_Lookup(tp, name) == nullptr) {
    return {Ci_DictVersionTag(dict), PyDict_GetItemWithError(dict, name)};
  }

  return {0, nullptr};
}

PyObject* __attribute__((noinline))
LoadModuleAttrCache::lookupSlowPath(BorrowedRef<> object, BorrowedRef<> name) {
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().load_module_attr.misses++;
#endif
  auto [version, value] = getModuleAttribute(object, name);

  if (value != nullptr) {
#if PY_VERSION_HEX >= 0x030E0000
    PyObject* dict = getModuleDict(object);
    BorrowedRef<PyUnicodeObject> uname{name};
    if (hasOnlyUnicodeKeys(dict)) {
      cache_ = cinderx::getModuleState()->cache_manager->getGlobalCache(
          dict, dict, uname);
    }
#else
    value_ = value;
    version_ = version;
#if PY_VERSION_HEX < 0x030C0000
    attrCacheStats311().load_module_attr.fills++;
#endif
#endif
    module_ = object;

    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    return Py_NewRef(value);
  }

  auto generic = Ref<>::steal(PyObject_GetAttr(object, name));
  return generic == nullptr ? nullptr : generic.release();
}

LoadMethodResult LoadModuleMethodCache::lookupHelper(
    LoadModuleMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadModuleMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
#if PY_VERSION_HEX >= 0x030E0000
  if (module_obj_ == obj && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return {Py_None, Py_NewRef(res)};
    }
  }
#else
  if (module_obj_ == obj && value_ != nullptr) {
    BorrowedRef<PyDictObject> dict = getModuleDict(obj);
    ci_dict_version_tag_t version =
        dict != nullptr ? Ci_DictVersionTag(dict) : 0;
    if (module_version_ == version) {
#if PY_VERSION_HEX < 0x030C0000
      attrCacheStats311().load_module_method.hits++;
#endif
      // Py_NewRef on both halves: the LoadMethodResult contract hands the
      // caller owned references, and 3.11's None is mortal -- returning a
      // borrowed None here over-decrefed it (fatal at shutdown).  The
      // immortal-None arms above could not see this.
      return {Py_NewRef(Py_None), Py_NewRef(value_)};
    }
#if PY_VERSION_HEX < 0x030C0000
    attrCacheStats311().load_module_method.invalidations++;
#endif
  }
#endif

  return lookupSlowPath(obj, name);
}

BorrowedRef<> LoadModuleMethodCache::moduleObj() {
  return module_obj_;
}

#if PY_VERSION_HEX < 0x030E0000
BorrowedRef<> LoadModuleMethodCache::value() {
  return value_;
}
#endif

LoadMethodResult __attribute__((noinline))
LoadModuleMethodCache::lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name) {
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().load_module_method.misses++;
#endif
  auto [version, res] = getModuleAttribute(obj, name);

  if (res != nullptr) {
    if (PyFunction_Check(res) || PyCFunction_Check(res) ||
        Py_TYPE(res) == &PyMethodDescr_Type) {
      module_obj_ = obj;
#if PY_VERSION_HEX >= 0x030E0000
      BorrowedRef<PyUnicodeObject> uname{name};
      cache_ = cinderx::getModuleState()->cache_manager->getGlobalCache(
          getModuleDict(obj), getModuleDict(obj), uname);
#else
      module_version_ = version;
      value_ = res;
#if PY_VERSION_HEX < 0x030C0000
      attrCacheStats311().load_module_method.fills++;
#endif
#endif
    }
    // Both halves are owned by the caller (LoadMethodResult contract),
    // and 3.11's None is mortal: the value half is strengthened from the
    // borrowed dict entry, and None must be owned exactly like the hit
    // arm above -- a borrowed None here leaks a decref per slow-path
    // call.  On 3.12+ None is immortal and the NewRef is free.
    return {Py_NewRef(Py_None), Py_NewRef(res)};
  }
  auto generic_res = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (generic_res != nullptr) {
    return {Py_NewRef(Py_None), generic_res.release()};
  }
  return {nullptr, nullptr};
}

#if PY_VERSION_HEX < 0x030C0000
AttrCacheStats311& attrCacheStats311() {
  static AttrCacheStats311 stats;
  return stats;
}
#endif

void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type) {
  ac_watcher.typeChanged(type);
  ac_descr_watcher.typeChanged(
      type, [](AttributeCache* cache, PyTypeObject* tp) {
        cache->descrTypeChanged(tp);
      });
  ltac_watcher.typeChanged(type);
  lm_watcher.typeChanged(type);
  ltm_watcher.typeChanged(type);
}

} // namespace jit
