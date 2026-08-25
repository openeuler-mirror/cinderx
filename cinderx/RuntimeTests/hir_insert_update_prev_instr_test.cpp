// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <unordered_set>

using namespace jit::hir;

class InsertUpdatePrevInstrTest : public RuntimeTest {};

namespace {

int countIf(const Function& func, auto pred) {
  int count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (pred(instr)) {
        count++;
      }
    }
  }
  return count;
}

std::unordered_set<int> publishedOffsets(const Function& func) {
  std::unordered_set<int> offsets;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.IsUpdatePrevInstr()) {
        offsets.insert(instr.bytecodeOffset().asIndex().value());
      }
    }
  }
  return offsets;
}

int firstPublishedOffset(
    BorrowedRef<PyCodeObject> code,
    int opcode,
    bool after_caches) {
  for (const auto& instr : jit::BytecodeInstructionBlock{code}) {
    if (instr.opcode() != opcode) {
      continue;
    }
    return after_caches ? instr.nextInstrOffset().asIndex().value() - 1
                        : instr.opcodeIndex().value();
  }
  return -1;
}

std::unique_ptr<Function> compileAndRunPass(
    RuntimeTest* test,
    const char* src) {
  std::unique_ptr<Function> irfunc;
  test->CompileToHIR(src, "test", irfunc);
  if (irfunc == nullptr) {
    return nullptr;
  }
  SSAify{}.Run(*irfunc);
  Simplify{}.Run(*irfunc);
  PhiElimination{}.Run(*irfunc);
  InsertUpdatePrevInstr{}.Run(*irfunc);
  return irfunc;
}

} // namespace

TEST_F(InsertUpdatePrevInstrTest, RedundantStoresEliminated) {
  // Four len() calls on consecutive lines produce four arbitrary-execution
  // points on different source lines. The additions produce more. Without
  // dead store elimination, each line change emits its own UpdatePrevInstr.
  // With the optimization, consecutive UpdatePrevInstr stores separated only
  // by non-arbitrary-execution instructions are collapsed.
  const char* src = R"(
def test(a):
  w = len(a)
  x = len(a)
  y = len(a)
  z = len(a)
  return w + x + y + z
)";
  auto irfunc = compileAndRunPass(this, src);
  ASSERT_NE(irfunc, nullptr);

  int update_count =
      countIf(*irfunc, [](const Instr& i) { return i.IsUpdatePrevInstr(); });
  int arbitrary_count = countIf(*irfunc, hasArbitraryExecution);

  // There must be at least one UpdatePrevInstr.
  ASSERT_GT(update_count, 0);
  // There must be multiple arbitrary-execution points (len calls + additions).
  ASSERT_GE(arbitrary_count, 7);
  // The optimization must eliminate at least one redundant store: each
  // consecutive pair of arbitrary-execution instructions on different lines
  // with nothing observable between them has its first store removed.
  EXPECT_LT(update_count, arbitrary_count);
}

TEST_F(InsertUpdatePrevInstrTest, SingleCallPreservesStore) {
  const char* src = R"(
def test(a):
  return len(a)
)";
  auto irfunc = compileAndRunPass(this, src);
  ASSERT_NE(irfunc, nullptr);

  int update_count =
      countIf(*irfunc, [](const Instr& i) { return i.IsUpdatePrevInstr(); });
  EXPECT_GT(update_count, 0);
}

#if PY_VERSION_HEX < 0x030C0000
TEST_F(
    InsertUpdatePrevInstrTest,
    PythonVisibleBoundariesPublishPrecisePositions) {
  struct Case {
    const char* source;
    int opcode;
    bool after_caches;
  };
  const Case cases[] = {
      {R"(
def test(function):
  return function()
)",
       CALL,
       true},
      {R"(
def test(value):
  return value.member
)",
       LOAD_ATTR,
       false},
      {R"(
def test(value):
  return value + 1
)",
       BINARY_OP,
       false},
  };

  for (const Case& test_case : cases) {
    auto irfunc = compileAndRunPass(this, test_case.source);
    ASSERT_NE(irfunc, nullptr);
    int expected = firstPublishedOffset(
        irfunc->code, test_case.opcode, test_case.after_caches);
    ASSERT_GE(expected, 0);
    auto offsets = publishedOffsets(*irfunc);
    EXPECT_TRUE(offsets.contains(expected))
        << "missing Stock-compatible publication for opcode "
        << test_case.opcode << " at code-unit " << expected;
  }
}
#endif
