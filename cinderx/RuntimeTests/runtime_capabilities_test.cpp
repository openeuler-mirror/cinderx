// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/runtime_capabilities.h"

#include <algorithm>
#include <ios>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>

namespace cinderx::runtime_capabilities {
namespace {

class PartialReadBuffer final : public std::stringbuf {
 public:
  explicit PartialReadBuffer(std::string contents)
      : std::stringbuf(std::move(contents)) {}

 protected:
  std::streamsize xsgetn(char* output, std::streamsize count) override {
    return std::stringbuf::xsgetn(output, std::min<std::streamsize>(count, 1));
  }
};

TEST(RuntimeCapabilitiesTest, ParsesValidNodeRanges) {
  EXPECT_EQ(parseNumaNodeList("0"), 1);
  EXPECT_EQ(parseNumaNodeList("0-3"), 4);
  EXPECT_EQ(parseNumaNodeList("0,2-4,7"), 5);
  EXPECT_EQ(parseNumaNodeList("1,2,3"), 3);
}

TEST(RuntimeCapabilitiesTest, RejectsOverlappingAndOutOfOrderRanges) {
  EXPECT_EQ(parseNumaNodeList("0-2,2-3"), std::nullopt);
  EXPECT_EQ(parseNumaNodeList("1-3,2-4"), std::nullopt);
  EXPECT_EQ(parseNumaNodeList("2,1"), std::nullopt);
  EXPECT_EQ(parseNumaNodeList("1,1"), std::nullopt);
}

TEST(RuntimeCapabilitiesTest, RejectsMalformedAndTruncatedLists) {
  constexpr std::string_view malformed[] = {
      "", "-1", "0-", "0,", ",0", "0--1", "0 1", "0\n1"};
  for (std::string_view nodes : malformed) {
    EXPECT_EQ(parseNumaNodeList(nodes), std::nullopt) << nodes;
  }
}

TEST(RuntimeCapabilitiesTest, EnforcesNodeIdBoundary) {
  EXPECT_EQ(parseNumaNodeList("65535"), 1);
  EXPECT_EQ(parseNumaNodeList("0-65535"), 65536);
  EXPECT_EQ(parseNumaNodeList("65536"), std::nullopt);
  EXPECT_EQ(parseNumaNodeList("0-65536"), std::nullopt);
  EXPECT_EQ(parseNumaNodeList("99999999999999999999"), std::nullopt);
}

TEST(RuntimeCapabilitiesTest, ReadsOnlyACompleteTopologySnapshot) {
  std::istringstream valid{"0,2-4\r\n"};
  EXPECT_EQ(readNumaNodeCount(valid), 4);

  std::istringstream truncated{"0-"};
  EXPECT_EQ(readNumaNodeCount(truncated), std::nullopt);

  std::istringstream oversized(std::string(4097, '0'));
  EXPECT_EQ(readNumaNodeCount(oversized), std::nullopt);
}

TEST(RuntimeCapabilitiesTest, RejectsBadAndPartialStreams) {
  std::istringstream bad{"0-3\n"};
  bad.setstate(std::ios::badbit);
  EXPECT_EQ(readNumaNodeCount(bad), std::nullopt);

  PartialReadBuffer partial_buffer{"0-3\n"};
  std::istream partial{&partial_buffer};
  EXPECT_EQ(readNumaNodeCount(partial), std::nullopt);
}

TEST(RuntimeCapabilitiesTest, SnapshotIsBoundedSortedAndStable) {
  const Snapshot& first = getSnapshot();
  const Snapshot& second = getSnapshot();
  EXPECT_EQ(&first, &second);
  ASSERT_LE(first.cpu_capability_count, first.cpu_capabilities.size());
  EXPECT_TRUE(std::is_sorted(
      first.cpu_capabilities.begin(),
      first.cpu_capabilities.begin() + first.cpu_capability_count));
  if (first.numa_node_count.has_value()) {
    EXPECT_GE(*first.numa_node_count, 1);
    EXPECT_LE(*first.numa_node_count, 65536);
  }
}

} // namespace
} // namespace cinderx::runtime_capabilities
