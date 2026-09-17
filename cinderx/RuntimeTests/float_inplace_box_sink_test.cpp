// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/primitive_box_remat.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <iostream>
#include <string>
#include <vector>

using namespace jit::hir;

#if PY_VERSION_HEX < 0x030C0000
class FloatInPlaceBoxSinkTest : public RuntimeTest {};

namespace {

std::unique_ptr<Function> slowPathHIR(
    const std::string& op,
    const std::string& locals = "Locals<2> v0 v1",
    const std::string& gap = "",
    const std::string& tail = "") {
  std::string hir = R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = RefineType<FloatExact> v1
    v3 = PrimitiveUnbox<CDouble> v2
    v4 = PrimitiveBox<CDouble> v3
    v12 = Assign v4
    Snapshot {
      CurInstrOffset 2
)" + locals +
      R"(
      Stack<2> v0 v4
    }
)" + gap +
      R"(
    CondBranchCheckType<1, 2, FloatExact> v0
  }
  bb 1 {
    v5 = RefineType<FloatExact> v0
    v8 = FloatBinaryOp<)" +
      op + R"(> v5 v2
    Branch<3>
  }
  bb 2 {
    v9 = InPlaceOp<)" +
      op + R"(> v0 v4
    Branch<3>
  }
  bb 3 {
    v10 = Phi<1, 2> v8 v9
)" + tail +
      R"(
    Return v10
  }
}
)";
  auto func = HIRParser{}.ParseHIR(hir.c_str());
  if (func != nullptr) {
    reflowTypes(*func);
    CopyPropagation{}.Run(*func);
    auto* source = &*func->cfg.blocks.begin();
    for (auto& instr : *source) {
      if (instr.IsSnapshot()) {
        auto* check = CheckInstrumentation::create();
        check->setFrameState(*get_frame_state(instr));
        check->InsertAfter(instr);
        break;
      }
    }
  }
  return func;
}

InPlaceOp* slowOperation(Function& func) {
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsInPlaceOp()) {
        return static_cast<InPlaceOp*>(&instr);
      }
    }
  }
  return nullptr;
}

void checkUnmoved(std::unique_ptr<Function> func) {
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
  auto* slow = slowOperation(*func);
  ASSERT_NE(slow, nullptr);
  Instr* box = slow->right()->instr();
  ASSERT_TRUE(box->IsPrimitiveBox());
  BasicBlock* source = box->block();
  ASSERT_NE(source, slow->block());
  PrimitiveBoxRemat{}.Run(*func);
  EXPECT_EQ(box->block(), source);
  EXPECT_EQ(slow->right()->instr(), box);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
}

} // namespace

TEST_F(FloatInPlaceBoxSinkTest, SplitsFlaggedAddAndSubtractOnlyOnce) {
  for (const std::string op : {"Add", "Subtract"}) {
    std::string hir =
        "fun test {\n  bb 0 {\n"
        "    v0 = LoadArg<0>\n    v1 = LoadArg<1>\n"
        "    v2 = InPlaceOp<" +
        op +
        ", FloatFastPath> v0 v1\n"
        "    Return v2\n  }\n}\n";
    auto func = HIRParser{}.ParseHIR(hir.c_str());
    ASSERT_NE(func, nullptr);
    reflowTypes(*func);
    Simplify{}.Run(*func);
    Simplify{}.Run(*func);
    size_t branches = 0;
    size_t fast = 0;
    size_t slow = 0;
    for (const auto& block : func->cfg.blocks) {
      for (const auto& instr : block) {
        branches += instr.IsCondBranchCheckType();
        fast += instr.IsDoubleBinaryOp();
        if (instr.IsInPlaceOp()) {
          ++slow;
          EXPECT_FALSE(static_cast<const InPlaceOp&>(instr).hasFloatFastPath());
        }
      }
    }
    EXPECT_EQ(branches, 2);
    EXPECT_EQ(fast, 1);
    EXPECT_EQ(slow, 1);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(FloatInPlaceBoxSinkTest, MovesTemporaryAndRewritesPreOperationStack) {
  for (const std::string op : {"Add", "Subtract"}) {
    auto func = slowPathHIR(op);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    auto* slow = slowOperation(*func);
    ASSERT_NE(slow, nullptr);
    auto* box = static_cast<PrimitiveBox*>(slow->right()->instr());
    Register* unboxed = box->value();
    PrimitiveBoxRemat{}.Run(*func);
    EXPECT_EQ(box->block(), slow->block());
    EXPECT_EQ(&slow->block()->front(), box);
    EXPECT_EQ(slow->right(), box->output());
    size_t checked_states = 0;
    for (auto& block : func->cfg.blocks) {
      for (auto& instr : block) {
        if (instr.IsCheckInstrumentation()) {
          auto* fs = get_frame_state(instr);
          ASSERT_NE(fs, nullptr);
          ASSERT_EQ(fs->stack.size(), 2);
          EXPECT_EQ(fs->stack.top(), unboxed);
          EXPECT_EQ(fs->stack.top()->type(), TCDouble);
          ++checked_states;
        }
      }
    }
    EXPECT_EQ(checked_states, 1);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
  }
}

TEST_F(FloatInPlaceBoxSinkTest, RetainsNamedAndAliasedObjects) {
  checkUnmoved(slowPathHIR("Add", "Locals<3> v0 v1 v4"));
  checkUnmoved(slowPathHIR("Subtract", "Locals<3> v0 v1 v12"));
}

TEST_F(FloatInPlaceBoxSinkTest, RetainsBoxesAcrossCallsAndOtherAllocations) {
  checkUnmoved(
      slowPathHIR("Add", "Locals<2> v0 v1", "    v20 = VectorCall<0> v0\n"));
  checkUnmoved(slowPathHIR(
      "Subtract", "Locals<2> v0 v1", "    v20 = PrimitiveBox<CDouble> v3\n"));
}

TEST_F(FloatInPlaceBoxSinkTest, RetainsAdditionalDirectAndLaterStackUses) {
  checkUnmoved(slowPathHIR(
      "Add", "Locals<2> v0 v1", "    v20 = PrimitiveUnbox<CDouble> v4\n"));
  checkUnmoved(slowPathHIR(
      "Subtract",
      "Locals<2> v0 v1",
      "",
      "    Snapshot {\n      CurInstrOffset 4\n      Stack<1> v4\n    }\n"));
}
#endif
