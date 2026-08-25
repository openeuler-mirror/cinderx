// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <sstream>

using namespace jit;

namespace jit::lir {
class LIRVerifyTest : public RuntimeTest {};

TEST_F(LIRVerifyTest, TestImmediateFallthroughOK) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %1
       %2:Object = Move[0x5]:Object
BB %1 - preds: %0
       %3:Object = Move [0x5]:Object
                   Return %3:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), true);
}

TEST_F(LIRVerifyTest, TestNonImmediateFallthroughDisallowed) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %2
       %2:Object = Move[0x5]:Object
BB %1 - preds: %0
       %3:Object = Move [0x5]:Object
BB %2 - preds: %0
       %4:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  testing::internal::CaptureStdout();
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), false);
  std::string output = testing::internal::GetCapturedStdout();
#if PY_VERSION_HEX < 0x030C0000
  ASSERT_EQ(
      output, "ERROR: Basic block 0 has no terminator or valid fallthrough.\n");
#else
  ASSERT_EQ(
      output,
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "2.\n");
#endif
}

TEST_F(LIRVerifyTest, TestSingleSuccessorOK) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %1
       %2:Object = Move[0x5]:Object
BB %1 - preds: %0 - succs: %2
       %3:Object = Move [0x5]:Object
BB %2 - preds: %1
       %4:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), true);
}

TEST_F(LIRVerifyTest, TestAllSuccessorsChecked) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %1 %2
       %2:Object = Move[0x5]:Object
BB %1 - preds: %0 - succs %2
       %3:Object = Move [0x5]:Object
BB %2 - preds: %0 %1
       %4:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  testing::internal::CaptureStdout();
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), false);
  std::string output = testing::internal::GetCapturedStdout();
  ASSERT_TRUE(
      output ==
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "2.\n");
}

TEST_F(LIRVerifyTest, TestExplicitBranchOK) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %2
       %2:Object = Move[0x5]:Object
       Branch BB%2
BB %1 - succs: %2
       %3:Object = Move [0x5]:Object
BB %2 - preds: %0 %1
       %4:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), true);
}

TEST_F(LIRVerifyTest, TestExplicitConditionalBranchOK) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %1 %2
       %2:Object = Move[0x5]:Object
       BranchZ BB%2
BB %1 - succs: %2
       %3:Object = Move [0x5]:Object
BB %2 - preds: %0 %1
       %4:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), true);
}

TEST_F(LIRVerifyTest, TestFallthroughToBlockInDifferentSectionDisallowed) {
  auto lir_input_str = fmt::format(R"(Function:
BB %0 - succs: %1 - section: .text
       %2:Object = Move[0x5]:Object
BB %1 - preds: %0 - section: .coldtext
       %3:Object = Move [0x5]:Object
                   Return %2:Object
)");
  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  testing::internal::CaptureStdout();
  ASSERT_EQ(verifyPostRegAllocInvariants(parsed_func.get(), std::cout), false);
  std::string output = testing::internal::GetCapturedStdout();
#if PY_VERSION_HEX < 0x030C0000
  ASSERT_EQ(
      output, "ERROR: Basic block 0 has no terminator or valid fallthrough.\n");
#else
  ASSERT_EQ(
      output,
      "ERROR: Basic block 0 does not contain a jump to non-immediate successor "
      "1.\n");
#endif
}

#if PY_VERSION_HEX < 0x030C0000
TEST_F(LIRVerifyTest, TestUnreachableIsValidTerminator) {
  Parser parser;
  auto parsed_func = parser.parse(R"(Function:
BB %0
       Unreachable
)");
  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), errors))
      << errors.str();
}

TEST_F(LIRVerifyTest, TestAlwaysFailGuardIsValidTerminator) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kGuard,
      nullptr,
      Imm{InstrGuardKind::kAlwaysFail},
      Imm{0},
      Imm{0},
      Imm{0});

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors)) << errors.str();
}

TEST_F(LIRVerifyTest, TestConditionalGuardIsNotATerminator) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kGuard,
      nullptr,
      Imm{InstrGuardKind::kNotZero},
      Imm{0},
      Imm{1});

  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(),
      "ERROR: Basic block 0 has no terminator or valid fallthrough.\n");
}

TEST_F(LIRVerifyTest, TestEmptyBasicBlockRejected) {
  Function function;
  function.allocateBasicBlock();
  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(errors.str(), "ERROR: Basic block 0 has no instructions.\n");
}

TEST_F(LIRVerifyTest, TestEmptyBasicBlockWithImmediateFallthroughOK) {
  Function function;
  auto* empty = function.allocateBasicBlock();
  auto* next = function.allocateBasicBlock();
  empty->addSuccessor(next);
  next->allocateInstr(Instruction::kUnreachable, nullptr);

  std::ostringstream errors;
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, errors)) << errors.str();
}

TEST_F(LIRVerifyTest, TestEmptyPhiRejected) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(Instruction::kPhi, nullptr, OutVReg{DataType::kObject});
  block->allocateInstr(Instruction::kUnreachable, nullptr);
  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(errors.str(), "ERROR: Basic block 0 contains an empty Phi.\n");
}

TEST_F(LIRVerifyTest, TestBranchWithoutOperandRejected) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(Instruction::kBranch, nullptr);
  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(), "ERROR: Branch in block 0 must have exactly one input.\n");
}

TEST_F(LIRVerifyTest, TestBranchWithInvalidOperandRejected) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* branch = block->allocateInstr(Instruction::kBranch, nullptr);
  branch->allocateImmediateInput(0)->setNone();
  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(errors.str(), "ERROR: Branch target in block 0 must be a label.\n");
}

TEST_F(LIRVerifyTest, TestBlockWithoutTerminatorOrFallthroughRejected) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(Instruction::kNop, nullptr);
  std::ostringstream errors;
  EXPECT_FALSE(verifyPostRegAllocInvariants(&function, errors));
  EXPECT_EQ(
      errors.str(),
      "ERROR: Basic block 0 has no terminator or valid fallthrough.\n");
}
#endif

} // namespace jit::lir
