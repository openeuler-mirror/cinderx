// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/util.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/loop_phi_unbox.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <array>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace jit;
using namespace jit::hir;

#if PY_VERSION_HEX < 0x030C0000
class LoopPhiUnboxTest : public RuntimeTest {};

namespace {

std::unique_ptr<Function> loopHIR(
    const std::string& primitive,
    bool unbound = false,
    bool object_consumer = false,
    bool on_stack = false) {
  const std::string seed = unbound ? "v1 = LoadConst<Nullptr>"
                                   : "v1 = PrimitiveBox<" + primitive + "> v0";
  const std::string zero =
      primitive == "CDouble" ? "CDouble[0.0]" : "CInt64[0]";
  const std::string one = primitive == "CDouble" ? "CDouble[1.0]" : "CInt64[1]";
  std::string source =
      "fun test {\n"
      "  bb 0 {\n"
      "    v0 = LoadConst<" +
      zero +
      ">\n"
      "    " +
      seed +
      "\n"
      "    v2 = LoadConst<CBool[true]>\n"
      "    Branch<1>\n"
      "  }\n"
      "  bb 1 {\n"
      "    v3 = Phi<0, 2> v1 v5\n"
      "    Snapshot {\n"
      "      CurInstrOffset 4\n"
      "      Locals<1> v3\n" +
      (on_stack ? "      Stack<1> v3\n" : "") +
      "    }\n"
      "    CondBranch<2, 3> v2\n"
      "  }\n"
      "  bb 2 {\n"
      "    v4 = LoadConst<" +
      one +
      ">\n"
      "    v5 = PrimitiveBox<" +
      primitive +
      "> v4\n"
      "    Branch<1>\n"
      "  }\n"
      "  bb 3 {\n"
      "    v6 = LoadConst<NoneType>\n"
      "    Return " +
      (object_consumer ? "v3" : "v6") +
      "\n"
      "  }\n"
      "}\n";
  auto func = HIRParser{}.ParseHIR(source.c_str());
  if (func != nullptr) {
    reflowTypes(*func);
  }
  return func;
}

Snapshot* loopSnapshot(Function& func) {
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsSnapshot()) {
        return static_cast<Snapshot*>(&instr);
      }
    }
  }
  return nullptr;
}

size_t objectPhis(Function& func) {
  size_t count = 0;
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsPhi() && instr.output()->type().couldBe(TObject)) {
        ++count;
      }
    }
  }
  return count;
}

} // namespace

TEST_F(LoopPhiUnboxTest, UnboxesIntegerAndFloatFrameStatePhis) {
  for (const std::string primitive : {"CInt64", "CDouble"}) {
    auto func = loopHIR(primitive);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    ASSERT_EQ(objectPhis(*func), 1);
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 0);
    auto* snapshot = loopSnapshot(*func);
    ASSERT_NE(snapshot, nullptr);
    auto* frame = snapshot->frameState();
    ASSERT_EQ(frame->localsplus.size(), 1);
    EXPECT_TRUE(
        frame->localsplus[0]->type() <=
        (primitive == "CDouble" ? TCDouble : TCInt64));
    EXPECT_TRUE(frame->localsplus[0]->instr()->IsPhi());
    EXPECT_TRUE(frame->local_bound.empty());
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, RetainsPhiWhenInputBoxesHaveSlowArmConsumers) {
  auto func = loopHIR("CInt64");
  ASSERT_NE(func, nullptr);
  std::vector<PrimitiveBox*> boxes;
  for (auto& block : func->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsPrimitiveBox()) {
        boxes.push_back(static_cast<PrimitiveBox*>(&instr));
      }
    }
  }
  ASSERT_EQ(boxes.size(), 2);
  for (auto* box : boxes) {
    auto* consumer = BinaryOp::create(
        func->env.AllocateRegister(),
        BinaryOpKind::kSubscript,
        box->output(),
        box->output(),
        FrameState{},
        /* array_subscr_slow_path= */ true);
    consumer->InsertAfter(*box);
  }
  reflowTypes(*func);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
  auto* snapshot = loopSnapshot(*func);
  ASSERT_NE(snapshot, nullptr);
  Register* original = snapshot->frameState()->localsplus[0];
  LoopPhiUnbox{}.Run(*func);
  EXPECT_EQ(objectPhis(*func), 1);
  EXPECT_EQ(snapshot->frameState()->localsplus[0], original);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
}

TEST_F(LoopPhiUnboxTest, RetainsPhiWhenOneInputBoxHasAnObjectConsumer) {
  for (const std::string primitive : {"CInt64", "CDouble"}) {
    for (size_t escaping_input : {size_t{0}, size_t{1}}) {
      SCOPED_TRACE(primitive);
      SCOPED_TRACE(escaping_input);
      auto func = loopHIR(primitive);
      ASSERT_NE(func, nullptr);
      std::vector<PrimitiveBox*> boxes;
      for (auto& block : func->cfg.blocks) {
        for (auto& instr : block) {
          if (instr.IsPrimitiveBox()) {
            boxes.push_back(static_cast<PrimitiveBox*>(&instr));
          }
        }
      }
      ASSERT_EQ(boxes.size(), 2u);
      auto* box = boxes[escaping_input];
      auto* consumer = BinaryOp::create(
          func->env.AllocateRegister(),
          BinaryOpKind::kSubscript,
          box->output(),
          box->output(),
          FrameState{});
      consumer->InsertAfter(*box);
      reflowTypes(*func);
      ASSERT_TRUE(checkFunc(*func, std::cerr));
      auto* snapshot = loopSnapshot(*func);
      ASSERT_NE(snapshot, nullptr);
      Register* original = snapshot->frameState()->localsplus[0];
      LoopPhiUnbox{}.Run(*func);
      // The other input can die, but that saving cannot justify re-boxing
      // a Phi that may still hold the object this consumer observed.
      EXPECT_EQ(objectPhis(*func), 1u);
      EXPECT_EQ(snapshot->frameState()->localsplus[0], original);
      EXPECT_EQ(consumer->GetOperand(1), box->output());
      ASSERT_TRUE(checkFunc(*func, std::cerr));
    }
  }
}

TEST_F(LoopPhiUnboxTest, RetainsPhiWithAnObjectConsumer) {
  for (const std::string primitive : {"CInt64", "CDouble"}) {
    auto func = loopHIR(primitive, false, true);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    auto* snapshot = loopSnapshot(*func);
    ASSERT_NE(snapshot, nullptr);
    Register* original = snapshot->frameState()->localsplus[0];
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 1);
    EXPECT_EQ(snapshot->frameState()->localsplus[0], original);
    EXPECT_TRUE(snapshot->frameState()->local_bound.empty());
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, RejectsPotentiallyUnboundOperandStackValues) {
  auto func = loopHIR("CInt64", true, false, true);
  ASSERT_NE(func, nullptr);
  auto* snapshot = loopSnapshot(*func);
  ASSERT_NE(snapshot, nullptr);
  Register* original = snapshot->frameState()->localsplus[0];
  LoopPhiUnbox{}.Run(*func);
  EXPECT_EQ(objectPhis(*func), 1);
  EXPECT_EQ(snapshot->frameState()->localsplus[0], original);
  EXPECT_EQ(snapshot->frameState()->stack.top(), original);
  EXPECT_TRUE(snapshot->frameState()->local_bound.empty());
}

TEST_F(LoopPhiUnboxTest, CarriesUnboundLocalFlagsIntoDeoptMetadata) {
  for (const std::string primitive : {"CInt64", "CDouble"}) {
    auto func = loopHIR(primitive, true);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 0);
    auto* snapshot = loopSnapshot(*func);
    ASSERT_NE(snapshot, nullptr);
    FrameState* frame = snapshot->frameState();
    ASSERT_EQ(frame->localsplus.size(), 1);
    ASSERT_EQ(frame->local_bound.size(), 1);
    Register* value = frame->localsplus[0];
    Register* flag = frame->local_bound[0];
    ASSERT_NE(flag, nullptr);
    EXPECT_TRUE(flag->type() <= TCInt64);
    EXPECT_TRUE(flag->instr()->IsPhi());
    std::unordered_set<Register*> visited;
    frame->visitUses([&](Register*& reg) {
      visited.insert(reg);
      return true;
    });
    EXPECT_TRUE(visited.contains(value));
    EXPECT_TRUE(visited.contains(flag));

    auto* check = CheckInstrumentation::create(*frame);
    check->InsertAfter(*snapshot);
    check->live_regs().emplace_back(
        value,
        RefKind::kUncounted,
        primitive == "CDouble" ? ValueKind::kDouble : ValueKind::kSigned);
    check->live_regs().emplace_back(
        flag, RefKind::kUncounted, ValueKind::kSigned);
    DeoptLiveRegFilter live_filter{*check};
    EXPECT_TRUE(live_filter.isUsed(check->live_regs()[1]));
    auto metadata = DeoptMetadata::fromInstr(*check);
    const auto& deopt_frame = metadata.innermostFrame();
    ASSERT_EQ(deopt_frame.localsplus_bound.size(), 1);
    EXPECT_EQ(
        metadata.getLocalBoundFlag(0, deopt_frame), &metadata.live_values[1]);
    EXPECT_EQ(metadata.getLocalValue(0, deopt_frame), &metadata.live_values[0]);
    EXPECT_EQ(metadata.getLocalBoundFlag(1, deopt_frame), nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, ReadsBothBoundFlagStates) {
  uint64_t regs[codegen::NUM_REGS]{};
  MemoryView memory{regs};
  LiveValue flag{
      codegen::PhyLocation{0},
      RefKind::kUncounted,
      ValueKind::kSigned,
      LiveValue::Source::kUnknown};
  EXPECT_FALSE(memory.readBoundFlag(flag));
  regs[0] = 1;
  EXPECT_TRUE(memory.readBoundFlag(flag));
}
namespace {

void replaceSeedWithConstant(Function& func, BorrowedRef<> value) {
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsPrimitiveBox()) {
        auto* replacement = LoadConst::create(
            instr.output(), Type::fromObject(func.env.addReference(value)));
        instr.ReplaceWith(*replacement);
        delete &instr;
        reflowTypes(func);
        return;
      }
    }
  }
  FAIL() << "Missing seed PrimitiveBox";
}

} // namespace

TEST_F(LoopPhiUnboxTest, RetainsNonCachedIntegerConstants) {
  for (long value : {-6L, 257L, 1000L}) {
    auto func = loopHIR("CInt64");
    ASSERT_NE(func, nullptr);
    auto constant = Ref<>::steal(PyLong_FromLong(value));
    ASSERT_NE(constant, nullptr);
    replaceSeedWithConstant(*func, constant);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    Register* original = loopSnapshot(*func)->frameState()->localsplus[0];
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 1);
    EXPECT_EQ(loopSnapshot(*func)->frameState()->localsplus[0], original);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, RetainsFloatObjectConstants) {
  for (double value : {-0.0, 0.0, 1.0, 1000.0}) {
    auto func = loopHIR("CDouble");
    ASSERT_NE(func, nullptr);
    auto constant = Ref<>::steal(PyFloat_FromDouble(value));
    ASSERT_NE(constant, nullptr);
    replaceSeedWithConstant(*func, constant);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    Register* original = loopSnapshot(*func)->frameState()->localsplus[0];
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 1);
    EXPECT_EQ(loopSnapshot(*func)->frameState()->localsplus[0], original);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, AllowsCanonicalCachedIntegerConstants) {
  for (long value : {-5L, 0L, 256L}) {
    auto func = loopHIR("CInt64");
    ASSERT_NE(func, nullptr);
    auto constant = Ref<>::steal(PyLong_FromLong(value));
    ASSERT_NE(constant, nullptr);
    replaceSeedWithConstant(*func, constant);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 0);
    EXPECT_TRUE(
        loopSnapshot(*func)->frameState()->localsplus[0]->type() <= TCInt64);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(LoopPhiUnboxTest, RetainsDistinctPhisFedByTheSameBox) {
  auto func = loopHIR("CInt64");
  ASSERT_NE(func, nullptr);
  auto* frame = loopSnapshot(*func)->frameState();
  auto* first = static_cast<Phi*>(frame->localsplus[0]->instr());
  std::unordered_map<BasicBlock*, Register*> inputs;
  for (size_t i = 0; i < first->NumOperands(); ++i) {
    inputs.emplace(first->basic_blocks()[i], first->GetOperand(i));
  }
  Register* output = func->env.AllocateRegister();
  auto* second = Phi::create(output, inputs);
  second->InsertBefore(*first);
  frame->localsplus.push_back(output);
  frame->nlocals = frame->localsplus.size();
  reflowTypes(*func);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
  LoopPhiUnbox{}.Run(*func);
  EXPECT_EQ(objectPhis(*func), 2);
  EXPECT_EQ(frame->localsplus[0], first->output());
  EXPECT_EQ(frame->localsplus[1], output);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
}

TEST_F(LoopPhiUnboxTest, RetainsAliasedFrameStateSlots) {
  for (bool source_box : {false, true}) {
    auto func = loopHIR("CInt64");
    ASSERT_NE(func, nullptr);
    auto* frame = loopSnapshot(*func)->frameState();
    Register* original = frame->localsplus[0];
    auto* phi = static_cast<Phi*>(original->instr());
    frame->localsplus.push_back(source_box ? phi->GetOperand(0) : original);
    frame->nlocals = frame->localsplus.size();
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    LoopPhiUnbox{}.Run(*func);
    EXPECT_EQ(objectPhis(*func), 1);
    EXPECT_EQ(frame->localsplus[0], original);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}
TEST_F(LoopPhiUnboxTest, PreservesCachedConstantRefcountsInCompileWorkers) {
  constexpr size_t worker_count = 8;
  constexpr size_t functions_per_worker = 64;
  auto constant = Ref<>::steal(PyLong_FromLong(256));
  ASSERT_NE(constant, nullptr);
  std::array<std::vector<std::unique_ptr<Function>>, worker_count> functions;
  for (auto& batch : functions) {
    for (size_t i = 0; i < functions_per_worker; ++i) {
      auto func = loopHIR("CInt64");
      ASSERT_NE(func, nullptr);
      replaceSeedWithConstant(*func, constant);
      ASSERT_TRUE(checkFunc(*func, std::cerr));
      batch.push_back(std::move(func));
    }
  }
  const Py_ssize_t initial_refcount = Py_REFCNT(constant.get());
  std::array<std::exception_ptr, worker_count> failures{};
  std::atomic<size_t> ready{0};
  std::atomic<bool> begin{false};
  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  auto& threaded = getThreadedCompileContext();
  ASSERT_FALSE(threaded.compileRunning());
  threaded.startCompile({});
  {
    // Join before closing the compile context, including thread-creation
    // exceptions. All HIR/Python objects are created and destroyed on main.
    SCOPE_EXIT({
      begin.store(true, std::memory_order_release);
      for (auto& worker : workers) {
        if (worker.joinable()) {
          worker.join();
        }
      }
      threaded.endCompile();
    });
    for (size_t i = 0; i < worker_count; ++i) {
      workers.emplace_back([&, i] {
        ready.fetch_add(1, std::memory_order_release);
        while (!begin.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        try {
          for (auto& func : functions[i]) {
            LoopPhiUnbox{}.Run(*func);
          }
        } catch (...) {
          failures[i] = std::current_exception();
        }
      });
    }
    while (ready.load(std::memory_order_acquire) != worker_count) {
      std::this_thread::yield();
    }
    begin.store(true, std::memory_order_release);
  }
  ASSERT_FALSE(threaded.compileRunning());
  EXPECT_EQ(Py_REFCNT(constant.get()), initial_refcount);
  for (size_t i = 0; i < worker_count; ++i) {
    ASSERT_EQ(failures[i], nullptr) << "Compile worker " << i << " threw";
    for (auto& func : functions[i]) {
      EXPECT_EQ(objectPhis(*func), 0);
      EXPECT_TRUE(
          loopSnapshot(*func)->frameState()->localsplus[0]->type() <= TCInt64);
      ASSERT_TRUE(checkFunc(*func, std::cerr));
    }
  }
}
#endif
