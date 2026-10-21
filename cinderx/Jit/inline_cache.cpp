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
#include <string>

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
  if (!getConfig().collect_attr_cache_stats || stat == nullptr) {
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

// The metaclass counterpart: a metaclass that defines __getattr__ carries
// the slot hook, but the hook's first half is whatever __getattribute__
// resolves to on the metaclass.  Only type.__getattribute__ (type_getattro)
// is what the type-receiver cache replicates; a metaclass that overrides
// __getattribute__ must not be cached (it may hide class attributes and
// route them to __getattr__).
bool metaclassHookUsesTypeGetAttr(BorrowedRef<PyTypeObject> metatype) {
  static PyObject* type_getattribute =
      PyDict_GetItemString(PyType_Type.tp_dict, "__getattribute__");
  BorrowedRef<> getattribute =
      _PyType_Lookup(metatype, &_Py_ID(__getattribute__));
  return type_getattribute != nullptr && getattribute == type_getattribute;
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

#if PY_VERSION_HEX < 0x030C0000
// a kSplit entry stays "valid" for as long as the type
// version holds, yet once this type's instances materialize and re-key
// their dicts every access pays the full-probe slow path forever -- the
// cache never re-kinds on its own (the yaml Reader/Scanner/Emitter
// pattern).  Demote to kDict: the hint-indexed read serves materialized
// dicts (asm kdict arm on hits), and values-form receivers still take the
// type-cached-keys values read inside DictMutator.  set_dict re-derives
// the version from the live tag, which the caller's pull validation just
// proved current.
PyObject* AttributeMutator::demoteSplitToDict(
    SplitMutator* split,
    PyObject* obj,
    PyObject* name) {
  AttributeMutator* mutator = from(split);
  mutator->set_dict(mutator->type());
  return mutator->dict_.getAttr(obj, name);
}

int AttributeMutator::demoteSplitToDictStore(
    SplitMutator* split,
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  AttributeMutator* mutator = from(split);
  mutator->set_dict(mutator->type());
  return mutator->dict_.setAttr(obj, name, value);
}
#endif

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
// a materialized receiver used to fall all the way to
// PyObject_SetAttr/PyObject_GetAttr (full generic lookup + _PyType_Lookup)
// even though the split entry knows the exact slot.  When the dict still
// shares the captured keys, the value sits at ma_values[val_offset]; a
// re-keyed dict does one dict probe.  Class-side shadowing stays pinned
// by the entry's pull-validated type version (descr was NULL at fill).
int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  PyDictValues* values = *_PyObject_ValuesPointer(obj);
  if (values == nullptr) {
    PyDictObject* dict =
        reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
    if (dict != nullptr && dict->ma_keys == keys &&
        dict->ma_values != nullptr && val_offset >= 0 &&
        static_cast<size_t>(val_offset) + 2 <
            reinterpret_cast<const uint8_t*>(dict->ma_values)[-1]) {
      if (dict->ma_values->values[val_offset] != nullptr) {
        // Overwrite only; an insertion needs the insertion-order and
        // ma_used bookkeeping of the generic path.  The dict's version
        // advances as stock's PyDict_SetItem would advance it.
        replaceMaterializedDictValue311(
            dict, &dict->ma_values->values[val_offset], value);
        return 0;
      }
    }
    // Store side of the demotion: a re-keyed materialized dict is a
    // permanent state; re-kind the entry so future stores take the
    // hint-indexed overwrite instead of the full generic setattr chain.
    if (dict != nullptr && dict->ma_keys != keys) {
      return AttributeMutator::demoteSplitToDictStore(this, obj, name, value);
    }
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
    PyDictObject* dict =
        reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
    if (dict != nullptr) {
      if (dict->ma_keys == keys && dict->ma_values != nullptr &&
          val_offset >= 0 &&
          static_cast<size_t>(val_offset) + 2 <
              reinterpret_cast<const uint8_t*>(dict->ma_values)[-1]) {
        PyObject* result = dict->ma_values->values[val_offset];
        if (result != nullptr) {
          return Py_NewRef(result);
        }
        return getAttrFallback(obj, name);
      }
      // A re-keyed dict is a permanent state for this instance; re-kind
      // the entry so future accesses take the hint-indexed path instead
      // of one hash probe per access.
      return AttributeMutator::demoteSplitToDict(this, obj, name);
    }
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
  // store side: hint-indexed overwrite on the
  // materialized receiver dict, validated by interned-key identity.
  // Overwrite only -- an insertion or a deleted slot needs the generic
  // path's ma_used / insertion-order / resize bookkeeping.  The new
  // value is increfed before the old one is released, so storing the
  // same object back is safe.
  {
    PyDictObject* mdict =
        reinterpret_cast<PyDictObject*>(*_PyObject_ManagedDictPointer(obj));
    if (mdict == nullptr && *_PyObject_ValuesPointer(obj) != nullptr) {
      // A re-keyed receiver demoted this type-keyed entry to kDict while
      // its siblings still use live split values.  Stock stores into those
      // values in place; PyObject_GenericGetDict below would materialize
      // every sibling unconditionally.
      return PyObject_SetAttr(obj, name, value);
    }
    if (mdict != nullptr) {
      PyDictKeysObject* keys = mdict->ma_keys;
      if (DK_IS_UNICODE(keys)) {
        Py_ssize_t hint = dict_hint;
        if (hint < 0 || hint >= keys->dk_nentries ||
            DK_UNICODE_ENTRIES(keys)[hint].me_key != name) {
          hint = getDictKeysIndex(keys, name);
          if (hint >= 0) {
            dict_hint = hint;
          }
        }
        if (hint >= 0) {
          if (mdict->ma_values != nullptr) {
            if (static_cast<size_t>(hint) + 2 <
                reinterpret_cast<const uint8_t*>(mdict->ma_values)[-1]) {
              if (mdict->ma_values->values[hint] != nullptr) {
                replaceMaterializedDictValue311(
                    mdict, &mdict->ma_values->values[hint], value);
                return 0;
              }
            }
          } else {
            PyDictUnicodeEntry* ep = &DK_UNICODE_ENTRIES(keys)[hint];
            if (ep->me_value != nullptr) {
              replaceMaterializedDictValue311(mdict, &ep->me_value, value);
              return 0;
            }
          }
        }
      }
    }
  }
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
    // Full-keys era: live inline values cannot hold this attribute, so
    // the __getattr__/AttributeError fallback would be exact.  In the
    // late-attr era the shared keys can still grow, so a values-live
    // instance may hold the attribute inline -- run the full generic
    // protocol instead of asserting absence.
    {
      // Values-form fast read through the type's cached shared keys.  The
      // hint is validated by interned-pointer key equality,
      // capacity-guarded by the values prefix byte, and only a non-null
      // slot is a hit -- everything else is the old path.
      {
        PyDictValues* values = *_PyObject_ValuesPointer(obj);
        PyDictKeysObject* keys = getSplitKeys(Py_TYPE(obj));
        if (values != nullptr && keys != nullptr && DK_IS_UNICODE(keys)) {
          Py_ssize_t hint = values_hint;
          if (hint < 0 || hint >= keys->dk_nentries ||
              DK_UNICODE_ENTRIES(keys)[hint].me_key != name) {
            hint = getDictKeysIndex(keys, name);
            if (hint >= 0) {
              values_hint = hint;
            }
          }
          if (hint >= 0 &&
              static_cast<size_t>(hint) + 2 <
                  reinterpret_cast<const uint8_t*>(values)[-1]) {
            PyObject* v = values->values[hint];
            if (v != nullptr) {
              return Py_NewRef(v);
            }
          }
        }
      }
      // The generic lookup alone is only the inner half of a hook
      // receiver's tp_getattro: slot_tp_getattr_hook runs it with
      // AttributeError suppressed and then calls the pinned __getattr__.
      // A demoted entry (kSplit -> kDict) whose receiver deleted the
      // attribute from its inline values arrives here and must take the
      // hook, not raise.
      GetAttrHookSnapshot311 hook(getattr_method);
      PyObject* result = PyObject_GenericGetAttr(obj, name);
      if (result == nullptr && hook.suppressAttributeError()) {
        return hook.call(obj, name);
      }
      return result;
    }
  }
  // Materialized receiver: indexed read validated by key identity,
  // replacing the full hash lookup on the steady path.  A deleted entry
  // (me_value NULL) and any mismatch fall through to the exact slow
  // protocol below.
  {
    PyDictKeysObject* keys = dict->ma_keys;
    if (DK_IS_UNICODE(keys)) {
      Py_ssize_t hint = dict_hint;
      if (hint < 0 || hint >= keys->dk_nentries ||
          DK_UNICODE_ENTRIES(keys)[hint].me_key != name) {
        hint = getDictKeysIndex(keys, name);
        if (hint >= 0) {
          dict_hint = hint;
        }
      }
      if (hint >= 0) {
        PyObject* v;
        if (dict->ma_values != nullptr) {
          if (static_cast<size_t>(hint) + 2 <
              reinterpret_cast<const uint8_t*>(dict->ma_values)[-1]) {
            v = dict->ma_values->values[hint];
          } else {
            v = nullptr;
          }
        } else {
          v = DK_UNICODE_ENTRIES(keys)[hint].me_value;
        }
        if (v != nullptr) {
          return Py_NewRef(v);
        }
      }
    }
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
  // Everything this call needs is captured up front: a mega-cache entry is
  // shared storage that a reentrant fill (reached from the instance peek
  // below, whose dict probe can run a key's __eq__) may overwrite while the
  // call is in flight, so the members are never re-read after this point.
  auto descr_guard = Ref<>::create(descr);
  PyObject* held = descr_guard.get();
  const uint32_t captured_keys_version = keys_version;
  BorrowedRef<PyTypeObject> descr_type(Py_TYPE(held));
  descrsetfunc setter = descr_type->tp_descr_set;
  descrgetfunc getter = descr_type->tp_descr_get;

  if (setter != nullptr && getter != nullptr) {
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(held, obj, type);
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
  if (captured_keys_version == 0 ||
      !isValidKeysVersion(captured_keys_version, obj)) {
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
    if (captured_keys_version == 0 ||
        reinterpret_cast<PyDictObject*>(dict.get())->ma_keys->dk_version !=
            captured_keys_version) {
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
    return getter(held, obj, type);
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
#if PY_VERSION_HEX < 0x030C0000
  dict_.dict_hint = -1;
  dict_.values_hint = -1;
#endif
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

bool canCacheAttribute(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    uint32_t& keys_version);

#if PY_VERSION_HEX < 0x030C0000
// a process-wide (receiver type, name) -> AttributeMutator
// table behind the per-site entries.  A site whose four entries are taken
// by other receiver types used to run the full lookup on every further
// miss and then attempt -- and refuse -- a fill each time (sqlglot: 1.4M
// "entries-full" refusals over four iterations on Expression.args /
// arg_types across dozens of Expression subclasses; sqlalchemy and pprint
// show the same shape).  Such a miss now fills a direct-mapped global
// entry, and the next miss on that (type, name) pair anywhere in the
// process is answered by it through the same pull-validated mutator the
// site entries use.  Names are compared by identity (code-object interned
// strings); a collision simply overwrites.  Nothing is watched: on 3.11
// every entry validates the receiver type's version tag on the hit path.
namespace {
// The name is held: the table outlives the code object whose co_names
// supplied it, and a released name's address could be reused by another
// string that the identity compare would then accept.
struct MegaEntry311 {
  AttributeMutator mut;
  PyObject* name{nullptr};
};

// Slots compare names by identity, so a slot's name object must stay alive
// for as long as the slot can be read, or its address could be reused by
// another string the comparison would then accept.  Holding a reference per
// slot made the held count grow with every newly occupied (type, name) slot:
// bounded by the tables, but regrtest's reference-leak hunt, which empties
// CPython's own type attribute cache between repetitions and cannot reach
// these tables, reads that growth as a leak.  Every distinct name is held
// once instead, in a registry keyed by content, and the slots borrow the
// registered object.  A name that dies (code built by eval) and comes back
// as a new object with the same content maps to the registered one; the
// identity compare then misses for it, which only costs the slow path.
UnorderedMap<std::string, PyObject*> s_mega_names_311;

PyObject* megaRegisteredName311(PyObject* name) {
  Py_ssize_t size = 0;
  const char* utf8 = PyUnicode_AsUTF8AndSize(name, &size);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  try {
    auto [it, inserted] = s_mega_names_311.try_emplace(
        std::string(utf8, static_cast<size_t>(size)), nullptr);
    if (inserted) {
      Py_INCREF(name);
      it->second = name;
    }
    return it->second;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

inline void megaSetName311(PyObject*& slot, PyObject* name) {
  slot = name == nullptr ? nullptr : megaRegisteredName311(name);
}
constexpr size_t kMegaEntries311 = 2048;
MegaEntry311 s_mega_load_311[kMegaEntries311];
MegaEntry311 s_mega_store_311[kMegaEntries311];

inline size_t megaIndex311(PyTypeObject* type, PyObject* name) {
  uint64_t h = (reinterpret_cast<uintptr_t>(type) >> 4) * 0x9E3779B97F4A7C15ull;
  h ^= (reinterpret_cast<uintptr_t>(name) >> 4) * 0xC2B2AE3D27D4EB4Full;
  return static_cast<size_t>(h >> 40) & (kMegaEntries311 - 1);
}

inline MegaEntry311&
megaEntry311(bool is_set, PyTypeObject* type, PyObject* name) {
  return (
      is_set ? s_mega_store_311 : s_mega_load_311)[megaIndex311(type, name)];
}

// A validated global entry for (Py_TYPE(obj), name), or nullptr.  A stale
// entry (type version moved on) is dropped on the way.
inline AttributeMutator*
megaLookup311(bool is_set, PyTypeObject* tp, PyObject* name) {
  MegaEntry311& me = megaEntry311(is_set, tp, name);
  if (me.name != name || me.mut.type() != tp) {
    return nullptr;
  }
  if (!me.mut.typeVersionMatches(tp) || !me.mut.descrVersionMatches()) {
    me.mut.reset();
    megaSetName311(me.name, nullptr);
    return nullptr;
  }
  return &me.mut;
}

// Method sites: the four per-site entries are filled first come, and a
// receiver type arriving after that ran the full lookup on every call
// (sqlglot: Expression._set_parent over the subclass population, 217K
// refused fills over four iterations).  The overflow population lives in a
// global (type, name) table of the same Entry shape; a site miss that finds
// its type there copies the entry into a round-robin site slot and looks up
// again, so the inline peek path serves the type until it is evicted.
// The name is registered (see megaSetName311).
struct MegaMethodEntry311 {
  LoadMethodCache::Entry entry;
  PyObject* name{nullptr};
};
MegaMethodEntry311 s_mega_method_311[kMegaEntries311];
uint8_t s_mega_method_rr_311 = 0;

inline MegaMethodEntry311& megaMethodEntry311(
    PyTypeObject* type,
    PyObject* name) {
  return s_mega_method_311[megaIndex311(type, name)];
}

// The two site fill forms for an entry outside the site (no watch; 3.11
// validates the type version on every hit): the keys-version proof when
// the type's shared keys can give one, else the per-hit shadow peek.
bool fillMethodEntry311(
    LoadMethodCache::Entry& entry,
    PyTypeObject* type,
    PyObject* value,
    PyObject* name) {
  uint32_t keys_version = 0;
  bool cacheable = canCacheAttribute(type, name, keys_version);
  entry.type = type;
  entry.value = value;
  entry.keys_version = cacheable ? keys_version : 0;
  entry.type_version = type->tp_version_tag;
  entry.peek_shadow = !cacheable;
  entry.inst_attr = false;
  entry.peek_keys = nullptr;
  entry.peek_nentries = 0;
  entry.peek_hint = -1;
  return true;
}
} // namespace
#endif

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
    // tp_ slot takes precedence.
    return;
  }

  AttributeMutator* mut = findEmptyEntry();
  bool mega = false;
  if (mut == nullptr) {
#if PY_VERSION_HEX < 0x030C0000
    MegaEntry311& me = megaEntry311(is_set, type, name);
    me.mut.reset();
    megaSetName311(me.name, name);
    mut = &me.mut;
    mega = true;
#else
    return;
#endif
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
        if (!mega) {
          ac_descr_watcher.watch(descr_type, this);
        }
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
    if (!mega) {
      ac_watcher.watch(type, this);
    }
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
          if (!mega) {
            ac_watcher.watch(type, this);
          }
          countAttrCacheFill311(is_set);
          return;
        }
      }

      if (keys->dk_nentries == SHARED_KEYS_MAX_SIZE) {
        mut->set_dict(type);
        if (!mega) {
          ac_watcher.watch(type, this);
        }
        countAttrCacheFill311(is_set);
        return;
      }
#if PY_VERSION_HEX < 0x030C0000
      // A late-added attribute that never entered the type's cached shared
      // keys lives in materialized combined dicts; refusing the fill retried
      // (and re-refused) on every miss (go: 9.2M futile fill calls in 3
      // rounds).  kDict serves it: materialized dicts get a direct dict
      // lookup, values-live instances take the generic path (the 3.11
      // DictMutator::getAttr below reads inline values), and class-level
      // changes stay pinned by the pull-validated type version.  3.12+
      // DictMutator treats a values-live instance as a miss, so the shape
      // stays refused there.
      mut->set_dict(type);
      if (!mega) {
        ac_watcher.watch(type, this);
      }
      countAttrCacheFill311(is_set);
#endif
      return;
    }

    // set_split handles __getattr__ fallback too
    mut->set_split(type, index, keys, inline_values);
  } else {
    // combined handles __getattr__ fallback too
    mut->set_combined(type);
  }
  if (!mega) {
    ac_watcher.watch(type, this);
  }
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
#if PY_VERSION_HEX < 0x030C0000
  if (AttributeMutator* mut = megaLookup311(true, tp, name)) {
    attrCacheStats311().store_attr.hits++;
    return mut->setAttr(obj, name, value);
  }
#endif
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


#if PY_VERSION_HEX < 0x030C0000
PyObject* AttributeCache::typeRecvGetAttr(PyObject* obj, PyObject* name) {
  if (type_recv_ == nullptr) {
    return nullptr;
  }
  auto tp = reinterpret_cast<PyTypeObject*>(obj);
  PyTypeObject* metatype = Py_TYPE(obj);
  for (auto& e : type_recv_->entries) {
    if (e.form == TypeRecvEntry::Form::kNone) {
      continue;
    }
    if (e.form == TypeRecvEntry::Form::kValue) {
      if (e.recv != tp) {
        continue;
      }
      // Metatype version (no new shadowing data descriptor on the
      // metatype side), then receiver version (any MRO-layer change
      // propagates through PyType_Modified).  A stale entry is cleared
      // in place -- versions are monotonic, it can never hit again --
      // without dereferencing its borrowed payload.
      if (!Ci_Type_HasValidVersionTag(metatype) ||
          metatype->tp_version_tag != e.meta_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      if (!Ci_Type_HasValidVersionTag(tp) ||
          tp->tp_version_tag != e.recv_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      // The class dict still holds the payload (receiver version); its
      // type must not have grown a __get__ since the fill.
      PyTypeObject* pt = Py_TYPE(e.payload);
      if (!Ci_Type_HasValidVersionTag(pt) ||
          pt->tp_version_tag != e.payload_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      return Py_NewRef(e.payload);
    }
    if (e.form == TypeRecvEntry::Form::kDescrGet) {
      if (e.recv != tp) {
        continue;
      }
      if (!Ci_Type_HasValidVersionTag(metatype) ||
          metatype->tp_version_tag != e.meta_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      if (!Ci_Type_HasValidVersionTag(tp) ||
          tp->tp_version_tag != e.recv_version) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      // type_getattro's second step for a non-data-descriptor-shadowed
      // class attribute that carries __get__.  The descriptor's type is
      // pinned by its version (it may have lost __get__), and the
      // descriptor itself is held across the call: its __get__ may delete
      // it from the class.
      PyTypeObject* pt = Py_TYPE(e.payload);
      if (!Ci_Type_HasValidVersionTag(pt) ||
          pt->tp_version_tag != e.payload_version ||
          pt->tp_descr_get == nullptr) {
        e = TypeRecvEntry{};
        return nullptr;
      }
      // slot_tp_getattr_hook pins the metatype's __getattr__ BEFORE the
      // generic lookup runs the descriptor; the descriptor may republish
      // or delete the hook, and this access must still use the pinned one.
      GetAttrHookSnapshot311 hook(obj);
      auto held = Ref<>::create(e.payload);
      PyObject* result = pt->tp_descr_get(held, nullptr, obj);
      if (result == nullptr && hook.suppressAttributeError()) {
        result = hook.call(obj, name);
      }
      return result;
    }
    // kMetaDescr: keyed on the metatype (a data descriptor with this
    // name serves every class of the metatype); receiver-independent.
    if (e.recv != metatype) {
      continue;
    }
    if (!Ci_Type_HasValidVersionTag(metatype) ||
        metatype->tp_version_tag != e.meta_version) {
      e = TypeRecvEntry{};
      return nullptr;
    }
    PyTypeObject* pt = Py_TYPE(e.payload);
    if (!Ci_Type_HasValidVersionTag(pt) ||
        pt->tp_version_tag != e.payload_version ||
        pt->tp_descr_get == nullptr) {
      e = TypeRecvEntry{};
      return nullptr;
    }
    GetAttrHookSnapshot311 hook(obj);
    auto held = Ref<>::create(e.payload);
    PyObject* result =
        pt->tp_descr_get(held, obj, reinterpret_cast<PyObject*>(metatype));
    if (result == nullptr && hook.suppressAttributeError()) {
      result = hook.call(obj, name);
    }
    return result;
  }
  return nullptr;
}

void AttributeCache::typeRecvTryFill(
    PyObject* obj,
    PyObject* name,
    PyObject* result) {
  auto tp = reinterpret_cast<PyTypeObject*>(obj);
  PyTypeObject* metatype = Py_TYPE(obj);
  // Standard type_getattro is replicable; the __getattr__ slot hook is
  // too on the HIT side (its first half is type_getattro: a class-dict
  // hit returns before __getattr__ runs).  A cache miss returns nullptr
  // and falls back to the full protocol including the hook, so there is
  // no "assert absent" path.  Other custom metatype getattros are not
  // cached.
  if ((metatype->tp_getattro != PyType_Type.tp_getattro &&
       (metatype->tp_getattro != Ci_tp_getattr_hook ||
        !metaclassHookUsesTypeGetAttr(metatype))) ||
      !ensureVersionTag(metatype)) {
    return;
  }
  if (type_recv_ == nullptr) {
    // Allocated on the call path, where a C++ exception has no handler:
    // the fill is an optimisation, so an allocation failure skips it and
    // the attribute value already produced is the answer.
    type_recv_.reset(new (std::nothrow) TypeRecvEntries());
    if (type_recv_ == nullptr) {
      return;
    }
  }
  // Slot choice: same-key slot first (refill), then an empty slot, else
  // round-robin replacement (polymorphic receivers are the point).
  auto pick_slot = [&](PyTypeObject* key) -> TypeRecvEntry* {
    TypeRecvEntry* slot = nullptr;
    for (auto& e : type_recv_->entries) {
      if (e.recv == key) {
        return &e;
      }
      if (slot == nullptr && e.form == TypeRecvEntry::Form::kNone) {
        slot = &e;
      }
    }
    if (slot == nullptr) {
      slot = &type_recv_->entries[type_recv_->rr];
      type_recv_->rr = (type_recv_->rr + 1) % TypeRecvEntries::kNumEntries;
    }
    return slot;
  };

  BorrowedRef<> meta_attr = _PyType_Lookup(metatype, name);
  if (meta_attr != nullptr &&
      Py_TYPE(meta_attr.get())->tp_descr_get != nullptr &&
      PyDescr_IsData(meta_attr.get())) {
    PyTypeObject* pt = Py_TYPE(meta_attr.get());
    if (!ensureVersionTag(pt)) {
      return;
    }
    *pick_slot(metatype) = TypeRecvEntry{
        metatype,
        0,
        metatype->tp_version_tag,
        TypeRecvEntry::Form::kMetaDescr,
        meta_attr.get(),
        pt->tp_version_tag};
    return;
  }
  // A same-named metatype candidate that is not a data descriptor now may
  // become one later without any version the kValue / kDescrGet entries
  // check moving: its heap type gains __set__ (PyType_Modified on that type
  // reaches neither the metatype nor the receiver class), or the object
  // takes another type through __class__ assignment (allowed between
  // mutable types, and for module objects even though their type is
  // immutable).  Such sites stay on the generic protocol; only a candidate
  // of an immutable, non-module type cannot change.
  if (meta_attr != nullptr &&
      (!PyType_HasFeature(Py_TYPE(meta_attr.get()), Py_TPFLAGS_IMMUTABLETYPE) ||
       PyModule_Check(meta_attr.get()))) {
    return;
  }
  TypeRecvEntry* slot = pick_slot(tp);

  BorrowedRef<> raw = _PyType_Lookup(tp, name);
  if (raw == nullptr) {
    return;
  }
  PyObject* payload = nullptr;
  PyTypeObject* rt = Py_TYPE(raw.get());
  if (raw.get() == result) {
    // Identity-stable forms: a plain class var with no __get__, or a
    // known descriptor type whose class-level access returns itself
    // (function / wrapper / method descriptors).  A custom descriptor
    // that happened to return itself this time is not taken.
    if (rt->tp_descr_get == nullptr || rt == &PyFunction_Type ||
        rt == &PyWrapperDescr_Type || rt == &PyMethodDescr_Type) {
      payload = raw.get();
    }
  }
  // A staticmethod is NOT unwrapped into a value form: staticmethod.__init__
  // can rebind its callable in place without any type version change (and
  // drop the previous callable's last reference), so the class-level read
  // must run staticmethod.__get__ per hit, the kDescrGet form below.
  if (payload == nullptr && rt->tp_descr_get != nullptr &&
      !PyDescr_IsData(raw.get())) {
    // anything else with __get__ (classmethod, the C
    // classmethod descriptor, custom descriptors) is served by running
    // that __get__ on each hit, exactly as type_getattro does once the
    // lookups are done.  A data descriptor on the receiver class is left
    // to the general protocol (its class-level get is the same call, but
    // keep the admitted set narrow).
    if (!ensureVersionTag(tp) || !ensureVersionTag(rt)) {
      return;
    }
    *slot = TypeRecvEntry{
        tp,
        tp->tp_version_tag,
        metatype->tp_version_tag,
        TypeRecvEntry::Form::kDescrGet,
        raw.get(),
        rt->tp_version_tag};
    return;
  }
  if (payload == nullptr || !ensureVersionTag(tp) || !ensureVersionTag(rt)) {
    return;
  }
  *slot = TypeRecvEntry{
      tp,
      tp->tp_version_tag,
      metatype->tp_version_tag,
      TypeRecvEntry::Form::kValue,
      payload,
      rt->tp_version_tag};
}
#endif // PY_VERSION_HEX < 0x030C0000

PyObject*
LoadAttrCache::invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name) {
#if PY_VERSION_HEX < 0x030C0000
  // PyObject_GetAttr completes an AttributeError with the name and the
  // receiver once the lookup has failed, whichever path raised it; the
  // cached paths below (a descriptor's __get__, a pinned __getattr__ hook)
  // raise without passing through it, so the completion is made here, at
  // the same point of the protocol, for the receiver as the site saw it.
  PyObject* result = cache->doInvoke(obj, name);
  if (result == nullptr && PyErr_ExceptionMatches(PyExc_AttributeError)) {
    Cix_set_attribute_error_context(obj, name);
  }
  return result;
#else
  return cache->doInvoke(obj, name);
#endif
}

PyObject* LoadAttrCache::doInvoke(PyObject* obj, PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
#if PY_VERSION_HEX < 0x030C0000
  // method_getattro's delegation.  A name the (static,
  // never modified) method type does not define is looked up on im_func;
  // the site remembers that verdict and continues with the function as
  // the receiver, whose loads the ordinary entries cache (docutils reads
  // method.__name__ on every visitor dispatch).
  if (tp == &PyMethod_Type) {
    if (method_delegate_ == 0) {
      method_delegate_ =
          _PyType_Lookup(&PyMethod_Type, name) == nullptr ? 1 : 2;
    }
    if (method_delegate_ == 1) {
      obj = reinterpret_cast<PyMethodObject*>(obj)->im_func;
      tp = Py_TYPE(obj);
    }
  }
#endif
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
#if PY_VERSION_HEX < 0x030C0000
  if (AttributeMutator* mut = megaLookup311(false, tp, name)) {
    attrCacheStats311().load_attr.hits++;
    return mut->getAttr(obj, name);
  }
  if (PyType_Check(obj)) {
    if (PyObject* result = typeRecvGetAttr(obj, name)) {
      attrCacheStats311().load_attr.hits++;
      return result;
    }
    if (PyErr_Occurred()) {
      // A cached descriptor's __get__ raised and the hook pinned before
      // the call (typeRecvGetAttr) did not absorb it: this is
      // type_getattro's own outcome, so it must not fall into the generic
      // slow path with the error pending.
      return nullptr;
    }
  }
#endif
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
#if PY_VERSION_HEX < 0x030C0000
  // The general fill refuses class receivers (metatype getattro takes
  // the tp-slot early return); give them the type-receiver entries.
  if (PyType_Check(obj)) {
    typeRecvTryFill(obj, name, result.get());
  }
#endif

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

#if PY_VERSION_HEX < 0x030C0000
void replaceMaterializedDictValue311(
    PyDictObject* dict,
    PyObject** slot,
    PyObject* value) {
  PyObject* old_value = *slot;
  JIT_DCHECK(old_value != nullptr, "Only existing values may be overwritten");
  Py_INCREF(value);
  *slot = value;
  dict->ma_version_tag = Cix_PyDict_NextVersion();
  // Mirror insertdict's MAINTAIN_TRACKING: a container stored into a
  // so-far-untracked dict must make the dict visible to the cycle collector.
  if (!_PyObject_GC_IS_TRACKED(reinterpret_cast<PyObject*>(dict)) &&
      _PyObject_GC_MAY_BE_TRACKED(value)) {
    PyObject_GC_Track(reinterpret_cast<PyObject*>(dict));
  }
  Py_DECREF(old_value);
}
#endif

LoadMethodCache::~LoadMethodCache() {
  for (auto& entry : entries_) {
    if (entry.type != nullptr) {
      lm_watcher.unwatch(entry.type, this);
      entry.type.reset();
      entry.value.reset();
    }
  }
}

#if PY_VERSION_HEX < 0x030C0000
void LoadMethodCache::Entry::clearPeekKeys() {
  peek_keys = nullptr;
  peek_nentries = 0;
  peek_hint = -1;
  peek_keys_version = 0;
}
#endif

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
  // a live same-type entry that declines (shadow, keys
  // era) must keep taking the full protocol; only a receiver type absent
  // from the site consults the global table below.
  bool matched = false;

  for (auto& entry : entries_) {
    if (entry.type == tp) {
#if PY_VERSION_HEX < 0x030C0000
      // Pull-based invalidation: no watcher retires entries on 3.11.
      if (tp->tp_version_tag != entry.type_version) {
        entry.type.reset();
        entry.value.reset();
        entry.clearPeekKeys();
        attrCacheStats311().load_method.invalidations++;
        continue;
      }
      matched = true;
#endif
#if PY_VERSION_HEX < 0x030C0000
      if (entry.peek_shadow) {
        // instance-attribute callable -- read the slot
        // through the memo and hand it back unbound ({Py_None, slot}),
        // exactly what the slow path returns for a shadowed name.
        if (entry.inst_attr) {
          if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
            PyDictValues* values = *_PyObject_ValuesPointer(obj.get());
            if (values != nullptr) {
              PyDictKeysObject* live_keys = getSplitKeys(tp);
              if (live_keys != nullptr && live_keys == entry.peek_keys &&
                  live_keys->dk_nentries == entry.peek_nentries &&
                  entry.peek_hint >= 0 &&
                  DK_UNICODE_ENTRIES(live_keys)[entry.peek_hint].me_key ==
                      name.get() &&
                  static_cast<size_t>(entry.peek_hint) + 2 <
                      reinterpret_cast<const uint8_t*>(values)[-1]) {
                PyObject* slot = values->values[entry.peek_hint];
                if (slot != nullptr) {
                  attrCacheStats311().load_method.hits++;
                  Py_INCREF(Py_None);
                  Py_INCREF(slot);
                  return {Py_None, slot};
                }
              }
            }
          }
          break; // anything else: full protocol (which also re-fills)
        }
        // values-form receivers answer the no-shadow
        // question from the memo -- three loads and two compares instead
        // of a keys hash probe per call.
        if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT)) {
          PyDictValues* values = *_PyObject_ValuesPointer(obj.get());
          if (values != nullptr) {
            PyDictKeysObject* live_keys = getSplitKeys(tp);
            if (live_keys != nullptr && live_keys == entry.peek_keys &&
                live_keys->dk_nentries == entry.peek_nentries) {
              if (entry.peek_hint == Entry::kPeekAbsent) {
                attrCacheStats311().load_method.hits++;
                PyObject* result = entry.value;
                Py_INCREF(result);
                Py_INCREF(obj.get());
                return {result, obj.get()};
              }
              if (entry.peek_hint >= 0 &&
                  DK_UNICODE_ENTRIES(live_keys)[entry.peek_hint].me_key ==
                      name.get() &&
                  static_cast<size_t>(entry.peek_hint) + 2 <
                      reinterpret_cast<const uint8_t*>(values)[-1]) {
                if (values->values[entry.peek_hint] == nullptr) {
                  attrCacheStats311().load_method.hits++;
                  PyObject* result = entry.value;
                  Py_INCREF(result);
                  Py_INCREF(obj.get());
                  return {result, obj.get()};
                }
                break; // shadowed: full protocol
              }
            }
          }
        }
        // materialized receivers get the same anchored
        // no-shadow memo, keyed on the receiver dict's own keys table.
        // {keys ptr, dk_nentries} is a sound absence anchor: an insert
        // grows dk_nentries, a re-insert of a deleted name appends a new
        // entry, and a resize replaces the keys object; deleting other
        // keys cannot create a shadow.  A present name is re-proved live
        // per call (me_value / ma_values slot), so a deleted shadow
        // falls back to the fast return naturally.  Split-materialized
        // dicts (ma_values set) keep the probing path below.
        PyDictObject* lm_mdict = nullptr;
        if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT) &&
            *_PyObject_ValuesPointer(obj.get()) == nullptr) {
          lm_mdict = reinterpret_cast<PyDictObject*>(
              *_PyObject_ManagedDictPointer(obj.get()));
          // A split-materialized dict (ma_values set, ma_keys still the
          // shared table) carries the same absence anchor: a name outside
          // the keys table can only enter it by append (dk_nentries grows)
          // or by a table swap.  The present-name (hint >= 0) memo below
          // reads me_value, which is only meaningful for combined dicts.
          if (lm_mdict != nullptr) {
            PyDictKeysObject* mkeys = lm_mdict->ma_keys;
            // A combined-keys table must also carry the recorded keys
            // version: the pointer and dk_nentries alone are satisfied
            // by a table reallocated at the same address after
            // dict.clear() (see Entry::peek_keys_version).
            // The keys version is required for every anchor, combined or
            // shared: it is unique for the life of the process, so a
            // table of another origin reallocated at the anchor's address
            // (a freed combined table reused for a class's shared keys)
            // cannot satisfy it.  The AArch64 stub checks the same three.
            if (DK_IS_UNICODE(mkeys) && mkeys == entry.peek_keys &&
                mkeys->dk_nentries == entry.peek_nentries &&
                mkeys->dk_version != 0 &&
                mkeys->dk_version == entry.peek_keys_version) {
              if (entry.peek_hint == Entry::kPeekAbsent) {
                attrCacheStats311().load_method.hits++;
                PyObject* result = entry.value;
                Py_INCREF(result);
                Py_INCREF(obj.get());
                return {result, obj.get()};
              }
              if (entry.peek_hint >= 0 &&
                  DK_UNICODE_ENTRIES(mkeys)[entry.peek_hint].me_key ==
                      name.get()) {
                // A split-materialized dict keeps its values in ma_values,
                // not in the shared table's me_value (always NULL there).
                PyObject* slot_value = lm_mdict->ma_values != nullptr
                    ? lm_mdict->ma_values->values[entry.peek_hint]
                    : DK_UNICODE_ENTRIES(mkeys)[entry.peek_hint].me_value;
                if (slot_value == nullptr) {
                  attrCacheStats311().load_method.hits++;
                  PyObject* result = entry.value;
                  Py_INCREF(result);
                  Py_INCREF(obj.get());
                  return {result, obj.get()};
                }
                break; // shadowed: full protocol
              }
            }
          }
        }
        // values-form receiver whose memo did not answer
        // (a fresh or adopted entry, or the shared keys grew): one scan of
        // the shared keys settles the shadow question and refreshes the
        // memo, and the refreshed entry is published to the global table so
        // that a later adoption of this (type, name) pair starts with a
        // memo that the three-load path above can answer.  The former
        // order scanned twice per hit (peek, then refresh) and every
        // adoption arrived without a memo.
        if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT) &&
            *_PyObject_ValuesPointer(obj.get()) != nullptr) {
          PyDictValues* values = *_PyObject_ValuesPointer(obj.get());
          PyDictKeysObject* live_keys = getSplitKeys(tp);
          if (live_keys != nullptr && DK_IS_UNICODE(live_keys)) {
            Py_ssize_t idx = getDictKeysIndex(live_keys, name);
            if (idx >= 0 && idx < live_keys->dk_nentries &&
                values->values[idx] != nullptr) {
              break; // shadowed: full protocol
            }
            entry.peek_keys = live_keys;
            entry.peek_nentries = live_keys->dk_nentries;
            entry.peek_hint = idx >= 0 ? idx : Entry::kPeekAbsent;
            entry.peek_keys_version =
                dictGetKeysVersion(PyInterpreterState_Get(), live_keys);
            MegaMethodEntry311& me = megaMethodEntry311(tp, name);
            if (me.name == nullptr ||
                (me.name == name && me.entry.type == tp)) {
              me.entry = entry;
              megaSetName311(me.name, name);
            }
            attrCacheStats311().load_method.hits++;
            PyObject* result = entry.value;
            Py_INCREF(result);
            Py_INCREF(obj.get());
            return {result, obj.get()};
          }
        }
        // prove per hit that the instance does not shadow the
        // method; a shadow (or a peek error) takes the full slow path.
        // The probe can run user code (a non-unicode key's __eq__) that
        // rebinds the class attribute and drops the cached value's last
        // reference; stock holds the descriptor it looked up before
        // probing the instance dict and returns that one.  Pin it the
        // same way.
        auto pinned_value = Ref<>::create(entry.value.get());
        PyObject* shadow = nullptr;
        int found = peekInstanceAttr311(obj, name, &shadow);
        if (found < 0) {
          // The probe ran user code that raised (a non-unicode key's
          // __eq__).  Stock's _PyObject_GetMethod propagates that error
          // from its single dict lookup; clearing it and re-probing on
          // the slow path would run the comparison a second time.
          return {nullptr, nullptr};
        }
        if (found != 0) {
          // The instance shadows the method: the probe already found the
          // attribute (strengthened), which is exactly the slow path's
          // unbound answer -- re-probing there would compare a non-unicode
          // key's __eq__ a second time.
          Py_INCREF(Py_None);
          return {Py_None, shadow};
        }
        // The probe may have run user code (a non-unicode key's __eq__)
        // that replaced the receiver's __dict__: the dict captured before
        // the probe can be freed by now.  Re-read it for the memo refresh
        // below.  The class-side answer stays the pre-probe entry value,
        // as stock returns the descriptor it looked up before probing the
        // instance dict; the next call re-validates the type version.
        if (lm_mdict != nullptr) {
          lm_mdict = *_PyObject_ValuesPointer(obj.get()) == nullptr
              ? reinterpret_cast<PyDictObject*>(
                    *_PyObject_ManagedDictPointer(obj.get()))
              : nullptr;
        }
        // Refresh the memo for values-form receivers.
        if (PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT) &&
            *_PyObject_ValuesPointer(obj.get()) != nullptr) {
          PyDictKeysObject* live_keys = getSplitKeys(tp);
          if (live_keys != nullptr && DK_IS_UNICODE(live_keys)) {
            Py_ssize_t idx = getDictKeysIndex(live_keys, name);
            entry.peek_keys = live_keys;
            entry.peek_nentries = live_keys->dk_nentries;
            entry.peek_hint = idx >= 0 ? idx : Entry::kPeekAbsent;
            entry.peek_keys_version =
                dictGetKeysVersion(PyInterpreterState_Get(), live_keys);
          }
        } else if (
            lm_mdict != nullptr && lm_mdict->ma_values == nullptr &&
            DK_IS_UNICODE(lm_mdict->ma_keys) &&
            (entry.peek_keys == nullptr ||
             entry.peek_keys == lm_mdict->ma_keys)) {
          // refresh the materialized-receiver anchor.
          // First-claim sticky: only an empty anchor or the same keys
          // table refreshes.  A foreign keys object (a different
          // receiver instance at a multi-instance site) keeps the probing
          // path -- refreshing would pay a full linear keys scan
          // (getDictKeysIndex) per alternation, which regressed go by
          // 29% in the first version.
          PyDictKeysObject* mkeys = lm_mdict->ma_keys;
          // The anchor needs a keys version; a table that cannot be
          // versioned (allocator exhausted) is not anchored.
          uint32_t keys_version =
              dictGetKeysVersion(PyInterpreterState_Get(), mkeys);
          if (keys_version != 0) {
            Py_ssize_t idx = getDictKeysIndex(mkeys, name);
            entry.peek_keys = mkeys;
            entry.peek_nentries = mkeys->dk_nentries;
            entry.peek_hint = idx >= 0 ? idx : Entry::kPeekAbsent;
            entry.peek_keys_version = keys_version;
          }
        }
        attrCacheStats311().load_method.hits++;
        PyObject* result = pinned_value.release();
        Py_INCREF(obj);
        return {result, obj};
      }
#endif
      if (!isValidKeysVersion(entry.keys_version, obj)) {
#if PY_VERSION_HEX < 0x030C0000
        // the keys-version proof has lapsed (the type's
        // shared keys grew since the fill) while the entry itself is still
        // valid -- its type version was checked above.  Convert it to the
        // per-hit shadow peek in place, the same conversion the fill makes
        // after a full slow-path lookup, publish that form to the global
        // table so that a later adoption does not bring the lapsed proof
        // back (docutils: every re-adoption of a node class at
        // _fast_findall ran the full protocol, 133K times per iteration),
        // and take the hit path.
        if (!entry.inst_attr) {
          entry.keys_version = 0;
          entry.peek_shadow = true;
          entry.peek_keys = nullptr;
          entry.peek_nentries = 0;
          entry.peek_hint = -1;
          MegaMethodEntry311& me = megaMethodEntry311(tp, name);
          me.entry = entry;
          megaSetName311(me.name, name);
          return lookup(obj, name);
        }
#endif
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

#if PY_VERSION_HEX < 0x030C0000
  if (!matched) {
    MegaMethodEntry311& me = megaMethodEntry311(tp, name);
    if (me.name == name && me.entry.type == tp) {
      if (tp->tp_version_tag == me.entry.type_version) {
        // Adopt into a site slot and take the regular hit path (the
        // adopted entry now matches, so this recursion ends there or in
        // the slow path).
        Entry& victim = entries_[s_mega_method_rr_311 & 3];
        s_mega_method_rr_311++;
        victim = me.entry;
        attrCacheStats311().load_method.hits++;
        return lookup(obj, name);
      }
      me.entry = Entry{};
      megaSetName311(me.name, nullptr);
    }
  }
#endif
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

#if PY_VERSION_HEX < 0x030C0000
// Fold the lazily created class-receiver cache's miss records into the
// site's own, so that the public collection sees them and the clear reaches
// them; called before the child is read, cleared or dropped.
void LoadMethodCache::absorbTypeRecvStats311() {
  if (type_recv_311_ == nullptr || cache_stats_ == nullptr) {
    return;
  }
  const CacheStats* child = type_recv_311_->cacheStats();
  if (child == nullptr) {
    return;
  }
  for (const auto& [key, miss] : child->misses) {
    auto it = cache_stats_->misses.insert({key, CacheMiss{0, miss.reason}});
    it.first->second.count += miss.count;
  }
  type_recv_311_->clearCacheStats();
}
#endif

void LoadMethodCache::clearCacheStats() {
#if PY_VERSION_HEX < 0x030C0000
  absorbTypeRecvStats311();
#endif
  cache_stats_->misses.clear();
}

const CacheStats* LoadMethodCache::cacheStats() {
#if PY_VERSION_HEX < 0x030C0000
  absorbTypeRecvStats311();
#endif
  return cache_stats_.get();
}

LoadMethodResult __attribute__((noinline))
LoadMethodCache::lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name) {
  PyTypeObject* tp = Py_TYPE(obj);
  PyObject* descr;
  descrgetfunc f = nullptr;
  PyObject* attr;
  bool is_method = false;
#if PY_VERSION_HEX < 0x030C0000
  attrCacheStats311().load_method.misses++;
  {
    if (PyType_Check(obj) && !type_recv_off_311_) {
      // serve class receivers from the type-receiver cache
      // (classmethod -> {function, cls}, plain function/staticmethod ->
      // {None, callable}), pull-validated on the class and metaclass
      // version tags exactly as the compile-time-typed site form is.
      if (type_recv_311_ == nullptr) {
        // Allocated on the call path, where a C++ exception has no
        // handler: without the child the class receiver takes the generic
        // path below, which is only slower.
        type_recv_311_.reset(new (std::nothrow) LoadTypeMethodCache());
        if (type_recv_311_ != nullptr && cache_stats_ != nullptr) {
          try {
            type_recv_311_->initCacheStats(
                cache_stats_->filename.c_str(),
                cache_stats_->method_name.c_str());
          } catch (const std::bad_alloc&) {
            // Statistics only; the child serves without them.
          }
        }
      }
      if (type_recv_311_ == nullptr) {
        type_recv_off_311_ = true;
      } else if (
          *type_recv_311_->typeAddr() ==
          reinterpret_cast<PyTypeObject*>(obj.get())) {
        type_recv_misses_311_ = 0;
        return LoadTypeMethodCache::getValueHelper(type_recv_311_.get(), obj);
      } else if (++type_recv_misses_311_ > 8) {
        // Polymorphic class receivers: stop refilling, use the generic path.
        type_recv_off_311_ = true;
        absorbTypeRecvStats311();
        type_recv_311_.reset();
      } else {
        return type_recv_311_->lookup(BorrowedRef<PyTypeObject>{obj}, name);
      }
    }
  }
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
      // cache the lookup shape when nothing on the class
      // side can ever take precedence (descr == NULL, pinned by the
      // receiver type version) and the receiver is values-form.
      if (descr == nullptr && PyType_HasFeature(tp, Py_TPFLAGS_MANAGED_DICT) &&
          Ci_Type_HasValidVersionTag(tp) &&
          *_PyObject_ValuesPointer(obj.get()) != nullptr) {
        PyDictKeysObject* live_keys = getSplitKeys(tp);
        if (live_keys != nullptr && DK_IS_UNICODE(live_keys)) {
          Py_ssize_t idx = getDictKeysIndex(live_keys, name);
          if (idx >= 0) {
            for (auto& entry : entries_) {
              bool same = entry.type == tp;
              if (entry.type != nullptr && !same) {
                continue;
              }
              if (same && !entry.inst_attr) {
                break; // slot owned by a real method entry; leave it
              }
              lm_watcher.watch(tp, this);
              entry.type = tp;
              entry.value = nullptr;
              entry.keys_version = 0;
              entry.type_version = tp->tp_version_tag;
              entry.peek_shadow = true;
              entry.inst_attr = true;
              entry.peek_keys = live_keys;
              entry.peek_nentries = live_keys->dk_nentries;
              entry.peek_hint = idx;
              attrCacheStats311().load_method.fills++;
              break;
            }
          }
        }
      }
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

#if PY_VERSION_HEX < 0x030C0000
  {
    // this fill only runs after a slow-path lookup, so a
    // same-type entry that is still here serves only part of the receiver
    // population (its keys-version era).  Upgrade it to the per-hit
    // shadow peek, which serves values-form and dict-form instances
    // alike, instead of leaving four stale eras pinned in the slots.
    for (auto& entry : entries_) {
      if (entry.type == type) {
        lm_watcher.watch(type, this);
        entry.value = value;
        entry.keys_version = 0;
        entry.type_version = type->tp_version_tag;
        entry.peek_shadow = true;
        entry.inst_attr = false;
        attrCacheStats311().load_method.fills++;
        // publish the upgraded form; the global entry
        // for this pair still carries the keys-version proof that just
        // failed, and every adoption of it would repeat this slow path.
        MegaMethodEntry311& me = megaMethodEntry311(type, name);
        me.entry = entry;
        megaSetName311(me.name, name);
        return;
      }
    }
  }
#endif

  for (auto& entry : entries_) {
    if (entry.type == nullptr) {
      uint32_t keys_version = 0;
      if (!canCacheAttribute(type, name, keys_version)) {
#if PY_VERSION_HEX < 0x030C0000
        // dk_version == 0 (late-attr keys growth) blocks the
        // no-shadow proof; cache anyway and peek the instance storage on
        // every hit (go: Square.find alone was 2.8M slow lookups).
        lm_watcher.watch(type, this);
        entry.type = type;
        entry.value = value;
        entry.keys_version = 0;
        entry.type_version = type->tp_version_tag;
        entry.peek_shadow = true;
        entry.inst_attr = false;
        entry.clearPeekKeys();
        attrCacheStats311().load_method.fills++;
        return;
#else
        break;
#endif
      }

      lm_watcher.watch(type, this);
      entry.type = type;
      entry.value = value;
      entry.keys_version = keys_version;
#if PY_VERSION_HEX < 0x030C0000
      entry.type_version = type->tp_version_tag;
      entry.peek_shadow = false;
      entry.inst_attr = false;
      entry.clearPeekKeys();
      attrCacheStats311().load_method.fills++;
#endif
      return;
    }
  }
#if PY_VERSION_HEX < 0x030C0000
  // no free site slot -- keep the resolution in the
  // global (type, name) table for the next miss to adopt.
  MegaMethodEntry311& me = megaMethodEntry311(type, name);
  me.entry = Entry{};
  megaSetName311(me.name, nullptr);
  if (fillMethodEntry311(me.entry, type, value, name)) {
    megaSetName311(me.name, name);
    attrCacheStats311().load_method.fills++;
  }
#endif
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
  if (cache->wrapper_form_ != WrapperForm::kPlain) {
    // The classmethod / staticmethod object is the cached value; its
    // callable is read now.  An uninitialised wrapper or one re-bound to
    // something that is not a plain function takes the full protocol.
    PyObject* callable = cache->wrapper_form_ == WrapperForm::kClassMethod
        ? Ci_PyClassMethod_GetFunc(cache->value_)
        : Ci_PyStaticMethod_GetFunc(cache->value_);
    if (callable == nullptr ||
        (cache->wrapper_form_ == WrapperForm::kClassMethod &&
         Py_TYPE(callable) != &PyFunction_Type)) {
      cache->type_ = nullptr;
      cache->value_.reset();
      cache->wrapper_form_ = WrapperForm::kPlain;
      attrCacheStats311().load_type_method.invalidations++;
      return cache->lookup(receiver, cache->name_);
    }
    attrCacheStats311().load_type_method.hits++;
    Py_INCREF(callable);
    if (cache->wrapper_form_ == WrapperForm::kClassMethod) {
      Py_INCREF(obj);
      return {callable, obj};
    }
    Py_INCREF(Py_None);
    return {Py_None, callable};
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

#if PY_VERSION_HEX < 0x030C0000
LoadMethodResult LoadTypeMethodCache::lookupOrHitHelper311(
    LoadTypeMethodCache* cache,
    PyTypeObject* obj,
    PyObject* name) {
  if (cache->type_ == obj) {
    return getValueHelper(cache, reinterpret_cast<PyObject*>(obj));
  }
  return cache->lookup(obj, name);
}
#endif

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
    if (res == nullptr) {
      return {nullptr, nullptr};
    }
    Py_INCREF(Py_None);
    return {Py_None, res};
  }
  if (_PyType_GetDict(obj) == nullptr) {
    if (PyType_Ready(obj) < 0) {
      return {nullptr, nullptr};
    }
  }

  descrgetfunc meta_get = nullptr;
#if PY_VERSION_HEX < 0x030C0000
  // The two MRO lookups below follow type_getattro's own order and can run
  // Python (a non-str key of equal hash in a class namespace compares with
  // __eq__), changing either type or giving the receiver another metatype in
  // the middle; an entry filled from their answer would then pin versions
  // the answer was not computed under.  Both tags are snapshotted here and
  // the answer is cached only when both were valid on entry, still hold
  // their snapshot afterwards, and the receiver still has this metatype: a
  // change zeroes a tag, and a reassigned tag is never an earlier value.
  // 3.11 assigns tags only inside _PyType_Lookup, so a type without one is
  // served without a fill and the lookups below tag it for the next visit;
  // no lookup is added for that, since an extra owner probe would run such
  // an __eq__ before the metatype's data descriptor is consulted.  A name
  // the type cache refuses (over 100 characters) is never cached here.
  const bool name_cacheable = PyUnicode_CheckExact(name) &&
      PyUnicode_IS_READY(name) && PyUnicode_GET_LENGTH(name) <= 100;
  const unsigned int meta_version = metatype->tp_version_tag;
  const unsigned int obj_version = obj->tp_version_tag;
#endif
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
      if (res == nullptr) {
        return {nullptr, nullptr};
      }
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
  }
  // A same-named metatype candidate that is not a data descriptor now may
  // become one later without the metatype or the receiver class changing
  // version: its heap type gains __set__, or the object takes another type
  // through __class__ assignment (mutable types, and module objects even
  // though their type is immutable).  Nothing found below is cached while
  // such a candidate exists.
#if PY_VERSION_HEX < 0x030C0000
  const bool fillable_candidate = meta_attribute == nullptr ||
      (PyType_HasFeature(Py_TYPE(meta_attribute), Py_TPFLAGS_IMMUTABLETYPE) &&
       !PyModule_Check(meta_attribute));
#else
  const bool fillable_candidate = true;
#endif

  /* No data descriptor found on metatype. Look in tp_dict of this
   * type and its bases */
  PyObject* attribute = _PyType_Lookup(obj, name);
#if PY_VERSION_HEX < 0x030C0000
  const bool fillable = fillable_candidate && name_cacheable &&
      meta_version != 0 && obj_version != 0 &&
      metatype->tp_version_tag == meta_version &&
      obj->tp_version_tag == obj_version && Py_TYPE(obj) == metatype;
#else
  const bool fillable = fillable_candidate;
#endif
  if (attribute != nullptr) {
    // Releasing the metatype candidate can run its __del__, which can
    // remove the owner attribute: hold the owner attribute first.
    auto attribute_owner = Ref<>::create(attribute);
    Py_XDECREF(meta_attribute);
    BorrowedRef<PyTypeObject> attribute_type = Py_TYPE(attribute);
    if (attribute_type == &PyClassMethod_Type) {
      // Only an initialised classmethod over a plain function is served
      // directly: the callable is read from the wrapper on every hit, so an
      // in-place __init__ is seen and nothing is borrowed past the class
      // dict.  Everything else (uninitialised, a callable with its own
      // __get__, a non-function) is the descriptor's own business below.
      PyObject* cm_callable = Ci_PyClassMethod_GetFunc(attribute);
      if (cm_callable != nullptr && Py_TYPE(cm_callable) == &PyFunction_Type) {
        Py_INCREF(obj);
        Py_INCREF(cm_callable);
#if PY_VERSION_HEX < 0x030C0000
        if (fillable) {
          fill(obj, attribute, true, WrapperForm::kClassMethod);
        }
#else
        fill(obj, cm_callable, true);
#endif
        return {cm_callable, obj};
      }
    } else if (attribute_type == &PyStaticMethod_Type) {
      PyObject* sm_callable = Ci_PyStaticMethod_GetFunc(attribute);
      if (sm_callable != nullptr) {
        Py_INCREF(sm_callable);
        Py_INCREF(Py_None);
#if PY_VERSION_HEX < 0x030C0000
        if (fillable) {
          fill(obj, attribute, false, WrapperForm::kStaticMethod);
        }
#else
        fill(obj, sm_callable, false);
#endif
        return {Py_None, sm_callable};
      }
    } else if (PyFunction_Check(attribute)) {
      Py_INCREF(attribute);
      Py_INCREF(Py_None);
      if (fillable) {
        fill(obj, attribute, false);
      }
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
      if (res == nullptr) {
        return {nullptr, nullptr};
      }
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
    if (res == nullptr) {
      return {nullptr, nullptr};
    }
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
#if PY_VERSION_HEX < 0x030C0000
  wrapper_form_ = WrapperForm::kPlain;
#endif
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
    bool is_unbound_meth
#if PY_VERSION_HEX < 0x030C0000
    ,
    WrapperForm form
#endif
) {
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
  wrapper_form_ = form;
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
