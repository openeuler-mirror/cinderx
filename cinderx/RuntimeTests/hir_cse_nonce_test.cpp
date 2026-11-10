// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/hir_cse.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <iostream>

class HirCSENonceTest : public RuntimeTest {
 protected:
  static size_t count(const jit::hir::Function& func, jit::hir::Opcode opcode) {
    size_t result = 0;
    for (const auto& block : func.cfg.blocks) {
      for (const auto& instr : block) {
        result += instr.opcode() == opcode;
      }
    }
    return result;
  }
};

TEST_F(HirCSENonceTest, PreservesExplicitlyIdentifiedDeoptimizationSites) {
  auto func = jit::hir::HIRParser{}.ParseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = RefineType<LongExact> v0
    v2 = IsCompactLong v1
    Guard v2
    Guard v2
    Return v0
  }
}
)");
  ASSERT_NE(func, nullptr);
  jit::hir::reflowTypes(*func);
  int nonce = 0;
  for (auto& block : func->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsGuard()) {
        instr.asDeoptBase()->set_nonce(nonce++);
      }
    }
  }
  ASSERT_EQ(nonce, 2);
  jit::hir::HirCSE{}.Run(*func);
  int guards = 0;
  for (auto& block : func->cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsGuard()) {
        EXPECT_EQ(instr.asDeoptBase()->nonce(), guards++);
      }
    }
  }
  EXPECT_EQ(guards, 2);
}

TEST_F(HirCSENonceTest, PrimitiveKeysPreserveTypesOperatorsAndOperandOrder) {
  auto func = jit::hir::HIRParser{}.ParseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = RefineType<LongExact> v0
    v3 = RefineType<LongExact> v1
    v4 = PrimitiveUnbox<CInt64> v2
    v5 = PrimitiveUnbox<CInt64> v2
    v6 = PrimitiveUnbox<CInt32> v2
    v7 = PrimitiveUnbox<CInt64> v3
    v8 = IntBinaryOp<Add> v4 v7
    v9 = IntBinaryOp<Add> v5 v7
    v10 = IntBinaryOp<Subtract> v4 v7
    v11 = IntBinaryOp<Subtract> v7 v4
    v12 = PrimitiveCompare<Equal> v4 v7
    v13 = PrimitiveCompare<Equal> v5 v7
    v14 = PrimitiveCompare<NotEqual> v4 v7
    v15 = PrimitiveCompare<Equal> v7 v4
    v16 = PrimitiveUnaryOp<NegateInt> v4
    v17 = PrimitiveUnaryOp<NegateInt> v5
    v18 = PrimitiveUnaryOp<InvertInt> v4
    Return v0
  }
}
)");
  ASSERT_NE(func, nullptr);
  jit::hir::reflowTypes(*func);
  ASSERT_TRUE(jit::hir::checkFunc(*func, std::cerr));
  jit::hir::HirCSE{}.Run(*func);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kPrimitiveUnbox), 3);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kIntBinaryOp), 3);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kPrimitiveCompare), 3);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kPrimitiveUnaryOp), 2);
  EXPECT_TRUE(jit::hir::checkFunc(*func, std::cerr));
}

TEST_F(HirCSENonceTest, SiblingExpressionsAndGuardsDoNotEscapeTheirScope) {
  auto func = jit::hir::HIRParser{}.ParseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v1 = LoadArg<1>
    v2 = RefineType<LongExact> v0
    CondBranch<1, 2> v1
  }
  bb 1 (preds 0) {
    v3 = IsCompactLong v2
    Guard v3
    Branch<3>
  }
  bb 2 (preds 0) {
    v4 = IsCompactLong v2
    Guard v4
    Branch<3>
  }
  bb 3 (preds 1, 2) {
    v5 = IsCompactLong v2
    Guard v5
    Return v0
  }
}
)");
  ASSERT_NE(func, nullptr);
  jit::hir::reflowTypes(*func);
  ASSERT_TRUE(jit::hir::checkFunc(*func, std::cerr));
  jit::hir::HirCSE{}.Run(*func);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kIsCompactLong), 3);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kGuard), 3);
  EXPECT_TRUE(jit::hir::checkFunc(*func, std::cerr));
}

TEST_F(HirCSENonceTest, MemoryReadsAndObjectBoxesStayDistinct) {
  auto func = jit::hir::HIRParser{}.ParseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadArg<0>
    v3 = LoadConst<CInt64[1000]>
    v4 = PrimitiveBox<CInt64> v3
    v5 = PrimitiveBox<CInt64> v3
    Return v0
  }
}
)");
  ASSERT_NE(func, nullptr);
  auto* ret = func->cfg.entry_block->GetTerminator();
  for (size_t i = 0; i < 2; ++i) {
    auto* field = jit::hir::LoadField::create(
        func->env.AllocateRegister(),
        ret->GetOperand(0),
        "value",
        16,
        jit::hir::TObject);
    field->InsertBefore(*ret);
  }
  jit::hir::reflowTypes(*func);
  ASSERT_TRUE(jit::hir::checkFunc(*func, std::cerr));
  jit::hir::HirCSE{}.Run(*func);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kLoadField), 2);
  EXPECT_EQ(count(*func, jit::hir::Opcode::kPrimitiveBox), 2);
  EXPECT_TRUE(jit::hir::checkFunc(*func, std::cerr));
}
