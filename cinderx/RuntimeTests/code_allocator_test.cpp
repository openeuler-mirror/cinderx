// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/python.h"

#include <gtest/gtest.h>

#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/config.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

using namespace jit;
using namespace jit::codegen;

namespace {

class CodeAllocatorTest : public ::testing::Test {
 public:
  void SetUp() override {
    saved_config_ = getConfig();
    getMutableConfig().multiple_code_sections = true;
    getMutableConfig().use_huge_pages = true;
    code_allocator_ = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  }

  void TearDown() override {
    code_allocator_.reset();
    getMutableConfig() = saved_config_;
  }

  Config saved_config_;
  std::unique_ptr<ICodeAllocator> code_allocator_;
};

const uint8_t* addressAtOffset(const void* base, uintptr_t offset) {
  return reinterpret_cast<const uint8_t*>(
      reinterpret_cast<uintptr_t>(base) + offset);
}

TEST_F(CodeAllocatorTest, AddSplitCodeCopiesHotAndColdSections) {
  asmjit::CodeHolder code;
  code.init(code_allocator_->asmJitEnvironment());

  asmjit::Section* coldtext = nullptr;
  ASSERT_EQ(
      code.newSection(
          &coldtext,
          codeSectionName(CodeSection::kCold),
          SIZE_MAX,
          code.textSection()->flags(),
          code.textSection()->alignment()),
      asmjit::kErrorOk);

  arch::Builder as(&code);

  constexpr std::array<uint8_t, 3> kHotText{{0x11, 0x12, 0x13}};
  constexpr std::array<uint8_t, 5> kColdText{{0x31, 0x32, 0x33, 0x34, 0x35}};

  ASSERT_EQ(as.section(code.textSection()), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kHotText.data(), kHotText.size()), asmjit::kErrorOk);

  ASSERT_EQ(as.section(coldtext), asmjit::kErrorOk);
  ASSERT_EQ(as.embed(kColdText.data(), kColdText.size()), asmjit::kErrorOk);

  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = code_allocator_->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);
  ASSERT_NE(result.addr, kHotText.data());
  ASSERT_NE(result.addr, kColdText.data());

  ASSERT_EQ(code.textSection()->offset(), 0);

  EXPECT_EQ(
      std::memcmp(
          addressAtOffset(result.addr, code.textSection()->offset()),
          kHotText.data(),
          kHotText.size()),
      0);
  EXPECT_EQ(
      std::memcmp(
          addressAtOffset(result.addr, coldtext->offset()),
          kColdText.data(),
          kColdText.size()),
      0);
}

TEST_F(CodeAllocatorTest, ReclaimingAllocatorReturnsUsedBytesToBaseline) {
  getMutableConfig().use_huge_pages = false;
  getMutableConfig().multiple_code_sections = false;
  auto allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());
  ASSERT_EQ(allocator->usedBytes(), 0u);

  asmjit::CodeHolder code;
  code.init(allocator->asmJitEnvironment());
  arch::Builder as(&code);
  constexpr std::array<uint8_t, 8> kText{
      {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);

  AllocateResult result = allocator->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_NE(result.addr, nullptr);
  EXPECT_GE(allocator->usedBytes(), kText.size());
  EXPECT_TRUE(allocator->contains(result.addr));

  // Symmetric accounting is the plateau contract: what allocation added,
  // release subtracts, so identical compile/retire churn nets to zero.
  ASSERT_EQ(allocator->releaseCode(result.addr), asmjit::kErrorOk);
  EXPECT_EQ(allocator->usedBytes(), 0u)
      << "release did not subtract what allocation added";
  EXPECT_FALSE(allocator->contains(result.addr));
}

TEST_F(CodeAllocatorTest, ReclaimingAllocatorFailsClosedOnUnknownRelease) {
  getMutableConfig().use_huge_pages = false;
  getMutableConfig().multiple_code_sections = false;
  auto allocator = std::unique_ptr<ICodeAllocator>(CodeAllocator::make());

  int local = 0;
  EXPECT_NE(allocator->releaseCode(&local), asmjit::kErrorOk)
      << "an address the allocator never issued was released as a success";

  asmjit::CodeHolder code;
  code.init(allocator->asmJitEnvironment());
  arch::Builder as(&code);
  constexpr std::array<uint8_t, 4> kText{{0x11, 0x22, 0x33, 0x44}};
  ASSERT_EQ(as.embed(kText.data(), kText.size()), asmjit::kErrorOk);
  ASSERT_EQ(as.finalize(), asmjit::kErrorOk);
  AllocateResult result = allocator->addCode(&code);
  ASSERT_EQ(result.error, asmjit::kErrorOk);
  ASSERT_EQ(allocator->releaseCode(result.addr), asmjit::kErrorOk);
  EXPECT_NE(allocator->releaseCode(result.addr), asmjit::kErrorOk)
      << "double release reported success";
}

#if PY_VERSION_HEX < 0x030C0000
TEST(CodeAllocatorLayoutTest, AlignsAArch64ColdSectionOffset) {
  SplitAllocationLayout layout = computeSplitAllocationLayout(4, 8);

  EXPECT_GE(layout.hot_capacity, 4);
  EXPECT_LE(layout.hot_capacity + 8, layout.chunk_size);
  EXPECT_EQ(layout.hot_capacity, 1024 * 1024);
  EXPECT_EQ(layout.hot_capacity % 4, 0);
}
#endif

} // namespace
