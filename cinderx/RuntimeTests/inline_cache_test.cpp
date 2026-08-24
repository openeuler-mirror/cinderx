// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX > 0x030E0000
#include <pycore_unicodeobject.h>
#endif

#include <cstring>
#include <memory>
#include <new>

class InlineCacheTest : public RuntimeTest {};

class TestLoadAttrCache : public jit::LoadAttrCache {
 public:
  std::span<jit::AttributeMutator> entriesForTest() {
    return entries();
  }
};

struct TestLoadAttrCacheDeleter {
  void operator()(TestLoadAttrCache* cache) const {
    cache->~TestLoadAttrCache();
    PyMem_Free(cache);
  }
};

std::unique_ptr<TestLoadAttrCache, TestLoadAttrCacheDeleter>
makeTestLoadAttrCache() {
  void* mem = PyMem_Calloc(1, jit::AttributeCacheSizeTrait::size());
  JIT_CHECK(mem != nullptr, "Failed to allocate test load attr cache");
  return std::unique_ptr<TestLoadAttrCache, TestLoadAttrCacheDeleter>{
      new (mem) TestLoadAttrCache()};
}

int countEntriesForType(TestLoadAttrCache* cache, PyTypeObject* type) {
  int count = 0;
  for (auto& entry : cache->entriesForTest()) {
    if (entry.type() == type) {
      count++;
    }
  }
  return count;
}

int countEntriesForDescrType(TestLoadAttrCache* cache, PyTypeObject* type) {
  int count = 0;
  for (auto& entry : cache->entriesForTest()) {
    if (entry.watchedDescrType() == type) {
      count++;
    }
  }
  return count;
}

void expectUnicodeEquals(PyObject* obj, const char* expected) {
  ASSERT_TRUE(PyUnicode_Check(obj)) << "Expected a unicode result";
  ASSERT_EQ(PyUnicode_CompareWithASCIIString(obj, expected), 0);
}

TEST_F(InlineCacheTest, AttributeMutatorTaggedKindMaskKeepsTypePointer) {
  jit::AttributeMutator mutator;
  ASSERT_TRUE(mutator.isEmpty());
  EXPECT_EQ(mutator.type(), nullptr);

  auto* type = &PyBaseObject_Type;
  mutator.set_combined(type);
  EXPECT_FALSE(mutator.isEmpty());
  EXPECT_EQ(mutator.type(), type);
  EXPECT_EQ(mutator.watchedDescrType(), nullptr);

  mutator.set_split(type, 0, nullptr, false);
  EXPECT_EQ(mutator.type(), type);
  EXPECT_EQ(mutator.watchedDescrType(), nullptr);

  mutator.set_split(type, 0, nullptr, true);
  EXPECT_EQ(mutator.type(), type);
  EXPECT_EQ(mutator.watchedDescrType(), nullptr);

  mutator.set_descr_or_classvar(type, Py_None, 0);
  EXPECT_EQ(mutator.type(), type);
  EXPECT_EQ(mutator.watchedDescrType(), nullptr);

  mutator.set_data_descr(type, Py_None);
  EXPECT_EQ(mutator.type(), type);
  EXPECT_EQ(mutator.watchedDescrType(), Py_TYPE(Py_None));

  mutator.reset();
  EXPECT_TRUE(mutator.isEmpty());
  EXPECT_EQ(mutator.type(), nullptr);
}

TEST_F(InlineCacheTest, LoadTypeMethodCacheLookUp) {
  const char* src = R"(
from abc import ABCMeta, abstractmethod

class RequestContext:

  @classmethod
  def class_meth(cls):
    pass

  @staticmethod
  def static_meth():
    pass

  def regular_meth():
    pass

class_meth = RequestContext.class_meth.__func__
static_meth = RequestContext.static_meth
regular_meth = RequestContext.regular_meth
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* klass = PyDict_GetItemString(locals, "RequestContext");
  ASSERT_NE(klass, nullptr) << "Couldn't get class RequestContext";

  auto py_class_meth = Ref<>::steal(PyUnicode_FromString("class_meth"));
  jit::LoadTypeMethodCache cache;
  auto res = cache.lookup(klass, py_class_meth);
  ASSERT_EQ(res.self_or_null, klass)
      << "Expected instance to be equal to class from cache look up";
  PyObject* class_meth = PyDict_GetItemString(locals, "class_meth");
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, class_meth, Py_EQ), 1)
      << "Expected method " << class_meth << " to be equal from cache lookup";
  ASSERT_EQ(cache.value(), res.callable)
      << "Expected method " << py_class_meth << " to be cached";

  for (auto& meth : {"static_meth", "regular_meth"}) {
    auto name = Ref<>::steal(PyUnicode_FromString(meth));
    jit::LoadTypeMethodCache methCache;
    auto methRes = methCache.lookup(klass, name);
    PyObject* py_meth = PyDict_GetItemString(locals, meth);
#if PY_VERSION_HEX < 0x030E0000
    ASSERT_EQ(methRes.callable, Py_None)
        << "Expected first part of cache result to be Py_None";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.self_or_null, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.self_or_null)
        << "Expected method " << meth << " to be cached";
#else
    ASSERT_EQ(methRes.self_or_null, nullptr)
        << "Expected first part of cache result to be nullptr";
    ASSERT_EQ(PyObject_RichCompareBool(methRes.callable, py_meth, Py_EQ), 1)
        << "Expected method " << meth << " to be equal from cache lookup";
    ASSERT_EQ(methCache.value(), methRes.callable)
        << "Expected method " << meth << " to be cached";
#endif
  }
}

TEST_F(InlineCacheTest, LoadModuleMethodCacheLookUp) {
  const char* src = R"(
import functools
module_meth = functools._unwrap_partial
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  PyObject* functools_mod = PyDict_GetItemString(locals, "functools");
  ASSERT_NE(functools_mod, nullptr) << "Couldn't get module functools";

  PyObject* module_meth = PyDict_GetItemString(locals, "module_meth");
  ASSERT_NE(module_meth, nullptr) << "Couldn't get PyObject module_meth";

  PyObject* name_obj = PyUnicode_FromString("_unwrap_partial");
  ASSERT_NE(name_obj, nullptr) << "Couldn't create name object";
#if PY_VERSION_HEX >= 0x030E0000
  _PyUnicode_InternImmortal(PyInterpreterState_Get(), &name_obj);
#endif
  auto name = Ref<>::steal(name_obj);

  jit::LoadModuleMethodCache cache;
  auto res = cache.lookup(functools_mod, name);
#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(res.self_or_null, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(Py_None, res.callable)
      << "Expected Py_None to be returned from cache lookup";
#else
  ASSERT_EQ(PyObject_RichCompareBool(res.callable, module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
  ASSERT_EQ(nullptr, res.self_or_null)
      << "Expected nullptr to be returned in self_or_null from cache lookup";
#endif

#if PY_VERSION_HEX < 0x030E0000
  ASSERT_EQ(PyObject_RichCompareBool(cache.value(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#else
  ASSERT_EQ(PyObject_RichCompareBool(*cache.cache(), module_meth, Py_EQ), 1)
      << "Expected method " << name << " to be cached";
#endif
  ASSERT_EQ(
      PyObject_RichCompareBool(cache.moduleObj(), functools_mod, Py_EQ), 1)
      << "Expected functools to be cached as an obj";
}

#if PY_VERSION_HEX >= 0x030C0000
// The two tests below exercise the push-invalidation registry
// (notifyICsTypeChanged clearing entries through the TypeWatcher maps).
// 3.11 has no type watchers: the registry is deliberately inert there and
// entries retire by pull instead -- AttrCache311Test covers that side.
TEST_F(InlineCacheTest, LoadAttrCacheKeepsSharedDescrTypeWatcher) {
  runStockCode(R"(
class Descr:
    def __get__(self, obj, typ):
        return "descr"

    def __set__(self, obj, val):
        raise RuntimeError("unimplemented")

class T1:
    foo = Descr()

class T2:
    foo = Descr()

t1 = T1()
t2 = T2()
)");

  auto cache = makeTestLoadAttrCache();
  auto name = Ref<>::steal(PyUnicode_FromString("foo"));
  ASSERT_NE(name, nullptr);

  auto t1 = getGlobal("t1");
  auto t2 = getGlobal("t2");
  auto descr_type_obj = getGlobal("Descr");
  ASSERT_TRUE(PyType_Check(descr_type_obj.get()));
  auto* descr_type = reinterpret_cast<PyTypeObject*>(descr_type_obj.get());

  auto t1_result = Ref<>::steal(
      jit::LoadAttrCache::invoke(cache.get(), t1.get(), name.get()));
  ASSERT_NE(t1_result, nullptr);
  expectUnicodeEquals(t1_result.get(), "descr");

  auto t2_result = Ref<>::steal(
      jit::LoadAttrCache::invoke(cache.get(), t2.get(), name.get()));
  ASSERT_NE(t2_result, nullptr);
  expectUnicodeEquals(t2_result.get(), "descr");

  ASSERT_EQ(countEntriesForType(cache.get(), Py_TYPE(t1.get())), 1);
  ASSERT_EQ(countEntriesForType(cache.get(), Py_TYPE(t2.get())), 1);
  ASSERT_EQ(countEntriesForDescrType(cache.get(), descr_type), 2);

  jit::notifyICsTypeChanged(Py_TYPE(t1.get()));
  EXPECT_EQ(countEntriesForType(cache.get(), Py_TYPE(t1.get())), 0);
  EXPECT_EQ(countEntriesForType(cache.get(), Py_TYPE(t2.get())), 1);
  EXPECT_EQ(countEntriesForDescrType(cache.get(), descr_type), 1);

  jit::notifyICsTypeChanged(descr_type);
  EXPECT_EQ(countEntriesForType(cache.get(), Py_TYPE(t2.get())), 0);
  EXPECT_EQ(countEntriesForDescrType(cache.get(), descr_type), 0);
}

TEST_F(InlineCacheTest, LoadAttrCacheWatchesHeapImmutableTypes) {
  runStockCode(R"(
from cinderx import freeze_type

class Frozen:
    pass

freeze_type(Frozen)
obj = Frozen()
obj.foo = "cached"
)");

  auto obj = getGlobal("obj");
  auto* type = Py_TYPE(obj.get());
  ASSERT_TRUE(PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE));
  ASSERT_TRUE(PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE));

  auto cache = makeTestLoadAttrCache();
  auto name = Ref<>::steal(PyUnicode_FromString("foo"));
  ASSERT_NE(name, nullptr);

  auto result = Ref<>::steal(
      jit::LoadAttrCache::invoke(cache.get(), obj.get(), name.get()));
  ASSERT_NE(result, nullptr);
  expectUnicodeEquals(result.get(), "cached");
  ASSERT_EQ(countEntriesForType(cache.get(), type), 1);

  jit::notifyICsTypeChanged(type);
  EXPECT_EQ(countEntriesForType(cache.get(), type), 0);
}
#endif // PY_VERSION_HEX >= 0x030C0000
