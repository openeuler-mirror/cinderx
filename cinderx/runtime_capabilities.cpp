// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/runtime_capabilities.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <istream>
#include <string_view>

#if defined(__linux__) && defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

namespace cinderx::runtime_capabilities {
namespace {

constexpr unsigned int kMaxNodeId = 65535;
constexpr int kMaxNodeCount = static_cast<int>(kMaxNodeId) + 1;
constexpr std::size_t kMaxNodeListLength = 4096;

void appendCpuCapability(
    Snapshot& snapshot,
    std::string_view capability) noexcept {
  if (snapshot.cpu_capability_count < snapshot.cpu_capabilities.size()) {
    snapshot.cpu_capabilities[snapshot.cpu_capability_count++] = capability;
  }
}

void detectCpuCapabilities(Snapshot& snapshot) noexcept {
#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx")) {
    appendCpuCapability(snapshot, "avx");
  }
  if (__builtin_cpu_supports("avx2")) {
    appendCpuCapability(snapshot, "avx2");
  }
  if (__builtin_cpu_supports("avx512f")) {
    appendCpuCapability(snapshot, "avx512f");
  }
  if (__builtin_cpu_supports("bmi2")) {
    appendCpuCapability(snapshot, "bmi2");
  }
  if (__builtin_cpu_supports("sse2")) {
    appendCpuCapability(snapshot, "sse2");
  }
  if (__builtin_cpu_supports("sse4.2")) {
    appendCpuCapability(snapshot, "sse4.2");
  }
#elif defined(__linux__) && defined(__aarch64__)
  const unsigned long hwcap = getauxval(AT_HWCAP);
#ifdef HWCAP_AES
  if ((hwcap & HWCAP_AES) != 0) {
    appendCpuCapability(snapshot, "aes");
  }
#endif
#ifdef HWCAP_ASIMD
  if ((hwcap & HWCAP_ASIMD) != 0) {
    appendCpuCapability(snapshot, "asimd");
  }
#endif
#ifdef HWCAP_ATOMICS
  if ((hwcap & HWCAP_ATOMICS) != 0) {
    appendCpuCapability(snapshot, "atomics");
  }
#endif
#ifdef HWCAP_CRC32
  if ((hwcap & HWCAP_CRC32) != 0) {
    appendCpuCapability(snapshot, "crc32");
  }
#endif
#ifdef HWCAP_FP
  if ((hwcap & HWCAP_FP) != 0) {
    appendCpuCapability(snapshot, "fp");
  }
#endif
#ifdef HWCAP_PMULL
  if ((hwcap & HWCAP_PMULL) != 0) {
    appendCpuCapability(snapshot, "pmull");
  }
#endif
#ifdef HWCAP_SHA1
  if ((hwcap & HWCAP_SHA1) != 0) {
    appendCpuCapability(snapshot, "sha1");
  }
#endif
#ifdef HWCAP_SHA2
  if ((hwcap & HWCAP_SHA2) != 0) {
    appendCpuCapability(snapshot, "sha2");
  }
#endif
#ifdef HWCAP_SVE
  if ((hwcap & HWCAP_SVE) != 0) {
    appendCpuCapability(snapshot, "sve");
  }
#endif
#if defined(AT_HWCAP2) && defined(HWCAP2_SVE2)
  const unsigned long hwcap2 = getauxval(AT_HWCAP2);
  if ((hwcap2 & HWCAP2_SVE2) != 0) {
    appendCpuCapability(snapshot, "sve2");
  }
#endif
#endif

  auto begin = snapshot.cpu_capabilities.begin();
  std::sort(begin, begin + snapshot.cpu_capability_count);
}

std::optional<int> detectNumaNodeCount() noexcept {
#ifdef __linux__
  try {
    std::ifstream online_nodes("/sys/devices/system/node/online");
    if (!online_nodes.is_open()) {
      return std::nullopt;
    }
    return readNumaNodeCount(online_nodes);
  } catch (...) {
    // Runtime capabilities are advisory. Preserve the one-time snapshot even
    // if constructing the platform stream itself fails.
    return std::nullopt;
  }
#else
  return std::nullopt;
#endif
}

Snapshot detectSnapshot() noexcept {
  Snapshot snapshot;
  detectCpuCapabilities(snapshot);
  snapshot.numa_node_count = detectNumaNodeCount();
  return snapshot;
}

} // namespace

std::optional<int> parseNumaNodeList(std::string_view nodes) noexcept {
  if (nodes.empty()) {
    return std::nullopt;
  }

  std::size_t position = 0;
  unsigned int previous_end = 0;
  bool first_range = true;
  int node_count = 0;

  auto parse_node_id = [&](unsigned int& value) {
    if (position == nodes.size() || nodes[position] < '0' ||
        nodes[position] > '9') {
      return false;
    }
    value = 0;
    do {
      const unsigned int digit = nodes[position] - '0';
      if (value > (kMaxNodeId - digit) / 10) {
        return false;
      }
      value = value * 10 + digit;
      position++;
    } while (position < nodes.size() && nodes[position] >= '0' &&
             nodes[position] <= '9');
    return true;
  };

  while (position < nodes.size()) {
    unsigned int range_start;
    if (!parse_node_id(range_start)) {
      return std::nullopt;
    }
    unsigned int range_end = range_start;
    if (position < nodes.size() && nodes[position] == '-') {
      position++;
      if (!parse_node_id(range_end) || range_end < range_start) {
        return std::nullopt;
      }
    }
    if (!first_range && range_start <= previous_end) {
      return std::nullopt;
    }

    const unsigned int range_size = range_end - range_start + 1;
    if (range_size > static_cast<unsigned int>(kMaxNodeCount - node_count)) {
      return std::nullopt;
    }
    node_count += range_size;
    previous_end = range_end;
    first_range = false;

    if (position == nodes.size()) {
      break;
    }
    if (nodes[position] != ',') {
      return std::nullopt;
    }
    position++;
    if (position == nodes.size()) {
      return std::nullopt;
    }
  }

  return node_count > 0 ? std::optional<int>{node_count} : std::nullopt;
}

std::optional<int> readNumaNodeCount(std::istream& nodes) noexcept {
  try {
    std::array<char, kMaxNodeListLength + 1> buffer{};
    nodes.read(buffer.data(), buffer.size());
    const std::streamsize bytes_read = nodes.gcount();

    // A bounded complete read ends at EOF before filling the buffer. Reject
    // badbit and any short read that left bytes behind in the streambuf; both
    // indicate that parsing would operate on a partial topology snapshot.
    if (nodes.bad() || bytes_read <= 0 ||
        bytes_read == static_cast<std::streamsize>(buffer.size()) ||
        !nodes.eof() ||
        nodes.rdbuf()->sgetc() != std::char_traits<char>::eof()) {
      return std::nullopt;
    }

    std::size_t length = static_cast<std::size_t>(bytes_read);
    while (length > 0 &&
           (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
      length--;
    }
    return parseNumaNodeList(std::string_view{buffer.data(), length});
  } catch (...) {
    return std::nullopt;
  }
}

const Snapshot& getSnapshot() noexcept {
  static const Snapshot snapshot = detectSnapshot();
  return snapshot;
}

} // namespace cinderx::runtime_capabilities
