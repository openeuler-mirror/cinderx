// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/RuntimeTests/fixtures.h"

#include "cinderx/Jit/hir/hir.h"

using ArrayStoreTest = RuntimeTest;

TEST_F(ArrayStoreTest, OversizedIndexUsesIndexErrorConversion) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def store_array_double(a):
    a[1267650600228229401496703205376] = 1.5
)",
      "store_array_double",
      irfunc);
  ASSERT_NE(irfunc, nullptr);
  bool found = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsIndexUnbox()) {
        found = true;
        EXPECT_EQ(
            static_cast<const jit::hir::IndexUnbox&>(instr).exception(),
            PyExc_IndexError);
      }
    }
  }
  EXPECT_TRUE(found);
}

// Test that getStdlibArrayType returns a valid type object.
TEST_F(ArrayStoreTest, GetStdlibArrayTypeReturnsNonNull) {
  auto func = compileAndGet(
      R"(
def dummy():
    pass
)",
      "dummy");
  ASSERT_NE(func, nullptr);

  // Import array module to ensure it's available
  auto array_mod = Ref<>::steal(PyImport_ImportModule("array"));
  ASSERT_NE(array_mod, nullptr);

  auto array_type = Ref<>::steal(
      PyObject_GetAttrString(array_mod, "array"));
  ASSERT_NE(array_type, nullptr);
  ASSERT_TRUE(PyType_Check(array_type));
}

// Test that StdlibArrayObject layout is correct for array('d').
TEST_F(ArrayStoreTest, ArrayDoubleLayoutValidation) {
  auto array_mod = Ref<>::steal(PyImport_ImportModule("array"));
  ASSERT_NE(array_mod, nullptr);

  auto array_type = Ref<>::steal(
      PyObject_GetAttrString(array_mod, "array"));
  ASSERT_NE(array_type, nullptr);

  // Create array('d', [1.5]) and verify we can read the value
  auto d_str = Ref<>::steal(PyUnicode_InternFromString("d"));
  ASSERT_NE(d_str, nullptr);
  auto val_list = Ref<>::steal(PyList_New(1));
  ASSERT_NE(val_list, nullptr);
  auto val = PyFloat_FromDouble(1.5);
  ASSERT_NE(val, nullptr);
  PyList_SET_ITEM(val_list, 0, val);

  auto args = Ref<>::steal(PyTuple_Pack(2, d_str.get(), val_list.get()));
  ASSERT_NE(args, nullptr);

  auto arr = Ref<>::steal(PyObject_CallObject(array_type, args));
  ASSERT_NE(arr, nullptr);

  // Verify it has the right typecode
  auto tc = Ref<>::steal(PyObject_GetAttrString(arr, "typecode"));
  ASSERT_NE(tc, nullptr);
  auto tc_str = PyUnicode_AsUTF8(tc);
  ASSERT_STREQ(tc_str, "d");

  // Verify the value is correct
  auto item = Ref<>::steal(PyObject_GetItem(arr, PyLong_FromLong(0)));
  ASSERT_NE(item, nullptr);
  double dval = PyFloat_AsDouble(item);
  ASSERT_EQ(dval, 1.5);
}

// Test that STORE_SUBSCR on array('d') generates StoreArrayItem in HIR.
TEST_F(ArrayStoreTest, StoreSubscrArrayDoubleGeneratesStoreArrayItem) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
from array import array
def store_array_double(a):
    a[0] = 1.5
)",
      "store_array_double",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  // Walk the HIR and check for StoreArrayItem with TCDouble type
  bool found_store_array_item = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsStoreArrayItem()) {
        auto* sai = static_cast<const jit::hir::StoreArrayItem*>(&instr);
        if (sai->type() <= jit::hir::TCDouble) {
          found_store_array_item = true;
        }
      }
    }
  }

  EXPECT_TRUE(found_store_array_item)
      << "Expected StoreArrayItem(TCDouble) in HIR for array('d') store";
}

// Test that STORE_SUBSCR with unknown index/value shapes stays generic.
TEST_F(ArrayStoreTest, StoreSubscrUnknownShapeGeneratesGenericPath) {
  std::unique_ptr<jit::hir::Function> irfunc;
  CompileToHIR(
      R"(
def store_list(a, i, v):
    a[i] = v
)",
      "store_list",
      irfunc);

  ASSERT_NE(irfunc, nullptr);

  bool found_store_array_item = false;
  bool found_store_subscr = false;
  for (auto& block : irfunc->cfg.blocks) {
    for (auto it = block.begin(); it != block.end(); ++it) {
      auto& instr = *it;
      if (instr.IsStoreArrayItem()) {
        found_store_array_item = true;
      }
      if (instr.IsStoreSubscr()) {
        found_store_subscr = true;
      }
    }
  }

  EXPECT_FALSE(found_store_array_item)
      << "Did not expect StoreArrayItem for unknown store shapes";
  EXPECT_TRUE(found_store_subscr) << "Expected StoreSubscr in HIR";
}

TEST_F(ArrayStoreTest, CompiledStoreArrayDoubleFastPathUpdatesArray) {
  runCode(R"(
from array import array
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def store_array_double(a):
    a[1] = 42.5

for _ in range(20):
    store_array_double(array("d", [1.0, 2.0, 3.0]))

assert jit.force_compile(store_array_double)
counts = jit.get_function_hir_opcode_counts(store_array_double)
assert counts.get("StoreArrayItem", 0) > 0

arr = array("d", [1.0, 2.0, 3.0])
store_array_double(arr)
assert list(arr) == [1.0, 42.5, 3.0]
)");
}

TEST_F(ArrayStoreTest, CompiledStoreArrayDoubleGuardMissUsesGenericStore) {
  runCode(R"(
from array import array
import cinderx.jit as jit

jit.enable_specialized_opcodes()

def store_any(a):
    a[1] = 4.5

def generic_store(a):
    a[1] = 4.5

def exception_info(func, *args):
    try:
        func(*args)
    except Exception as exc:
        return type(exc), str(exc)
    return None, None

for _ in range(20):
    store_any(array("d", [1.0, 2.0, 3.0]))

assert jit.force_compile(store_any)
counts = jit.get_function_hir_opcode_counts(store_any)
assert counts.get("StoreArrayItem", 0) > 0

lst = [1, 2, 3]
store_any(lst)
assert lst == [1, 4.5, 3]

jit_error = exception_info(store_any, array("i", [1, 2, 3]))
generic_error = exception_info(generic_store, array("i", [1, 2, 3]))
assert jit_error[0] is generic_error[0]
assert jit_error[1] == generic_error[1]

jit_error = exception_info(store_any, (1, 2, 3))
generic_error = exception_info(generic_store, (1, 2, 3))
assert jit_error[0] is generic_error[0]
assert jit_error[1] == generic_error[1]
)");
}
