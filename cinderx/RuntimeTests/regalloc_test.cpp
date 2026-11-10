// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/regalloc.h"

#include <fmt/ostream.h>

#include <algorithm>
#include <sstream>
#include <vector>

using namespace jit;

namespace jit::lir {
class LinearScanAllocatorTest : public ::testing::Test {
 public:
  static bool LiveIntervalPtrLess(
      const LiveInterval* lhs,
      const LiveInterval* rhs) {
    if (lhs->operand == rhs->operand) {
      return lhs->startLocation() < rhs->startLocation();
    }
    return lhs->operand < rhs->operand;
  }

  UnorderedMap<const Operand*, int> buildOperandToIndexMap(
      const UnorderedMap<int, Instruction*>& map) {
    UnorderedMap<const Operand*, int> res;
    for (auto& m : map) {
      res.emplace(m.second->output(), m.first);
    }
    return res;
  }

  template <typename T>
  UnorderedMap<int, T> buildIndexMap(
      const UnorderedMap<const Operand*, T>& opnd_interval,
      const UnorderedMap<const Operand*, int>& opnd_index) {
    UnorderedMap<int, T> res;
    for (auto& oi : opnd_interval) {
      auto iter = opnd_index.find(oi.first);

      if (iter == opnd_index.end()) {
        // cannot find an index for an operand, clear the result
        // and make the caller assert.
        res.clear();
        break;
      }

      res.emplace(iter->second, oi.second);
    }

    return res;
  }

  void runAllocator(Function* func) {
    LinearScanAllocator allocator{func};
    allocator.run();
  }
};

TEST_F(LinearScanAllocatorTest, IntervalIntersectWithRange) {
  Operand operand;

  LiveInterval i1{&operand};
  i1.addRange(LiveRange{10, 30});
  i1.addRange(LiveRange{40, 60});

  EXPECT_EQ(i1.intersectWith(LiveRange{0, 10}), INVALID_LOCATION);
  EXPECT_EQ(i1.intersectWith(LiveRange{10, 11}), 10);
  EXPECT_EQ(i1.intersectWith(LiveRange{20, 50}), 20);
  EXPECT_EQ(i1.intersectWith(LiveRange{35, 45}), 40);
  EXPECT_EQ(i1.intersectWith(LiveRange{40, 50}), 40);
  EXPECT_EQ(i1.intersectWith(LiveRange{50, 70}), 50);
  EXPECT_EQ(i1.intersectWith(LiveRange{60, 65}), INVALID_LOCATION);
}

TEST_F(LinearScanAllocatorTest, IntervalIntersectWithInterval) {
  Operand operand1;
  Operand operand2;

  LiveInterval i1{&operand1};
  LiveInterval i2{&operand2};

  EXPECT_EQ(i1.intersectWith(i2), INVALID_LOCATION);
  EXPECT_EQ(i2.intersectWith(i1), INVALID_LOCATION);

  i1.addRange(LiveRange{20, 50});
  i2.addRange(LiveRange{10, 30});
  i2.addRange(LiveRange{40, 60});
  EXPECT_EQ(i1.intersectWith(i2), 20);
  EXPECT_EQ(i2.intersectWith(i1), 20);

  // Make sure the results are unchanged if i1 has more ranges than i2.
  i1.addRange(LiveRange{100, 200});
  i1.addRange(LiveRange{300, 400});
  EXPECT_EQ(i1.intersectWith(i2), 20);
  EXPECT_EQ(i2.intersectWith(i1), 20);
}

TEST_F(LinearScanAllocatorTest, RegAllocationNoSpill) {
  const char* lir_source = R"(
Function:
BB %0 - succs: %2
      %1 = Move 0(0x0)
           Branch BB%2

BB %2 - succs: %5 %8
      %3 = Add %1, 8(0x8)
           CondBranch %3, BB%5, BB%8

BB %5 - succs: %11
      %6 = Add %1, 8(0x8)
           Branch BB%11

BB %8 - succs: %11
      %9 = Add %1, 16(0x10)
           Branch BB%11

BB %11 - succs: %14
     %12 = Phi (BB%5, %6), (BB%8, %9)
           Return %12

BB %14

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);
  auto opnd_id_map = buildOperandToIndexMap(parser.getOutputInstrMap());

  LinearScanAllocator lsallocator(lir_func.get());
  lsallocator.calculateLiveIntervals();
  auto id_interval = buildIndexMap(lsallocator.intervalMap(), opnd_id_map);
  ASSERT_FALSE(id_interval.empty());

  std::vector<int> vregs;
  vregs.reserve(id_interval.size());
  for (auto& ii : id_interval) {
    vregs.push_back(ii.first);
  }

  std::sort(vregs.begin(), vregs.end());

  std::stringstream ss_ranges;
  for (auto& vreg : vregs) {
    ss_ranges << vreg << ": " << id_interval.at(vreg) << "\n";
  }

  std::string live_expected = R"(1: [2, 12), [15, 17)
3: [7, 9)
6: [12, 15)
9: [17, 20)
12: [20, 24)
)";

  ASSERT_EQ(ss_ranges.str(), live_expected);

  auto index_uses_map = buildIndexMap(lsallocator.vreg_phy_uses_, opnd_id_map);
  std::stringstream ss_uses;
  for (auto& vreg : vregs) {
    auto sep = "";
    for (auto& s : index_uses_map[vreg]) {
      fmt::print(ss_uses, "{}{}", sep, s);
      sep = " ";
    }
    ss_uses << "\n";
  }

  std::string uses_expected = R"(2 6 11 16
7 8
12
17

)";
  ASSERT_EQ(ss_uses.str(), uses_expected);

  lsallocator.linearScan();

  std::stringstream allocated;
  for (auto& interval : lsallocator.intervalList()) {
    fmt::print(
        allocated,
        "{}->{}\n",
        opnd_id_map.at(interval->operand),
        interval->allocated_loc.loc);
  }

  std::string allocated_expected = R"(1->0
3->1
6->0
9->0
12->0
)";

  ASSERT_EQ(allocated.str(), allocated_expected);
}

TEST_F(LinearScanAllocatorTest, RegAllocation) {
  const char* lir_source = R"(Function:
BB %0 - succs: %5 %8
  %1 = Move 0(0x0)
  %2 = Add %1, 0(0x0)
  %3 = Add %1, 8(0x8)
  CondBranch %2, BB%5, BB%8
BB %5 - succs: %25
  %6 = Call 1024(0x400), %2, %3
  Branch BB%25
BB %8 - succs: %25
  %9 = Add %2, %3
  %10 = Add %9, 1
  %11 = Add %10, 1
  %12 = Add %11, 1
  %13 = Add %12, 1
  %14 = Add %13, 1
  %15 = Add %14, 1
  %16 = Add %15, 1
  %17 = Add %16, 1
  %18 = Call 1024(0x400), %3, %2, %9, %12, %13, %14, %15, %16, %17
  %19 = Add %2, %9
  %20 = Add %10, %11
  %21 = Add %20, %13
  %22 = Add %21, %15
  %23 = Call 1024(0x400), %19, %18, %22
  Branch BB%25
BB %25 - succs: %28
  %26 = Phi (BB%8, %23), (BB%5, %6)
  Return %26
BB %28

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);
  auto opnd_id_map = buildOperandToIndexMap(parser.getOutputInstrMap());

  LinearScanAllocator lsallocator(lir_func.get());
  lir_func->sortBasicBlocks();
  lsallocator.calculateLiveIntervals();
  lsallocator.linearScan();
  ASSERT_FALSE(lsallocator.intervalList().empty());

  ASSERT_GT(lsallocator.getFrameSize(), 0)
      << "Incorrect results - no registers have been spilled.";

  std::vector<LiveInterval*> intervals;
  UnorderedMap<int, std::vector<LiveInterval*>> loc_interval_map;
  UnorderedMap<const Operand*, std::vector<LiveInterval*>> vreg_location_map;
  for (auto& alloc : lsallocator.intervalList()) {
    if (!opnd_id_map.contains(alloc->operand)) {
      continue;
    }
    loc_interval_map[alloc->allocated_loc.loc].push_back(alloc.get());
    vreg_location_map[alloc->operand].push_back(alloc.get());
  }

  // check if the intervals allocated to the same location do not overlop
  for (auto& pair : loc_interval_map) {
    auto& loc_intervals = pair.second;
    for (size_t i = 0; i < loc_intervals.size() - 1; i++) {
      for (size_t j = i + 1; j < loc_intervals.size(); j++) {
        auto ival1 = loc_intervals[i];
        auto ival2 = loc_intervals[j];
        auto intersection = ival1->intersectWith(*ival2);

        ASSERT_EQ(intersection, -1) << fmt::format(
            "Location {} has conflicting intervals: {} intersects with {}",
            pair.first,
            *ival1,
            *ival2);
      }
    }
  }

  // check if the same virtual register does not allocate to multiple locations
  // at the same time
  for (auto& pair : vreg_location_map) {
    auto& vreg_intervals = pair.second;
    for (size_t i = 0; i < vreg_intervals.size() - 1; i++) {
      for (size_t j = i + 1; j < vreg_intervals.size(); j++) {
        auto ival1 = vreg_intervals[i];
        auto ival2 = vreg_intervals[j];
        auto intersection = ival1->intersectWith(*ival2);

        ASSERT_EQ(intersection, -1) << fmt::format(
            "Vreg {} has conflicting intervals: {} intersects with {}",
            opnd_id_map.at(pair.first),
            *ival1,
            *ival2);
      }
    }
  }
}

TEST_F(LinearScanAllocatorTest, InoutRegTest) {
  // OptimizeMoveSequence should not set reg operands that are also output
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  auto a =
      bb->allocateInstr(Instruction::kMove, nullptr, lir::OutVReg(), Imm(0));
  auto b =
      bb->allocateInstr(Instruction::kMove, nullptr, lir::OutVReg(), Imm(0));

  auto add = bb->allocateInstr(
      Instruction::kAdd, nullptr, lir::OutVReg(), lir::VReg(a), lir::VReg(b));

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      lir::OutPhyReg{codegen::arch::reg_general_return_loc},
      lir::VReg(add));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  runAllocator(lirfunc.get());

  ASSERT_TRUE(
      add->output()->getPhyRegister() == add->getInput(0)->getPhyRegister() ||
      add->output()->getPhyRegister() == add->getInput(1)->getPhyRegister());
}

TEST_F(LinearScanAllocatorTest, CallWithSideEffectTest) {
  // RewriteLIR should not remove function calls
  // since they may have side effects
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  auto a = bb->allocateInstr(Instruction::kCall, nullptr, lir::OutVReg());

  auto b =
      bb->allocateInstr(Instruction::kMove, nullptr, lir::OutVReg(), Imm(0));

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      lir::OutPhyReg{codegen::arch::reg_general_return_loc},
      lir::VReg(b));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);
  ASSERT_TRUE(a->opcode() == Instruction::kCall);
  ASSERT_TRUE(a->output()->type() == lir::Operand::kVreg);
  runAllocator(lirfunc.get());
  ASSERT_TRUE(a->opcode() == Instruction::kCall);
  ASSERT_TRUE(a->output()->type() == lir::Operand::kNone);
}

TEST_F(
    LinearScanAllocatorTest,
    Arm64LoopCallLiveVRegPrefersReservedCalleeSavedReg) {
#if !defined(CINDER_AARCH64)
  GTEST_SKIP() << "ARM64 register preference is target-specific";
#else
  const char* lir_source = R"(Function:
BB %0 - succs: %2
  %1:64bit = Move 0(0x0):64bit
  Branch BB%2
BB %2 - succs: %5
  %3:64bit = Phi (BB%0, %1), (BB%5, %7)
  Branch BB%5
BB %5 - succs: %2 %10
  %6:64bit = Call 1024(0x400), %3:64bit
  %7:64bit = Add %3:64bit, 1
  CondBranch %7, BB%2, BB%10
BB %10
  Return %3:64bit

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);
  auto opnd_id_map = buildOperandToIndexMap(parser.getOutputInstrMap());
  const auto* loop_value = parser.getOutputInstrMap().at(3)->output();

  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();
  allocator.linearScan();

  bool found_preferred_reg = false;
  for (auto& interval : allocator.intervalList()) {
    if (interval->operand != loop_value) {
      continue;
    }
    found_preferred_reg |= interval->allocated_loc == codegen::X27;
  }

  ASSERT_TRUE(found_preferred_reg) << fmt::format(
      "loop-carried value %{} should prefer X27 across loop call",
      opnd_id_map.at(loop_value));
#endif
}

TEST_F(
    LinearScanAllocatorTest,
    Arm64LoopCallPreferenceKeepsPreallocatedRegister) {
#if !defined(CINDER_AARCH64)
  GTEST_SKIP() << "ARM64 register preference is target-specific";
#else
  auto lir_source = fmt::format(
      R"(Function:
BB %0 - succs: %2
  %1:64bit = Bind {}:64bit
  Branch BB%2
BB %2 - succs: %5
  %3:64bit = Phi (BB%0, %1), (BB%5, %7)
  Branch BB%5
BB %5 - succs: %2 %10
  %6:64bit = Call 1024(0x400), %3:64bit
  %7:64bit = Add %3:64bit, %1:64bit
  CondBranch %7, BB%2, BB%10
BB %10
  Return %3:64bit

)",
      codegen::X19);

  Parser parser;
  auto lir_func = parser.parse(lir_source);
  auto opnd_id_map = buildOperandToIndexMap(parser.getOutputInstrMap());
  const auto* bound_value = parser.getOutputInstrMap().at(1)->output();

  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();
  allocator.linearScan();

  bool found_bound_interval = false;
  for (auto& interval : allocator.intervalList()) {
    if (interval->operand != bound_value) {
      continue;
    }
    found_bound_interval = true;
    ASSERT_EQ(interval->allocated_loc, codegen::X19) << fmt::format(
        "preallocated loop value %{} must not be overwritten by preference",
        opnd_id_map.at(bound_value));
  }

  ASSERT_TRUE(found_bound_interval);
#endif
}

TEST_F(
    LinearScanAllocatorTest,
    Arm64LoopCallPreferenceWithCompetingCandidatesAllocatesLegally) {
#if !defined(CINDER_AARCH64)
  GTEST_SKIP() << "ARM64 register preference is target-specific";
#else
  const char* lir_source = R"(Function:
BB %0 - succs: %4
  %1:64bit = Move 0(0x0):64bit
  %2:64bit = Move 1(0x1):64bit
  Branch BB%4
BB %4 - succs: %8
  %5:64bit = Phi (BB%0, %1), (BB%8, %11)
  %6:64bit = Phi (BB%0, %2), (BB%8, %12)
  Branch BB%8
BB %8 - succs: %4 %15
  %9:64bit = Call 1024(0x400), %5:64bit, %6:64bit
  %11:64bit = Add %5:64bit, 1
  %12:64bit = Add %6:64bit, 1
  %13:64bit = Add %11:64bit, %12:64bit
  CondBranch %13, BB%4, BB%15
BB %15
  Return %13:64bit

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);

  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();
  allocator.linearScan();

  UnorderedMap<int, std::vector<LiveInterval*>> loc_interval_map;
  for (auto& interval : allocator.intervalList()) {
    if (!interval->allocated_loc.is_register()) {
      continue;
    }
    loc_interval_map[interval->allocated_loc.loc].push_back(interval.get());
  }

  for (auto& [loc, intervals] : loc_interval_map) {
    for (size_t i = 0; i < intervals.size(); i++) {
      for (size_t j = i + 1; j < intervals.size(); j++) {
        ASSERT_EQ(intervals[i]->intersectWith(*intervals[j]), INVALID_LOCATION)
            << fmt::format(
                   "Location {} has conflicting intervals: {} intersects "
                   "with {}",
                   loc,
                   *intervals[i],
                   *intervals[j]);
      }
    }
  }
#endif
}

TEST_F(LinearScanAllocatorTest, CoversHintMatchesPlainBinarySearch) {
  // covers() resumes its search from the range examined by the previous
  // call.  Query one interval in a scrambled order (forward hits, gaps,
  // end boundaries, then backward jumps that leave the hint ahead of the
  // query) and require every answer to match a fresh interval queried once.
  const std::vector<LiveRange> ranges{{10, 20}, {40, 50}, {80, 90}, {200, 210}};
  const std::vector<LIRLocation> queries{
      5,
      10,
      19,
      20,
      45,
      60,
      85,
      90,
      150,
      205,
      210,
      215,
      // Backward queries: the hint now sits on the last range, ahead of the
      // query, so covers() must fall back to the binary search.
      45,
      41,
      205,
      12,
      5,
      85,
      42,
      49,
      50,
      200,
      209,
      11};

  Operand operand;
  LiveInterval hinted{&operand};
  for (const auto& range : ranges) {
    hinted.addRange(range);
  }

  for (LIRLocation loc : queries) {
    LiveInterval plain{&operand};
    for (const auto& range : ranges) {
      plain.addRange(range);
    }
    EXPECT_EQ(hinted.covers(loc), plain.covers(loc)) << fmt::format(
        "covers({}) diverged from the binary-search answer", loc);
  }

  // The exact expected answers, so a shared bug in both paths cannot hide.
  const std::vector<bool> expected{false, true,  true,  false, true,  false,
                                   true,  false, false, true,  false, false,
                                   true,  true,  true,  true,  false, true,
                                   true,  true,  false, true,  true,  true};
  for (size_t i = 0; i < queries.size(); i++) {
    EXPECT_EQ(hinted.covers(queries[i]), expected[i])
        << fmt::format("covers({}) gave the wrong answer", queries[i]);
  }
}

TEST_F(LinearScanAllocatorTest, FixedIntervalsAreSortedAndMerged) {
  // Fixed physical-register intervals collect their ranges with plain
  // emplace_back during live-interval construction and are finalized by a
  // sort-and-merge pass at the end of calculateLiveIntervals.  Two calls
  // make the argument registers' ranges non-adjacent, so the merge must
  // leave every fixed interval sorted, pairwise separated, and seeded at
  // the very beginning of the function.
  const char* lir_source = R"(Function:
BB %0 - succs: %2
  %1 = Move 0(0x0)
  Branch BB%2
BB %2 - succs: %8 %10
  %3 = Call 1024(0x400), %1
  %4 = Add %3, 1
  %5 = Call 1024(0x400), %4
  CondBranch %5, BB%8, BB%10
BB %8
  Return %5
BB %10
  Return %4

)";

  Parser parser;
  auto lir_func = parser.parse(lir_source);

  // Only live-interval construction is of interest here (the sort-and-merge
  // runs at its end); like RegAllocationNoSpill, skip the scan and rewrite.
  LinearScanAllocator allocator(lir_func.get());
  lir_func->sortBasicBlocks();
  allocator.calculateLiveIntervals();

  bool saw_fixed = false;
  for (auto& entry : allocator.intervalMap()) {
    const LiveInterval& interval = entry.second;
    if (!interval.isFixed()) {
      continue;
    }
    saw_fixed = true;
    ASSERT_FALSE(interval.ranges.empty())
        << fmt::format("fixed interval has no ranges: {}", interval);
    EXPECT_EQ(interval.ranges.front().start, -1) << fmt::format(
        "fixed interval lost its function-entry seed range: {}", interval);
    for (size_t i = 0; i + 1 < interval.ranges.size(); i++) {
      // Sorted, and genuinely separate: neither overlapping nor adjacent
      // (adjacent ranges must have been merged away).
      EXPECT_LT(interval.ranges[i].end, interval.ranges[i + 1].start)
          << fmt::format(
                 "fixed interval has unsorted or unmerged ranges: {}",
                 interval);
    }
  }

  ASSERT_TRUE(saw_fixed) << "the LIR should have reserved physical registers";
}

} // namespace jit::lir
