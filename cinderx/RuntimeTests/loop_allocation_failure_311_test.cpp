// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <algorithm>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/observe.h"

using namespace jit;

namespace {

// A one-shot allocator fault, restored before making assertions or retrying.
// The optional counter gate isolates primitive reconstruction after deopt
// from allocations performed by the function entry and loop itself.
struct ObjectAllocationFault {
  PyMemAllocatorEx original{};
  size_t match_size{0};
  size_t observed_size{0};
  size_t failures{0};
  uint64_t organic_at_failure{0};
  uint64_t forced_at_failure{0};
  bool pending{true};
  bool fail{true};
  bool after_forced_deopt{false};
  uint64_t forced_before{0};

  bool reject(size_t size) {
    if (!pending || (match_size != 0 && size != match_size) ||
        (after_forced_deopt &&
         triggerStatsSnapshot().forced_deopt_hits <= forced_before)) {
      return false;
    }
    pending = false;
    observed_size = size;
    if (fail) {
      ++failures;
      auto stats = triggerStatsSnapshot();
      organic_at_failure = stats.organic_deopt_hits;
      forced_at_failure = stats.forced_deopt_hits;
    }
    return fail;
  }
};

template <typename F>
PyObject* withObjectAllocator(ObjectAllocationFault& fault, F&& operation) {
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &fault.original);
  PyMemAllocatorEx allocator{};
  allocator.ctx = &fault;
  allocator.malloc = [](void* context, size_t size) -> void* {
    auto& f = *static_cast<ObjectAllocationFault*>(context);
    return f.reject(size) ? nullptr : f.original.malloc(f.original.ctx, size);
  };
  allocator.calloc = [](void* context, size_t n, size_t size) -> void* {
    auto& f = *static_cast<ObjectAllocationFault*>(context);
    return f.reject(n * size) ? nullptr
                              : f.original.calloc(f.original.ctx, n, size);
  };
  allocator.realloc = [](void* context, void* ptr, size_t size) -> void* {
    auto& f = *static_cast<ObjectAllocationFault*>(context);
    return f.reject(size) ? nullptr
                          : f.original.realloc(f.original.ctx, ptr, size);
  };
  allocator.free = [](void* context, void* ptr) {
    auto& f = *static_cast<ObjectAllocationFault*>(context);
    f.original.free(f.original.ctx, ptr);
  };
  PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
  SCOPE_EXIT(PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &fault.original));
  return operation();
}

bool takeMemoryError() {
  bool result = PyErr_ExceptionMatches(PyExc_MemoryError);
  if (!result && PyErr_Occurred()) {
    PyErr_PrintEx(0);
  } else {
    PyErr_Clear();
  }
  return result;
}

int tracebackOffset(BorrowedRef<PyCodeObject> code) {
  PyObject *type, *value, *traceback;
  PyErr_Fetch(&type, &value, &traceback);
  int offset = -1;
  {
    auto cursor = Ref<>::create(traceback);
    while (cursor != nullptr && cursor != Py_None) {
      auto frame = Ref<>::steal(PyObject_GetAttrString(cursor, "tb_frame"));
      if (frame == nullptr) {
        break;
      }
      auto* pyframe = reinterpret_cast<PyFrameObject*>(frame.get());
      auto owner = Ref<PyCodeObject>::steal(PyFrame_GetCode(pyframe));
      if (owner == code) {
        offset = PyFrame_GetLasti(pyframe);
        break;
      }
      cursor = Ref<>::steal(PyObject_GetAttrString(cursor, "tb_next"));
    }
  }
  PyErr_Restore(type, value, traceback);
  return offset;
}

void holdFloatFreelist(std::vector<Ref<>>& held) {
  // CPython 3.11 caches at most 100 free floats. Keep more alive so the
  // next PyFloat_FromDouble must reach the object allocator.
  for (int i = 0; i < 128; ++i) {
    held.push_back(Ref<>::steal(PyFloat_FromDouble(i + 0.25)));
    JIT_CHECK(held.back() != nullptr, "failed draining the float freelist");
  }
}

} // namespace

class LoopAllocationFailure311Test : public RuntimeTest {};

TEST_F(
    LoopAllocationFailure311Test,
    RangeIteratorAllocationFailurePreservesArgumentsAndCanRetry) {
  auto start = Ref<>::steal(PyLong_FromLong(1000));
  auto stop = Ref<>::steal(PyLong_FromLong(1004));
  auto step = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(start, nullptr);
  ASSERT_NE(stop, nullptr);
  const auto start_refs = Py_REFCNT(start.get());
  const auto stop_refs = Py_REFCNT(stop.get());
  const auto step_refs = Py_REFCNT(step.get());
  ObjectAllocationFault fault;
  Ref<> failed = Ref<>::steal(withObjectAllocator(
      fault, [&]() { return JITRT_RangeIter311(3, start, stop, step); }));
  bool memory_error = takeMemoryError();
  EXPECT_EQ(fault.failures, 1u);
  EXPECT_EQ(failed, nullptr);
  EXPECT_TRUE(memory_error);
  EXPECT_EQ(Py_REFCNT(start.get()), start_refs);
  EXPECT_EQ(Py_REFCNT(stop.get()), stop_refs);
  EXPECT_EQ(Py_REFCNT(step.get()), step_refs);
  auto iter = Ref<>::steal(JITRT_RangeIter311(3, start, stop, step));
  ASSERT_NE(iter, nullptr);
  ASSERT_TRUE(Py_IS_TYPE(iter.get(), &PyRangeIter_Type));
  for (long value = 1000; value < 1004; ++value) {
    auto item = Ref<>::steal(PyIter_Next(iter));
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(PyLong_AsLong(item), value);
  }
  auto exhausted = Ref<>::steal(PyIter_Next(iter));
  EXPECT_EQ(exhausted, nullptr);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(
    LoopAllocationFailure311Test,
    PrimitiveReconstructionAllocationFailurePreservesErrorAndCanRetry) {
  for (bool is_double : {false, true}) {
    SCOPED_TRACE(is_double);
    std::vector<Ref<>> held;
    if (is_double) {
      holdFloatFreelist(held);
    }
    uint64_t regs[codegen::NUM_REGS]{};
    regs[0] = is_double ? bit_cast<uint64_t>(2001.0) : uint64_t{2000};
    MemoryView memory{regs};
    LiveValue value{
        codegen::PhyLocation{0},
        hir::RefKind::kUncounted,
        is_double ? hir::ValueKind::kDouble : hir::ValueKind::kSigned,
        LiveValue::Source::kUnknown};
    ObjectAllocationFault fault;
    auto failed = Ref<>::steal(withObjectAllocator(
        fault, [&]() { return memory.readOwned(value).release(); }));
    bool memory_error = takeMemoryError();
    EXPECT_EQ(fault.failures, 1u);
    EXPECT_EQ(failed, nullptr);
    EXPECT_TRUE(memory_error);
    auto retried = memory.readOwned(value);
    ASSERT_NE(retried, nullptr);
    if (is_double) {
      EXPECT_TRUE(PyFloat_CheckExact(retried));
      EXPECT_DOUBLE_EQ(PyFloat_AsDouble(retried), 2001.0);
    } else {
      EXPECT_TRUE(PyLong_CheckExact(retried));
      EXPECT_EQ(PyLong_AsLong(retried), 2000);
    }
  }
}

class LoopAllocationFailureRuntime311Test : public RuntimeTest {};

TEST_F(
    LoopAllocationFailureRuntime311Test,
    FusedRangeAllocationFailureRaisesBeforeBodyAndReleasesReferences) {
  SKIP_311_EXECUTABLE_COMPILE();
  for (bool caught : {false, true}) {
    for (bool iterator_allocation : {false, true}) {
      SCOPED_TRACE(caught);
      SCOPED_TRACE(iterator_allocation);
      const char* source = caught ? R"(
def target(stop, seen):
    try:
        for value in range(1000, stop):
            seen.append(value)
    except MemoryError:
        seen.append('caught')
    finally:
        seen.append('finally')
    return seen
)"
                                  : R"(
def target(stop, seen):
    for value in range(1000, stop):
        seen.append(value)
    return seen
)";
      Ref<PyFunctionObject> func(compileAndGet(source, "target"));
      ASSERT_NE(func, nullptr);
      auto start = Ref<>::steal(PyLong_FromLong(1000));
      auto stop = Ref<>::steal(PyLong_FromLong(1004));
      auto seen = Ref<>::steal(PyList_New(0));
      ASSERT_NE(start, nullptr);
      ASSERT_NE(stop, nullptr);
      ASSERT_NE(seen, nullptr);
      auto args = Ref<>::steal(PyTuple_Pack(2, stop.get(), seen.get()));
      ASSERT_NE(args, nullptr);
      for (int i = 0; i < 32; ++i) {
        auto warm = Ref<>::steal(PyObject_Call(func, args, nullptr));
        ASSERT_NE(warm, nullptr);
      }
      ASSERT_EQ(
          PyList_SetSlice(seen, 0, PyList_GET_SIZE(seen.get()), nullptr), 0);
      auto irfunc = compileToFinalHIRForTest(func);
      ASSERT_NE(irfunc, nullptr);
      ASSERT_NE(
          hir::HIRPrinter{}.setFullSnapshots(true).ToString(*irfunc).find(
              "StoreField<index@"),
          std::string::npos);
      ASSERT_EQ(compileFunction(func), Result::OK);
      ObjectAllocationFault probe;
      probe.fail = false;
      auto allocated = Ref<>::steal(withObjectAllocator(probe, [&]() {
        if (iterator_allocation) {
          return JITRT_RangeIter311(2, start, stop, nullptr);
        }
#ifdef Py_REF_DEBUG
        // Reference-debug builds use the existing generic boxing helper.
        return JITRT_BoxI64(1000);
#else
        return PyLong_FromLong(1000);
#endif
      }));
      ASSERT_NE(allocated, nullptr);
      ASSERT_GT(probe.observed_size, 0u);
      auto refs_before = Py_REFCNT(seen.get());
      auto before = triggerStatsSnapshot();
      ObjectAllocationFault fault;
      fault.match_size = probe.observed_size;
      auto failed = Ref<>::steal(withObjectAllocator(
          fault, [&]() { return PyObject_Call(func, args, nullptr); }));
      bool error_set = PyErr_Occurred() != nullptr;
      bool memory_error = PyErr_ExceptionMatches(PyExc_MemoryError);
      if (!caught) {
        EXPECT_TRUE(takeMemoryError());
      } else {
        PyErr_Clear();
      }
      EXPECT_EQ(fault.failures, 1u);
      // No earlier organic exit may silently move the fault to Stock.
      EXPECT_EQ(fault.organic_at_failure, before.organic_deopt_hits);
      EXPECT_EQ(fault.forced_at_failure, before.forced_deopt_hits);
      EXPECT_GT(
          triggerStatsSnapshot().machine_code_entries,
          before.machine_code_entries);
      if (caught) {
        EXPECT_EQ(failed, seen);
        EXPECT_FALSE(error_set);
        ASSERT_EQ(PyList_GET_SIZE(seen.get()), 2);
        EXPECT_EQ(
            PyUnicode_CompareWithASCIIString(
                PyList_GET_ITEM(seen.get(), 0), "caught"),
            0);
        EXPECT_EQ(
            PyUnicode_CompareWithASCIIString(
                PyList_GET_ITEM(seen.get(), 1), "finally"),
            0);
      } else {
        EXPECT_EQ(failed, nullptr);
        EXPECT_TRUE(memory_error);
        EXPECT_EQ(PyList_GET_SIZE(seen.get()), 0);
      }
      failed.reset();
      PyGC_Collect();
      EXPECT_EQ(Py_REFCNT(seen.get()), refs_before);
      ASSERT_EQ(
          PyList_SetSlice(seen, 0, PyList_GET_SIZE(seen.get()), nullptr), 0);
      auto retried = Ref<>::steal(PyObject_Call(func, args, nullptr));
      ASSERT_NE(retried, nullptr);
      ASSERT_EQ(PyList_GET_SIZE(seen.get()), caught ? 5 : 4);
      for (Py_ssize_t i = 0; i < 4; ++i) {
        EXPECT_EQ(PyLong_AsLong(PyList_GET_ITEM(seen.get(), i)), 1000 + i);
      }
      if (caught) {
        EXPECT_EQ(
            PyUnicode_CompareWithASCIIString(
                PyList_GET_ITEM(seen.get(), 4), "finally"),
            0);
      }
    }
  }
}

TEST_F(
    LoopAllocationFailureRuntime311Test,
    ForcedDeoptReboxingFailureRaisesAndReleasesLiveReferences) {
  SKIP_311_EXECUTABLE_COMPILE();
  for (bool caught : {false, true}) {
    for (bool is_double : {false, true}) {
      SCOPED_TRACE(caught);
      SCOPED_TRACE(is_double);
      const std::string source =
          "import sys\n"
          "visits = []\n"
          "def inspect_frame():\n"
          "    visits.append('called')\n"
          "    return 42\n"
          "def target(n, cb, items):\n"
          "    for i in range(n):\n"
          "        value = None\n"
          "        value = i * " +
          std::string(is_double ? "1000.5" : "1000") +
          (caught ? "\n    try:\n        items[0]\n"
                    "    except MemoryError:\n"
                    "        return sys.exc_info()[2].tb_lasti\n"
                  : "\n    items[0]\n") +
          "    return cb()\n";
      Ref<PyFunctionObject> func(compileAndGet(source.c_str(), "target"));
      ASSERT_NE(func, nullptr);
      auto n = Ref<>::steal(PyLong_FromLong(3));
      auto items = Ref<>::steal(PyList_New(1));
      ASSERT_NE(items, nullptr);
      PyList_SET_ITEM(items.get(), 0, Py_NewRef(Py_None));
      auto callback = getGlobal("inspect_frame");
      auto visits = getGlobal("visits");
      ASSERT_NE(callback, nullptr);
      ASSERT_NE(visits, nullptr);
      auto args =
          Ref<>::steal(PyTuple_Pack(3, n.get(), callback.get(), items.get()));
      ASSERT_NE(args, nullptr);
      for (int i = 0; i < 32; ++i) {
        auto warm = Ref<>::steal(PyObject_Call(func, args, nullptr));
        ASSERT_NE(warm, nullptr);
      }
      ASSERT_EQ(
          PyList_SetSlice(visits, 0, PyList_GET_SIZE(visits.get()), nullptr),
          0);
      ASSERT_EQ(compileFunction(func), Result::OK);
      auto* compiled = reinterpret_cast<CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func));
      ASSERT_NE(compiled, nullptr);
      auto* runtime = compiled->runtime();
      ASSERT_NE(runtime, nullptr);
      auto code = BorrowedRef<PyCodeObject>{func->func_code};
      int value_index = -1;
      for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(code->co_localsplusnames);
           ++i) {
        if (PyUnicode_CompareWithASCIIString(
                PyTuple_GET_ITEM(code->co_localsplusnames, i), "value") == 0) {
          value_index = i;
          break;
        }
      }
      ASSERT_GE(value_index, 0);
      const DeoptMetadata* chosen = nullptr;
      for (const auto& meta : runtime->deoptMetadatas()) {
        if (meta.frame_meta.empty()) {
          continue;
        }
        const auto& frame = meta.innermostFrame();
        if (static_cast<size_t>(value_index) >= frame.localsplus.size()) {
          continue;
        }
        const LiveValue* live_value = meta.getLocalValue(value_index, frame);
        if (meta.forceable && meta.forced_site_row < 0 &&
            BytecodeInstruction(frame.code, frame.cause_instr_idx).opcode() ==
                BINARY_SUBSCR &&
            meta.getLocalBoundFlag(value_index, frame) != nullptr &&
            live_value != nullptr &&
            live_value->value_kind ==
                (is_double ? hir::ValueKind::kDouble
                           : hir::ValueKind::kSigned) &&
            (chosen == nullptr ||
             frame.cause_instr_idx <
                 chosen->innermostFrame().cause_instr_idx)) {
          chosen = &meta;
        }
      }
      ASSERT_NE(chosen, nullptr)
          << "must exercise the value primitive local and its bound flag";
      BytecodeInstruction cause{code, chosen->innermostFrame().cause_instr_idx};
      int error_offset = BCOffset(cause.opcodeIndex()).value();
      std::vector<Ref<>> held;
      if (is_double) {
        holdFloatFreelist(held);
      }
      ObjectAllocationFault probe;
      probe.fail = false;
      auto boxed = Ref<>::steal(withObjectAllocator(probe, [&]() {
        return is_double ? PyFloat_FromDouble(2001.0)
                         : PyLong_FromSsize_t(2000);
      }));
      ASSERT_NE(boxed, nullptr);
      ASSERT_GT(probe.observed_size, 0u);
      auto refs_before = Py_REFCNT(items.get());
      auto before = triggerStatsSnapshot();
      ASSERT_TRUE(runtime->armForcedDeopt(chosen->site_id, 1, false));
      ObjectAllocationFault fault;
      fault.match_size = probe.observed_size;
      fault.after_forced_deopt = true;
      fault.forced_before = before.forced_deopt_hits;
      auto failed = Ref<>::steal(withObjectAllocator(
          fault, [&]() { return PyObject_Call(func, args, nullptr); }));
      if (caught) {
        EXPECT_EQ(PyErr_Occurred(), nullptr);
        ASSERT_NE(failed, nullptr);
        EXPECT_EQ(PyLong_AsLong(failed), error_offset);
      } else {
        EXPECT_EQ(tracebackOffset(code), error_offset);
        EXPECT_TRUE(takeMemoryError());
        EXPECT_EQ(failed, nullptr);
      }
      EXPECT_EQ(fault.failures, 1u);
      auto after = triggerStatsSnapshot();
      EXPECT_GT(after.machine_code_entries, before.machine_code_entries);
      EXPECT_EQ(after.forced_deopt_hits, before.forced_deopt_hits + 1);
      EXPECT_EQ(PyList_GET_SIZE(visits.get()), 0);
      failed.reset();
      PyGC_Collect();
      EXPECT_EQ(Py_REFCNT(items.get()), refs_before);
      auto retried = Ref<>::steal(PyObject_Call(func, args, nullptr));
      ASSERT_NE(retried, nullptr);
      EXPECT_EQ(PyList_GET_SIZE(visits.get()), 1);
      EXPECT_EQ(PyLong_AsLong(retried), 42);
    }
  }
}
#endif
