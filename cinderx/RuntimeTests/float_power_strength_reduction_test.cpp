// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cfloat>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace jit::hir;

class FloatPowerStrengthReductionTest : public RuntimeTest {};

namespace {

struct BinaryShape {
  BinaryOpKind op;
  std::string_view left;
  std::string_view right;
};

struct CompareShape {
  PrimitiveCompareOp op;
  std::string_view left;
  std::string_view right;
};

struct ExpectedShape {
  std::string_view exponent;
  std::vector<BinaryShape> binary_ops;
  std::vector<CompareShape> compares;
  std::string_view result;
};

struct ActualShape {
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

std::string constantName(double value) {
  if (value == 0.0) {
    return "0";
  }
  if (value == 0.5) {
    return "0.5";
  }
  if (value == 1.0) {
    return "1";
  }
  if (value == DBL_MIN) {
    return "DBL_MIN";
  }
  if (value == DBL_MAX) {
    return "DBL_MAX";
  }
  if (value == -DBL_MAX) {
    return "-DBL_MAX";
  }
  return "unexpected CDouble constant";
}

std::string operandName(Register* reg, const ActualShape& shape) {
  if (shape.unboxes.size() == 1 && reg == shape.unboxes.front()->output()) {
    return "x";
  }
  for (std::size_t i = 0; i < shape.binary_ops.size(); i++) {
    if (reg == shape.binary_ops[i]->output()) {
      return "d" + std::to_string(i);
    }
  }
  Instr* producer = reg->instr();
  if (producer != nullptr && producer->IsLoadConst()) {
    Type type = static_cast<const LoadConst*>(producer)->type();
    if (type.hasDoubleSpec()) {
      return constantName(type.doubleSpec());
    }
  }
  return "unexpected operand";
}

std::unique_ptr<Function> parseAndSimplifyHIR(const char* hir) {
  auto func = HIRParser{}.ParseHIR(hir);
  if (func != nullptr) {
    // HIRParser leaves register types at TTop. Simplify needs the exponent's
    // object specialization and the refined base type to select this rewrite.
    reflowTypes(*func);
    Simplify{}.Run(*func);
  }
  return func;
}

std::unique_ptr<Function> parseAndSimplify(std::string_view exponent) {
  std::string hir = hirForExponent(exponent);
  return parseAndSimplifyHIR(hir.c_str());
}

} // namespace

TEST_F(FloatPowerStrengthReductionTest, RewritesRecognizedConstantExponents) {
  const std::vector<ExpectedShape> cases{
      {"0.5",
       {{BinaryOpKind::kPower, "x", "0.5"}},
       {{PrimitiveCompareOp::kGreaterThan, "x", "0"}},
       "d0"},
      {"1.0", {}, {}, "x"},
      {"1.5",
       {{BinaryOpKind::kPower, "x", "0.5"},
        {BinaryOpKind::kMultiply, "x", "d0"}},
       {{PrimitiveCompareOp::kGreaterThanEqual, "x", "0"},
        {PrimitiveCompareOp::kGreaterThan, "d1", "-DBL_MAX"},
        {PrimitiveCompareOp::kLessThan, "d1", "DBL_MAX"}},
       "d1"},
      {"2.0",
       {{BinaryOpKind::kMultiply, "x", "x"}},
       {{PrimitiveCompareOp::kLessThan, "d0", "DBL_MAX"}},
       "d0"},
      {"3.0",
       {{BinaryOpKind::kMultiply, "x", "x"},
        {BinaryOpKind::kMultiply, "d0", "x"}},
       {{PrimitiveCompareOp::kGreaterThan, "d1", "-DBL_MAX"},
        {PrimitiveCompareOp::kLessThan, "d1", "DBL_MAX"}},
       "d1"},
      {"-0.5",
       {{BinaryOpKind::kPower, "x", "0.5"},
        {BinaryOpKind::kTrueDivide, "1", "d0"}},
       {{PrimitiveCompareOp::kGreaterThan, "x", "0"},
        {PrimitiveCompareOp::kGreaterThan, "d1", "-DBL_MAX"},
        {PrimitiveCompareOp::kLessThan, "d1", "DBL_MAX"}},
       "d1"},
      {"-1.0",
       {{BinaryOpKind::kTrueDivide, "1", "x"}},
       {{PrimitiveCompareOp::kNotEqual, "x", "0"},
        {PrimitiveCompareOp::kGreaterThan, "d0", "-DBL_MAX"},
        {PrimitiveCompareOp::kLessThan, "d0", "DBL_MAX"}},
       "d0"},
      {"-1.5",
       {{BinaryOpKind::kPower, "x", "0.5"},
        {BinaryOpKind::kMultiply, "x", "d0"},
        {BinaryOpKind::kTrueDivide, "1", "d1"}},
       {{PrimitiveCompareOp::kGreaterThanEqual, "d1", "DBL_MIN"},
        {PrimitiveCompareOp::kLessThan, "d1", "DBL_MAX"}},
       "d2"},
      {"-2.0",
       {{BinaryOpKind::kMultiply, "x", "x"},
        {BinaryOpKind::kTrueDivide, "1", "d0"}},
       {{PrimitiveCompareOp::kGreaterThanEqual, "d0", "DBL_MIN"},
        {PrimitiveCompareOp::kLessThan, "d0", "DBL_MAX"}},
       "d1"},
  };

  for (const auto& expected : cases) {
    SCOPED_TRACE("exponent = " + std::string(expected.exponent));
    auto func = parseAndSimplify(expected.exponent);
    ASSERT_NE(func, nullptr);
    ASSERT_TRUE(checkFunc(*func, std::cerr));

    ActualShape actual = collectShape(*func);
#if PY_VERSION_HEX < 0x030C0000
    // CP311 preserves libm pow rounding for runtime bases, including these
    // exponents. Verify the retained operation instead of skipping coverage.
    EXPECT_TRUE(actual.unboxes.empty());
    EXPECT_TRUE(actual.binary_ops.empty());
    EXPECT_TRUE(actual.compares.empty());
    EXPECT_TRUE(actual.guards.empty());
    EXPECT_TRUE(actual.boxes.empty());
    ASSERT_EQ(actual.float_binary_ops.size(), 1);
    const auto* power = actual.float_binary_ops.front();
    EXPECT_EQ(power->op(), BinaryOpKind::kPower);
    ASSERT_TRUE(power->right()->type().hasObjectSpec());
    ASSERT_TRUE(PyFloat_CheckExact(power->right()->type().objectSpec()));
    EXPECT_DOUBLE_EQ(
        PyFloat_AS_DOUBLE(power->right()->type().objectSpec()),
        std::stod(std::string(expected.exponent)));
#else
    ASSERT_EQ(actual.unboxes.size(), 1);
    EXPECT_EQ(actual.unboxes.front()->type(), TCDouble);
    ASSERT_EQ(actual.boxes.size(), 1);
    EXPECT_EQ(actual.boxes.front()->type(), TCDouble);
    EXPECT_TRUE(actual.float_binary_ops.empty());

    ASSERT_EQ(actual.binary_ops.size(), expected.binary_ops.size());
    for (std::size_t i = 0; i < expected.binary_ops.size(); i++) {
      const auto& expected_op = expected.binary_ops[i];
      const auto* actual_op = actual.binary_ops[i];
      EXPECT_EQ(actual_op->op(), expected_op.op);
      EXPECT_EQ(
          operandName(actual_op->left(), actual),
          std::string(expected_op.left));
      EXPECT_EQ(
          operandName(actual_op->right(), actual),
          std::string(expected_op.right));
    }

    ASSERT_EQ(actual.compares.size(), expected.compares.size());
    ASSERT_EQ(actual.guards.size(), expected.compares.size());
    for (std::size_t i = 0; i < expected.compares.size(); i++) {
      const auto& expected_compare = expected.compares[i];
      const auto* actual_compare = actual.compares[i];
      EXPECT_EQ(actual_compare->op(), expected_compare.op);
      EXPECT_EQ(
          operandName(actual_compare->left(), actual),
          std::string(expected_compare.left));
      EXPECT_EQ(
          operandName(actual_compare->right(), actual),
          std::string(expected_compare.right));
      EXPECT_EQ(actual.guards[i]->GetOperand(0), actual_compare->output());
    }

    EXPECT_EQ(
        operandName(actual.boxes.front()->value(), actual),
        std::string(expected.result));
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
  ASSERT_TRUE(power->right()->type().hasObjectSpec());
  ASSERT_TRUE(PyFloat_Check(power->right()->type().objectSpec()));
  EXPECT_DOUBLE_EQ(PyFloat_AS_DOUBLE(power->right()->type().objectSpec()), 2.5);
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
