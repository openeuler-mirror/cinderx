// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/lir/block_builder.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace jit;
using namespace jit::lir;

class CodeLayoutTest : public RuntimeTest {};

TEST_F(CodeLayoutTest, ColdSinkPreservesStableOrderAndControlFlow) {
  Function func;
  auto* hot1 = func.allocateBasicBlock();
  auto* cold1 = func.allocateBasicBlock();
  auto* hot2 = func.allocateBasicBlock();
  auto* cold2 = func.allocateBasicBlock();
  cold1->setSection(codegen::CodeSection::kCold);
  cold2->setSection(codegen::CodeSection::kCold);
  hot1->addSuccessor(cold1);
  cold1->addSuccessor(hot2);
  hot2->addSuccessor(cold2);

  func.sinkColdBlocks();

  EXPECT_EQ(
      func.basicblocks(), (std::vector<BasicBlock*>{hot1, hot2, cold1, cold2}));
  EXPECT_EQ(hot1->successors(), (std::vector<BasicBlock*>{cold1}));
  EXPECT_EQ(cold1->predecessors(), (std::vector<BasicBlock*>{hot1}));
  EXPECT_EQ(cold1->successors(), (std::vector<BasicBlock*>{hot2}));
  EXPECT_EQ(hot2->predecessors(), (std::vector<BasicBlock*>{cold1}));
  EXPECT_EQ(hot2->successors(), (std::vector<BasicBlock*>{cold2}));
  EXPECT_EQ(cold2->predecessors(), (std::vector<BasicBlock*>{hot2}));
  func.sinkColdBlocks();
  EXPECT_EQ(
      func.basicblocks(), (std::vector<BasicBlock*>{hot1, hot2, cold1, cold2}));
}

TEST_F(CodeLayoutTest, PostAllocationAddsBranchesForDisplacedSuccessors) {
  for (size_t edges : {1, 2}) {
    SCOPED_TRACE(edges);
    Function func;
    auto* source = func.allocateBasicBlock();
    auto* cold = func.allocateBasicBlock();
    auto* hot = func.allocateBasicBlock();
    cold->setSection(codegen::CodeSection::kCold);
    for (size_t i = 0; i < edges; ++i) {
      source->addSuccessor(cold);
    }
    func.sinkColdBlocks();
    ASSERT_EQ(func.basicblocks()[1], hot);

    codegen::Environ env;
    PostRegAllocRewrite{&func, &env}.run();
    ASSERT_NE(source->getLastInstr(), nullptr);
    EXPECT_EQ(source->getLastInstr()->opcode(), Instruction::kBranch);
    EXPECT_EQ(source->getLastInstr()->getInput(0)->getBasicBlock(), cold);
    EXPECT_EQ(source->successors().size(), edges);
  }
}

TEST_F(CodeLayoutTest, PostAllocationKeepsAdjacentSameSectionFallthrough) {
  Function func;
  auto* source = func.allocateBasicBlock();
  auto* target = func.allocateBasicBlock();
  source->addSuccessor(target);
  codegen::Environ env;
  PostRegAllocRewrite{&func, &env}.run();
  EXPECT_TRUE(source->instructions().empty());
}

TEST_F(CodeLayoutTest, PostAllocationBranchesAcrossAdjacentCodeSections) {
  Function func;
  auto* source = func.allocateBasicBlock();
  auto* target = func.allocateBasicBlock();
  target->setSection(codegen::CodeSection::kCold);
  source->addSuccessor(target);
  codegen::Environ env;
  PostRegAllocRewrite{&func, &env}.run();
  ASSERT_NE(source->getLastInstr(), nullptr);
  EXPECT_EQ(source->getLastInstr()->opcode(), Instruction::kBranch);
  EXPECT_EQ(source->getLastInstr()->getInput(0)->getBasicBlock(), target);
}

TEST_F(CodeLayoutTest, AppendingKnownSuccessorDoesNotDuplicateItsEdge) {
  Function func;
  codegen::Environ env;
  BasicBlockBuilder builder{&env, &func};
  auto* source = builder.allocateBlock();
  auto* target = builder.allocateBlock();
  source->addSuccessor(target);
  builder.switchBlock(source);
  builder.appendBlock(target);
  EXPECT_EQ(source->successors(), (std::vector<BasicBlock*>{target}));
  EXPECT_EQ(target->predecessors(), (std::vector<BasicBlock*>{source}));
}

#if defined(CINDER_AARCH64)
TEST_F(CodeLayoutTest, NearDeoptPlacementHonorsSizeAndSectionBoundaries) {
  if (!g_spike_deoptsink) {
    GTEST_SKIP() << "SPIKE_DEOPTSINK is disabled";
  }
  const bool old_sections = getConfig().multiple_code_sections;
  SCOPE_EXIT(getMutableConfig().multiple_code_sections = old_sections);
  struct Case {
    size_t instructions;
    bool sections;
    bool deferred;
  };
  for (const Case& item : {
           Case{10, false, true},
           Case{8192, false, true},
           Case{8193, false, false},
           Case{10, true, false},
       }) {
    SCOPED_TRACE(item.instructions);
    SCOPED_TRACE(item.sections);
    getMutableConfig().multiple_code_sections = item.sections;
    auto allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
    asmjit::CodeHolder code;
    ASSERT_EQ(code.init(allocator->asmJitEnvironment()), asmjit::kErrorOk);
    codegen::arch::Builder as{&code};
    codegen::Environ env;
    env.as = &as;
    CodeRuntime runtime{nullptr, nullptr, nullptr};
    runtime.addDeoptMetadata(DeoptMetadata{});
    env.code_rt = &runtime;

    Function func;
    auto* first = func.allocateBasicBlock();
    auto* second = func.allocateBasicBlock();
    first->addSuccessor(second);
    first->allocateInstr(
        Instruction::kGuard,
        nullptr,
        Imm{kNotZero},
        Imm{0},
        PhyReg{0, DataType::k64bit},
        Imm{0});
    for (size_t i = 0; i < item.instructions - 2; ++i) {
      second->allocateInstr(
          Instruction::kMove, nullptr, OutPhyReg{1, DataType::k64bit}, Imm{1});
    }
    auto done = as.newLabel();
    second->allocateInstr(Instruction::kBranch, nullptr, AsmLbl{done});
    ASSERT_EQ(func.getNumInstrs(), item.instructions);
    codegen::emitLIRBlocks(&env, &func);
    ASSERT_TRUE(env.aarch64_near_deopt_branches.empty());
    ASSERT_EQ(env.deopt_exits.size(), 1);
    as.bind(done);
    as.mov(asmjit::a64::x0, 7);
    as.ret(codegen::arch::lr);
    as.bind(env.deopt_exits[0].label);
    as.mov(asmjit::a64::x0, 0);
    as.ret(codegen::arch::lr);
    ASSERT_EQ(as.finalize(), asmjit::kErrorOk);
    ASSERT_EQ(code.flatten(), asmjit::kErrorOk);
    ASSERT_EQ(code.resolveUnresolvedLinks(), asmjit::kErrorOk);

    const uint8_t* bytes = code.textSection()->data();
    uint32_t guard;
    std::memcpy(&guard, bytes, sizeof(guard));
    ASSERT_EQ(guard & 0xff00001fU, 0xb4000000U); // cbz x0
    int32_t displacement = (guard >> 5) & 0x7ffff;
    if (displacement & 0x40000) {
      displacement -= 0x80000;
    }
    const size_t near_offset = static_cast<size_t>(displacement * 4);
    const size_t second_offset =
        code.labelOffsetFromBase(env.block_label_map.at(second));
    EXPECT_EQ(near_offset > second_offset, item.deferred);
    uint32_t stub;
    std::memcpy(&stub, bytes + near_offset, sizeof(stub));
    ASSERT_EQ(stub & 0xfc000000U, 0x14000000U); // b deopt
    int32_t stub_displacement = stub & 0x03ffffff;
    if (stub_displacement & 0x02000000) {
      stub_displacement -= 0x04000000;
    }
    EXPECT_EQ(
        static_cast<int64_t>(near_offset) + stub_displacement * 4LL,
        code.labelOffsetFromBase(env.deopt_exits[0].label));
    auto compiled = allocator->addCode(&code);
    ASSERT_EQ(compiled.error, asmjit::kErrorOk);
    auto entry = reinterpret_cast<uint64_t (*)(uint64_t)>(compiled.addr);
    EXPECT_EQ(entry(1), 7);
    EXPECT_EQ(entry(0), 0);
  }
}
#endif
