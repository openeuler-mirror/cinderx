// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/util.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX < 0x030C0000
#include "internal/pycore_code.h"

class QuickenedCount311Test : public RuntimeTest {};

namespace {
Py_ssize_t stockQuickenedCount() {
  auto value =
      Ref<>::steal(PyObject_CallNoArgs(PySys_GetObject("_getquickenedcount")));
  JIT_CHECK(value != nullptr, "stock quickened count failed");
  return PyLong_AsSsize_t(value);
}
} // namespace

TEST_F(QuickenedCount311Test, CreditPreservesPendingException) {
  Ref<PyFunctionObject> function(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(function, nullptr);
  auto* code = reinterpret_cast<PyCodeObject*>(function->func_code);
  ASSERT_NE(code->co_warmup, 0);
  auto before = stockQuickenedCount();
  auto marker = Ref<>::steal(PyUnicode_FromString("keep this exception"));
  PyErr_SetObject(PyExc_ValueError, marker);
  PyObject *saved_type, *saved_value, *saved_tb;
  PyErr_Fetch(&saved_type, &saved_value, &saved_tb);
  auto expected_type = Ref<>::steal(saved_type);
  auto expected_value = Ref<>::steal(saved_value);
  auto expected_tb = Ref<>::steal(saved_tb);
  PyErr_Restore(
      Py_XNewRef(saved_type), Py_XNewRef(saved_value), Py_XNewRef(saved_tb));
  code->co_warmup = 0;
  _PyCode_Quicken(code);
  PyObject *type, *value, *traceback;
  PyErr_Fetch(&type, &value, &traceback);
  EXPECT_EQ(type, expected_type.get());
  EXPECT_EQ(value, expected_value.get());
  EXPECT_EQ(traceback, expected_tb.get());
  Py_XDECREF(type);
  Py_XDECREF(value);
  Py_XDECREF(traceback);
  EXPECT_EQ(code->co_warmup, 0);
  EXPECT_EQ(stockQuickenedCount(), before + 1);
}

TEST_F(QuickenedCount311Test, AllocationFailureDefersWithoutPublishingCredit) {
  Ref<PyFunctionObject> function(compileAndGet("def f(): return 1", "f"));
  ASSERT_NE(function, nullptr);
  auto* code = reinterpret_cast<PyCodeObject*>(function->func_code);
  ASSERT_NE(code->co_warmup, 0);
  auto first_instruction = _PyCode_CODE(code)[0];
  auto before = stockQuickenedCount();
  struct Fault {
    PyMemAllocatorEx original;
    bool pending = true;
  } fault;
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &fault.original);
  PyMemAllocatorEx allocator{};
  allocator.ctx = &fault;
  allocator.malloc = [](void* context, size_t size) -> void* {
    auto& f = *static_cast<Fault*>(context);
    if (f.pending) {
      f.pending = false;
      return nullptr;
    }
    return f.original.malloc(f.original.ctx, size);
  };
  allocator.calloc = [](void* context, size_t nelem, size_t elsize) -> void* {
    auto& f = *static_cast<Fault*>(context);
    return f.original.calloc(f.original.ctx, nelem, elsize);
  };
  allocator.realloc = [](void* context, void* pointer, size_t size) -> void* {
    auto& f = *static_cast<Fault*>(context);
    return f.original.realloc(f.original.ctx, pointer, size);
  };
  allocator.free = [](void* context, void* pointer) {
    auto& f = *static_cast<Fault*>(context);
    f.original.free(f.original.ctx, pointer);
  };
  {
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
    SCOPE_EXIT(PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &fault.original));
    code->co_warmup = 0;
    _PyCode_Quicken(code);
  }
  ASSERT_FALSE(fault.pending) << "the accounting allocation was not exercised";
  EXPECT_EQ(PyErr_Occurred(), nullptr);
  EXPECT_NE(code->co_warmup, 0);
  EXPECT_EQ(_PyCode_CODE(code)[0], first_instruction);
  EXPECT_EQ(stockQuickenedCount(), before);
  // The optional optimization can retry once the allocation succeeds.
  code->co_warmup = 0;
  _PyCode_Quicken(code);
  EXPECT_EQ(stockQuickenedCount(), before + 1);
}
#endif
