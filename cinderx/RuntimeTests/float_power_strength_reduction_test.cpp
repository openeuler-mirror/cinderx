// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <algorithm>
#include <cfloat>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace jit::hir;

class FloatPowerStrengthReductionTest : public RuntimeTest {};

namespace {

struct ActualShape {
  std::vector<const Instr*> instructions;
  std::vector<const PrimitiveUnbox*> unboxes;
  std::vector<const DoubleBinaryOp*> binary_ops;
  std::vector<const PrimitiveCompare*> compares;
  std::vector<const Guard*> guards;
  std::vector<const PrimitiveBox*> boxes;
  std::vector<const FloatBinaryOp*> float_binary_ops;
  std::vector<const Return*> returns;
};

std::string hirForExponent(std::string_view exponent) {
  std::string hir = R"(
fun test {
  bb 0 {
    v1 = LoadArg<0>
    v2 = LoadConst<MortalFloatExact[)";
  hir += exponent;
  hir += R"(]>
    v3 = RefineType<FloatExact> v1
    v4 = FloatBinaryOp<Power> v3 v2
    Return v4
  }
}
)";
  return hir;
}

ActualShape collectShape(const Function& func) {
  ActualShape shape;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      shape.instructions.push_back(&instr);
      if (instr.IsPrimitiveUnbox()) {
        shape.unboxes.push_back(static_cast<const PrimitiveUnbox*>(&instr));
      } else if (instr.IsDoubleBinaryOp()) {
        shape.binary_ops.push_back(static_cast<const DoubleBinaryOp*>(&instr));
      } else if (instr.IsPrimitiveCompare()) {
        shape.compares.push_back(static_cast<const PrimitiveCompare*>(&instr));
      } else if (instr.IsGuard()) {
        shape.guards.push_back(static_cast<const Guard*>(&instr));
      } else if (instr.IsPrimitiveBox()) {
        shape.boxes.push_back(static_cast<const PrimitiveBox*>(&instr));
      } else if (instr.IsFloatBinaryOp()) {
        shape.float_binary_ops.push_back(
            static_cast<const FloatBinaryOp*>(&instr));
      } else if (instr.IsReturn()) {
        shape.returns.push_back(static_cast<const Return*>(&instr));
      }
    }
  }
  return shape;
}

std::unique_ptr<Function> parseAndSimplifyHIR(
    const char* hir,
    FrameState* power_frame = nullptr) {
  auto func = HIRParser{}.ParseHIR(hir);
  if (func != nullptr) {
    if (power_frame != nullptr) {
      for (auto& block : func->cfg.blocks) {
        for (auto& instr : block) {
          if (!instr.IsFloatBinaryOp()) {
            continue;
          }
          auto& power = static_cast<FloatBinaryOp&>(instr);
          if (power.op() != BinaryOpKind::kPower) {
            continue;
          }
          // Both input objects must remain available when a guard resumes
          // the original Power bytecode, including guards after libm pow.
          *power_frame = FrameState{jit::BCOffset{12}};
          power_frame->localsplus = {power.left()};
          power_frame->nlocals = 1;
          power_frame->stack.push(power.left());
          power_frame->stack.push(power.right());
          power.setFrameState(*power_frame);
          power.setBytecodeOffset(power_frame->cur_instr_offs);
        }
      }
    }
    // HIRParser leaves register types at TTop. Simplify needs the exponent's
    // object specialization and refined base type to test Power handling.
    reflowTypes(*func);
    Simplify{}.Run(*func);
  }
  return func;
}

std::unique_ptr<Function> parseAndSimplify(
    std::string_view exponent,
    FrameState* power_frame = nullptr) {
  std::string hir = hirForExponent(exponent);
  return parseAndSimplifyHIR(hir.c_str(), power_frame);
}

} // namespace

TEST_F(
    FloatPowerStrengthReductionTest,
    PreservesExactPowerForConstantExponents) {
  for (const std::string_view exponent :
       {"0.5", "1.0", "1.5", "2.0", "3.0", "-0.5", "-1.0", "-1.5", "-2.0"}) {
    SCOPED_TRACE("exponent = " + std::string(exponent));
    FrameState power_frame;
    auto func = parseAndSimplify(exponent, &power_frame);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));
    ActualShape actual = collectShape(*func);
#if PY_VERSION_HEX >= 0x030E0000
    ASSERT_EQ(actual.unboxes.size(), 1);
    ASSERT_EQ(actual.binary_ops.size(), 1);
    ASSERT_EQ(actual.compares.size(), 2);
    ASSERT_EQ(actual.guards.size(), 2);
    ASSERT_EQ(actual.boxes.size(), 1);
    EXPECT_TRUE(actual.float_binary_ops.empty());
    const auto* power = actual.binary_ops.front();
    EXPECT_EQ(power->op(), BinaryOpKind::kPower);
    EXPECT_EQ(power->left(), actual.unboxes.front()->output());
    EXPECT_EQ(power->output()->type(), TCDouble);
    ASSERT_TRUE(power->right()->type().hasDoubleSpec());
    EXPECT_DOUBLE_EQ(
        power->right()->type().doubleSpec(), std::stod(std::string(exponent)));
    EXPECT_EQ(actual.boxes.front()->GetOperand(0), power->output());
    EXPECT_TRUE(actual.boxes.front()->output()->type() <= TFloatExact);
    ASSERT_NE(actual.boxes.front()->frameState(), nullptr);
    EXPECT_EQ(*actual.boxes.front()->frameState(), power_frame);

    const std::vector<PrimitiveCompareOp> comparisons = {
        PrimitiveCompareOp::kGreaterThan, PrimitiveCompareOp::kLessThan};
    auto position = [&](const Instr* instr) {
      return std::find(
          actual.instructions.begin(), actual.instructions.end(), instr);
    };
    for (size_t i = 0; i < comparisons.size(); ++i) {
      SCOPED_TRACE("guard = " + std::to_string(i));
      const auto* compare = actual.compares[i];
      const auto* guard = actual.guards[i];
      EXPECT_EQ(compare->op(), comparisons[i]);
      EXPECT_EQ(
          compare->GetOperand(0),
          i == 0 ? actual.unboxes.front()->output() : power->output());
      ASSERT_TRUE(compare->GetOperand(1)->type().hasDoubleSpec());
      EXPECT_DOUBLE_EQ(
          compare->GetOperand(1)->type().doubleSpec(), i == 0 ? 0.0 : DBL_MAX);
      EXPECT_EQ(guard->GetOperand(0), compare->output());
      EXPECT_LT(position(compare), position(guard));
      if (i == 0) {
        EXPECT_LT(position(guard), position(power));
      } else {
        EXPECT_LT(position(power), position(compare));
        EXPECT_LT(position(guard), position(actual.boxes.front()));
      }
      ASSERT_NE(guard->frameState(), nullptr);
      EXPECT_EQ(*guard->frameState(), power_frame);
      EXPECT_EQ(guard->bytecodeOffset(), power_frame.cur_instr_offs);
    }
#else
    EXPECT_TRUE(actual.unboxes.empty());
    EXPECT_TRUE(actual.binary_ops.empty());
    EXPECT_TRUE(actual.compares.empty());
    EXPECT_TRUE(actual.guards.empty());
    EXPECT_TRUE(actual.boxes.empty());
    ASSERT_EQ(actual.float_binary_ops.size(), 1);
    const auto* power = actual.float_binary_ops.front();
    EXPECT_EQ(power->op(), BinaryOpKind::kPower);
    EXPECT_EQ(power->output()->type(), TObject);
    ASSERT_TRUE(power->right()->type().hasObjectSpec());
    ASSERT_TRUE(PyFloat_CheckExact(power->right()->type().objectSpec()));
    EXPECT_DOUBLE_EQ(
        PyFloat_AS_DOUBLE(power->right()->type().objectSpec()),
        std::stod(std::string(exponent)));
#endif
  }
}

TEST_F(FloatPowerStrengthReductionTest, LeavesOtherConstantExponentAsPower) {
  auto func = parseAndSimplify("2.5");
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cerr));

  ActualShape actual = collectShape(*func);
  EXPECT_TRUE(actual.unboxes.empty());
  EXPECT_TRUE(actual.binary_ops.empty());
  EXPECT_TRUE(actual.compares.empty());
  EXPECT_TRUE(actual.guards.empty());
  EXPECT_TRUE(actual.boxes.empty());

  ASSERT_EQ(actual.float_binary_ops.size(), 1);
  const auto* power = actual.float_binary_ops.front();
  EXPECT_EQ(power->op(), BinaryOpKind::kPower);
  EXPECT_EQ(power->output()->type(), TObject);
  ASSERT_TRUE(power->right()->type().hasObjectSpec());
  ASSERT_TRUE(PyFloat_Check(power->right()->type().objectSpec()));
  EXPECT_DOUBLE_EQ(PyFloat_AS_DOUBLE(power->right()->type().objectSpec()), 2.5);
}

TEST_F(FloatPowerStrengthReductionTest, PowerConsumerUsesGuardedResultType) {
  auto func = parseAndSimplifyHIR(R"(
fun test {
  bb 0 {
    v1 = LoadArg<0>
    v2 = LoadConst<MortalFloatExact[0.5]>
    v3 = RefineType<FloatExact> v1
    v4 = FloatBinaryOp<Power> v3 v2
    v5 = LoadConst<MortalFloatExact[1.0]>
    v6 = BinaryOp<Add> v4 v5
    Return v6
  }
}
)");
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cerr));
  ActualShape actual = collectShape(*func);
#if PY_VERSION_HEX >= 0x030E0000
  EXPECT_TRUE(actual.float_binary_ops.empty());
  ASSERT_EQ(actual.binary_ops.size(), 2);
  ASSERT_EQ(actual.guards.size(), 2);
  EXPECT_EQ(actual.binary_ops[0]->op(), BinaryOpKind::kPower);
  EXPECT_EQ(actual.binary_ops[1]->op(), BinaryOpKind::kAdd);
  EXPECT_EQ(actual.binary_ops[1]->left(), actual.binary_ops[0]->output());
  ASSERT_EQ(actual.returns.size(), 1);
  auto* result = actual.returns.front()->GetOperand(0);
  EXPECT_TRUE(result->type() <= TFloatExact);
  ASSERT_TRUE(result->instr()->IsPrimitiveBox());
  EXPECT_EQ(result->instr()->GetOperand(0), actual.binary_ops[1]->output());
#else
  ASSERT_EQ(actual.float_binary_ops.size(), 1);
  EXPECT_EQ(actual.float_binary_ops.front()->output()->type(), TObject);
  EXPECT_TRUE(actual.unboxes.empty());
  EXPECT_TRUE(actual.binary_ops.empty());
  ASSERT_EQ(actual.returns.size(), 1);
  auto* consumer = actual.returns.front()->GetOperand(0)->instr();
  ASSERT_TRUE(consumer->IsBinaryOp());
  EXPECT_EQ(static_cast<const BinaryOp*>(consumer)->op(), BinaryOpKind::kAdd);
#endif
}

TEST_F(FloatPowerStrengthReductionTest, ConstantBaseStillConstantFolds) {
  const char* hir = R"(
fun test {
  bb 0 {
    v1 = LoadConst<MortalFloatExact[4.0]>
    v2 = LoadConst<MortalFloatExact[1.5]>
    v3 = FloatBinaryOp<Power> v1 v2
    Return v3
  }
}
)";
  auto func = parseAndSimplifyHIR(hir);
  ASSERT_NE(func, nullptr);
  ASSERT_TRUE(checkFunc(*func, std::cerr));

  ActualShape actual = collectShape(*func);
  EXPECT_TRUE(actual.unboxes.empty());
  EXPECT_TRUE(actual.binary_ops.empty());
  EXPECT_TRUE(actual.compares.empty());
  EXPECT_TRUE(actual.guards.empty());
  EXPECT_TRUE(actual.boxes.empty());
  EXPECT_TRUE(actual.float_binary_ops.empty());

  ASSERT_EQ(actual.returns.size(), 1);
  Type result_type = actual.returns.front()->GetOperand(0)->type();
  ASSERT_TRUE(result_type.hasObjectSpec());
  ASSERT_TRUE(PyFloat_Check(result_type.objectSpec()));
  EXPECT_DOUBLE_EQ(PyFloat_AS_DOUBLE(result_type.objectSpec()), 8.0);
}
