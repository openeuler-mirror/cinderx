// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>

#if PY_VERSION_HEX < 0x030B0000 || PY_VERSION_HEX >= 0x030C0000
// The pull-validated cache arms under test exist only on CPython 3.11.
#else

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

// PyMemberDef and T_OBJECT_EX for the kMemberDescr dispatch fixtures.
#include <structmember.h>

#include <memory>
#include <new>
#include <string>

namespace {

struct CacheDeleter {
  void operator()(jit::LoadAttrCache* cache) const {
    cache->~LoadAttrCache();
    PyMem_Free(cache);
  }
};

std::unique_ptr<jit::LoadAttrCache, CacheDeleter> makeLoadAttrCache() {
  void* mem = PyMem_Calloc(1, jit::AttributeCacheSizeTrait::size());
  JIT_CHECK(mem != nullptr, "Failed to allocate load attr cache");
  return std::unique_ptr<jit::LoadAttrCache, CacheDeleter>{
      new (mem) jit::LoadAttrCache()};
}

struct StoreCacheDeleter {
  void operator()(jit::StoreAttrCache* cache) const {
    cache->~StoreAttrCache();
    PyMem_Free(cache);
  }
};

std::unique_ptr<jit::StoreAttrCache, StoreCacheDeleter> makeStoreAttrCache() {
  void* mem = PyMem_Calloc(1, jit::AttributeCacheSizeTrait::size());
  JIT_CHECK(mem != nullptr, "Failed to allocate store attr cache");
  return std::unique_ptr<jit::StoreAttrCache, StoreCacheDeleter>{
      new (mem) jit::StoreAttrCache()};
}

// A data descriptor whose slots delete the descriptor from its owner
// class mid-call (dropping what may be its last reference) and then keep
// using their own storage.  Stock survives this because
// GenericGetAttr/GenericSetAttr hold a strong reference across the slot
// call; the cached dispatch must provide the same ownership.  The
// "armed" latch lets the priming call fill the cache before the
// self-deleting call runs, and died_mid_slot turns the use-after-free
// into a Release-visible verdict (the ASAN leg gives the memory one).
struct SelfDeletingDescr {
  PyObject_HEAD long payload;
};

long sdd_dealloc_count = 0;
bool sdd_died_mid_slot = false;
bool sdd_armed = false;
PyObject* sdd_owner = nullptr; // borrowed: the owner class

void sdd_dealloc(PyObject* self) {
  sdd_dealloc_count++;
  PyObject_Free(self);
}

PyObject* sdd_descr_get(PyObject* self, PyObject*, PyObject*) {
  if (sdd_armed) {
    long before = sdd_dealloc_count;
    if (PyObject_DelAttrString(sdd_owner, "x") < 0) {
      return nullptr;
    }
    if (sdd_dealloc_count != before) {
      // `self` is already dead; touching payload would be the UAF.
      sdd_died_mid_slot = true;
      Py_RETURN_NONE;
    }
  }
  return PyLong_FromLong(reinterpret_cast<SelfDeletingDescr*>(self)->payload);
}

int sdd_descr_set(PyObject* self, PyObject*, PyObject* value) {
  if (value == nullptr) {
    PyErr_SetString(PyExc_AttributeError, "cannot delete");
    return -1;
  }
  if (sdd_armed) {
    long before = sdd_dealloc_count;
    if (PyObject_DelAttrString(sdd_owner, "x") < 0) {
      return -1;
    }
    if (sdd_dealloc_count != before) {
      sdd_died_mid_slot = true;
      return 0;
    }
  }
  reinterpret_cast<SelfDeletingDescr*>(self)->payload += 1;
  return 0;
}

PyTypeObject SelfDeletingDescr_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) //
};

} // namespace

class AttrCache311Test : public RuntimeTest {};

TEST_F(AttrCache311Test, DictKeysLookupSkipsDeletedUnicodeEntries) {
  auto dict = Ref<>::steal(PyDict_New());
  ASSERT_NE(dict, nullptr);
  auto deleted = Ref<>::steal(PyUnicode_InternFromString("deleted"));
  auto retained = Ref<>::steal(PyUnicode_InternFromString("retained"));
  auto value = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyDict_SetItem(dict, deleted, value), 0);
  ASSERT_EQ(PyDict_SetItem(dict, retained, value), 0);
  ASSERT_EQ(PyDict_DelItem(dict, deleted), 0);

  auto* keys = reinterpret_cast<PyDictObject*>(dict.get())->ma_keys;
  ASSERT_TRUE(DK_IS_UNICODE(keys));
  ASSERT_EQ(DK_UNICODE_ENTRIES(keys)[0].me_key, nullptr);
  EXPECT_GE(getDictKeysIndex(keys, retained), 0);
  EXPECT_EQ(getDictKeysIndex(keys, deleted), -1);
}

TEST_F(AttrCache311Test, SplitEntrySurvivesMaterializationAndRekey) {
  const char* src = R"(
class P:
    def __init__(self):
        self.x = 7

inst = P()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(inst, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 7);

  auto dict = Ref<>::steal(PyObject_GenericGetDict(inst, nullptr));
  ASSERT_NE(dict, nullptr);
  auto materialized =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(materialized, nullptr);
  EXPECT_EQ(PyLong_AsLong(materialized), 7);

  auto non_unicode_key = Ref<>::steal(PyLong_FromLong(42));
  auto marker = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyDict_SetItem(dict, non_unicode_key, marker), 0);
  auto rekeyed =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(rekeyed, nullptr);
  EXPECT_EQ(PyLong_AsLong(rekeyed), 7);
}

TEST_F(AttrCache311Test, StoreEntrySurvivesMaterializationAndRekey) {
  const char* src = R"(
class P:
    def __init__(self):
        self.x = 1

inst = P()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(inst, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cache = makeStoreAttrCache();

  auto two = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, two), 0);
  auto dict = Ref<>::steal(PyObject_GenericGetDict(inst, nullptr));
  ASSERT_NE(dict, nullptr);
  auto* dict_obj = reinterpret_cast<PyDictObject*>(dict.get());

  auto three = Ref<>::steal(PyLong_FromLong(3));
  uint64_t version = dict_obj->ma_version_tag;
  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, three), 0);
  EXPECT_EQ(PyLong_AsLong(PyDict_GetItem(dict, name)), 3);
  EXPECT_NE(dict_obj->ma_version_tag, version);

  auto non_unicode_key = Ref<>::steal(PyLong_FromLong(42));
  auto marker = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyDict_SetItem(dict, non_unicode_key, marker), 0);
  auto four = Ref<>::steal(PyLong_FromLong(4));
  version = dict_obj->ma_version_tag;
  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, four), 0);
  EXPECT_EQ(PyLong_AsLong(PyDict_GetItem(dict, name)), 4);
  EXPECT_NE(dict_obj->ma_version_tag, version);
}

TEST_F(AttrCache311Test, DictDemotionDoesNotMaterializeSiblingValues) {
  const char* src = R"(
class P:
    def __init__(self):
        self.x = 1

normal = P()
rekeyed = P()
vars(rekeyed)[42] = "general"
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> normal = PyDict_GetItemString(globals, "normal");
  BorrowedRef<> rekeyed = PyDict_GetItemString(globals, "rekeyed");
  ASSERT_NE(normal, nullptr);
  ASSERT_NE(rekeyed, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto two = Ref<>::steal(PyLong_FromLong(2));
  auto three = Ref<>::steal(PyLong_FromLong(3));
  auto cache = makeStoreAttrCache();

  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), normal, name, two), 0);
  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), rekeyed, name, two), 0);
  ASSERT_NE(*_PyObject_ValuesPointer(normal.get()), nullptr);
  ASSERT_EQ(*_PyObject_ManagedDictPointer(normal.get()), nullptr);

  ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), normal, name, three), 0);
  EXPECT_NE(*_PyObject_ValuesPointer(normal.get()), nullptr);
  EXPECT_EQ(*_PyObject_ManagedDictPointer(normal.get()), nullptr);
  auto current = Ref<>::steal(PyObject_GetAttr(normal, name));
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(PyLong_AsLong(current), 3);
}

TEST_F(AttrCache311Test, PullCheckRetiresStaleEntriesOnTypeMutation) {
  const char* src = R"(
class P:
    def __init__(self, x):
        self.x = x

inst = P(3)
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> klass = PyDict_GetItemString(globals, "P");
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(klass, nullptr);
  ASSERT_NE(inst, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));

  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  uint64_t misses = stats.load_attr.misses;
  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 3);
  EXPECT_EQ(stats.load_attr.misses, misses + 1);
  EXPECT_EQ(stats.load_attr.fills, fills + 1);

  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyLong_AsLong(second), 3);
  EXPECT_EQ(stats.load_attr.hits, hits + 1);

  // Any class mutation bumps tp_version_tag; the next use must retire the
  // entry by pull, refill, and still answer correctly.
  auto marker = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyObject_SetAttrString(klass, "marker", marker), 0);
  uint64_t invalidations = stats.load_attr.invalidations;
  fills = stats.load_attr.fills;
  auto third =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(PyLong_AsLong(third), 3);
  EXPECT_EQ(stats.load_attr.invalidations, invalidations + 1);
  EXPECT_EQ(stats.load_attr.fills, fills + 1);
}

TEST_F(AttrCache311Test, CacheTrafficDoesNotMaterializeTheInstanceDict) {
  const char* src = R"(
class P:
    cv = "classvar"

    def __init__(self, x):
        self.x = x

inst = P(7)
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(inst, nullptr);
  ASSERT_TRUE(PyType_HasFeature(Py_TYPE(inst.get()), Py_TPFLAGS_MANAGED_DICT));
  ASSERT_NE(*_PyObject_ValuesPointer(inst.get()), nullptr);
  ASSERT_EQ(*_PyObject_ManagedDictPointer(inst.get()), nullptr);

  auto x_name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cv_name = Ref<>::steal(PyUnicode_InternFromString("cv"));
  auto cache_x = makeLoadAttrCache();
  auto cache_cv = makeLoadAttrCache();

  // Misses (which fill), hits, and the class-var shadow peek all run
  // against live inline values; none of them may convert the values into
  // a real dict.
  for (int i = 0; i < 4; i++) {
    auto x =
        Ref<>::steal(jit::LoadAttrCache::invoke(cache_x.get(), inst, x_name));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(PyLong_AsLong(x), 7);
    auto cv =
        Ref<>::steal(jit::LoadAttrCache::invoke(cache_cv.get(), inst, cv_name));
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(PyUnicode_CompareWithASCIIString(cv, "classvar"), 0);
  }

  EXPECT_NE(*_PyObject_ValuesPointer(inst.get()), nullptr)
      << "cache traffic dropped the inline values";
  EXPECT_EQ(*_PyObject_ManagedDictPointer(inst.get()), nullptr)
      << "cache traffic materialized the instance dict";
}

TEST_F(AttrCache311Test, KeysVersionAllocatorIssuesFromTheUpperRange) {
  const char* src = R"(
class A:
    def __init__(self):
        self.a = 1

class B:
    def __init__(self):
        self.b = 1

a = A()
b = B()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<PyTypeObject> a_type{PyDict_GetItemString(globals, "A")};
  BorrowedRef<PyTypeObject> b_type{PyDict_GetItemString(globals, "B")};
  ASSERT_NE(a_type, nullptr);
  ASSERT_NE(b_type, nullptr);

  PyDictKeysObject* a_keys =
      reinterpret_cast<PyHeapTypeObject*>(a_type.get())->ht_cached_keys;
  PyDictKeysObject* b_keys =
      reinterpret_cast<PyHeapTypeObject*>(b_type.get())->ht_cached_keys;
  ASSERT_NE(a_keys, nullptr);
  ASSERT_NE(b_keys, nullptr);

  PyInterpreterState* interp = PyInterpreterState_Get();
  uint32_t a_version = dictGetKeysVersion(interp, a_keys);
  uint32_t b_version = dictGetKeysVersion(interp, b_keys);
  // The 3.11 allocator (shared with the vendored specializer) issues from
  // the top half of the 32-bit range so it can never collide with
  // libpython's private bottom-up stream; distinct keys objects get
  // distinct numbers, and re-asking is stable.
  EXPECT_GE(a_version, UINT32_C(1) << 31);
  EXPECT_GE(b_version, UINT32_C(1) << 31);
  EXPECT_NE(a_version, b_version);
  EXPECT_EQ(dictGetKeysVersion(interp, a_keys), a_version);
}

TEST_F(AttrCache311Test, ModuleMethodHitOwnsBothResultHalves) {
  auto mod = Ref<>::steal(PyModule_New("attr_cache_311_mod"));
  ASSERT_NE(mod, nullptr);
  auto builtins = Ref<>::steal(PyImport_ImportModule("builtins"));
  ASSERT_NE(builtins, nullptr);
  auto abs_fn = Ref<>::steal(PyObject_GetAttrString(builtins, "abs"));
  ASSERT_NE(abs_fn, nullptr);
  ASSERT_TRUE(PyCFunction_Check(abs_fn.get()));
  ASSERT_EQ(PyObject_SetAttrString(mod, "f", abs_fn), 0);
  auto name = Ref<>::steal(PyUnicode_InternFromString("f"));

  // 3.11's None is mortal: any arm that returned a borrowed None would
  // decrement it to death over enough iterations, so every arm -- cold
  // slow path, hit, non-function value, generic fallback -- runs 64
  // rounds against a refcount baseline.
  //
  // Absolute refcount baselines are order-sensitive in a full-suite run:
  // GC of prior tests' garbage and one-time lazy initialization inside
  // the first lookup of a given shape both move the count for reasons
  // that are not per-call ownership bugs.  So: GC is drained and held
  // off (RAII so a failing ASSERT cannot leak the disabled state), and
  // each measured block runs ONE priming round before its baseline --
  // a per-call borrow still shows up as a full -64.
  struct GcOff {
    GcOff() {
      PyGC_Collect();
      PyGC_Disable();
    }
    ~GcOff() {
      PyGC_Enable();
    }
  } gc_off;

  auto run_round = [&](BorrowedRef<> lookup_name, PyObject* expected_value) {
    jit::LoadModuleMethodCache cold;
    auto res = cold.lookupHelper(&cold, mod, lookup_name);
    ASSERT_EQ(res.callable, Py_None);
    if (expected_value != nullptr) {
      ASSERT_EQ(res.self_or_null, expected_value);
    } else {
      ASSERT_NE(res.self_or_null, nullptr);
    }
    Py_DECREF(res.callable);
    Py_DECREF(res.self_or_null);
  };

  {
    // Cold fill plus hits on one cache: prime, baseline, then measure
    // the slow-path fill and 64 hits together.
    jit::LoadModuleMethodCache prime;
    auto first = prime.lookupHelper(&prime, mod, name);
    ASSERT_EQ(first.callable, Py_None);
    ASSERT_EQ(first.self_or_null, abs_fn.get());
    Py_DECREF(first.callable);
    Py_DECREF(first.self_or_null);

    Py_ssize_t none_refcount = Py_REFCNT(Py_None);
    jit::LoadModuleMethodCache cache;
    for (int i = 0; i < 64; i++) {
      auto hit = cache.lookupHelper(&cache, mod, name);
      ASSERT_EQ(hit.callable, Py_None);
      ASSERT_EQ(hit.self_or_null, abs_fn.get());
      Py_DECREF(hit.callable);
      Py_DECREF(hit.self_or_null);
    }
    // Fresh caches per round keep every iteration on the cold slow path.
    for (int i = 0; i < 64; i++) {
      run_round(name, abs_fn.get());
    }
    EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);
  }

  // A non-function value in the module dict returns through the same
  // slow-path arm without filling the cache; its None half must be owned
  // as well.
  auto value = Ref<>::steal(PyLong_FromLong(7));
  ASSERT_EQ(PyObject_SetAttrString(mod, "v", value), 0);
  auto vname = Ref<>::steal(PyUnicode_InternFromString("v"));
  run_round(vname, value.get());
  Py_ssize_t none_refcount = Py_REFCNT(Py_None);
  for (int i = 0; i < 64; i++) {
    run_round(vname, value.get());
  }
  EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);

  // A name absent from the module dict resolves through the
  // PyObject_GetAttr generic arm (here: a type attribute); that return
  // must own its None half too.
  auto sname = Ref<>::steal(PyUnicode_InternFromString("__str__"));
  run_round(sname, nullptr);
  none_refcount = Py_REFCNT(Py_None);
  for (int i = 0; i < 64; i++) {
    run_round(sname, nullptr);
  }
  EXPECT_EQ(Py_REFCNT(Py_None), none_refcount);
}

TEST_F(AttrCache311Test, DescriptorSurvivesDeletingItselfMidSlot) {
  if (SelfDeletingDescr_Type.tp_name == nullptr) {
    SelfDeletingDescr_Type.tp_name = "SelfDeletingDescr";
    SelfDeletingDescr_Type.tp_basicsize = sizeof(SelfDeletingDescr);
    SelfDeletingDescr_Type.tp_dealloc = sdd_dealloc;
    SelfDeletingDescr_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    SelfDeletingDescr_Type.tp_descr_get = sdd_descr_get;
    SelfDeletingDescr_Type.tp_descr_set = sdd_descr_set;
    ASSERT_GE(PyType_Ready(&SelfDeletingDescr_Type), 0);
  }

  auto run_arm = [&](bool is_store) {
    const char* src = R"(
class C:
    pass

inst = C()
)";
    Ref<PyObject> globals(MakeGlobals());
    ASSERT_NE(globals, nullptr);
    auto result =
        Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
    ASSERT_NE(result, nullptr);
    BorrowedRef<> klass = PyDict_GetItemString(globals, "C");
    BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
    ASSERT_NE(klass, nullptr);
    ASSERT_NE(inst, nullptr);

    SelfDeletingDescr* descr =
        PyObject_New(SelfDeletingDescr, &SelfDeletingDescr_Type);
    ASSERT_NE(descr, nullptr);
    descr->payload = 42;
    // The 3.11 fill refuses descriptor types without a valid version tag
    // (the JIT never assigns one itself); an ordinary attribute lookup
    // through the type makes CPython assign it, exactly as organic use
    // would have.
    {
      auto warmed = Ref<>::steal(PyObject_GetAttrString(
          reinterpret_cast<PyObject*>(descr), "__class__"));
      ASSERT_NE(warmed, nullptr);
    }
    ASSERT_EQ(
        PyObject_SetAttrString(klass, "x", reinterpret_cast<PyObject*>(descr)),
        0);
    // The class dict now holds the ONLY reference.
    Py_DECREF(descr);

    sdd_owner = klass;
    sdd_armed = false;
    sdd_died_mid_slot = false;
    sdd_dealloc_count = 0;
    auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
    auto value = Ref<>::steal(PyLong_FromLong(7));
    const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

    if (is_store) {
      auto cache = makeStoreAttrCache();
      // Prime: fills the kDataDescr entry through the working slot.  The
      // stats assertions make a vacuous pass impossible: a refused fill
      // would leave every call on the stock-protected slow path and the
      // test would prove nothing.
      uint64_t fills = stats.store_attr.fills;
      ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, value), 0);
      ASSERT_EQ(stats.store_attr.fills, fills + 1);
      sdd_armed = true;
      // Hit: the slot deletes the descriptor's only other reference
      // mid-call; the dispatch's strong hold must keep it alive.
      uint64_t hits = stats.store_attr.hits;
      ASSERT_EQ(jit::StoreAttrCache::invoke(cache.get(), inst, name, value), 0);
      ASSERT_EQ(stats.store_attr.hits, hits + 1);
    } else {
      auto cache = makeLoadAttrCache();
      uint64_t fills = stats.load_attr.fills;
      auto first =
          Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
      ASSERT_NE(first, nullptr);
      EXPECT_EQ(PyLong_AsLong(first), 42);
      ASSERT_EQ(stats.load_attr.fills, fills + 1);
      sdd_armed = true;
      uint64_t hits = stats.load_attr.hits;
      auto second =
          Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
      ASSERT_NE(second, nullptr);
      EXPECT_EQ(PyLong_AsLong(second), 42);
      ASSERT_EQ(stats.load_attr.hits, hits + 1);
    }

    EXPECT_FALSE(sdd_died_mid_slot)
        << "the descriptor was deallocated while its own slot was running";
    // The dispatch guard was the last reference: the descriptor died
    // exactly once, after the slot returned.
    EXPECT_EQ(sdd_dealloc_count, 1);
    sdd_armed = false;
    sdd_owner = nullptr;
  };

  run_arm(/*is_store=*/false);
  run_arm(/*is_store=*/true);
}

TEST_F(AttrCache311Test, MetaclassDescriptorSurvivesDeletingItselfMidSlot) {
  if (SelfDeletingDescr_Type.tp_name == nullptr) {
    SelfDeletingDescr_Type.tp_name = "SelfDeletingDescr";
    SelfDeletingDescr_Type.tp_basicsize = sizeof(SelfDeletingDescr);
    SelfDeletingDescr_Type.tp_dealloc = sdd_dealloc;
    SelfDeletingDescr_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    SelfDeletingDescr_Type.tp_descr_get = sdd_descr_get;
    SelfDeletingDescr_Type.tp_descr_set = sdd_descr_set;
    ASSERT_GE(PyType_Ready(&SelfDeletingDescr_Type), 0);
  }

  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result = Ref<>::steal(PyRun_String(
      "class Meta(type):\n"
      "    pass\n"
      "class C(metaclass=Meta):\n"
      "    pass\n",
      Py_file_input,
      globals,
      globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> meta = PyDict_GetItemString(globals, "Meta");
  BorrowedRef<> klass = PyDict_GetItemString(globals, "C");
  ASSERT_NE(meta, nullptr);
  ASSERT_NE(klass, nullptr);

  SelfDeletingDescr* descr =
      PyObject_New(SelfDeletingDescr, &SelfDeletingDescr_Type);
  ASSERT_NE(descr, nullptr);
  descr->payload = 42;
  {
    auto warmed = Ref<>::steal(PyObject_GetAttrString(
        reinterpret_cast<PyObject*>(descr), "__class__"));
    ASSERT_NE(warmed, nullptr);
  }
  ASSERT_EQ(
      PyObject_SetAttrString(meta, "x", reinterpret_cast<PyObject*>(descr)), 0);
  Py_DECREF(descr);

  sdd_owner = meta;
  sdd_armed = false;
  sdd_died_mid_slot = false;
  sdd_dealloc_count = 0;
  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), klass, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 42);

  sdd_armed = true;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), klass, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyLong_AsLong(second), 42);
  EXPECT_FALSE(sdd_died_mid_slot);
  EXPECT_EQ(sdd_dealloc_count, 1);

  sdd_armed = false;
  sdd_owner = nullptr;
}

// --- __getattr__ hook ownership -------------------------------------------
//
// The MR review found dispatch arms handing a BORROWED __getattr__ hook to
// callGetAttr(): the split value-miss fallback, the GetAttrMutator
// valid-keys hit, and the kMemberDescr AttributeError route.  A native
// hook whose call deletes the owner class's __getattr__ entry -- possibly
// the hook's last reference -- must survive its own call frame, exactly
// as stock slot_tp_getattr_hook guarantees by holding a strong reference
// across call_attribute().  The died_mid_call latch turns the
// use-after-free into a Release-visible verdict (the ASAN leg gives the
// memory one).

namespace {

struct SelfDeletingHook {
  PyObject_HEAD long payload;
};

long sdh_dealloc_count = 0;
bool sdh_died_mid_call = false;
bool sdh_armed = false;
PyObject* sdh_owner = nullptr; // borrowed: the class owning __getattr__

void sdh_dealloc(PyObject* self) {
  sdh_dealloc_count++;
  PyObject_Free(self);
}

PyObject* sdh_call(PyObject* self, PyObject* args, PyObject* /* kwargs */) {
  // Read the payload while `self` is provably alive; after the armed
  // deletion the object may already be gone and must not be touched.
  long payload = reinterpret_cast<SelfDeletingHook*>(self)->payload;
  PyObject* name = PyTuple_GET_ITEM(args, 0);
  if (sdh_armed) {
    long before = sdh_dealloc_count;
    if (PyObject_DelAttrString(sdh_owner, "__getattr__") < 0) {
      return nullptr;
    }
    if (sdh_dealloc_count != before) {
      sdh_died_mid_call = true;
      return PyUnicode_FromString("died");
    }
  }
  return PyUnicode_FromFormat("hook:%ld:%U", payload, name);
}

PyTypeObject SelfDeletingHook_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) //
};

// Installs a fresh hook instance as `klass.__getattr__`, leaving the
// class dict as the hook's ONLY reference, and resets the latch state.
void installSelfDeletingHook(BorrowedRef<> klass, long payload) {
  if (SelfDeletingHook_Type.tp_name == nullptr) {
    SelfDeletingHook_Type.tp_name = "SelfDeletingHook";
    SelfDeletingHook_Type.tp_basicsize = sizeof(SelfDeletingHook);
    SelfDeletingHook_Type.tp_dealloc = sdh_dealloc;
    SelfDeletingHook_Type.tp_flags = Py_TPFLAGS_DEFAULT;
    SelfDeletingHook_Type.tp_call = sdh_call;
    JIT_CHECK(
        PyType_Ready(&SelfDeletingHook_Type) >= 0, "hook type init failed");
  }
  SelfDeletingHook* hook =
      PyObject_New(SelfDeletingHook, &SelfDeletingHook_Type);
  JIT_CHECK(hook != nullptr, "hook allocation failed");
  hook->payload = payload;
  int set = PyObject_SetAttrString(
      klass, "__getattr__", reinterpret_cast<PyObject*>(hook));
  Py_DECREF(hook);
  JIT_CHECK(set == 0, "installing __getattr__ failed");
  sdh_owner = klass;
  sdh_armed = false;
  sdh_died_mid_call = false;
  sdh_dealloc_count = 0;
}

// A C base type exposing a T_OBJECT_EX member, so a Python subclass with
// __getattr__ reaches the kMemberDescr dispatch arm.
struct MemberHost {
  PyObject_HEAD PyObject* slotval;
};

PyMemberDef member_host_members[] = {
    {"slotval", T_OBJECT_EX, offsetof(MemberHost, slotval), 0, nullptr},
    {nullptr, 0, 0, 0, nullptr}};

void member_host_dealloc(PyObject* self) {
  Py_XDECREF(reinterpret_cast<MemberHost*>(self)->slotval);
  Py_TYPE(self)->tp_free(self);
}

PyTypeObject MemberHost_Type = {
    PyVarObject_HEAD_INIT(nullptr, 0) //
};

void ensureMemberHostType() {
  if (MemberHost_Type.tp_name != nullptr) {
    return;
  }
  MemberHost_Type.tp_name = "MemberHost";
  MemberHost_Type.tp_basicsize = sizeof(MemberHost);
  MemberHost_Type.tp_dealloc = member_host_dealloc;
  MemberHost_Type.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
  MemberHost_Type.tp_members = member_host_members;
  MemberHost_Type.tp_new = PyType_GenericNew;
  JIT_CHECK(PyType_Ready(&MemberHost_Type) >= 0, "member host init failed");
}

} // namespace

TEST_F(AttrCache311Test, GetAttrHookSurvivesDeletingItselfMidCall) {
  const char* src = R"(
class C:
    def __init__(self):
        self.x = 1

inst = C()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> klass = PyDict_GetItemString(globals, "C");
  BorrowedRef<> inst = PyDict_GetItemString(globals, "inst");
  ASSERT_NE(klass, nullptr);
  ASSERT_NE(inst, nullptr);
  installSelfDeletingHook(klass, 5);

  // Pre-version the shared keys so the fill records a NONZERO keys
  // version: the armed call must go down the valid-keys hit arm, which
  // calls the fill-time cached hook, not the zero-version peek arm
  // (that one pins through GetAttrHookSnapshot311 already).
  PyDictKeysObject* keys = reinterpret_cast<PyHeapTypeObject*>(
                               reinterpret_cast<PyTypeObject*>(klass.get()))
                               ->ht_cached_keys;
  ASSERT_NE(keys, nullptr);
  ASSERT_GE(
      dictGetKeysVersion(PyInterpreterState_Get(), keys), UINT32_C(1) << 31);
  ASSERT_NE(*_PyObject_ValuesPointer(inst.get()), nullptr);

  auto name = Ref<>::steal(PyUnicode_InternFromString("missing"));
  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  // Cold call: the slow path resolves through the (unarmed) hook and
  // fills the kGetAttr entry.
  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(first, "hook:5:missing"), 0);
  ASSERT_EQ(stats.load_attr.fills, fills + 1)
      << "the __getattr__ entry never filled; the rest is vacuous";

  // Armed hit: the valid-keys arm calls the cached hook, and the hook's
  // call deletes the class's __getattr__ entry -- its own last
  // reference.  The pin must keep it alive through its call frame.
  sdh_armed = true;
  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), inst, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(second, "hook:5:missing"), 0);
  ASSERT_EQ(stats.load_attr.hits, hits + 1);

  EXPECT_FALSE(sdh_died_mid_call)
      << "the __getattr__ hook was deallocated during its own call";
  EXPECT_EQ(sdh_dealloc_count, 1);
  sdh_armed = false;
  sdh_owner = nullptr;
}

TEST_F(AttrCache311Test, SplitMissHookSurvivesDeletingItselfMidCall) {
  const char* src = R"(
class S:
    def __init__(self, with_x):
        if with_x:
            self.x = 11

filled = S(True)
victim = S(False)
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> klass = PyDict_GetItemString(globals, "S");
  BorrowedRef<> filled_inst = PyDict_GetItemString(globals, "filled");
  BorrowedRef<> victim = PyDict_GetItemString(globals, "victim");
  ASSERT_NE(klass, nullptr);
  ASSERT_NE(filled_inst, nullptr);
  ASSERT_NE(victim, nullptr);
  installSelfDeletingHook(klass, 7);

  auto name = Ref<>::steal(PyUnicode_InternFromString("x"));
  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  // Cold call on the populated instance fills the kSplit entry ("x"
  // lives in the shared keys).
  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), filled_inst, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 11);
  ASSERT_EQ(stats.load_attr.fills, fills + 1)
      << "the split entry never filled; the rest is vacuous";

  // Armed hit on the victim: its value slot is empty, so the fallback
  // resolves __getattr__ from the type dict as a borrowed pointer and
  // calls it; the call deletes the entry holding the last reference.
  ASSERT_NE(*_PyObject_ValuesPointer(victim.get()), nullptr);
  sdh_armed = true;
  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), victim, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(second, "hook:7:x"), 0);
  ASSERT_EQ(stats.load_attr.hits, hits + 1);

  EXPECT_FALSE(sdh_died_mid_call)
      << "the __getattr__ hook was deallocated during its own call";
  EXPECT_EQ(sdh_dealloc_count, 1);
  sdh_armed = false;
  sdh_owner = nullptr;
}

TEST_F(AttrCache311Test, MemberDescrMissRoutesToThePinnedHook) {
  ensureMemberHostType();
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  ASSERT_EQ(
      PyDict_SetItemString(
          globals, "MemberHost", reinterpret_cast<PyObject*>(&MemberHost_Type)),
      0);
  const char* src = R"(
class N(MemberHost):
    pass

full = N()
full.slotval = "present"
empty = N()
)";
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> klass = PyDict_GetItemString(globals, "N");
  BorrowedRef<> full = PyDict_GetItemString(globals, "full");
  BorrowedRef<> empty = PyDict_GetItemString(globals, "empty");
  ASSERT_NE(klass, nullptr);
  ASSERT_NE(full, nullptr);
  ASSERT_NE(empty, nullptr);
  installSelfDeletingHook(klass, 9);

  auto name = Ref<>::steal(PyUnicode_InternFromString("slotval"));
  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  // Cold call on the populated instance fills the kMemberDescr entry
  // (the member is a data descriptor from the C base) and caches the
  // hook alongside it.
  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), full, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(first, "present"), 0);
  ASSERT_EQ(stats.load_attr.fills, fills + 1)
      << "the member entry never filled; the rest is vacuous";

  // Armed hit on the empty instance: the T_OBJECT_EX read raises
  // AttributeError, which routes to the pinned cached hook; the hook's
  // call deletes the class's __getattr__ entry -- its last reference.
  sdh_armed = true;
  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), empty, name));
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(second, "hook:9:slotval"), 0);
  ASSERT_EQ(stats.load_attr.hits, hits + 1);

  EXPECT_FALSE(sdh_died_mid_call)
      << "the __getattr__ hook was deallocated during its own call";
  EXPECT_EQ(sdh_dealloc_count, 1);
  sdh_armed = false;
  sdh_owner = nullptr;
}

TEST_F(AttrCache311Test, MemberDescrMissWithoutHookPropagatesAttributeError) {
  ensureMemberHostType();
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  ASSERT_EQ(
      PyDict_SetItemString(
          globals, "MemberHost", reinterpret_cast<PyObject*>(&MemberHost_Type)),
      0);
  const char* src = R"(
class P(MemberHost):
    pass

full = P()
full.slotval = "present"
empty = P()
)";
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> full = PyDict_GetItemString(globals, "full");
  BorrowedRef<> empty = PyDict_GetItemString(globals, "empty");
  ASSERT_NE(full, nullptr);
  ASSERT_NE(empty, nullptr);

  auto name = Ref<>::steal(PyUnicode_InternFromString("slotval"));
  auto cache = makeLoadAttrCache();
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  uint64_t fills = stats.load_attr.fills;
  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), full, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(first, "present"), 0);
  ASSERT_EQ(stats.load_attr.fills, fills + 1)
      << "the member entry never filled; the rest is vacuous";

  // With no __getattr__ on the receiver, the cached member read must
  // propagate the AttributeError instead of clearing it.
  uint64_t hits = stats.load_attr.hits;
  auto second =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), empty, name));
  ASSERT_EQ(second, nullptr);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_AttributeError));
  PyErr_Clear();
  ASSERT_EQ(stats.load_attr.hits, hits + 1);
}

// --- Type-receiver caches -------------------------------------------------
//
// The 3.11 IR does not currently hand these caches a statically-typed
// receiver (no global or builtin load carries an object spec, so the
// type(obj) / isinstance / len specializations never fire), which is why
// the Python-level coverage test still sees zero traffic.  The caches
// themselves are real, pull-validated code, and these tests drive their
// helpers directly so the guards are proven rather than assumed.

namespace {

// Build `class Meta(type): pass` / `class T(metaclass=Meta): ...` and hand
// back the pieces the tests need.
struct TypeFixture {
  Ref<PyObject> globals;
  BorrowedRef<PyTypeObject> meta;
  BorrowedRef<PyTypeObject> type;
};

TypeFixture makeTypeFixture(PyObject* globals, const char* extra_body) {
  std::string src =
      "class Meta(type):\n"
      "    pass\n"
      "class T(metaclass=Meta):\n";
  src += extra_body;
  TypeFixture f;
  f.globals = Ref<PyObject>::create(globals);
  JIT_CHECK(f.globals != nullptr, "no globals");
  auto result = Ref<>::steal(
      PyRun_String(src.c_str(), Py_file_input, f.globals, f.globals));
  JIT_CHECK(result != nullptr, "fixture source failed");
  f.meta =
      reinterpret_cast<PyTypeObject*>(PyDict_GetItemString(f.globals, "Meta"));
  f.type =
      reinterpret_cast<PyTypeObject*>(PyDict_GetItemString(f.globals, "T"));
  return f;
}

} // namespace

TEST_F(AttrCache311Test, TypeAttrCacheRetiresOnOwnerAndMetaclassMutation) {
  Ref<PyObject> g(MakeGlobals());
  auto f = makeTypeFixture(g, "    CONST = 41\n");
  ASSERT_NE(f.type, nullptr);
  ASSERT_NE(f.meta, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("CONST"));
  auto recv = reinterpret_cast<PyObject*>(f.type.get());

  jit::LoadTypeAttrCache cache;
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  // Fill, then hit.
  uint64_t fills = stats.load_type_attr.fills;
  auto first = Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyLong_AsLong(first), 41);
  ASSERT_EQ(stats.load_type_attr.fills, fills + 1)
      << "the type-attr cache never filled; the rest is vacuous";
  uint64_t hits = stats.load_type_attr.hits;
  auto again = Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(PyLong_AsLong(again), 41);
  ASSERT_EQ(stats.load_type_attr.hits, hits + 1);

  // Owner mutation: the class dict changes, the version tag moves.
  auto updated = Ref<>::steal(PyLong_FromLong(99));
  ASSERT_EQ(PyObject_SetAttrString(recv, "CONST", updated), 0);
  auto after_owner =
      Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(after_owner, nullptr);
  EXPECT_EQ(PyLong_AsLong(after_owner), 99)
      << "a stale owner-version entry served the old class attribute";

  // Metaclass mutation: a same-named DATA descriptor on the metaclass wins
  // over the class dict in stock type_getattro, and mutating the metaclass
  // never bumps the owner's version tag -- only the metaclass guard sees it.
  const char* meta_src = "Meta.CONST = property(lambda cls: 'meta-descr')\n";
  auto meta_result =
      Ref<>::steal(PyRun_String(meta_src, Py_file_input, f.globals, f.globals));
  ASSERT_NE(meta_result, nullptr);
  auto after_meta =
      Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(after_meta, nullptr);
  EXPECT_TRUE(PyUnicode_Check(after_meta.get()))
      << "the metaclass data descriptor did not take over";
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(after_meta, "meta-descr"), 0);
}

TEST_F(AttrCache311Test, TypeAttrCacheRetiresWhenValueTypeGainsDescriptor) {
  // A plain class attribute is cachable only because ITS type has no
  // tp_descr_get today.  When that heap type later grows __get__, stock
  // routes the load through the descriptor; the value-type guard is what
  // makes the cached entry step aside.
  // Everything is defined in one pass: assigning to T after the fact would
  // run PyType_Modified and strip the owner's version tag, and the fill
  // legitimately refuses a type it cannot pin.
  Ref<PyObject> globals(MakeGlobals());
  const char* src =
      "class Box:\n"
      "    pass\n"
      "class Meta(type):\n"
      "    pass\n"
      "class T(metaclass=Meta):\n"
      "    V = Box()\n";
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<PyTypeObject> type{PyDict_GetItemString(globals, "T")};
  ASSERT_NE(type, nullptr);
  auto name = Ref<>::steal(PyUnicode_InternFromString("V"));
  auto recv = reinterpret_cast<PyObject*>(type.get());

  jit::LoadTypeAttrCache cache;
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();
  uint64_t fills = stats.load_type_attr.fills;
  auto first = Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(first, nullptr);
  ASSERT_EQ(stats.load_type_attr.fills, fills + 1)
      << "the plain class attribute never cached; the rest is vacuous";
  auto again = Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_EQ(again, first);

  const char* grow =
      "Box.__get__ = lambda self, obj, objtype=None: 'now-descr'\n";
  auto grown =
      Ref<>::steal(PyRun_String(grow, Py_file_input, globals, globals));
  ASSERT_NE(grown, nullptr);
  auto after = Ref<>::steal(jit::LoadTypeAttrCache::invoke(&cache, recv, name));
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(PyUnicode_Check(after.get()))
      << "the value's new __get__ was not honored by the cached load";
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(after, "now-descr"), 0);
}

TEST_F(AttrCache311Test, TypeMethodCacheRetiresOnMetaclassDataDescriptor) {
  Ref<PyObject> g(MakeGlobals());
  auto f = makeTypeFixture(
      g,
      "    @classmethod\n"
      "    def cm(cls):\n"
      "        return 'class-cm'\n");
  auto name = Ref<>::steal(PyUnicode_InternFromString("cm"));

  jit::LoadTypeMethodCache cache;
  const jit::AttrCacheStats311& stats = jit::attrCacheStats311();

  uint64_t fills = stats.load_type_method.fills;
  auto filled = jit::LoadTypeMethodCache::lookupHelper(&cache, f.type, name);
  ASSERT_NE(filled.callable, nullptr);
  Py_XDECREF(filled.callable);
  Py_XDECREF(filled.self_or_null);
  ASSERT_EQ(stats.load_type_method.fills, fills + 1)
      << "the classmethod never cached; the rest is vacuous";

  // Fast-path hit through the helper the inline arm calls.
  uint64_t hits = stats.load_type_method.hits;
  auto hit = jit::LoadTypeMethodCache::getValueHelper(
      &cache, reinterpret_cast<PyObject*>(f.type.get()));
  ASSERT_NE(hit.callable, nullptr);
  Py_XDECREF(hit.callable);
  Py_XDECREF(hit.self_or_null);
  ASSERT_EQ(stats.load_type_method.hits, hits + 1);

  // The metaclass grows a same-named data descriptor.  The owner class is
  // untouched, so only the metaclass guard can catch this.
  const char* meta_src =
      "Meta.cm = property(lambda cls: (lambda: 'meta-cm'))\n";
  auto meta_result =
      Ref<>::steal(PyRun_String(meta_src, Py_file_input, f.globals, f.globals));
  ASSERT_NE(meta_result, nullptr);

  uint64_t invalidations = stats.load_type_method.invalidations;
  auto after = jit::LoadTypeMethodCache::getValueHelper(
      &cache, reinterpret_cast<PyObject*>(f.type.get()));
  EXPECT_EQ(stats.load_type_method.invalidations, invalidations + 1)
      << "the stale entry was served after the metaclass gained a data "
         "descriptor";
  ASSERT_NE(after.self_or_null, nullptr);
  auto produced = Ref<>::steal(PyObject_CallNoArgs(after.self_or_null));
  Py_XDECREF(after.callable);
  Py_XDECREF(after.self_or_null);
  ASSERT_NE(produced, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(produced, "meta-cm"), 0);
}

TEST_F(AttrCache311Test, MethodPeekTracksMaterializedShadowChanges) {
  const char* src = R"(
class P:
    def method(self):
        return "class"

p = P()
q = P()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> p = PyDict_GetItemString(globals, "p");
  BorrowedRef<> q = PyDict_GetItemString(globals, "q");
  auto name = Ref<>::steal(PyUnicode_InternFromString("method"));
  jit::LoadMethodCache cache;

  auto first = jit::LoadMethodCache::lookupHelper(&cache, p, name);
  ASSERT_NE(first.callable, nullptr);
  Py_XDECREF(first.callable);
  Py_XDECREF(first.self_or_null);

  auto p_dict = Ref<>::steal(PyObject_GenericGetDict(p, nullptr));
  auto q_dict = Ref<>::steal(PyObject_GenericGetDict(q, nullptr));
  ASSERT_NE(p_dict, nullptr);
  ASSERT_NE(q_dict, nullptr);
  auto shadow_src = Ref<>::steal(PyRun_String(
      "shadow = lambda: 'instance'\n", Py_file_input, globals, globals));
  ASSERT_NE(shadow_src, nullptr);
  BorrowedRef<> shadow = PyDict_GetItemString(globals, "shadow");
  ASSERT_EQ(PyDict_SetItem(p_dict, name, shadow), 0);

  auto shadowed = jit::LoadMethodCache::lookupHelper(&cache, p, name);
  EXPECT_EQ(shadowed.callable, Py_None);
  EXPECT_EQ(shadowed.self_or_null, shadow.get());
  Py_XDECREF(shadowed.callable);
  Py_XDECREF(shadowed.self_or_null);

  auto unshadowed = jit::LoadMethodCache::lookupHelper(&cache, q, name);
  ASSERT_NE(unshadowed.callable, nullptr);
  EXPECT_EQ(unshadowed.self_or_null, q.get());
  Py_XDECREF(unshadowed.callable);
  Py_XDECREF(unshadowed.self_or_null);

  ASSERT_EQ(PyDict_DelItem(p_dict, name), 0);
  auto restored = jit::LoadMethodCache::lookupHelper(&cache, p, name);
  ASSERT_NE(restored.callable, nullptr);
  EXPECT_EQ(restored.self_or_null, p.get());
  Py_XDECREF(restored.callable);
  Py_XDECREF(restored.self_or_null);
}

TEST_F(AttrCache311Test, MethodPeekPropagatesCombinedDictKeyErrorOnce) {
  const char* src = R"(
class UniqueError(Exception):
    pass

class Key:
    count = 0
    def __hash__(self):
        return hash("method")
    def __eq__(self, other):
        Key.count += 1
        raise UniqueError("combined-key-error")

class P:
    def method(self):
        return "class"

p = P()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> inst = PyDict_GetItemString(globals, "p");
  BorrowedRef<> key_type = PyDict_GetItemString(globals, "Key");
  BorrowedRef<> unique_error = PyDict_GetItemString(globals, "UniqueError");
  auto name = Ref<>::steal(PyUnicode_InternFromString("method"));
  jit::LoadMethodCache cache;

  auto first = jit::LoadMethodCache::lookupHelper(&cache, inst, name);
  ASSERT_NE(first.callable, nullptr);
  Py_XDECREF(first.callable);
  Py_XDECREF(first.self_or_null);

  auto late = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_EQ(PyObject_SetAttrString(inst, "late", late), 0);
  auto upgraded = jit::LoadMethodCache::lookupHelper(&cache, inst, name);
  ASSERT_NE(upgraded.callable, nullptr);
  Py_XDECREF(upgraded.callable);
  Py_XDECREF(upgraded.self_or_null);

  auto dict = Ref<>::steal(PyObject_GenericGetDict(inst, nullptr));
  auto key = Ref<>::steal(PyObject_CallNoArgs(key_type));
  auto marker = Ref<>::steal(PyLong_FromLong(2));
  ASSERT_EQ(PyDict_SetItem(dict, key, marker), 0);
  auto zero = Ref<>::steal(PyLong_FromLong(0));
  ASSERT_EQ(PyObject_SetAttrString(key_type, "count", zero), 0);

  auto failed = jit::LoadMethodCache::lookupHelper(&cache, inst, name);
  EXPECT_EQ(failed.callable, nullptr);
  EXPECT_EQ(failed.self_or_null, nullptr);
  ASSERT_TRUE(PyErr_ExceptionMatches(unique_error));
  PyErr_Clear();
  auto count = Ref<>::steal(PyObject_GetAttrString(key_type, "count"));
  EXPECT_EQ(PyLong_AsLong(count), 1);
}

TEST_F(AttrCache311Test, TypeReceiverCacheRetiresOnMetaclassMutation) {
  const char* src = R"(
class Meta(type):
    pass

class T(metaclass=Meta):
    value = "class"
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> type = PyDict_GetItemString(globals, "T");
  auto name = Ref<>::steal(PyUnicode_InternFromString("value"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(first, nullptr);
  auto hit = Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(hit, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(hit, "class"), 0);

  auto mutate = Ref<>::steal(PyRun_String(
      "Meta.value = property(lambda cls: 'meta')\n",
      Py_file_input,
      globals,
      globals));
  ASSERT_NE(mutate, nullptr);
  auto after =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(after, "meta"), 0);
}

TEST_F(AttrCache311Test, TypeReceiverCachePropagatesDescriptorError) {
  const char* src = R"(
class RaisingDescriptor:
    def __get__(self, obj, owner):
        if obj.raise_now:
            raise RuntimeError("cached descriptor error")
        return "ok"
    def __set__(self, obj, value):
        raise AssertionError("not used")

class Meta(type):
    value = RaisingDescriptor()

class T(metaclass=Meta):
    raise_now = False
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> type = PyDict_GetItemString(globals, "T");
  auto name = Ref<>::steal(PyUnicode_InternFromString("value"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(first, "ok"), 0);

  ASSERT_EQ(PyObject_SetAttrString(type, "raise_now", Py_True), 0);
  EXPECT_EQ(jit::LoadAttrCache::invoke(cache.get(), type, name), nullptr);
  ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
  PyErr_Clear();
}

TEST_F(AttrCache311Test, TypeReceiverCacheRetiresOnPayloadTypeMutation) {
  const char* src = R"(
class Value:
    pass

class T:
    value = Value()
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> type = PyDict_GetItemString(globals, "T");
  auto name = Ref<>::steal(PyUnicode_InternFromString("value"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(first, nullptr);
  auto hit = Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_EQ(hit.get(), first.get());

  auto mutate = Ref<>::steal(PyRun_String(
      "Value.__get__ = lambda self, obj, owner: 'descriptor'\n",
      Py_file_input,
      globals,
      globals));
  ASSERT_NE(mutate, nullptr);
  auto after =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(after, "descriptor"), 0);
}

TEST_F(AttrCache311Test, TypeReceiverCacheRetiresOnDescriptorTypeMutation) {
  const char* src = R"(
class Descriptor:
    def __get__(self, obj, owner):
        return "meta"
    def __set__(self, obj, value):
        raise AssertionError("not used")

class Meta(type):
    value = Descriptor()

class T(metaclass=Meta):
    value = "class"
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals, nullptr);
  auto result =
      Ref<>::steal(PyRun_String(src, Py_file_input, globals, globals));
  ASSERT_NE(result, nullptr);
  BorrowedRef<> type = PyDict_GetItemString(globals, "T");
  auto name = Ref<>::steal(PyUnicode_InternFromString("value"));
  auto cache = makeLoadAttrCache();

  auto first =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(first, "meta"), 0);

  auto mutate = Ref<>::steal(PyRun_String(
      "del Descriptor.__get__\n", Py_file_input, globals, globals));
  ASSERT_NE(mutate, nullptr);
  auto after =
      Ref<>::steal(jit::LoadAttrCache::invoke(cache.get(), type, name));
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(after, "class"), 0);
}

#endif
