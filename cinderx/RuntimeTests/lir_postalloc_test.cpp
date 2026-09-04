// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit;
using namespace jit::codegen;

namespace jit::lir {
class LIRPostAllocRewriteTest : public RuntimeTest {};

TEST_F(LIRPostAllocRewriteTest, TestInsertBranchForSuccessorsInCondBranch) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
       CondBranch {}:Object, BB%1, BB%2
BB %1 - preds: %0 - succs: %3 %4
       CondBranch {}:Object, BB%3, BB%4
BB %2 - preds: %0 - succs: %3 %4
       CondBranch {}:Object, BB%3, BB%4
BB %3 - preds: %1 %2
       {} = Move {}:Object
BB %4 - preds: %1 %2
       {} = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
#if defined(CINDER_AARCH64)
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   CmpBranchNonZero {}:Object, BB%1

BB %2 - preds: %0 - succs: %3 %4
                   CmpBranchNonZero {}:Object, BB%3
                   Branch BB%4

BB %1 - preds: %0 - succs: %3 %4
                   CmpBranchZero {}:Object, BB%4

BB %3 - preds: %1 %2
{:>9}:Object = Move {}:Object

BB %4 - preds: %1 %2
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});
#else
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   Test {}:Object, {}:Object
                   BranchNZ BB%1

BB %2 - preds: %0 - succs: %3 %4
                   Test {}:Object, {}:Object
                   BranchNZ BB%3
                   Branch BB%4

BB %1 - preds: %0 - succs: %3 %4
                   Test {}:Object, {}:Object
                   BranchZ BB%4

BB %3 - preds: %1 %2
{:>9}:Object = Move {}:Object

BB %4 - preds: %1 %2
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64});
#endif
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

TEST_F(
    LIRPostAllocRewriteTest,
    TestInsertBranchForSuccessorsInCondBranchDifferentSection) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2 - section: .text
       CondBranch {}:Object, BB%1, BB%2
BB %1 - preds: %0 - section: .coldtext
       {}:Object = Move {}:Object
BB %2 - preds: %0
       {}:Object = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
#if defined(CINDER_AARCH64)
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   CmpBranchZero {}:Object, BB%2
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

BB %2 - preds: %0
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});
#else
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 %2
                   Test {}:Object, {}:Object
                   BranchZ BB%2
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

BB %2 - preds: %0
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{5, 64});
#endif
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

TEST_F(LIRPostAllocRewriteTest, TestInsertBranchInDifferentSection) {
  auto lir_input_str = fmt::format(
      R"(Function:
BB %0 - succs: %1 - section: .text
{:>9}:Object = Move {}:Object
BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object
)",
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{7, 64});

  Parser parser;
  auto parsed_func = parser.parse(lir_input_str);
  parsed_func->sortBasicBlocks();

  jit::codegen::Environ env_;
  PostRegAllocRewrite post_rewrite(parsed_func.get(), &env_);
  post_rewrite.run();

  std::stringstream ss;
  ss << *parsed_func;
  auto expected_lir_str = fmt::format(
      R"(Function:
BB %0 - succs: %1
{:>9}:Object = Move {}:Object
                   Branch BB%1

BB %1 - preds: %0 - section: .coldtext
{:>9}:Object = Move {}:Object

)",
      PhyLocation{0, 64},
      PhyLocation{13, 64},
      PhyLocation{0, 64},
      PhyLocation{7, 64});
  ASSERT_EQ(expected_lir_str, ss.str());
  ASSERT_TRUE(verifyPostRegAllocInvariants(parsed_func.get(), std::cout));
}

#if defined(CINDER_AARCH64)
// Helper to collect instructions from a block into a vector for easy indexing.
static std::vector<Instruction*> collectInstrs(BasicBlock& bb) {
  std::vector<Instruction*> result;
  for (auto& instr : bb.instructions()) {
    result.push_back(instr.get());
  }
  return result;
}

static void runPostAllocRewrite(Function& func) {
  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();
}

static void expectRegMove(
    Instruction* instr,
    PhyLocation dst,
    PhyLocation src,
    DataType data_type = DataType::kObject) {
  ASSERT_TRUE(instr->isMove());
  ASSERT_TRUE(instr->output()->isReg());
  ASSERT_TRUE(instr->getInput(0)->isReg());
  EXPECT_EQ(instr->output()->getPhyRegister(), dst);
  EXPECT_EQ(instr->getInput(0)->getPhyRegister(), src);
  EXPECT_EQ(instr->output()->dataType(), data_type);
  EXPECT_EQ(instr->getInput(0)->dataType(), data_type);
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveChainFoldsIntermediateGP) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 1);
  expectRegMove(instrs[0], X1, X0);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveChainSkipsSelfMoves) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X0, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 1);
  expectRegMove(instrs[0], X1, X0);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveChainFoldsIntermediateFP) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{D8, DataType::kDouble},
      PhyReg{D0, DataType::kDouble});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{D1, DataType::kDouble},
      PhyReg{D8, DataType::kDouble});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 1);
  expectRegMove(instrs[0], D1, D0, DataType::kDouble);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsIntermediateRealUse) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  auto* use = bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg{X9, DataType::k64bit},
      PhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[1]->isAdd());
  ASSERT_TRUE(instrs[1]->getInput(0)->isReg());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X8);
  EXPECT_EQ(instrs[1], use);
  expectRegMove(instrs[2], X1, X0, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsChainWhenInputNotLastUse) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  auto* later_use = bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg{X9, DataType::k64bit},
      PhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  expectRegMove(instrs[1], X1, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[2]->isAdd());
  EXPECT_EQ(instrs[2], later_use);
  ASSERT_TRUE(instrs[2]->getInput(0)->isReg());
  EXPECT_EQ(instrs[2]->getInput(0)->getPhyRegister(), X8);
  ASSERT_EQ(instrs[1], arg_move);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotFoldMixedWidthChain) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k32bit},
      PhyReg{X0, DataType::k32bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);
  expectRegMove(instrs[0], X8, X0, DataType::k32bit);
  expectRegMove(instrs[1], X1, X8, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsIntermediateInPlaceDef) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      PhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[1]->isAdd());
  ASSERT_TRUE(instrs[1]->output()->isNone());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X8);
  expectRegMove(instrs[2], X1, X8, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsUnlistedInPlaceDef) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  bb->allocateInstr(
      Instruction::kLShift,
      nullptr,
      PhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k8bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[1]->isLShift());
  ASSERT_TRUE(instrs[1]->output()->isNone());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X8);
  expectRegMove(instrs[2], X1, X8, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsReturnRegInPlaceDef) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      PhyReg{X0, DataType::k64bit},
      Imm{1, DataType::k64bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[1]->isAdd());
  ASSERT_TRUE(instrs[1]->output()->isNone());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X0);
  expectRegMove(instrs[2], X1, X8, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveKeepsExchangeDef) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      PhyReg{X0, DataType::k64bit});
  auto* exchange = bb->allocateInstr(
      Instruction::kExchange,
      nullptr,
      OutPhyReg{X9, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0, DataType::k64bit);
  ASSERT_TRUE(instrs[1]->isExchange());
  EXPECT_EQ(instrs[1], exchange);
  EXPECT_EQ(instrs[1]->output()->getPhyRegister(), X9);
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X8);
  expectRegMove(instrs[2], X1, X8, DataType::k64bit);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotCrossTStateLoad) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  bb->allocateInstr(
      Instruction::kLoadThreadState,
      nullptr,
      OutStk{PhyLocation(-16, 64), DataType::kObject});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0);
  ASSERT_TRUE(instrs[1]->isLoadThreadState());
  ASSERT_TRUE(instrs[1]->output()->isStack());
  expectRegMove(instrs[2], X1, X8);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotCrossEssentialBarrier) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  bb->allocateInstr(Instruction::kVariadicPush, nullptr);
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0);
  ASSERT_TRUE(instrs[1]->isVariadicPush());
  expectRegMove(instrs[2], X1, X8);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotCrossCallBarrier) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  bb->allocateInstr(Instruction::kCall, nullptr, Imm{0});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0);
  ASSERT_TRUE(instrs[1]->isCall());
  expectRegMove(instrs[2], X1, X8);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotCrossOSREntry) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::kObject},
      PhyReg{X0, DataType::kObject});
  bb->allocateInstr(Instruction::kOSREntry, nullptr, Imm{0});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 3);
  expectRegMove(instrs[0], X8, X0);
  ASSERT_TRUE(instrs[1]->isOSREntry());
  expectRegMove(instrs[2], X1, X8);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

TEST_F(LIRPostAllocRewriteTest, CallResultArgMoveDoesNotCrossRegisterClasses) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{D8, DataType::kDouble},
      PhyReg{D0, DataType::kDouble});
  auto* arg_move = bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X1, DataType::k64bit},
      PhyReg{D8, DataType::kDouble});
  arg_move->getInput(0)->setLastUse();

  runPostAllocRewrite(func);

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);
  expectRegMove(instrs[0], D8, D0, DataType::kDouble);
  ASSERT_TRUE(instrs[1]->isMove());
  ASSERT_TRUE(instrs[1]->output()->isReg());
  ASSERT_TRUE(instrs[1]->getInput(0)->isReg());
  EXPECT_EQ(instrs[1]->output()->getPhyRegister(), X1);
  EXPECT_EQ(instrs[1]->output()->dataType(), DataType::k64bit);
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), D8);
  EXPECT_EQ(instrs[1]->getInput(0)->dataType(), DataType::kDouble);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
}

static void checkSubWordCondBranchUsesTest(DataType data_type) {
  Function func;
  auto* entry = func.allocateBasicBlock();
  auto* true_bb = func.allocateBasicBlock();
  auto* false_bb = func.allocateBasicBlock();

  entry->addSuccessor(true_bb);
  entry->addSuccessor(false_bb);
  entry->allocateInstr(
      Instruction::kCondBranch, nullptr, PhyReg{X0, data_type});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*entry);
  if (codegen::arch::kBuildArch == codegen::arch::Arch::kAarch64) {
    ASSERT_EQ(instrs.size(), 1);
    EXPECT_TRUE(instrs[0]->isCmpBranch());
    ASSERT_EQ(instrs[0]->getNumInputs(), 2);
    EXPECT_EQ(instrs[0]->getInput(0)->dataType(), DataType::k32bit);
    EXPECT_TRUE(instrs[0]->getInput(1)->isLabel());
    return;
  }

  ASSERT_GE(instrs.size(), 2);
  EXPECT_TRUE(instrs[0]->isTest());
  EXPECT_EQ(instrs[0]->getInput(0)->dataType(), data_type);
  EXPECT_EQ(instrs[0]->getInput(1)->dataType(), data_type);
  EXPECT_TRUE(instrs[1]->isBranchNZ() || instrs[1]->isBranchZ());
  ASSERT_EQ(instrs[1]->getNumInputs(), 1);
  EXPECT_TRUE(instrs[1]->getInput(0)->isLabel());
}

TEST_F(LIRPostAllocRewriteTest, CondBranchSubWordStillUsesTest) {
  checkSubWordCondBranchUsesTest(DataType::k8bit);
  checkSubWordCondBranchUsesTest(DataType::k16bit);
}

TEST_F(LIRPostAllocRewriteTest, LoadAttrCachedFastPathKeepsOpcode) {
  Function func;
  auto* bb = func.allocateBasicBlock();
  bb->allocateInstr(
      Instruction::kLoadAttrCachedFastPath,
      nullptr,
      OutPhyReg{X0, DataType::kObject},
      Imm{0x1234, DataType::kObject},
      PhyReg{X1, DataType::k64bit},
      PhyReg{X2, DataType::kObject},
      PhyReg{X3, DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  Instruction* fast_path = nullptr;
  for (auto* instr : collectInstrs(*bb)) {
    ASSERT_FALSE(instr->isCall())
        << "LoadAttrCachedFastPath must not be rewritten to kCall";
    if (instr->isLoadAttrCachedFastPath()) {
      fast_path = instr;
    }
  }
  ASSERT_NE(fast_path, nullptr);
  EXPECT_EQ(fast_path->getNumInputs(), 1);
}

TEST_F(LIRPostAllocRewriteTest, LoadMethodCachedFastPathKeepsOpcode) {
  Function func;
  auto* bb = func.allocateBasicBlock();
  bb->allocateInstr(
      Instruction::kLoadMethodCachedFastPath,
      nullptr,
      OutPhyReg{X0, DataType::kObject},
      Imm{0x1234, DataType::kObject},
      PhyReg{X1, DataType::k64bit},
      PhyReg{X2, DataType::kObject},
      PhyReg{X3, DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  Instruction* fast_path = nullptr;
  for (auto* instr : collectInstrs(*bb)) {
    ASSERT_FALSE(instr->isCall());
    if (instr->isLoadMethodCachedFastPath()) {
      fast_path = instr;
    }
  }
  ASSERT_NE(fast_path, nullptr);
  EXPECT_EQ(fast_path->getNumInputs(), 1);
}

TEST_F(LIRPostAllocRewriteTest, StoreAttrCachedFastPathKeepsOpcode) {
  Function func;
  auto* bb = func.allocateBasicBlock();
  bb->allocateInstr(
      Instruction::kStoreAttrCachedFastPath,
      nullptr,
      OutPhyReg{X0, DataType::k32bit},
      Imm{0x1234, DataType::kObject},
      PhyReg{X1, DataType::k64bit},
      PhyReg{X2, DataType::kObject},
      PhyReg{X3, DataType::kObject},
      PhyReg{X4, DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  Instruction* fast_path = nullptr;
  for (auto* instr : collectInstrs(*bb)) {
    ASSERT_FALSE(instr->isCall());
    if (instr->isStoreAttrCachedFastPath()) {
      fast_path = instr;
    }
  }
  ASSERT_NE(fast_path, nullptr);
  EXPECT_EQ(fast_path->getNumInputs(), 1);
}

TEST_F(
    LIRPostAllocRewriteTest,
    BinaryOpExactLongAddSubFastPathKeepsOpcodeAndABI) {
#if defined(CINDER_AARCH64) && PY_VERSION_HEX >= 0x030E0000 &&  \
    PY_VERSION_HEX < 0x030F0000 && !defined(Py_GIL_DISABLED) && \
    !defined(Py_REF_DEBUG) && !defined(Py_STATS)
  Function func;
  auto* bb = func.allocateBasicBlock();
  constexpr uint64_t kGenericHelper = 0x12345678;
  bb->allocateInstr(
      Instruction::kBinaryOpExactLongAddSubFastPath,
      nullptr,
      OutPhyReg{X4, DataType::kObject},
      Imm{kGenericHelper, DataType::k64bit},
      PhyReg{X2, DataType::kObject},
      PhyReg{X3, DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 4);
  ASSERT_TRUE(instrs[0]->isMove());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), X0);
  EXPECT_EQ(instrs[0]->getInput(0)->getPhyRegister(), X2);
  ASSERT_TRUE(instrs[1]->isMove());
  EXPECT_EQ(instrs[1]->output()->getPhyRegister(), X1);
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), X3);

  auto* fast_path = instrs[2];
  ASSERT_TRUE(fast_path->isBinaryOpExactLongAddSubFastPath());
  ASSERT_FALSE(fast_path->isCall());
  EXPECT_TRUE(fast_path->output()->isNone());
  ASSERT_EQ(fast_path->getNumInputs(), 1);
  ASSERT_TRUE(fast_path->getInput(0)->isImm());
  EXPECT_EQ(fast_path->getInput(0)->getConstant(), kGenericHelper);

  ASSERT_TRUE(instrs[3]->isMove());
  EXPECT_EQ(instrs[3]->output()->getPhyRegister(), X4);
  EXPECT_EQ(instrs[3]->getInput(0)->getPhyRegister(), X0);
  ASSERT_TRUE(verifyPostRegAllocInvariants(&func, std::cout));
#else
  GTEST_SKIP() << "AArch64 CPython 3.14 GIL-only fast path";
#endif
}

// kAdd with one register input and one stack input should insert a Move from
// stack to GP scratch register before the Add, then rewrite the Add's stack
// input to use the scratch register.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteAddWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Add X0:64bit, [X29(-16)]:64bit
  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg{X0, DataType::k64bit},
      PhyReg{X0, DataType::k64bit},
      Stk{PhyLocation(-16, 64), DataType::k64bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Move (stack→scratch), Add (reg, scratch)
  ASSERT_EQ(instrs.size(), 2);

  // First instruction: Move from stack to scratch
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);
  EXPECT_TRUE(instrs[0]->getInput(0)->isStack());

  // Second instruction: Add with scratch register input
  EXPECT_TRUE(instrs[1]->isAdd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(instrs[1]->getInput(1)->getPhyRegister(), arch::reg_scratch_0_loc);
}

// kInc with a stack input should produce:
//   Move stack→scratch, Inc scratch, Move scratch→stack
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteIncWithStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Inc [X29(-24)]:Object
  bb->allocateInstr(
      Instruction::kInc,
      nullptr,
      OutStk{PhyLocation(-24, 64), DataType::kObject},
      Stk{PhyLocation(-24, 64), DataType::kObject});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  // Should now have: Move (stack→scratch), Inc (scratch), Move (scratch→stack)
  ASSERT_EQ(instrs.size(), 3);

  // Move from stack to scratch
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_scratch_0_loc);

  // Inc on scratch
  EXPECT_TRUE(instrs[1]->isInc());
  EXPECT_TRUE(instrs[1]->getInput(0)->isReg());
  EXPECT_EQ(instrs[1]->getInput(0)->getPhyRegister(), arch::reg_scratch_0_loc);

  // Move from scratch back to stack
  EXPECT_TRUE(instrs[2]->isMove());
  EXPECT_TRUE(instrs[2]->output()->isStack());
  EXPECT_TRUE(instrs[2]->getInput(0)->isReg());
  EXPECT_EQ(instrs[2]->getInput(0)->getPhyRegister(), arch::reg_scratch_0_loc);
}

// kFadd with a FP stack input should use FP scratch register (D16), not GP
// scratch (X13).
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteFaddWithFPStackInput) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // Fadd D0:Double, [X29(-32)]:Double
  bb->allocateInstr(
      Instruction::kFadd,
      nullptr,
      OutPhyReg{D0, DataType::kDouble},
      PhyReg{D0, DataType::kDouble},
      Stk{PhyLocation(-32, 64), DataType::kDouble});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Move from stack to FP scratch (D16)
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_TRUE(instrs[0]->output()->isReg());
  EXPECT_EQ(instrs[0]->output()->getPhyRegister(), arch::reg_fp_scratch_0_loc);
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::kDouble);

  // Fadd with FP scratch register input
  EXPECT_TRUE(instrs[1]->isFadd());
  EXPECT_TRUE(instrs[1]->getInput(1)->isReg());
  EXPECT_EQ(
      instrs[1]->getInput(1)->getPhyRegister(), arch::reg_fp_scratch_0_loc);
}

// kLessThanSigned with a k8bit stack input should widen the load to k32bit to
// preserve the sign-extended value produced during target selection.
TEST_F(LIRPostAllocRewriteTest, MemoryInputRewriteSignedCmpWidensSubWordToK32) {
  Function func;
  auto* bb = func.allocateBasicBlock();

  // LessThanSigned X0:8bit, [X29(-8)]:8bit
  bb->allocateInstr(
      Instruction::kLessThanSigned,
      nullptr,
      OutPhyReg{X0, DataType::k8bit},
      PhyReg{X0, DataType::k8bit},
      Stk{PhyLocation(-8, 8), DataType::k8bit});

  jit::codegen::Environ env;
  PostRegAllocRewrite rewrite(&func, &env);
  rewrite.run();

  auto instrs = collectInstrs(*bb);
  ASSERT_EQ(instrs.size(), 2);

  // Move from stack to scratch — should be widened to k32bit
  EXPECT_TRUE(instrs[0]->isMove());
  EXPECT_EQ(instrs[0]->output()->dataType(), DataType::k32bit);
  EXPECT_EQ(instrs[0]->getInput(0)->dataType(), DataType::k32bit);

  // The comparison input should also be k32bit
  EXPECT_TRUE(instrs[1]->isLessThanSigned());
  EXPECT_EQ(instrs[1]->getInput(1)->dataType(), DataType::k32bit);
}
#endif // CINDER_AARCH64

} // namespace jit::lir
