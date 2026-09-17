// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/printer.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
#include "internal/pycore_interp_structs.h"
#include "internal/pycore_runtime.h"
#endif

#include <bit>
#include <cstdint>
#include <vector>

class FloatBoxTstateTest : public RuntimeTest {};

namespace {

constexpr uint64_t kDoubleBits[] = {
    0x0000000000000000,
    0x8000000000000000,
    0x3ff0000000000001,
    0xbff8000000000000,
    0x0000000000000001,
    0x7fefffffffffffff,
    0x7ff0000000000000,
    0xfff0000000000000,
    0x7ff8000000001234,
    0xfff8000000005678,
    0x7ff0000000001234,
    0xfff0000000005678,
};

void expectFloatBits(PyObject* object, uint64_t bits) {
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(PyFloat_CheckExact(object));
  EXPECT_EQ(Py_REFCNT(object), 1);
  EXPECT_EQ(std::bit_cast<uint64_t>(PyFloat_AS_DOUBLE(object)), bits);
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 && \
    SIZEOF_VOID_P > 4 && !defined(Py_GIL_DISABLED)
  EXPECT_EQ(object->ob_refcnt_full, 1);
#endif
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
struct CreationRecord {
  int count{0};
  PyObject* last{nullptr};
};

int recordFloatCreation(PyObject* object, PyRefTracerEvent event, void* data) {
  if (event == PyRefTracer_CREATE && PyFloat_CheckExact(object)) {
    auto* record = static_cast<CreationRecord*>(data);
    record->count++;
    record->last = object;
  }
  return 0;
}
#endif

} // namespace

TEST_F(FloatBoxTstateTest, ReferenceHelperPreservesBitsAndDistinctLiveObjects) {
  for (uint64_t bits : kDoubleBits) {
    SCOPED_TRACE(bits);
    double value = std::bit_cast<double>(bits);
    auto first = Ref<>::steal(JITRT_BoxDouble(value));
    auto second = Ref<>::steal(JITRT_BoxDouble(value));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    expectFloatBits(first.get(), bits);
    expectFloatBits(second.get(), bits);
    EXPECT_NE(first.get(), second.get());
    EXPECT_EQ(PyErr_Occurred(), nullptr);
  }
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED)
TEST_F(FloatBoxTstateTest, ReferenceHelperReusesFreelistHeadWithFreshHeader) {
  void* previous_data = nullptr;
  auto previous_tracer = PyRefTracer_GetTracer(&previous_data);
  SCOPE_EXIT(PyRefTracer_SetTracer(previous_tracer, previous_data));
  ASSERT_EQ(PyRefTracer_SetTracer(nullptr, nullptr), 0);

  auto* tstate = PyThreadState_Get();
  auto* freelist = &tstate->interp->object_state.freelists.floats;
  ASSERT_GE(freelist->size, 0);
  for (uint64_t bits : kDoubleBits) {
    SCOPED_TRACE(bits);
    auto seed = Ref<>::steal(PyFloat_FromDouble(123.5));
    ASSERT_NE(seed, nullptr);
    seed.reset();
    void* expected = freelist->freelist;
    Py_ssize_t previous_size = freelist->size;
    ASSERT_NE(expected, nullptr);
    ASSERT_GT(previous_size, 0);

    auto result = Ref<>::steal(JITRT_BoxDouble(std::bit_cast<double>(bits)));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(static_cast<void*>(result.get()), expected);
    EXPECT_EQ(freelist->size, previous_size - 1);
    expectFloatBits(result.get(), bits);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
  }
}

TEST_F(FloatBoxTstateTest, ReferenceHelperAllocatesWhenFreelistIsEmpty) {
  void* previous_data = nullptr;
  auto previous_tracer = PyRefTracer_GetTracer(&previous_data);
  SCOPE_EXIT(PyRefTracer_SetTracer(previous_tracer, previous_data));
  ASSERT_EQ(PyRefTracer_SetTracer(nullptr, nullptr), 0);

  auto* tstate = PyThreadState_Get();
  auto* freelist = &tstate->interp->object_state.freelists.floats;
  std::vector<Ref<>> held;
  // Keep popped objects alive so that their decrefs cannot refill the list.
  while (freelist->freelist != nullptr) {
    auto item = Ref<>::steal(PyFloat_FromDouble(12.5));
    ASSERT_NE(item, nullptr);
    held.push_back(std::move(item));
  }
  ASSERT_EQ(freelist->size, 0);

  for (uint64_t bits : kDoubleBits) {
    SCOPED_TRACE(bits);
    auto result = Ref<>::steal(JITRT_BoxDouble(std::bit_cast<double>(bits)));
    ASSERT_NE(result, nullptr);
    expectFloatBits(result.get(), bits);
    EXPECT_EQ(freelist->freelist, nullptr);
    EXPECT_EQ(freelist->size, 0);
    held.push_back(std::move(result));
  }
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
TEST_F(FloatBoxTstateTest, ReferenceHelperTracksDynamicCreationTracer) {
  void* previous_data = nullptr;
  auto previous_tracer = PyRefTracer_GetTracer(&previous_data);
  SCOPE_EXIT(PyRefTracer_SetTracer(previous_tracer, previous_data));
  CreationRecord record;
  ASSERT_EQ(PyRefTracer_SetTracer(recordFloatCreation, &record), 0);

  auto traced = Ref<>::steal(JITRT_BoxDouble(-0.0));
  ASSERT_NE(traced, nullptr);
  EXPECT_EQ(record.count, 1);
  EXPECT_EQ(record.last, traced.get());
  expectFloatBits(traced.get(), 0x8000000000000000);

  ASSERT_EQ(PyRefTracer_SetTracer(nullptr, nullptr), 0);
  traced.reset();
#ifndef Py_GIL_DISABLED
  auto* freelist = &PyThreadState_Get()->interp->object_state.freelists.floats;
  ASSERT_NE(freelist->freelist, nullptr);
  void* expected = freelist->freelist;
  Py_ssize_t previous_size = freelist->size;
#endif
  auto untraced = Ref<>::steal(JITRT_BoxDouble(2.5));
  ASSERT_NE(untraced, nullptr);
  expectFloatBits(untraced.get(), 0x4004000000000000);
  EXPECT_EQ(record.count, 1);
#ifndef Py_GIL_DISABLED
  EXPECT_EQ(static_cast<void*>(untraced.get()), expected);
  EXPECT_EQ(freelist->size, previous_size - 1);
#endif

  ASSERT_EQ(PyRefTracer_SetTracer(recordFloatCreation, &record), 0);
  auto retraced = Ref<>::steal(JITRT_BoxDouble(3.5));
  ASSERT_NE(retraced, nullptr);
  EXPECT_EQ(record.count, 2);
  EXPECT_EQ(record.last, retraced.get());
  expectFloatBits(retraced.get(), 0x400c000000000000);
  EXPECT_NE(untraced.get(), retraced.get());
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif

TEST_F(FloatBoxTstateTest, PrimitiveBoxGuardsInlineAllocationAndReferenceCall) {
  auto irfunc = jit::hir::HIRParser{}.ParseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadConst<MortalFloatExact[2.5]>
    v1 = PrimitiveUnbox<CDouble> v0
    v2 = PrimitiveBox<CDouble> v1
    Return v2
  }
}
)");
  ASSERT_NE(irfunc, nullptr);
  jit::hir::reflowTypes(*irfunc);
  size_t boxes = 0;
  for (const auto& block : irfunc->cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsPrimitiveBox()) {
        ++boxes;
        ASSERT_EQ(instr.GetOperand(0)->type(), jit::hir::TCDouble);
      }
    }
  }
  ASSERT_EQ(boxes, 1);
  jit::codegen::Environ env;
  env.ctx = jit::getContext();
  jit::CodeRuntime runtime{irfunc->code, irfunc->builtins, irfunc->globals};
  env.code_rt = &runtime;
  jit::lir::LIRGenerator generator(irfunc.get(), &env);
  auto lir = generator.TranslateFunction();
  ASSERT_NE(lir, nullptr);

  auto expected_helper = reinterpret_cast<uint64_t>(JITRT_BoxDouble);
  size_t calls = 0;
  const jit::lir::Instruction* box_call = nullptr;
  for (const auto* block : lir->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (!instr->isCall() || instr->getNumInputs() == 0 ||
          !instr->getInput(0)->isImm()) {
        continue;
      }
      uint64_t target = instr->getInput(0)->getConstant();
      if (target != expected_helper) {
        continue;
      }
      ++calls;
      box_call = instr.get();
      EXPECT_EQ(target, expected_helper);
      ASSERT_EQ(instr->getNumInputs(), 2);
      EXPECT_EQ(instr->getInput(1)->dataType(), jit::lir::DataType::kDouble);
    }
  }
  EXPECT_EQ(calls, 1) << *lir;
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 &&           \
    SIZEOF_VOID_P == 8 && !defined(Py_GIL_DISABLED) && !defined(Py_DEBUG) && \
    !defined(Py_REF_DEBUG) && !defined(Py_TRACE_REFS) && !defined(Py_STATS)
  ASSERT_NE(box_call, nullptr);
  using Instruction = jit::lir::Instruction;
  auto definition = [](const jit::lir::OperandBase* operand) {
    return operand->isLinked()
        ? static_cast<const jit::lir::LinkedOperand*>(operand)->getLinkedInstr()
        : nullptr;
  };
  std::vector<const Instruction*> branches;
  for (const auto* block : lir->basicblocks()) {
    for (const auto& instr : block->instructions()) {
      if (instr->opcode() == Instruction::kCondBranch) {
        branches.push_back(instr.get());
      }
    }
  }
  ASSERT_EQ(branches.size(), 2) << *lir;
  const auto* tracer = definition(branches[0]->getInput(0));
  ASSERT_NE(tracer, nullptr);
  ASSERT_EQ(tracer->opcode(), Instruction::kMove);
  ASSERT_TRUE(tracer->getInput(0)->isMem());
  EXPECT_EQ(
      tracer->getInput(0)->getMemoryAddress(),
      static_cast<void*>(&_PyRuntime.ref_tracer.tracer_func));
  const auto& tracer_edges = branches[0]->basicblock()->successors();
  const auto& head_edges = branches[1]->basicblock()->successors();
  ASSERT_EQ(tracer_edges.size(), 2);
  ASSERT_EQ(head_edges.size(), 2);
  EXPECT_EQ(tracer_edges[0], box_call->basicblock());
  EXPECT_EQ(tracer_edges[1], branches[1]->basicblock());
  EXPECT_EQ(head_edges[1], box_call->basicblock());
  const auto* fast = head_edges[0];
  const auto* head = definition(branches[1]->getInput(0));
  ASSERT_NE(head, nullptr);
  ASSERT_TRUE(head->getInput(0)->isInd());
  const auto* head_memory = head->getInput(0)->getMemoryIndirect();
  EXPECT_EQ(head_memory->getOffset(), offsetof(_Py_freelist, freelist));
  const auto* freelist = definition(head_memory->getBaseRegOperand());
  ASSERT_NE(freelist, nullptr);
  ASSERT_EQ(freelist->opcode(), Instruction::kLea);
  const auto* freelist_memory = freelist->getInput(0)->getMemoryIndirect();
  EXPECT_EQ(
      freelist_memory->getOffset(),
      offsetof(PyInterpreterState, object_state.freelists.floats));
  const auto* interp = definition(freelist_memory->getBaseRegOperand());
  ASSERT_NE(interp, nullptr);
  ASSERT_TRUE(interp->getInput(0)->isInd());
  EXPECT_EQ(
      interp->getInput(0)->getMemoryIndirect()->getOffset(),
      offsetof(PyThreadState, interp));
  EXPECT_EQ(
      definition(interp->getInput(0)->getMemoryIndirect()->getBaseRegOperand()),
      env.asm_tstate);

  std::vector<const Instruction*> stores;
  const Instruction* decrement = nullptr;
  for (const auto& instr : fast->instructions()) {
    EXPECT_FALSE(instr->isCall());
    if (instr->output()->isInd()) {
      stores.push_back(instr.get());
    }
    if (instr->opcode() == Instruction::kDec) {
      ASSERT_EQ(decrement, nullptr);
      decrement = instr.get();
    }
  }
  ASSERT_EQ(stores.size(), 4) << *lir;
  ASSERT_NE(decrement, nullptr);
  EXPECT_EQ(
      definition(stores[0]->output()->getMemoryIndirect()->getBaseRegOperand()),
      freelist);
  EXPECT_EQ(
      stores[0]->output()->getMemoryIndirect()->getOffset(),
      offsetof(_Py_freelist, freelist));
  const auto* next = definition(stores[0]->getInput(0));
  ASSERT_NE(next, nullptr);
  ASSERT_TRUE(next->getInput(0)->isInd());
  EXPECT_EQ(next->getInput(0)->getMemoryIndirect()->getOffset(), 0);
  EXPECT_EQ(
      definition(next->getInput(0)->getMemoryIndirect()->getBaseRegOperand()),
      head);
  EXPECT_EQ(
      definition(stores[1]->output()->getMemoryIndirect()->getBaseRegOperand()),
      freelist);
  EXPECT_EQ(
      stores[1]->output()->getMemoryIndirect()->getOffset(),
      offsetof(_Py_freelist, size));
  EXPECT_EQ(
      definition(stores[1]->getInput(0)), definition(decrement->getInput(0)));
  const auto* size = definition(decrement->getInput(0));
  ASSERT_NE(size, nullptr);
  ASSERT_TRUE(size->getInput(0)->isInd());
  EXPECT_EQ(
      size->getInput(0)->getMemoryIndirect()->getOffset(),
      offsetof(_Py_freelist, size));
  EXPECT_EQ(
      definition(size->getInput(0)->getMemoryIndirect()->getBaseRegOperand()),
      freelist);
  EXPECT_EQ(
      definition(stores[2]->output()->getMemoryIndirect()->getBaseRegOperand()),
      head);
  EXPECT_EQ(
      stores[2]->output()->getMemoryIndirect()->getOffset(),
      offsetof(PyObject, ob_refcnt_full));
  ASSERT_TRUE(stores[2]->getInput(0)->isImm());
  EXPECT_EQ(stores[2]->getInput(0)->getConstant(), 1);
  EXPECT_EQ(stores[2]->output()->sizeInBits(), 64);
  EXPECT_EQ(
      definition(stores[3]->output()->getMemoryIndirect()->getBaseRegOperand()),
      head);
  EXPECT_EQ(
      stores[3]->output()->getMemoryIndirect()->getOffset(),
      offsetof(PyFloatObject, ob_fval));
  EXPECT_EQ(stores[3]->output()->dataType(), jit::lir::DataType::kDouble);
  EXPECT_EQ(
      stores[3]->getInput(0)->getDefine(), box_call->getInput(1)->getDefine());

  ASSERT_EQ(fast->successors().size(), 1);
  ASSERT_EQ(box_call->basicblock()->successors().size(), 1);
  const auto* join = fast->successors()[0];
  EXPECT_EQ(join, box_call->basicblock()->successors()[0]);
  const Instruction* result_phi = nullptr;
  bool exception_guard = false;
  for (const auto& instr : join->instructions()) {
    if (instr->opcode() == Instruction::kPhi) {
      result_phi = instr.get();
      ASSERT_EQ(instr->getNumInputs(), 4);
      EXPECT_EQ(instr->getInput(0)->getBasicBlock(), fast);
      EXPECT_EQ(definition(instr->getInput(1)), head);
      EXPECT_EQ(instr->getInput(2)->getBasicBlock(), box_call->basicblock());
      EXPECT_EQ(definition(instr->getInput(3)), box_call);
    } else if (instr->opcode() == Instruction::kGuard) {
      ASSERT_NE(result_phi, nullptr);
      ASSERT_GT(instr->getNumInputs(), 2);
      EXPECT_EQ(definition(instr->getInput(2)), result_phi);
      exception_guard = true;
    }
  }
  EXPECT_NE(result_phi, nullptr);
  EXPECT_TRUE(exception_guard) << *lir;
#else
  (void)box_call;
#endif
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED)
TEST_F(FloatBoxTstateTest, CompiledBoxPreservesIdentityAndDynamicTracer) {
  Ref<PyFunctionObject> func(compileAndGet(
      R"(
def box_value(x):
    return x * 1.0
)",
      "box_value"));
  ASSERT_NE(func, nullptr);
  auto input = Ref<>::steal(PyFloat_FromDouble(-0.0));
  ASSERT_NE(input, nullptr);
  PyObject* args[] = {input.get()};
  for (int i = 0; i < 100; ++i) {
    auto warmup = Ref<>::steal(PyObject_CallFunctionObjArgs(
        reinterpret_cast<PyObject*>(func.get()), input.get(), nullptr));
    ASSERT_NE(warmup, nullptr);
  }
  auto hir = buildHIR(func);
  ASSERT_NE(hir, nullptr);
  jit::Compiler::runPasses(*hir, jit::PassConfig::kAllExceptInliner);
  size_t boxes = 0;
  for (const auto& block : hir->cfg.blocks) {
    for (const auto& instr : block) {
      boxes += instr.IsPrimitiveBox();
    }
  }
  ASSERT_EQ(boxes, 1);
  ASSERT_EQ(jit::compileFunction(func), jit::Result::OK);
  auto compiled = jit::getContext()->lookupFunc(func);
  ASSERT_NE(compiled, nullptr);
  int guard_failures = 0;
  jit::getContext()->setGuardFailureCallback(
      [&](const jit::DeoptMetadata&) { ++guard_failures; });
  SCOPE_EXIT(jit::getContext()->clearGuardFailureCallback());
  void* previous_data = nullptr;
  auto previous_tracer = PyRefTracer_GetTracer(&previous_data);
  SCOPE_EXIT(PyRefTracer_SetTracer(previous_tracer, previous_data));
  ASSERT_EQ(PyRefTracer_SetTracer(nullptr, nullptr), 0);
  auto invoke = [&]() {
    return Ref<>::steal(
        compiled->invoke(reinterpret_cast<PyObject*>(func.get()), args, 1));
  };

  auto multiplier = Ref<>::steal(PyFloat_FromDouble(1.0));
  ASSERT_NE(multiplier, nullptr);
  for (uint64_t bits : kDoubleBits) {
    SCOPED_TRACE(bits);
    auto operand =
        Ref<>::steal(PyFloat_FromDouble(std::bit_cast<double>(bits)));
    ASSERT_NE(operand, nullptr);
    // The ordinary numeric slot is the oracle even for NaN propagation.
    auto reference = Ref<>::steal(PyNumber_Multiply(operand, multiplier));
    ASSERT_NE(reference, nullptr);
    ASSERT_TRUE(PyFloat_CheckExact(reference.get()));
    uint64_t expected_bits =
        std::bit_cast<uint64_t>(PyFloat_AS_DOUBLE(reference.get()));
    args[0] = operand.get();
    auto matrix_seed = Ref<>::steal(PyFloat_FromDouble(123.5));
    ASSERT_NE(matrix_seed, nullptr);
    matrix_seed.reset();
    void* expected_head =
        PyThreadState_Get()->interp->object_state.freelists.floats.freelist;
    ASSERT_NE(expected_head, nullptr);
    auto result = invoke();
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(static_cast<void*>(result.get()), expected_head);
    expectFloatBits(result.get(), expected_bits);
    auto other = invoke();
    ASSERT_NE(other, nullptr);
    expectFloatBits(other.get(), expected_bits);
    EXPECT_NE(result.get(), other.get());
    EXPECT_NE(result.get(), operand.get());
  }
  args[0] = input.get();

  auto seed = Ref<>::steal(PyFloat_FromDouble(123.5));
  ASSERT_NE(seed, nullptr);
  seed.reset();
  auto* freelist = &PyThreadState_Get()->interp->object_state.freelists.floats;
  void* expected = freelist->freelist;
  ASSERT_NE(expected, nullptr);
  auto first = invoke();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(static_cast<void*>(first.get()), expected);
  expectFloatBits(first.get(), 0x8000000000000000);
  auto second = invoke();
  ASSERT_NE(second, nullptr);
  expectFloatBits(second.get(), 0x8000000000000000);
  EXPECT_NE(first.get(), second.get());
  EXPECT_NE(first.get(), input.get());

  std::vector<Ref<>> held;
  while (freelist->freelist != nullptr) {
    auto item = Ref<>::steal(PyFloat_FromDouble(12.5));
    ASSERT_NE(item, nullptr);
    held.push_back(std::move(item));
  }
  auto allocated = invoke();
  ASSERT_NE(allocated, nullptr);
  expectFloatBits(allocated.get(), 0x8000000000000000);
  EXPECT_EQ(freelist->freelist, nullptr);

  // Refill before enabling tracing: an empty list would take the slow path
  // even if the generated tracer guard were missing.
  allocated.reset();
  ASSERT_NE(freelist->freelist, nullptr);
  CreationRecord record;
  ASSERT_EQ(PyRefTracer_SetTracer(recordFloatCreation, &record), 0);
  auto traced = invoke();
  ASSERT_NE(traced, nullptr);
  EXPECT_EQ(record.count, 1);
  EXPECT_EQ(record.last, traced.get());
  expectFloatBits(traced.get(), 0x8000000000000000);
  ASSERT_EQ(PyRefTracer_SetTracer(nullptr, nullptr), 0);
  traced.reset();
  expected = freelist->freelist;
  ASSERT_NE(expected, nullptr);
  auto untraced = invoke();
  ASSERT_NE(untraced, nullptr);
  EXPECT_EQ(static_cast<void*>(untraced.get()), expected);
  EXPECT_EQ(record.count, 1);
  expectFloatBits(untraced.get(), 0x8000000000000000);
  EXPECT_EQ(guard_failures, 0);
  EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif
