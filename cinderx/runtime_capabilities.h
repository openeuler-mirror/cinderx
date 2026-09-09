// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <array>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace cinderx::runtime_capabilities {

constexpr std::size_t kMaxCpuCapabilities = 16;

struct Snapshot {
  std::array<std::string_view, kMaxCpuCapabilities> cpu_capabilities{};
  std::size_t cpu_capability_count{0};
  std::optional<int> numa_node_count;
};

std::optional<int> parseNumaNodeList(std::string_view nodes) noexcept;
std::optional<int> readNumaNodeCount(std::istream& nodes) noexcept;
const Snapshot& getSnapshot() noexcept;

} // namespace cinderx::runtime_capabilities
