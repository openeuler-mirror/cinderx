// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace jit::lir {
class LIRPostGenerationRewriteTest : public RuntimeTest {};

static std::string runPostGenRewrite(const char* lir_input_str) {
  auto func = Parser().parse(lir_input_str);
  codegen::Environ env;
  PostGenerationRewrite(func.get(), &env).run();
  return fmt::format("{}", *func);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsLoadSecondCallResultDataType) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  %11:16bit = LoadSecondCallResult %10
  Return %11
)";

  std::string expected_lir_str = fmt::format(
      R"(Function:
BB %0
{}      %10:Object = Call {}
       %11:16bit = Move {}:16bit
                   Return %11:16bit

)",
      "",
      "0(0x0):64bit",
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 16});

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str.c_str());
}

TEST_F(LIRPostGenerationRewriteTest, DoesNotAllowMultipleLSCRPerCall) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  %11 = LoadSecondCallResult %10
  CondBranch %11, BB%1, BB%2
BB %1
  %12 = LoadSecondCallResult %10
  Return %12
BB %2
  Return %10
)";

  EXPECT_DEATH(
      runPostGenRewrite(lir_input_str),
      "Call output consumed by multiple LoadSecondCallResult instructions");
}

TEST_F(LIRPostGenerationRewriteTest, RewritesLoadSecondCallResultThroughPhis) {
  const char* lir_input_str = R"(Function:
BB %0
  %10 = Call 0
  CondBranch %10, BB%1, BB%2
BB %1
  %11 = Call 0
  CondBranch %11, BB%3, BB%4
BB %2
  %12 = Call 0
  CondBranch %12, BB%20, BB%21
BB %20
  %120 = Call 0
  Branch BB%22
BB %21
  %121 = Call 0
  Branch BB%22
BB %22
  %122 = Phi BB%20, %120, BB%21, %121
  Branch BB%5
BB %3
  Call 0
  Branch BB%5
BB %4
  Call 0
  Branch BB%5
BB %5
  %13 = Phi BB%22, %122, BB%3, %11, BB%4, %11, BB%6, %13
  %14:32bit = LoadSecondCallResult %13
  Branch BB%6
BB %6
  Call 0
  Branch BB%5
)";

  std::string expected_lir_str = fmt::format(
      R"(Function:
BB %0
      %10:Object = Call 0(0x0):64bit
                   CondBranch %10:Object, BB%1, BB%2

BB %1
      %11:Object = Call 0(0x0):64bit
      %139:32bit = Move {0}:32bit
                   CondBranch %11:Object, BB%3, BB%4

BB %2
      %12:Object = Call 0(0x0):64bit
                   CondBranch %12:Object, BB%20, BB%21

BB %20
     %120:Object = Call 0(0x0):64bit
      %137:32bit = Move {0}:32bit
                   Branch BB%22

BB %21
     %121:Object = Call 0(0x0):64bit
      %138:32bit = Move {0}:32bit
                   Branch BB%22

BB %22
     %122:Object = Phi (BB%20, %120:Object), (BB%21, %121:Object)
      %136:32bit = Phi (BB%20, %137:32bit), (BB%21, %138:32bit)
                   Branch BB%5

BB %3
                   Call 0(0x0):64bit
                   Branch BB%5

BB %4
                   Call 0(0x0):64bit
                   Branch BB%5

BB %5
      %13:Object = Phi (BB%22, %122:Object), (BB%3, %11:Object), (BB%4, %11:Object), (BB%6, %13:Object)
       %14:32bit = Phi (BB%22, %136:32bit), (BB%3, %139:32bit), (BB%4, %139:32bit), (BB%6, %14:32bit)
                   Branch BB%6

BB %6
                   Call 0(0x0):64bit
                   Branch BB%5

)",
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32},
      PhyLocation{codegen::arch::reg_general_auxilary_return_loc.loc, 32});

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str.c_str());
}

#if defined(CINDER_AARCH64)

#if PY_VERSION_HEX < 0x030C0000
TEST_F(LIRPostGenerationRewriteTest, LegalizesBothSelectImmediateValues) {
  Function func;
  BasicBlock* block = func.allocateBasicBlock();
  Instruction* condition = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg{DataType::k32bit},
      Imm{1, DataType::k32bit});
  Instruction* select64 = block->allocateInstr(
      Instruction::kSelect,
      nullptr,
      OutVReg{DataType::k64bit},
      VReg{condition},
      Imm{0, DataType::k64bit},
      Imm{UINT64_MAX, DataType::k64bit});
  Instruction* select32 = block->allocateInstr(
      Instruction::kSelect,
      nullptr,
      OutVReg{DataType::k32bit},
      VReg{condition},
      Imm{0, DataType::k32bit},
      Imm{UINT32_MAX, DataType::k32bit});

  codegen::Environ env;
  PostGenerationRewrite(&func, &env).run();

  auto expect_materialized = [](Instruction* select,
                                size_t input_idx,
                                DataType data_type,
                                uint64_t value) {
    ASSERT_TRUE(select->getInput(input_idx)->isLinked());
    auto* linked = static_cast<LinkedOperand*>(select->getInput(input_idx));
    Instruction* move = linked->getLinkedInstr();
    ASSERT_TRUE(move->isMove());
    EXPECT_EQ(move->output()->dataType(), data_type);
    ASSERT_TRUE(move->getInput(0)->isImm());
    EXPECT_EQ(move->getInput(0)->dataType(), data_type);
    EXPECT_EQ(move->getInput(0)->getConstant(), value);
  };

  expect_materialized(select64, 1, DataType::k64bit, 0);
  expect_materialized(select64, 2, DataType::k64bit, UINT64_MAX);
  expect_materialized(select32, 1, DataType::k32bit, 0);
  expect_materialized(select32, 2, DataType::k32bit, UINT32_MAX);
}
#endif

TEST_F(LIRPostGenerationRewriteTest, RetainsSplitAddSubImmediate) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1(0x1):64bit
  %2:64bit = Add %1:64bit, 8193(0x2001):64bit
  %3:64bit = Sub %2:64bit, 8193(0x2001):64bit
  Return %3:64bit
)";

  auto rewritten = runPostGenRewrite(lir_input_str);
  EXPECT_NE(rewritten.find("Add %1:64bit, 8193(0x2001):64bit"),
            std::string::npos);
  EXPECT_NE(rewritten.find("Sub %2:64bit, 8193(0x2001):64bit"),
            std::string::npos);
  EXPECT_EQ(rewritten.find("Move 8193(0x2001):64bit"), std::string::npos);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsSmallAddSubImmediate) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1(0x1):64bit
  %2:64bit = Add %1:64bit, 4095(0xfff):64bit
  %3:64bit = Sub %2:64bit, 4095(0xfff):64bit
  Return %3:64bit
)";

  auto rewritten = runPostGenRewrite(lir_input_str);
  EXPECT_NE(rewritten.find("Add %1:64bit, 4095(0xfff):64bit"),
            std::string::npos);
  EXPECT_NE(rewritten.find("Sub %2:64bit, 4095(0xfff):64bit"),
            std::string::npos);
  EXPECT_EQ(rewritten.find("Move 4095(0xfff):64bit"), std::string::npos);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsSplitEqualityImmediateOnly) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 8193(0x2001):64bit
  %2:64bit = Equal %1:64bit, 8193(0x2001):64bit
  %3:64bit = LessThanUnsigned %1:64bit, 8193(0x2001):64bit
  Return %2:64bit
)";

  auto rewritten = runPostGenRewrite(lir_input_str);
  EXPECT_NE(rewritten.find("Equal %1:64bit, 8193(0x2001):64bit"),
            std::string::npos);
  EXPECT_NE(rewritten.find("LessThanUnsigned %1:64bit, %"),
            std::string::npos);
  EXPECT_NE(rewritten.find("Move 8193(0x2001):64bit"),
            std::string::npos);
}

TEST_F(LIRPostGenerationRewriteTest, RetainsMaxSplitEqualityImmediate) {
  // Maximum split add/sub immediate: (0xfff << 12) + 0xfff.
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 16777215(0xffffff):64bit
  %2:64bit = Equal %1:64bit, 16777215(0xffffff):64bit
  %3:64bit = LessThanUnsigned %1:64bit, 16777215(0xffffff):64bit
  Return %2:64bit
)";

  auto rewritten = runPostGenRewrite(lir_input_str);
  EXPECT_NE(rewritten.find("Equal %1:64bit, 16777215(0xffffff):64bit"),
            std::string::npos);
  EXPECT_NE(rewritten.find("LessThanUnsigned %1:64bit, %"),
            std::string::npos);
  EXPECT_NE(rewritten.find("Move 16777215(0xffffff):64bit"),
            std::string::npos);
}

TEST_F(LIRPostGenerationRewriteTest, RewritesUnsplitAddImmediate) {
  const char* lir_input_str = R"(Function:
BB %0
  %1:64bit = Move 1(0x1):64bit
  %2:64bit = Add %1:64bit, 16777216(0x1000000):64bit
  Return %2:64bit
)";

  auto rewritten = runPostGenRewrite(lir_input_str);
  EXPECT_NE(rewritten.find("Move 16777216(0x1000000):64bit"),
            std::string::npos);
  EXPECT_NE(rewritten.find("Add %1:64bit, %"), std::string::npos);
}

#endif // CINDER_AARCH64

#if defined(CINDER_AARCH64)
TEST_F(LIRPostGenerationRewriteTest, MoveAbsoluteAddressUsesObjectDataType) {
  const char* lir_input_str = R"(Function:
BB %0
  %10:Object = Move [0x12345]
  Return %10
)";

  const char* expected_lir_str = R"(Function:
BB %0
      %12:Object = Move 74565(0x12345):Object
      %10:Object = Move [%12:Object]:Object
                   Return %10:Object

)";

  EXPECT_EQ(runPostGenRewrite(lir_input_str), expected_lir_str);
}
#endif // CINDER_AARCH64

} // namespace jit::lir
