// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cmath>
#include <limits>

using namespace jit::codegen;

namespace jit::lir {

#if defined(CINDER_AARCH64)

class AArch64PeepholeTest : public RuntimeTest {};

Instruction* appendIndirectStore(
    Function& function,
    Instruction::Opcode opcode,
    DataType output_type,
    uint64_t value,
    DataType input_type) {
  auto* block = function.allocateBasicBlock();
  auto* store = block->allocateInstr(
      opcode, nullptr, OutInd{X1, 24, output_type}, Imm{value, input_type});
  // OutInd currently preserves the default output type when constructed via
  // addOperands(), so make the tested store width explicit.
  store->output()->setDataType(output_type);
  return store;
}

Instruction* appendPhysicalIndirectStore(
    BasicBlock* block,
    Instruction::Opcode opcode,
    PhyLocation base,
    int32_t offset,
    DataType output_type,
    PhyLocation value,
    DataType input_type) {
  auto* store = block->allocateInstr(
      opcode,
      nullptr,
      OutInd{base, offset, output_type},
      PhyReg{value, input_type});
  // See appendIndirectStore(): the store width must be made explicit after
  // constructing the indirect output.
  store->output()->setDataType(output_type);
  return store;
}

Instruction* appendPhysicalIndirectLoad(
    BasicBlock* block,
    Instruction::Opcode opcode,
    PhyLocation base,
    int32_t offset,
    DataType input_type,
    PhyLocation destination,
    DataType output_type) {
  auto* load = block->allocateInstr(
      opcode,
      nullptr,
      OutPhyReg{destination, output_type},
      Ind{base, offset, input_type});
  // Ind currently preserves the default input type when constructed via
  // addOperands(), so make the tested load width explicit.
  static_cast<Operand*>(load->getInput(0))->setDataType(input_type);
  return load;
}

void runPostAllocRewrites(Function& function) {
  Environ env;
  PostRegAllocRewrite rewrite(&function, &env);
  rewrite.run();
}

TEST_F(AArch64PeepholeTest, KeepsCanonicalZeroStoresAsImmediates) {
  constexpr DataType kSupportedTypes[] = {
      DataType::k8bit,
      DataType::k16bit,
      DataType::k32bit,
      DataType::k64bit,
      DataType::kObject,
  };

  for (DataType data_type : kSupportedTypes) {
    SCOPED_TRACE(fmt::format("data type {}", data_type));
    Function function;
    auto* store = appendIndirectStore(
        function, Instruction::kMove, data_type, 0, data_type);

    runPostAllocRewrites(function);

    ASSERT_TRUE(store->output()->isInd());
    EXPECT_EQ(store->output()->dataType(), data_type);
    ASSERT_EQ(store->getNumInputs(), 1);
    auto* input = store->getInput(0);
    ASSERT_TRUE(input->isImm());
    EXPECT_EQ(input->getConstant(), 0);
    EXPECT_EQ(input->dataType(), data_type);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(
    AArch64PeepholeTest,
    CollapsesAdjacentLastUseZeroMaterializationsToImmediateStore) {
  constexpr DataType kSupportedTypes[] = {
      DataType::k32bit,
      DataType::k64bit,
      DataType::kObject,
  };

  for (DataType data_type : kSupportedTypes) {
    SCOPED_TRACE(fmt::format("data type {}", data_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, data_type},
        Imm{0, data_type});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 24, data_type},
        PhyReg{X8, data_type});
    store->output()->setDataType(data_type);
    store->getInput(0)->setLastUse();

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 1);
    ASSERT_TRUE(store->getInput(0)->isImm());
    EXPECT_EQ(store->getInput(0)->getConstant(), 0);
    EXPECT_EQ(store->getInput(0)->dataType(), data_type);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, CollapsesSubWordZeroStoresAfterRegisterWidening) {
  constexpr DataType kSubWordTypes[] = {
      DataType::k8bit,
      DataType::k16bit,
  };

  for (DataType data_type : kSubWordTypes) {
    SCOPED_TRACE(fmt::format("data type {}", data_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, data_type},
        Imm{0, data_type});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 24, data_type},
        PhyReg{X8, data_type});
    store->output()->setDataType(data_type);
    store->getInput(0)->setLastUse();

    runPostAllocRewrites(function);

    // rewriteSubWordRegMoves runs before the peephole dispatcher because
    // AArch64 has only W/X general-register operands.  It widens the
    // materialization to W8.  STRB/STRH read the low subword of WZR, so this
    // exact 32-bit-zero to byte/halfword-store shape remains lossless.
    EXPECT_EQ(block->getNumInstrs(), 1);
    ASSERT_TRUE(store->getInput(0)->isImm());
    EXPECT_EQ(store->getInput(0)->getConstant(), 0);
    EXPECT_EQ(store->getInput(0)->dataType(), data_type);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, CollapsesCanonicalWZeroToSubWordStores) {
  for (DataType store_type : {DataType::k8bit, DataType::k16bit}) {
    SCOPED_TRACE(fmt::format("store_type={}", store_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, DataType::k32bit},
        Imm{0, DataType::k32bit});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 24, store_type},
        PhyReg{X8, store_type});
    store->output()->setDataType(store_type);
    store->getInput(0)->setLastUse();

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 1);
    ASSERT_TRUE(store->getInput(0)->isImm());
    EXPECT_EQ(store->getInput(0)->getConstant(), 0);
    EXPECT_EQ(store->getInput(0)->dataType(), store_type);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, PairsAdjacentPointerWidthLoads) {
  Function function;
  auto* block = function.allocateBasicBlock();
  appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X8,
      DataType::k64bit);
  auto* upper_load = appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::k64bit,
      X9,
      DataType::kObject);
  const auto* upper_origin = upper_load->origin();

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 1);
  EXPECT_EQ(upper_load->opcode(), Instruction::kLoadPair);
  EXPECT_EQ(upper_load->origin(), upper_origin);
  EXPECT_TRUE(upper_load->output()->isNone());
  ASSERT_EQ(upper_load->getNumInputs(), 4);
  EXPECT_EQ(upper_load->getInput(0)->getConstant(), 0);
  EXPECT_EQ(upper_load->getInput(1)->getPhyRegister(), X1);
  EXPECT_EQ(upper_load->getInput(2)->getPhyRegister(), X8);
  EXPECT_EQ(upper_load->getInput(2)->dataType(), DataType::k64bit);
  EXPECT_EQ(upper_load->getInput(3)->getPhyRegister(), X9);
  EXPECT_EQ(upper_load->getInput(3)->dataType(), DataType::kObject);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, PairsDescendingPointerWidthLoads) {
  for (int32_t lower_offset : {-512, 0, 64, 504}) {
    SCOPED_TRACE(lower_offset);
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        X1,
        lower_offset + kPointerSize,
        DataType::kObject,
        X8,
        DataType::kObject);
    auto* lower_load = appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        X1,
        lower_offset,
        DataType::k64bit,
        X9,
        DataType::k64bit);
    const auto* origin = lower_load->origin();
    runPostAllocRewrites(function);
    ASSERT_EQ(block->getNumInstrs(), 1);
    ASSERT_EQ(lower_load->opcode(), Instruction::kLoadPair);
    EXPECT_EQ(lower_load->origin(), origin);
    EXPECT_EQ(
        lower_load->getInput(0)->getConstant(),
        static_cast<uint64_t>(static_cast<int64_t>(lower_offset)));
    EXPECT_EQ(lower_load->getInput(2)->getPhyRegister(), X9);
    EXPECT_EQ(lower_load->getInput(2)->dataType(), DataType::k64bit);
    EXPECT_EQ(lower_load->getInput(3)->getPhyRegister(), X8);
    EXPECT_EQ(lower_load->getInput(3)->dataType(), DataType::kObject);
    block->allocateInstr(Instruction::kReturn, nullptr);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, RejectsDescendingLoadRegisterHazards) {
  const PhyLocation destinations[][2] = {{X1, X9}, {X8, X1}, {X8, X8}};
  for (const auto& regs : destinations) {
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        X1,
        8,
        DataType::kObject,
        regs[0],
        DataType::kObject);
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        X1,
        0,
        DataType::kObject,
        regs[1],
        DataType::kObject);
    runPostAllocRewrites(function);
    for (const auto& instr : block->instructions()) {
      EXPECT_NE(instr->opcode(), Instruction::kLoadPair);
    }
    block->allocateInstr(Instruction::kReturn, nullptr);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, PairsLoadOffsetsAtEncodingBoundaries) {
  struct Case {
    int32_t lower_offset;
    PhyLocation base;
  };
  constexpr Case kCases[] = {
      {-512, X1},
      // The positive encoding boundary is only eight-byte aligned.
      {504, X1},
      // SP remains a legal LDP address base.
      {8, SP},
  };

  for (const auto& test_case : kCases) {
    SCOPED_TRACE(fmt::format(
        "lower_offset={} base={}", test_case.lower_offset, test_case.base.loc));
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        test_case.base,
        test_case.lower_offset,
        DataType::kObject,
        X8,
        DataType::kObject);
    auto* upper_load = appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        test_case.base,
        test_case.lower_offset + kPointerSize,
        DataType::k64bit,
        X9,
        DataType::k64bit);

    runPostAllocRewrites(function);

    ASSERT_EQ(block->getNumInstrs(), 1);
    ASSERT_EQ(upper_load->opcode(), Instruction::kLoadPair);
    ASSERT_EQ(upper_load->getNumInputs(), 4);
    EXPECT_EQ(
        upper_load->getInput(0)->getConstant(),
        static_cast<uint64_t>(static_cast<int64_t>(test_case.lower_offset)));
    EXPECT_EQ(upper_load->getInput(1)->getPhyRegister(), test_case.base);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, KeepsIneligibleAdjacentLoadsUnchanged) {
  struct Case {
    int32_t first_offset;
    int32_t second_offset;
    PhyLocation first_base;
    PhyLocation second_base;
    PhyLocation first_destination;
    PhyLocation second_destination;
    DataType first_input_type;
    DataType second_input_type;
    DataType first_output_type;
    DataType second_output_type;
  };
  constexpr Case kCases[] = {
      // Outside the signed scaled LDP displacement.
      {-520,
       -512,
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {512,
       520,
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // Extreme offsets must not overflow while computing their distance.
      {std::numeric_limits<int32_t>::min(),
       std::numeric_limits<int32_t>::max(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {std::numeric_limits<int32_t>::max(),
       std::numeric_limits<int32_t>::min(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // A wrapping int32_t subtraction would incorrectly produce eight.
      {std::numeric_limits<int32_t>::max() - 7,
       std::numeric_limits<int32_t>::min(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // Require consecutive, aligned addresses.
      {0,
       16,
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {4,
       12,
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // Both scalar loads must use the same GP/SP base.
      {0,
       8,
       X1,
       X2,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       D0,
       D0,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       XZR,
       XZR,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // LDP destinations are distinct GP registers and cannot destroy base.
      {0,
       8,
       X1,
       X1,
       X1,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X8,
       X1,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X8,
       X8,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       SP,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X8,
       SP,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       XZR,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X29,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X8,
       X29,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      // FP and subword loads retain their scalar lowering.
      {0,
       8,
       X1,
       X1,
       D8,
       D9,
       DataType::kDouble,
       DataType::kDouble,
       DataType::kDouble,
       DataType::kDouble},
      {0,
       8,
       X1,
       X1,
       X8,
       X9,
       DataType::k16bit,
       DataType::k16bit,
       DataType::k16bit,
       DataType::k16bit},
      {0,
       8,
       X1,
       X1,
       X8,
       X9,
       DataType::k32bit,
       DataType::k32bit,
       DataType::k32bit,
       DataType::k32bit},
      // Both the memory access and destination must be pointer-width.
      {0,
       8,
       X1,
       X1,
       X8,
       X9,
       DataType::k32bit,
       DataType::kObject,
       DataType::kObject,
       DataType::kObject},
      {0,
       8,
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject,
       DataType::k32bit,
       DataType::kObject},
  };

  for (const auto& test_case : kCases) {
    SCOPED_TRACE(fmt::format(
        "offsets=({}, {}) bases=({}, {}) destinations=({}, {}) "
        "input_types=({}, {}) output_types=({}, {})",
        test_case.first_offset,
        test_case.second_offset,
        test_case.first_base.loc,
        test_case.second_base.loc,
        test_case.first_destination.loc,
        test_case.second_destination.loc,
        test_case.first_input_type,
        test_case.second_input_type,
        test_case.first_output_type,
        test_case.second_output_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        test_case.first_base,
        test_case.first_offset,
        test_case.first_input_type,
        test_case.first_destination,
        test_case.first_output_type);
    appendPhysicalIndirectLoad(
        block,
        Instruction::kMove,
        test_case.second_base,
        test_case.second_offset,
        test_case.second_input_type,
        test_case.second_destination,
        test_case.second_output_type);

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 2);
    EXPECT_TRUE(block->getFirstInstr()->isMove());
    EXPECT_TRUE(block->getLastInstr()->isMove());
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, Keeps32BitAddressBasesUnpaired) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* lower_load = appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X8,
      DataType::kObject);
  auto* upper_load = appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X9,
      DataType::kObject);
  static_cast<Operand*>(
      lower_load->getInput(0)->getMemoryIndirect()->getBaseRegOperand())
      ->setDataType(DataType::k32bit);
  static_cast<Operand*>(
      upper_load->getInput(0)->getMemoryIndirect()->getBaseRegOperand())
      ->setDataType(DataType::k32bit);

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  EXPECT_TRUE(lower_load->isMove());
  EXPECT_TRUE(upper_load->isMove());
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(
    AArch64PeepholeTest,
    KeepsRelaxedIndexedStackAndInterruptedLoadsUnpaired) {
  enum class Rejection {
    kFirstRelaxed,
    kSecondRelaxed,
    kFirstIndexed,
    kSecondIndexed,
    kStackSlots,
    kInterrupted,
  };
  constexpr Rejection kRejections[] = {
      Rejection::kFirstRelaxed,
      Rejection::kSecondRelaxed,
      Rejection::kFirstIndexed,
      Rejection::kSecondIndexed,
      Rejection::kStackSlots,
      Rejection::kInterrupted,
  };

  for (Rejection rejection : kRejections) {
    SCOPED_TRACE(fmt::format("rejection={}", static_cast<int>(rejection)));
    Function function;
    auto* block = function.allocateBasicBlock();

    if (rejection == Rejection::kStackSlots) {
      block->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg{X8, DataType::kObject},
          Stk{PhyLocation{-16, 64}, DataType::kObject});
      block->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg{X9, DataType::kObject},
          Stk{PhyLocation{-8, 64}, DataType::kObject});
    } else {
      auto append_load = [&](bool first) {
        auto opcode = (first && rejection == Rejection::kFirstRelaxed) ||
                (!first && rejection == Rejection::kSecondRelaxed)
            ? Instruction::kMoveRelaxed
            : Instruction::kMove;
        auto offset = first ? 0 : kPointerSize;
        auto destination = first ? X8 : X9;
        bool indexed = (first && rejection == Rejection::kFirstIndexed) ||
            (!first && rejection == Rejection::kSecondIndexed);
        if (indexed) {
          auto* load = block->allocateInstr(
              opcode,
              nullptr,
              OutPhyReg{destination, DataType::kObject},
              Ind{PhyLocation(X1),
                  PhyLocation(X10),
                  1,
                  offset,
                  DataType::kObject});
          static_cast<Operand*>(load->getInput(0))
              ->setDataType(DataType::kObject);
        } else {
          appendPhysicalIndirectLoad(
              block,
              opcode,
              X1,
              offset,
              DataType::kObject,
              destination,
              DataType::kObject);
        }
      };

      append_load(true);
      if (rejection == Rejection::kInterrupted) {
        block->allocateInstr(Instruction::kNop, nullptr);
      }
      append_load(false);
    }

    const auto original_count = block->getNumInstrs();
    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), original_count);
    for (const auto& instr : block->instructions()) {
      EXPECT_NE(instr->opcode(), Instruction::kLoadPair);
    }
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, ExistingLoadPairIsAnOpaqueBarrier) {
  Function function;
  auto* block = function.allocateBasicBlock();
  appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X8,
      DataType::kObject);
  auto* load_pair = block->allocateInstr(
      Instruction::kLoadPair,
      nullptr,
      Imm{16, DataType::k64bit},
      PhyReg{X2, DataType::kObject},
      PhyReg{X10, DataType::kObject},
      PhyReg{X11, DataType::kObject});
  appendPhysicalIndirectLoad(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X9,
      DataType::kObject);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 3);
  EXPECT_EQ(load_pair->opcode(), Instruction::kLoadPair);
  ASSERT_EQ(load_pair->getNumInputs(), 4);
  EXPECT_EQ(load_pair->getInput(0)->getConstant(), 16);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, PairsAdjacentAlignedObjectStores) {
  Function function;
  auto* block = function.allocateBasicBlock();
  appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X8,
      DataType::kObject);
  auto* upper_store = appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X9,
      DataType::k64bit);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 1);
  EXPECT_EQ(upper_store->opcode(), Instruction::kStorePair);
  EXPECT_TRUE(upper_store->output()->isNone());
  ASSERT_EQ(upper_store->getNumInputs(), 4);
  EXPECT_EQ(upper_store->getInput(0)->getConstant(), 0);
  EXPECT_EQ(upper_store->getInput(1)->getPhyRegister(), X1);
  EXPECT_EQ(upper_store->getInput(2)->getPhyRegister(), X8);
  EXPECT_EQ(upper_store->getInput(3)->getPhyRegister(), X9);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, PairsAdjacentStackArgumentStores) {
  Function function;
  auto* block = function.allocateBasicBlock();
  appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      SP,
      0,
      DataType::kObject,
      X8,
      DataType::kObject);
  auto* upper_store = appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      SP,
      8,
      DataType::kObject,
      X9,
      DataType::kObject);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 1);
  EXPECT_EQ(upper_store->opcode(), Instruction::kStorePair);
  ASSERT_EQ(upper_store->getNumInputs(), 4);
  EXPECT_EQ(upper_store->getInput(0)->getConstant(), 0);
  EXPECT_EQ(upper_store->getInput(1)->getPhyRegister(), SP);
  EXPECT_EQ(upper_store->getInput(2)->getPhyRegister(), X8);
  EXPECT_EQ(upper_store->getInput(3)->getPhyRegister(), X9);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, PairsAllPointerWidthTypeAndSourceCombinations) {
  constexpr DataType kPointerTypes[] = {
      DataType::k64bit,
      DataType::kObject,
  };

  size_t case_count = 0;
  for (DataType first_memory_type : kPointerTypes) {
    for (DataType second_memory_type : kPointerTypes) {
      for (DataType first_value_type : kPointerTypes) {
        for (DataType second_value_type : kPointerTypes) {
          for (bool same_source : {false, true}) {
            case_count++;
            SCOPED_TRACE(fmt::format(
                "memory=({}, {}) values=({}, {}) same_source={}",
                first_memory_type,
                second_memory_type,
                first_value_type,
                second_value_type,
                same_source));

            Function function;
            auto* block = function.allocateBasicBlock();
            appendPhysicalIndirectStore(
                block,
                Instruction::kMove,
                X1,
                0,
                first_memory_type,
                X8,
                first_value_type);
            auto second_source = same_source ? X8 : X9;
            auto* upper_store = appendPhysicalIndirectStore(
                block,
                Instruction::kMove,
                X1,
                8,
                second_memory_type,
                second_source,
                second_value_type);

            runPostAllocRewrites(function);

            ASSERT_EQ(block->getNumInstrs(), 1);
            EXPECT_EQ(upper_store->opcode(), Instruction::kStorePair);
            ASSERT_EQ(upper_store->getNumInputs(), 4);
            EXPECT_EQ(upper_store->getInput(2)->getPhyRegister(), X8);
            EXPECT_EQ(
                upper_store->getInput(3)->getPhyRegister(), second_source);
            EXPECT_EQ(upper_store->getInput(2)->dataType(), first_value_type);
            EXPECT_EQ(upper_store->getInput(3)->dataType(), second_value_type);
            EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
          }
        }
      }
    }
  }
  EXPECT_EQ(case_count, 32);
}

TEST_F(AArch64PeepholeTest, PairsThreeAndFourStoreStreamsToFixedPoint) {
  for (size_t store_count : {size_t{3}, size_t{4}}) {
    SCOPED_TRACE(fmt::format("store_count={}", store_count));
    Function function;
    auto* block = function.allocateBasicBlock();
    constexpr PhyLocation kSources[] = {X8, X9, X10, X11};
    for (size_t i = 0; i < store_count; i++) {
      appendPhysicalIndirectStore(
          block,
          Instruction::kMove,
          X1,
          static_cast<int32_t>(i * kPointerSize),
          DataType::kObject,
          kSources[i],
          DataType::kObject);
    }

    runPostAllocRewrites(function);

    size_t pair_count = 0;
    size_t scalar_count = 0;
    for (const auto& instr : block->instructions()) {
      if (instr->opcode() == Instruction::kStorePair) {
        pair_count++;
      } else if (instr->isMove()) {
        scalar_count++;
      }
    }
    EXPECT_EQ(pair_count, store_count / 2);
    EXPECT_EQ(scalar_count, store_count % 2);
    EXPECT_EQ(block->getNumInstrs(), (store_count + 1) / 2);
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, IgnoresMultiplierWhenIndexIsAbsent) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* lower_store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{
          PhyLocation(X1), PhyLocation::REG_INVALID, 2, 0, DataType::kObject},
      PhyReg{X8, DataType::kObject});
  lower_store->output()->setDataType(DataType::kObject);
  auto* upper_store = appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X9,
      DataType::kObject);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 1);
  EXPECT_EQ(upper_store->opcode(), Instruction::kStorePair);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, KeepsDescendingAdjacentStoresUnchanged) {
  Function function;
  auto* block = function.allocateBasicBlock();
  appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X8,
      DataType::kObject);
  appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X9,
      DataType::kObject);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 2);
  EXPECT_TRUE(block->getFirstInstr()->isMove());
  EXPECT_TRUE(block->getLastInstr()->isMove());
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, FusesAlignedConstantFillIntoStorePair) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* materialize = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});
  appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      0,
      DataType::kObject,
      X8,
      DataType::k64bit);
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});
  auto* upper_store = appendPhysicalIndirectStore(
      block,
      Instruction::kMove,
      X1,
      8,
      DataType::kObject,
      X8,
      DataType::k64bit);

  runPostAllocRewrites(function);

  ASSERT_EQ(block->getNumInstrs(), 2);
  EXPECT_EQ(block->getFirstInstr(), materialize);
  EXPECT_EQ(upper_store->opcode(), Instruction::kStorePair);
  ASSERT_EQ(upper_store->getNumInputs(), 4);
  EXPECT_EQ(upper_store->getInput(0)->getConstant(), 0);
  EXPECT_EQ(upper_store->getInput(1)->getPhyRegister(), X1);
  EXPECT_EQ(upper_store->getInput(2)->getPhyRegister(), X8);
  EXPECT_EQ(upper_store->getInput(3)->getPhyRegister(), X8);
  EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
}

TEST_F(AArch64PeepholeTest, KeepsIneligibleStorePairsUnchanged) {
  struct Case {
    int32_t first_offset;
    int32_t second_offset;
    PhyLocation first_base;
    PhyLocation second_base;
    PhyLocation first_value;
    PhyLocation second_value;
    DataType first_type;
    DataType second_type;
  };
  constexpr Case kCases[] = {
      // Pair address is not 16-byte aligned.
      {8, 16, X1, X1, X8, X9, DataType::kObject, DataType::kObject},
      // Not consecutive pointer-width slots.
      {0, 16, X1, X1, X8, X9, DataType::kObject, DataType::kObject},
      // Different bases.
      {0, 8, X1, X2, X8, X9, DataType::kObject, DataType::kObject},
      // Base/value overlap is conservatively rejected.
      {0, 8, X1, X1, X1, X9, DataType::kObject, DataType::kObject},
      {0, 8, X1, X1, X8, X1, DataType::kObject, DataType::kObject},
      {0, 8, X1, X1, X1, X1, DataType::kObject, DataType::kObject},
      // SP is a legal address base but never a legal stored GP value.
      {0, 8, X1, X1, SP, X9, DataType::kObject, DataType::kObject},
      {0, 8, X1, X1, X8, SP, DataType::kObject, DataType::kObject},
      // Subword stores are not StorePair candidates.
      {0, 8, X1, X1, X8, X9, DataType::k32bit, DataType::k32bit},
      // FP stores stay on their existing scalar/vector lowering.
      {0, 8, X1, X1, D8, D9, DataType::kDouble, DataType::kDouble},
      // Below the signed scaled STP displacement.
      {-528, -520, X1, X1, X8, X9, DataType::kObject, DataType::kObject},
      // Outside the signed scaled STP displacement.
      {512, 520, X1, X1, X8, X9, DataType::kObject, DataType::kObject},
      // Extreme offsets must conservatively fall back without overflowing
      // while computing their distance.
      {std::numeric_limits<int32_t>::min(),
       std::numeric_limits<int32_t>::max(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject},
      {std::numeric_limits<int32_t>::max(),
       std::numeric_limits<int32_t>::min(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject},
      // The mathematical difference is not eight, even though a wrapping
      // int32_t subtraction would have produced eight.
      {std::numeric_limits<int32_t>::max() - 7,
       std::numeric_limits<int32_t>::min(),
       X1,
       X1,
       X8,
       X9,
       DataType::kObject,
       DataType::kObject},
  };

  for (const auto& test_case : kCases) {
    SCOPED_TRACE(fmt::format(
        "offsets=({}, {}) bases=({}, {}) values=({}, {}) types=({}, {})",
        test_case.first_offset,
        test_case.second_offset,
        test_case.first_base.loc,
        test_case.second_base.loc,
        test_case.first_value.loc,
        test_case.second_value.loc,
        test_case.first_type,
        test_case.second_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        test_case.first_base,
        test_case.first_offset,
        test_case.first_type,
        test_case.first_value,
        test_case.first_type);
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        test_case.second_base,
        test_case.second_offset,
        test_case.second_type,
        test_case.second_value,
        test_case.second_type);

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 2);
    EXPECT_TRUE(block->getFirstInstr()->isMove());
    EXPECT_TRUE(block->getLastInstr()->isMove());
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, KeepsIndexedAndMoveRelaxedStoresUnpaired) {
  {
    Function function;
    auto* block = function.allocateBasicBlock();
    auto* indexed_store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{PhyLocation(X1), PhyLocation(X10), 1, 0},
        PhyReg{X8, DataType::kObject});
    indexed_store->output()->setDataType(DataType::kObject);
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        X1,
        8,
        DataType::kObject,
        X9,
        DataType::kObject);

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 2);
  }

  {
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectStore(
        block,
        Instruction::kMoveRelaxed,
        X1,
        0,
        DataType::kObject,
        X8,
        DataType::kObject);
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        X1,
        8,
        DataType::kObject,
        X9,
        DataType::kObject);

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 2);
  }

  {
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        X1,
        0,
        DataType::kObject,
        X8,
        DataType::kObject);
    auto* indexed_store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{PhyLocation(X1), PhyLocation(X10), 1, 8, DataType::kObject},
        PhyReg{X9, DataType::kObject});
    indexed_store->output()->setDataType(DataType::kObject);

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 2);
  }

  {
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectStore(
        block,
        Instruction::kMove,
        X1,
        0,
        DataType::kObject,
        X8,
        DataType::kObject);
    appendPhysicalIndirectStore(
        block,
        Instruction::kMoveRelaxed,
        X1,
        8,
        DataType::kObject,
        X9,
        DataType::kObject);

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 2);
  }

  {
    Function function;
    auto* block = function.allocateBasicBlock();
    appendPhysicalIndirectStore(
        block,
        Instruction::kMoveRelaxed,
        X1,
        0,
        DataType::kObject,
        X8,
        DataType::kObject);
    appendPhysicalIndirectStore(
        block,
        Instruction::kMoveRelaxed,
        X1,
        8,
        DataType::kObject,
        X9,
        DataType::kObject);

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 2);
  }
}

TEST_F(AArch64PeepholeTest, KeepsMismatchedConstantFillUnchanged) {
  for (uint64_t second_value : {uint64_t{2}, uint64_t{3}}) {
    SCOPED_TRACE(fmt::format("second immediate {}", second_value));
    Function function;
    auto* block = function.allocateBasicBlock();
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, DataType::k64bit},
        Imm{1, DataType::k64bit});
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 0, DataType::kObject},
        PhyReg{X8, DataType::k64bit});
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, DataType::k64bit},
        Imm{second_value, DataType::k64bit});
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 8, DataType::kObject},
        PhyReg{X8, DataType::k64bit});

    runPostAllocRewrites(function);
    EXPECT_EQ(block->getNumInstrs(), 4);
  }
}

TEST_F(AArch64PeepholeTest, KeepsIneligibleConstantFillWindowsUnchanged) {
  enum class Rejection {
    kDifferentMaterializationType,
    kDifferentDestination,
    kFirstStoreDifferentSource,
    kSecondStoreDifferentSource,
    kFirstMaterializationRelaxed,
    kSecondMaterializationRelaxed,
    kFirstStoreRelaxed,
    kSecondStoreRelaxed,
    kInterruptedWindow,
  };
  constexpr Rejection kRejections[] = {
      Rejection::kDifferentMaterializationType,
      Rejection::kDifferentDestination,
      Rejection::kFirstStoreDifferentSource,
      Rejection::kSecondStoreDifferentSource,
      Rejection::kFirstMaterializationRelaxed,
      Rejection::kSecondMaterializationRelaxed,
      Rejection::kFirstStoreRelaxed,
      Rejection::kSecondStoreRelaxed,
      Rejection::kInterruptedWindow,
  };

  for (Rejection rejection : kRejections) {
    SCOPED_TRACE(fmt::format("rejection={}", static_cast<int>(rejection)));
    Function function;
    auto* block = function.allocateBasicBlock();
    auto first_materialization_opcode =
        rejection == Rejection::kFirstMaterializationRelaxed
        ? Instruction::kMoveRelaxed
        : Instruction::kMove;
    auto second_materialization_opcode =
        rejection == Rejection::kSecondMaterializationRelaxed
        ? Instruction::kMoveRelaxed
        : Instruction::kMove;
    auto first_store_opcode = rejection == Rejection::kFirstStoreRelaxed
        ? Instruction::kMoveRelaxed
        : Instruction::kMove;
    auto second_store_opcode = rejection == Rejection::kSecondStoreRelaxed
        ? Instruction::kMoveRelaxed
        : Instruction::kMove;
    auto second_materialization_type =
        rejection == Rejection::kDifferentMaterializationType
        ? DataType::kObject
        : DataType::k64bit;
    auto second_materialization_register =
        rejection == Rejection::kDifferentDestination ? X9 : X8;
    auto first_store_register =
        rejection == Rejection::kFirstStoreDifferentSource ? X9 : X8;
    auto second_store_register =
        rejection == Rejection::kSecondStoreDifferentSource
        ? X9
        : second_materialization_register;

    block->allocateInstr(
        first_materialization_opcode,
        nullptr,
        OutPhyReg{X8, DataType::k64bit},
        Imm{0x1122334455667788, DataType::k64bit});
    appendPhysicalIndirectStore(
        block,
        first_store_opcode,
        X1,
        0,
        DataType::kObject,
        first_store_register,
        DataType::k64bit);
    if (rejection == Rejection::kInterruptedWindow) {
      block->allocateInstr(
          Instruction::kMove,
          nullptr,
          OutPhyReg{X10, DataType::k64bit},
          Imm{7, DataType::k64bit});
    }
    block->allocateInstr(
        second_materialization_opcode,
        nullptr,
        OutPhyReg{second_materialization_register, second_materialization_type},
        Imm{0x1122334455667788, second_materialization_type});
    appendPhysicalIndirectStore(
        block,
        second_store_opcode,
        X1,
        8,
        DataType::kObject,
        second_store_register,
        second_materialization_type);

    const auto original_count = block->getNumInstrs();
    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), original_count);
    for (const auto& instr : block->instructions()) {
      EXPECT_NE(instr->opcode(), Instruction::kStorePair);
    }
    EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
  }
}

TEST_F(AArch64PeepholeTest, KeepsSubWordZeroStoreGuardFailuresUnchanged) {
  enum class Rejection {
    kNoLastUse,
    kDifferentSource,
    kNonAdjacent,
    kBaseReuse,
    kIndexReuse,
    kNonZero,
    kMaterializationRelaxed,
    kStoreRelaxed,
    k64BitMaterialization,
    kStoredOperandTooWide,
    kStoredOperandWrongSubWord,
  };
  constexpr Rejection kRejections[] = {
      Rejection::kNoLastUse,
      Rejection::kDifferentSource,
      Rejection::kNonAdjacent,
      Rejection::kBaseReuse,
      Rejection::kIndexReuse,
      Rejection::kNonZero,
      Rejection::kMaterializationRelaxed,
      Rejection::kStoreRelaxed,
      Rejection::k64BitMaterialization,
      Rejection::kStoredOperandTooWide,
      Rejection::kStoredOperandWrongSubWord,
  };

  for (DataType store_type : {DataType::k8bit, DataType::k16bit}) {
    for (Rejection rejection : kRejections) {
      SCOPED_TRACE(fmt::format(
          "store_type={} rejection={}",
          store_type,
          static_cast<int>(rejection)));
      Function function;
      auto* block = function.allocateBasicBlock();
      auto materialization_type = rejection == Rejection::k64BitMaterialization
          ? DataType::k64bit
          : DataType::k32bit;
      auto materialization_opcode =
          rejection == Rejection::kMaterializationRelaxed
          ? Instruction::kMoveRelaxed
          : Instruction::kMove;
      auto store_opcode = rejection == Rejection::kStoreRelaxed
          ? Instruction::kMoveRelaxed
          : Instruction::kMove;
      auto value = rejection == Rejection::kNonZero ? uint64_t{1} : uint64_t{0};
      auto source_register = rejection == Rejection::kDifferentSource ? X9 : X8;
      auto stored_value_type = rejection == Rejection::kStoredOperandTooWide
          ? DataType::k32bit
          : (rejection == Rejection::kStoredOperandWrongSubWord
                 ? (store_type == DataType::k8bit ? DataType::k16bit
                                                  : DataType::k8bit)
                 : store_type);

      block->allocateInstr(
          materialization_opcode,
          nullptr,
          OutPhyReg{X8, materialization_type},
          Imm{value, materialization_type});
      if (rejection == Rejection::kNonAdjacent) {
        block->allocateInstr(
            Instruction::kMove,
            nullptr,
            OutPhyReg{X10, DataType::k64bit},
            Imm{7, DataType::k64bit});
      }

      Instruction* store = nullptr;
      if (rejection == Rejection::kIndexReuse) {
        store = block->allocateInstr(
            store_opcode,
            nullptr,
            OutInd{PhyLocation(X1), PhyLocation(X8), 1, 24, store_type},
            PhyReg{source_register, stored_value_type});
      } else {
        auto base_register = rejection == Rejection::kBaseReuse ? X8 : X1;
        store = block->allocateInstr(
            store_opcode,
            nullptr,
            OutInd{base_register, 24, store_type},
            PhyReg{source_register, stored_value_type});
      }
      store->output()->setDataType(store_type);
      if (rejection != Rejection::kNoLastUse) {
        store->getInput(0)->setLastUse();
      }

      const auto original_count = block->getNumInstrs();
      runPostAllocRewrites(function);

      EXPECT_EQ(block->getNumInstrs(), original_count);
      ASSERT_TRUE(store->getInput(0)->isReg());
      EXPECT_EQ(store->getInput(0)->getPhyRegister(), source_register);
      EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
    }
  }
}

TEST_F(AArch64PeepholeTest, KeepsZeroMaterializationWithoutLastUseProof) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{0, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X1, 24, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
}

TEST_F(AArch64PeepholeTest, KeepsZeroMaterializationUsedByStoreAddress) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{0, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X8, 24, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);
  store->getInput(0)->setLastUse();

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
}

TEST_F(AArch64PeepholeTest, KeepsNonZeroMaterializationStore) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{1, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X1, 24, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);
  store->getInput(0)->setLastUse();

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
}

TEST_F(AArch64PeepholeTest, KeepsNonZeroImmediateStore) {
  Function function;
  auto* store = appendIndirectStore(
      function, Instruction::kMove, DataType::k64bit, 1, DataType::k64bit);

  runPostAllocRewrites(function);

  auto* input = store->getInput(0);
  ASSERT_TRUE(input->isImm());
  EXPECT_EQ(input->getConstant(), 1);
  EXPECT_EQ(input->dataType(), DataType::k64bit);
}

TEST_F(AArch64PeepholeTest, KeepsFloatingPointZeroStore) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X1, 24, DataType::kDouble},
      FPImm{0.0});
  store->output()->setDataType(DataType::kDouble);

  runPostAllocRewrites(function);

  ASSERT_TRUE(store->getInput(0)->isImm());
  EXPECT_TRUE(store->getInput(0)->isFp());
  EXPECT_EQ(store->getInput(0)->getFPConstant(), 0.0);
}

TEST_F(
    AArch64PeepholeTest,
    KeepsFloatingPointPositiveAndNegativeZeroMaterializationStores) {
  for (double value : {0.0, -0.0}) {
    SCOPED_TRACE(
        fmt::format("value={} signbit={}", value, std::signbit(value)));
    Function function;
    auto* block = function.allocateBasicBlock();
    auto* materialize = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{D8, DataType::kDouble},
        FPImm{value});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 24, DataType::kDouble},
        PhyReg{D8, DataType::kDouble});
    store->output()->setDataType(DataType::kDouble);
    store->getInput(0)->setLastUse();

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 2);
    ASSERT_TRUE(materialize->getInput(0)->isFp());
    EXPECT_EQ(
        std::signbit(materialize->getInput(0)->getFPConstant()),
        std::signbit(value));
    ASSERT_TRUE(store->getInput(0)->isReg());
    EXPECT_TRUE(store->getInput(0)->isFp());
    EXPECT_EQ(store->getInput(0)->getPhyRegister(), D8);
  }
}

TEST_F(AArch64PeepholeTest, KeepsAbsoluteAddressZeroStore) {
  Function function;
  auto* block = function.allocateBasicBlock();
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutMemImm{reinterpret_cast<void*>(0x1000), DataType::k64bit},
      Imm{0, DataType::k64bit});

  runPostAllocRewrites(function);

  EXPECT_TRUE(store->output()->isMem());
  ASSERT_TRUE(store->getInput(0)->isImm());
  EXPECT_EQ(store->getInput(0)->getConstant(), 0);
}

TEST_F(AArch64PeepholeTest, KeepsMoveRelaxedZeroStore) {
  Function function;
  auto* store = appendIndirectStore(
      function,
      Instruction::kMoveRelaxed,
      DataType::k64bit,
      0,
      DataType::k64bit);

  runPostAllocRewrites(function);

  EXPECT_TRUE(store->isMoveRelaxed());
  ASSERT_TRUE(store->getInput(0)->isImm());
  EXPECT_EQ(store->getInput(0)->getConstant(), 0);
}

TEST_F(AArch64PeepholeTest, KeepsZeroMaterializationUsedByStoreIndex) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{0, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{PhyLocation(X1), PhyLocation(X8), 1, 24},
      PhyReg{X8, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);
  store->getInput(0)->setLastUse();

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  ASSERT_TRUE(store->getInput(0)->isReg());
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
}

TEST_F(AArch64PeepholeTest, KeepsNonAdjacentZeroMaterialization) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{0, DataType::k64bit});
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X9, DataType::k64bit},
      Imm{7, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X1, 24, DataType::k64bit},
      PhyReg{X8, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);
  store->getInput(0)->setLastUse();

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 3);
  ASSERT_TRUE(store->getInput(0)->isReg());
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
}

TEST_F(AArch64PeepholeTest, KeepsMismatchedMaterializationAndStoreWidths) {
  constexpr std::pair<DataType, DataType> kMismatchedWidths[] = {
      {DataType::k64bit, DataType::k32bit},
      {DataType::k32bit, DataType::k64bit},
      {DataType::k64bit, DataType::k16bit},
  };

  for (auto [materialization_type, store_type] : kMismatchedWidths) {
    SCOPED_TRACE(fmt::format(
        "materialization={} store={}", materialization_type, store_type));
    Function function;
    auto* block = function.allocateBasicBlock();
    block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{X8, materialization_type},
        Imm{0, materialization_type});
    auto* store = block->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutInd{X1, 24, store_type},
        PhyReg{X8, store_type});
    store->output()->setDataType(store_type);
    store->getInput(0)->setLastUse();

    runPostAllocRewrites(function);

    EXPECT_EQ(block->getNumInstrs(), 2);
    ASSERT_TRUE(store->getInput(0)->isReg());
    EXPECT_EQ(store->getInput(0)->getPhyRegister(), X8);
  }
}

TEST_F(AArch64PeepholeTest, KeepsStoreFromDifferentPhysicalRegister) {
  Function function;
  auto* block = function.allocateBasicBlock();
  block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{X8, DataType::k64bit},
      Imm{0, DataType::k64bit});
  auto* store = block->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{X1, 24, DataType::k64bit},
      PhyReg{X9, DataType::k64bit});
  store->output()->setDataType(DataType::k64bit);
  store->getInput(0)->setLastUse();

  runPostAllocRewrites(function);

  EXPECT_EQ(block->getNumInstrs(), 2);
  ASSERT_TRUE(store->getInput(0)->isReg());
  EXPECT_EQ(store->getInput(0)->getPhyRegister(), X9);
}

TEST_F(AArch64PeepholeTest, CoreIntegerGuardCartesianProduct) {
  constexpr DataType kRegisterTypes[] = {
      DataType::k32bit,
      DataType::k64bit,
      DataType::kObject,
  };
  enum class AddressShape {
    kIndependent,
    kMaterializedRegisterAsBase,
    kMaterializedRegisterAsIndex,
  };
  constexpr AddressShape kAddressShapes[] = {
      AddressShape::kIndependent,
      AddressShape::kMaterializedRegisterAsBase,
      AddressShape::kMaterializedRegisterAsIndex,
  };

  size_t case_count = 0;
  size_t folded_count = 0;
  for (DataType materialization_type : kRegisterTypes) {
    for (DataType store_type : kRegisterTypes) {
      for (bool last_use : {false, true}) {
        for (bool same_register : {false, true}) {
          for (bool adjacent : {false, true}) {
            for (uint64_t value : {uint64_t{0}, uint64_t{1}}) {
              for (AddressShape address_shape : kAddressShapes) {
                case_count++;
                SCOPED_TRACE(fmt::format(
                    "materialization={} store={} last_use={} same_register={} "
                    "adjacent={} value={} address_shape={}",
                    materialization_type,
                    store_type,
                    last_use,
                    same_register,
                    adjacent,
                    value,
                    static_cast<int>(address_shape)));

                Function function;
                auto* block = function.allocateBasicBlock();
                block->allocateInstr(
                    Instruction::kMove,
                    nullptr,
                    OutPhyReg{X8, materialization_type},
                    Imm{value, materialization_type});
                if (!adjacent) {
                  block->allocateInstr(
                      Instruction::kMove,
                      nullptr,
                      OutPhyReg{X10, DataType::k64bit},
                      Imm{7, DataType::k64bit});
                }

                auto source_register = same_register ? X8 : X9;
                Instruction* store = nullptr;
                switch (address_shape) {
                  case AddressShape::kIndependent:
                    store = block->allocateInstr(
                        Instruction::kMove,
                        nullptr,
                        OutInd{X1, 24, store_type},
                        PhyReg{source_register, store_type});
                    break;
                  case AddressShape::kMaterializedRegisterAsBase:
                    store = block->allocateInstr(
                        Instruction::kMove,
                        nullptr,
                        OutInd{X8, 24, store_type},
                        PhyReg{source_register, store_type});
                    break;
                  case AddressShape::kMaterializedRegisterAsIndex:
                    store = block->allocateInstr(
                        Instruction::kMove,
                        nullptr,
                        OutInd{PhyLocation(X1), PhyLocation(X8), 1, 24},
                        PhyReg{source_register, store_type});
                    break;
                }
                store->output()->setDataType(store_type);
                if (last_use) {
                  store->getInput(0)->setLastUse();
                }

                runPostAllocRewrites(function);

                const bool should_fold = value == 0 && adjacent && last_use &&
                    same_register &&
                    address_shape == AddressShape::kIndependent &&
                    bitSize(materialization_type) == bitSize(store_type);
                if (should_fold) {
                  folded_count++;
                  EXPECT_EQ(block->getNumInstrs(), 1);
                  ASSERT_TRUE(store->getInput(0)->isImm());
                  EXPECT_EQ(store->getInput(0)->getConstant(), 0);
                  EXPECT_EQ(store->getInput(0)->dataType(), store_type);
                } else {
                  EXPECT_EQ(block->getNumInstrs(), adjacent ? 2 : 3);
                  ASSERT_TRUE(store->getInput(0)->isReg());
                  EXPECT_EQ(
                      store->getInput(0)->getPhyRegister(), source_register);
                }
                EXPECT_TRUE(verifyPostRegAllocInvariants(&function, std::cout));
              }
            }
          }
        }
      }
    }
  }

  EXPECT_EQ(case_count, 432);
  // k32->k32 plus the four bit-compatible combinations of k64/kObject.
  EXPECT_EQ(folded_count, 5);
}

#endif // CINDER_AARCH64

} // namespace jit::lir
