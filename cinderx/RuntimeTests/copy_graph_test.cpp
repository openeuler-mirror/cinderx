// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/copy_graph.h"

#include <algorithm>
#include <fmt/format.h>

#include <ostream>

using namespace jit::codegen;
using Op = CopyGraph::Op;

std::ostream& operator<<(std::ostream& os, const Op& op) {
  switch (op.kind) {
    case Op::Kind::kCopy:
      return os << fmt::format("Copy({} -> {})", op.from, op.to);
    case Op::Kind::kExchange:
      return os << fmt::format("Exchange({} <-> {})", op.from, op.to);
  }
  JIT_ABORT("Bad Op::Kind");
}

TEST(CopyGraphTest, SimpleChain) {
  CopyGraph cg;
  cg.addEdge(1, 2);
  cg.addEdge(2, 3);
  cg.addEdge(3, 4);
  cg.addEdge(4, 5);

  auto ops = cg.process();
  ASSERT_EQ(ops.size(), 4);
  EXPECT_EQ(ops[0], Op(Op::Kind::kCopy, 4, 5));
  EXPECT_EQ(ops[1], Op(Op::Kind::kCopy, 3, 4));
  EXPECT_EQ(ops[2], Op(Op::Kind::kCopy, 2, 3));
  EXPECT_EQ(ops[3], Op(Op::Kind::kCopy, 1, 2));
}

TEST(CopyGraphTest, SimpleCycle) {
  CopyGraph cg;
  cg.addEdge(0, 1);
  cg.addEdge(1, 0);

  auto ops = cg.process();
  ASSERT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0], Op(Op::Kind::kExchange, 0, 1));
}

TEST(CopyGraphTest, EdgeOrderPreservesDeterministicParallelCopies) {
  // Include a register cycle, a memory cycle and independent copies.
  const std::vector<std::pair<int, int>> edges{
      {0, 1}, {1, 0}, {-1, -2}, {-2, -1}, {2, 3}, {4, 5}};
  std::vector<int> order{0, 1, 2, 3, 4, 5};
  std::vector<Op> reference;
  do {
    CopyGraph graph;
    std::map<int, int> values{
        {-2, 12}, {-1, 11}, {0, 10}, {1, 20},
        {2, 30}, {3, 40}, {4, 50}, {5, 60}};
    auto expected = values;
    for (auto [from, to] : edges) {
      expected[to] = values.at(from);
    }
    for (int index : order) {
      auto [from, to] = edges[index];
      graph.addEdge(from, to);
    }
    auto operations = graph.process();
    if (reference.empty()) {
      reference = operations;
    } else {
      EXPECT_EQ(operations, reference);
    }
    for (const auto& operation : operations) {
      if (operation.kind == Op::Kind::kCopy) {
        values[operation.to] = values.at(operation.from);
      } else {
        std::swap(values.at(operation.from), values.at(operation.to));
      }
    }
    for (auto [location, value] : expected) {
      EXPECT_EQ(values.at(location), value);
    }
  } while (std::next_permutation(order.begin(), order.end()));
}

TEST(CopyGraphTest, WithCycles) {
  // Build a graph with two cycles and a few offshoots.
  CopyGraph cg;
  cg.addEdge(1, 2);
  cg.addEdge(2, 3);
  cg.addEdge(3, 4);
  cg.addEdge(4, 1);

  cg.addEdge(3, 5);
  cg.addEdge(5, 6);

  cg.addEdge(4, 7);

  cg.addEdge(8, -9);
  cg.addEdge(-9, 8);
  cg.addEdge(8, 10);

  auto ops = cg.process();
  EXPECT_EQ(ops.size(), 10);
  EXPECT_EQ(ops[0], Op(Op::Kind::kCopy, 5, 6));
  EXPECT_EQ(ops[1], Op(Op::Kind::kCopy, 3, 5));
  EXPECT_EQ(ops[2], Op(Op::Kind::kCopy, 4, 7));
  EXPECT_EQ(ops[3], Op(Op::Kind::kCopy, 8, 10));
  EXPECT_EQ(ops[4], Op(Op::Kind::kCopy, -9, CopyGraph::kTempLoc));
  EXPECT_EQ(ops[5], Op(Op::Kind::kCopy, 8, -9));
  EXPECT_EQ(ops[6], Op(Op::Kind::kCopy, CopyGraph::kTempLoc, 8));
  EXPECT_EQ(ops[7], Op(Op::Kind::kExchange, 1, 4));
  EXPECT_EQ(ops[8], Op(Op::Kind::kExchange, 4, 3));
  EXPECT_EQ(ops[9], Op(Op::Kind::kExchange, 3, 2));
}

TEST(CopyGraphTest, CopyGraphWithTypeMultiCycles) {
  constexpr int Types[] = {0, 1, 2, 3};

  CopyGraphWithType<int> cg;
  cg.addEdge(-1, -2, Types[0]);
  cg.addEdge(-2, -3, Types[1]);
  cg.addEdge(-3, -1, Types[2]);

  cg.addEdge(-4, -5, Types[3]);
  cg.addEdge(-5, -6, Types[3]);
  cg.addEdge(-6, -4, Types[3]);

  cg.addEdge(-7, -8, Types[0]);
  cg.addEdge(-8, -7, Types[1]);

  auto ops = cg.process();

  std::unordered_map<int, int> expected = {
      {-2, Types[0]},
      {-3, Types[1]},
      {-1, Types[2]},
      {-5, Types[3]},
      {-6, Types[3]},
      {-4, Types[3]},
      {-8, Types[0]},
      {-7, Types[1]},
  };

  for (auto& op : ops) {
    if (op.to == CopyGraph::kTempLoc) {
      continue;
    }
    EXPECT_EQ(op.type, jit::map_get(expected, op.to));
  }
}

struct PreferWiderType {
  int operator()(int from, int to) const {
    return std::max(from, to);
  }
};

TEST(CopyGraphTest, CopyGraphWithTypeResolvesExchangeType) {
  CopyGraphWithType<int, PreferWiderType> cg;
  cg.addEdge(0, 1, 32);
  cg.addEdge(1, 0, 64);

  auto ops = cg.process();

  ASSERT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0], Op(Op::Kind::kExchange, 0, 1));
  EXPECT_EQ(ops[0].type, 64);
}
