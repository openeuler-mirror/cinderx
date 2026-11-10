// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/printer.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
#include "internal/pycore_interp.h"

namespace {

class TupleLayout311Test : public RuntimeTest {
 protected:
  struct Lowered {
    std::unique_ptr<jit::hir::Function> hir;
    std::unique_ptr<jit::lir::Function> lir;
  };

  Lowered lower(const std::string& source) {
    auto hir = jit::hir::HIRParser{}.ParseHIR(source.c_str());
    if (hir == nullptr) {
      return {};
    }
    jit::hir::reflowTypes(*hir);
    jit::codegen::Environ env;
    env.ctx = jit::getContext();
    jit::CodeRuntime runtime{hir->code, hir->builtins, hir->globals};
    env.code_rt = &runtime;
    jit::lir::LIRGenerator generator(hir.get(), &env);
    auto lir = generator.TranslateFunction();
    return {std::move(hir), std::move(lir)};
  }

  size_t tupleAllocationPhis(const jit::lir::Function& lir) {
    size_t count = 0;
    for (const auto* block : lir.basicblocks()) {
      for (const auto& instr : block->instructions()) {
        if (instr->opcode() == jit::lir::Instruction::kPhi &&
            instr->origin() != nullptr && instr->origin()->IsMakeTuple()) {
          ++count;
        }
      }
    }
    return count;
  }

  size_t callsTo(const jit::lir::Function& lir, void* target) {
    size_t count = 0;
    for (const auto* block : lir.basicblocks()) {
      for (const auto& instr : block->instructions()) {
        if (instr->isCall() && instr->getNumInputs() > 0 &&
            instr->getInput(0)->isImm() &&
            instr->getInput(0)->getConstant() ==
                reinterpret_cast<uint64_t>(target)) {
          ++count;
        }
      }
    }
    return count;
  }
};

TEST_F(TupleLayout311Test, AdjacentInitializationSelectsFreelistAndFallback) {
  auto result = lower(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = MakeTuple<2>
    InitTupleElements<2> v2 v0 v1
    Return v2
  }
}
)");
  ASSERT_NE(result.lir, nullptr);
  EXPECT_EQ(callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_New)), 1);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  EXPECT_EQ(tupleAllocationPhis(*result.lir), 1) << *result.lir;
  const jit::lir::Instruction* fallback = nullptr;
  std::vector<const jit::lir::Instruction*> branches;
  for (const auto* block : result.lir->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->isCall() && instr->getNumInputs() > 0 &&
          instr->getInput(0)->isImm() &&
          instr->getInput(0)->getConstant() ==
              reinterpret_cast<uint64_t>(PyTuple_New)) {
        fallback = instr.get();
      }
      if (instr->opcode() == jit::lir::Instruction::kCondBranch) {
        branches.push_back(instr.get());
      }
    }
  }
  ASSERT_NE(fallback, nullptr);
  ASSERT_EQ(branches.size(), 2);
  const auto& head_edges = branches[0]->basicblock()->successors();
  const auto& tracing_edges = branches[1]->basicblock()->successors();
  ASSERT_EQ(head_edges.size(), 2);
  ASSERT_EQ(tracing_edges.size(), 2);
  EXPECT_EQ(head_edges[0], branches[1]->basicblock());
  EXPECT_EQ(head_edges[1], fallback->basicblock());
  EXPECT_EQ(tracing_edges[0], fallback->basicblock());
  const auto* fast = tracing_edges[1];
  for (const auto& instr : fast->instructions()) {
    EXPECT_FALSE(instr->isCall());
  }
  ASSERT_EQ(fast->successors().size(), 1);
  ASSERT_EQ(fallback->basicblock()->successors().size(), 1);
  EXPECT_EQ(fast->successors()[0], fallback->basicblock()->successors()[0]);
#else
  EXPECT_EQ(tupleAllocationPhis(*result.lir), 0) << *result.lir;
#endif
}

TEST_F(TupleLayout311Test, InterveningAllocationKeepsGenericTupleAllocation) {
  auto result = lower(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = MakeTuple<2>
    v3 = LoadConst<CInt64[17]>
    v4 = PrimitiveBox<CInt64> v3
    InitTupleElements<2> v2 v0 v1
    Return v2
  }
}
)");
  ASSERT_NE(result.lir, nullptr);
  EXPECT_EQ(callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_New)), 1);
  EXPECT_EQ(tupleAllocationPhis(*result.lir), 0) << *result.lir;
}

TEST_F(TupleLayout311Test, KnownTupleDeallocationRespectsReferenceDebug) {
  auto result = lower(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = MakeTuple<2>
    InitTupleElements<2> v2 v0 v1
    Decref v2
    Return v0
  }
}
)");
  ASSERT_NE(result.lir, nullptr);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  EXPECT_EQ(
      callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)),
      0);
#else
  EXPECT_EQ(
      callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)),
      1);
#endif
}

TEST_F(TupleLayout311Test, FreelistAllocationAndDeallocationRespectSizeLimits) {
  for (size_t size : {0, 1, 4, 5, 20, 21}) {
    SCOPED_TRACE(size);
    std::string source = "fun test {\n  bb 0 {\n    v0 = LoadArg<0>\n";
    source += "    v1 = MakeTuple<" + std::to_string(size) + ">\n";
    source += "    InitTupleElements<" + std::to_string(size) + "> v1";
    for (size_t i = 0; i < size; ++i) {
      source += " v0";
    }
    source += "\n    Decref v1\n    Return v0\n  }\n}\n";
    auto result = lower(source);
    ASSERT_NE(result.lir, nullptr);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
    EXPECT_EQ(tupleAllocationPhis(*result.lir), size >= 1 && size <= 20);
    EXPECT_EQ(
        callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)),
        size >= 1 && size <= 4 ? 0 : 1);
#else
    EXPECT_EQ(tupleAllocationPhis(*result.lir), 0);
    EXPECT_EQ(
        callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)),
        1);
#endif
  }
}

TEST_F(
    TupleLayout311Test,
    NullableTuplePhiKeepsItsNullCheckBeforeDeallocation) {
  auto result = lower(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    CondBranch<1, 2> v0
  }
  bb 1 (preds 0) {
    v1 = MakeTuple<1>
    InitTupleElements<1> v1 v0
    Branch<3>
  }
  bb 2 (preds 0) {
    v2 = LoadConst<Nullptr>
    Branch<3>
  }
  bb 3 (preds 1, 2) {
    v3 = Phi<1, 2> v1 v2
    XDecref v3
    Return v0
  }
}
)");
  ASSERT_NE(result.lir, nullptr);
  // A nullable tuple must test the pointer before reading its refcount.
  const jit::lir::Instruction* phi = nullptr;
  for (const auto* block : result.lir->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->opcode() == jit::lir::Instruction::kPhi &&
          instr->origin() != nullptr && instr->origin()->IsPhi()) {
        phi = instr.get();
      }
    }
  }
  ASSERT_NE(phi, nullptr);
  const auto* next = phi->basicblock()->getLastInstr();
  ASSERT_NE(next, nullptr);
  ASSERT_EQ(next->opcode(), jit::lir::Instruction::kCondBranch);
  ASSERT_TRUE(next->getInput(0)->isLinked());
  EXPECT_EQ(
      static_cast<const jit::lir::LinkedOperand*>(next->getInput(0))
          ->getLinkedInstr(),
      phi);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  EXPECT_EQ(callsTo(*result.lir, reinterpret_cast<void*>(PyObject_GC_Del)), 1);
#else
  EXPECT_EQ(callsTo(*result.lir, reinterpret_cast<void*>(PyObject_GC_Del)), 0);
#endif
}

TEST_F(TupleLayout311Test, DifferentTupleSizesAtPhiKeepGenericDeallocation) {
  auto result = lower(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    CondBranch<1, 2> v0
  }
  bb 1 (preds 0) {
    v1 = MakeTuple<1>
    InitTupleElements<1> v1 v0
    Branch<3>
  }
  bb 2 (preds 0) {
    v2 = MakeTuple<2>
    InitTupleElements<2> v2 v0 v0
    Branch<3>
  }
  bb 3 (preds 1, 2) {
    v3 = Phi<1, 2> v1 v2
    Decref v3
    Return v0
  }
}
)");
  ASSERT_NE(result.lir, nullptr);
  EXPECT_EQ(callsTo(*result.lir, reinterpret_cast<void*>(PyObject_GC_Del)), 0);
  EXPECT_EQ(
      callsTo(*result.lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)),
      1);
}

class TupleRuntime311Test : public TupleLayout311Test {};

TEST_F(TupleRuntime311Test, FreelistHitMissAndAllocationFailureAreObservable) {
  SKIP_311_EXECUTABLE_COMPILE();
  ASSERT_EQ(jit::getConfig().frame_mode, jit::FrameMode::kNormal);
  Ref<PyFunctionObject> fn(
      compileStockAndGet("def pair(a, b): return (a, b)", "pair"));
  ASSERT_NE(fn, nullptr);
  auto hir = jit::compileToFinalHIRForTest(fn);
  ASSERT_NE(hir, nullptr);
  jit::codegen::Environ env;
  env.ctx = jit::getContext();
  jit::CodeRuntime runtime{hir->code, hir->builtins, hir->globals};
  env.code_rt = &runtime;
  auto lir = jit::lir::LIRGenerator(hir.get(), &env).TranslateFunction();
  ASSERT_NE(lir, nullptr);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  ASSERT_EQ(tupleAllocationPhis(*lir), 1) << *lir;
#else
  ASSERT_EQ(tupleAllocationPhis(*lir), 0) << *lir;
#endif
  ASSERT_EQ(jit::compileFunction(fn), jit::Result::OK);
  auto a = Ref<>::steal(PyList_New(0));
  auto b = Ref<>::steal(PyList_New(0));
  PyObject* args[] = {a.get(), b.get()};
  auto& state = PyThreadState_Get()->interp->tuple;
  // Keep every drained tuple alive: releasing it would refill this size class.
  std::vector<Ref<>> held;
  while (state.numfree[1] > 0) {
    held.push_back(Ref<>::steal(PyTuple_New(2)));
  }
  ASSERT_EQ(state.numfree[1], 0);
  auto before_a = Py_REFCNT(a.get());
  auto before_b = Py_REFCNT(b.get());
  auto call = [&]() {
    return Ref<>::steal(PyObject_Vectorcall(fn, args, 2, nullptr));
  };
  auto value = call();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(PyTuple_GET_ITEM(value.get(), 0), a.get());
  EXPECT_EQ(PyTuple_GET_ITEM(value.get(), 1), b.get());
  EXPECT_EQ(state.numfree[1], 0); // empty-list fallback allocated a tuple
  auto* reused = value.get();
  value.reset();
  ASSERT_EQ(state.free_list[1], reinterpret_cast<PyTupleObject*>(reused));
  const int available = state.numfree[1];
  value = call();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value.get(), reused);
  EXPECT_EQ(state.numfree[1], available - 1);
  value.reset();
  while (state.numfree[1] > 0) {
    held.push_back(Ref<>::steal(PyTuple_New(2)));
  }
  struct Fault {
    PyMemAllocatorEx original;
    size_t hits{0};
  } fault;
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &fault.original);
  PyMemAllocatorEx allocator = fault.original;
  allocator.ctx = &fault;
  allocator.malloc = [](void* context, size_t size) -> void* {
    auto& f = *static_cast<Fault*>(context);
    // Only the two-element GC tuple allocation fails, including any retry
    // after collection. Frame/vectorcall setup remains unaffected.
    if (size ==
        sizeof(PyGC_Head) + offsetof(PyTupleObject, ob_item) +
            2 * sizeof(PyObject*)) {
      ++f.hits;
      return nullptr;
    }
    return f.original.malloc(f.original.ctx, size);
  };
  allocator.calloc = [](void* context, size_t n, size_t size) -> void* {
    auto& f = *static_cast<Fault*>(context);
    return f.original.calloc(f.original.ctx, n, size);
  };
  allocator.realloc = [](void* context, void* ptr, size_t size) -> void* {
    auto& f = *static_cast<Fault*>(context);
    return f.original.realloc(f.original.ctx, ptr, size);
  };
  allocator.free = [](void* context, void* ptr) {
    auto& f = *static_cast<Fault*>(context);
    f.original.free(f.original.ctx, ptr);
  };
  {
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
    SCOPE_EXIT(PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &fault.original));
    value = call();
  }
  EXPECT_GT(fault.hits, 0);
  EXPECT_EQ(value, nullptr);
  EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_MemoryError));
  PyErr_Clear();
  EXPECT_EQ(Py_REFCNT(a.get()), before_a);
  EXPECT_EQ(Py_REFCNT(b.get()), before_b);
  // Failure must leave the function and freelist usable on the next call.
  value = call();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(PyTuple_GET_ITEM(value.get(), 0), a.get());
}

TEST_F(TupleRuntime311Test, FreelistFullAfterCallbackFreesTheOuterTuple) {
  SKIP_311_EXECUTABLE_COMPILE();
  ASSERT_EQ(jit::getConfig().frame_mode, jit::FrameMode::kNormal);
  Ref<PyFunctionObject> fn(compileStockAndGet(
      "def discard(a, b, consume):\n    value = (a, b)\n    consume(value)\n",
      "discard"));
  ASSERT_NE(fn, nullptr);
  auto hir = jit::compileToFinalHIRForTest(fn);
  ASSERT_NE(hir, nullptr);
  jit::codegen::Environ env;
  env.ctx = jit::getContext();
  jit::CodeRuntime runtime{hir->code, hir->builtins, hir->globals};
  env.code_rt = &runtime;
  auto lir = jit::lir::LIRGenerator(hir.get(), &env).TranslateFunction();
  ASSERT_NE(lir, nullptr);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  ASSERT_EQ(tupleAllocationPhis(*lir), 1) << *lir;
  ASSERT_EQ(callsTo(*lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)), 0);
#endif
  ASSERT_EQ(jit::compileFunction(fn), jit::Result::OK);
  struct Observation {
    void* allocation{nullptr};
    int full_count{0};
    size_t freed{0};
    PyMemAllocatorEx original;
  } observation;
  static PyMethodDef consume_def = {
      "fill_freelist",
      [](PyObject* self, PyObject* tuple) -> PyObject* {
        auto* seen =
            static_cast<Observation*>(PyCapsule_GetPointer(self, "tuple-test"));
        seen->allocation = reinterpret_cast<PyGC_Head*>(tuple) - 1;
        std::vector<Ref<>> pool;
        for (int i = 0; i < PyTuple_MAXFREELIST; ++i) {
          auto value = Ref<>::steal(PyTuple_New(2));
          if (value == nullptr) {
            return nullptr;
          }
          pool.push_back(std::move(value));
        }
        pool.clear();
        seen->full_count = PyThreadState_Get()->interp->tuple.numfree[1];
        Py_RETURN_NONE;
      },
      METH_O,
      nullptr};
  auto capsule =
      Ref<>::steal(PyCapsule_New(&observation, "tuple-test", nullptr));
  ASSERT_NE(capsule, nullptr);
  auto consume =
      Ref<>::steal(PyCFunction_NewEx(&consume_def, capsule, nullptr));
  ASSERT_NE(consume, nullptr);
  PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &observation.original);
  auto allocator = observation.original;
  allocator.ctx = &observation;
  allocator.malloc = [](void* ctx, size_t size) -> void* {
    auto& seen = *static_cast<Observation*>(ctx);
    return seen.original.malloc(seen.original.ctx, size);
  };
  allocator.calloc = [](void* ctx, size_t n, size_t size) -> void* {
    auto& seen = *static_cast<Observation*>(ctx);
    return seen.original.calloc(seen.original.ctx, n, size);
  };
  allocator.realloc = [](void* ctx, void* ptr, size_t size) -> void* {
    auto& seen = *static_cast<Observation*>(ctx);
    return seen.original.realloc(seen.original.ctx, ptr, size);
  };
  allocator.free = [](void* ctx, void* ptr) {
    auto& seen = *static_cast<Observation*>(ctx);
    seen.freed += seen.allocation != nullptr && ptr == seen.allocation;
    seen.original.free(seen.original.ctx, ptr);
  };
  PyObject* args[] = {Py_None, Py_None, consume.get()};
  Ref<> result;
  {
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocator);
    SCOPE_EXIT(PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &observation.original));
    result = Ref<>::steal(PyObject_Vectorcall(fn, args, 3, nullptr));
  }
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(observation.full_count, PyTuple_MAXFREELIST);
  EXPECT_EQ(observation.freed, 1);
  EXPECT_EQ(PyThreadState_Get()->interp->tuple.numfree[1], PyTuple_MAXFREELIST);
}

TEST_F(TupleRuntime311Test, DestructorReentryAndForcedDeoptPreserveLiveTuple) {
  SKIP_311_EXECUTABLE_COMPILE();
  ASSERT_EQ(jit::getConfig().frame_mode, jit::FrameMode::kNormal);
  runStockCode(R"(
import dis
import gc
import sys
import cinderjit
import cinderx.jit as jit
import _cinderx

events = []
class Mortal:
    def __init__(self, index):
        self.index = index
    def __del__(self):
        # Re-enter the same size class while the outer tuple is being freed.
        pool = [tuple([None, None]) for _ in range(2100)]
        del pool
        gc.collect()
        events.append(self.index)

def discard(factory, consume):
    value = (factory(0), factory(1))
    consume(value)
    return None

def consume(value):
    assert value[0].index == 0 and value[1].index == 1

assert jit.force_compile(discard)
discard(Mortal, consume)
assert events == [1, 0], events

def live_pair(a, b, x, y):
    value = (a, b)
    total = x + y
    return value, total

a, b = Mortal(2), Mortal(3)
for _ in range(200):
    result = live_pair(a, b, 10, 20)
del result
assert jit.is_jit_compiled(live_pair) or jit.force_compile(live_pair)
ops = {i.offset: i.opname for i in dis.get_instructions(live_pair)}
sites = [s for s in cinderjit.deopt_sites(live_pair)
         if s['forceable'] and ops.get(s['bc_offset']) == 'BINARY_OP']
assert sites, cinderjit.deopt_sites(live_pair)
refs = sys.getrefcount(a), sys.getrefcount(b)
before = _cinderx._get_trigger_stats()['forced_deopt_hits']
assert cinderjit.force_deopt(live_pair, sites[0]['id'], n=1)
value, total = live_pair(a, b, 10, 20)
assert _cinderx._get_trigger_stats()['forced_deopt_hits'] == before + 1
assert value[0] is a and value[1] is b and total == 30
del value
assert (sys.getrefcount(a), sys.getrefcount(b)) == refs
assert events == [1, 0], events
del a, b
assert events == [1, 0, 2, 3], events
)");
  Ref<PyFunctionObject> discard(getGlobal("discard"));
  auto hir = jit::compileToFinalHIRForTest(discard);
  ASSERT_NE(hir, nullptr);
  jit::codegen::Environ env;
  env.ctx = jit::getContext();
  jit::CodeRuntime runtime{hir->code, hir->builtins, hir->globals};
  env.code_rt = &runtime;
  auto lir = jit::lir::LIRGenerator(hir.get(), &env).TranslateFunction();
  ASSERT_NE(lir, nullptr);
#if !defined(Py_DEBUG) && !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && \
    !defined(Py_STATS)
  EXPECT_EQ(tupleAllocationPhis(*lir), 1) << *lir;
  EXPECT_EQ(callsTo(*lir, reinterpret_cast<void*>(PyTuple_Type.tp_dealloc)), 0);
#endif
}

TEST_F(
    TupleRuntime311Test,
    FreelistReusePreservesGCReferencesAndDestructionOrder) {
  SKIP_311_EXECUTABLE_COMPILE();
  runStockCode(R"(
import gc
import sys
import tracemalloc
import cinderx.jit as jit
import _cinderx

def pair(a, b, consume):
    value = (a, b)
    consume(value)

def owned_pair(factory, consume):
    value = (factory(0), factory(1))
    consume(value)

assert jit.force_compile(pair)
assert jit.force_compile(owned_pair)
assert jit.is_jit_compiled(pair)
assert jit.is_jit_compiled(owned_pair)

destroyed = []
class Mortal:
    def __init__(self, index):
        self.index = index
    def __del__(self):
        destroyed.append(self.index)

def inspect(value):
    assert value[0].index == 0 and value[1].index == 1
    gc.collect()

def fill_freelist(value):
    inspect(value)
    pool = [tuple([None, None]) for _ in range(2100)]
    del pool

entries = _cinderx._get_trigger_stats()["machine_code_entries"]
for tracing in (False, True):
    if tracing:
        tracemalloc.start()
    try:
        a, b = Mortal(0), Mortal(1)
        before = (sys.getrefcount(a), sys.getrefcount(b))
        for _ in range(32):
            pair(a, b, inspect)
        assert (sys.getrefcount(a), sys.getrefcount(b)) == before
        del a, b
        destroyed.clear()
        for _ in range(32):
            owned_pair(Mortal, inspect)
        assert destroyed == [1, 0] * 32, destroyed
        destroyed.clear()
        owned_pair(Mortal, fill_freelist)
        assert destroyed == [1, 0], destroyed
    finally:
        if tracing:
            tracemalloc.stop()
assert _cinderx._get_trigger_stats()["machine_code_entries"] > entries
)");
}

} // namespace
#endif
