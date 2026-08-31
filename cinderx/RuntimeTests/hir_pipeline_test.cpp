// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// HIR pipeline golden tests (INFRA-1).
//
// Captures the full execution trace of Compiler::runPasses — the pass
// sequence and the content of the HIR after every pass — for a corpus of
// functions under several PassConfigs, and compares it against committed
// golden fingerprint files (one line per step: pass, occurrence, FNV-1a
// of the normalized post-pass HIR).
//
// This is the no-behavior-drift gate for the PassManager extraction
// (Phase B0 / I1): reordering, duplicating, omitting or gating any pass
// differently changes the trace and fails the test.  Fingerprints keep
// the committed baseline compact and its diffs reviewable; full per-pass
// HIR dumps for diagnosing a drift are printed on demand with
// HIR_PIPELINE_FULL_DUMP=1 and are never committed.
//
// Goldens live in RuntimeTests/hir_pipeline_golden/<py>.<minor>/<arch>/
// and are regenerated with UPDATE_HIR_PIPELINE_GOLDEN=1 (see
// TestScripts/update_hir_pipeline_golden.py).  Per the version policy the
// gated lines are Python 3.11 and 3.14; other versions/architectures only
// get goldens once generated on such a host, and the test skips (loudly)
// until then.

#include "cinderx/RuntimeTests/fixtures.h"

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/printer.h"

#include <fmt/format.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace jit;

namespace {

// Same normalization as HIRTest applies to expected/actual HIR: some
// toolchains suffix private symbols with ".lto_priv.<n>", which would make
// goldens toolchain-dependent.
std::string normalizeToolchainPrivateSymbols(std::string hir) {
  static const std::regex lto_priv_suffix{
      R"(([A-Za-z_][A-Za-z0-9_]*)\.lto_priv\.[0-9]+(@0x[0-9a-fA-F]+))"};
  return std::regex_replace(hir, lto_priv_suffix, "$1$2");
}

struct PipelineCase {
  const char* name;
  const char* src;
};

struct PipelineConfig {
  const char* name;
  PassConfig config;
};

// Corpus: one small function per interesting HIR shape, covering the
// scheduling units of Compiler::runPasses (simplification, accumulator
// promotion, dynamic comparison elimination, inlining, builtin method
// elimination, unbox CSE, cleanups, refcount insertion).  Keep sources
// free of imports and non-immortal object constants so dumps stay stable.
const std::vector<PipelineCase> kPipelineCases = {
    {"ReturnConstant", "def test():\n  return 1\n"},
    {"IntArithBranch",
     "def test(a, b):\n"
     "  if a > 0:\n"
     "    return a + b * 2\n"
     "  return a - b\n"},
    {"FloatAccumulate",
     "def test(xs):\n"
     "  total = 0.0\n"
     "  for x in xs:\n"
     "    total = total + x * 1.5\n"
     "  return total\n"},
    {"FloatCompare",
     "def test(a, b):\n"
     "  if a < b:\n"
     "    return 1.5\n"
     "  if a == b:\n"
     "    return 2.5\n"
     "  return 0.5\n"},
    {"BuiltinMethodCall",
     "def test(s, xs):\n"
     "  t = s.upper()\n"
     "  xs.append(t)\n"
     "  return len(xs)\n"},
    {"CallChain",
     "def square(x):\n"
     "  return x * x\n"
     "def test(a):\n"
     "  return square(a) + square(a + 1)\n"},
    {"BoolChain",
     "def test(a, b, c):\n"
     "  if a and b or not c:\n"
     "    return True\n"
     "  return False\n"},
    {"RepeatedUnbox",
     "def test(a, b):\n"
     "  x = float(a) + float(b)\n"
     "  y = float(a) - float(b)\n"
     "  return x * y\n"},
    {"PhiSimple",
     "def test(n):\n"
     "  last = 0\n"
     "  cur = 1\n"
     "  for i in range(n):\n"
     "    last, cur = cur, last + cur\n"
     "  return last\n"},
    {"TryExceptDiv",
     "def test(a, b):\n"
     "  try:\n"
     "    return a / b\n"
     "  except ZeroDivisionError:\n"
     "    return -1\n"},
    {"ClosureFactory",
     "def test(n):\n"
     "  k = n + 1\n"
     "  def inner(x):\n"
     "    return x + k\n"
     "  return inner(n) + inner(n + 1)\n"},
    {"LoopSumList",
     "def test(xs):\n"
     "  total = 0\n"
     "  for x in xs:\n"
     "    if x % 2 == 0:\n"
     "      total += x\n"
     "  return total\n"},
};

const std::vector<PipelineConfig> kPipelineConfigs = {
    {"kMinimal", PassConfig::kMinimal},
    {"kAllExceptInliner", PassConfig::kAllExceptInliner},
    {"kAll", PassConfig::kAll},
};

const char* archName() {
#if defined(CINDER_AARCH64)
  return "aarch64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

std::filesystem::path goldenRoot() {
  return std::filesystem::path(__FILE__).parent_path() /
      "hir_pipeline_golden";
}

std::filesystem::path goldenPath(
    const PipelineCase& c,
    const PipelineConfig& cfg) {
  return goldenRoot() / fmt::format("{}.{}", PY_MAJOR_VERSION, PY_MINOR_VERSION) /
      archName() / fmt::format("{}__{}.txt", c.name, cfg.name);
}

bool updateGoldenRequested() {
  const char* env = std::getenv("UPDATE_HIR_PIPELINE_GOLDEN");
  return env != nullptr && env[0] != '\0' && env != std::string_view("0");
}

// Fixed 64-bit FNV-1a.  std::hash<std::string> is implementation-defined
// and would make goldens non-portable across libstdc++ versions.
uint64_t fnv1a64(std::string_view s) {
  uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// One recorded pass execution: sequence number, pass label (with #n for
// repeated call points) and the normalized HIR after that pass.
struct PipelineStep {
  int seq;
  std::string label;
  std::string hir;
};

// Runs the whole runPasses pipeline under `config`, recording every pass
// execution in order together with the HIR after that pass.
std::vector<PipelineStep> capturePipelineSteps(
    hir::Function& irfunc,
    PassConfig config) {
  std::vector<PipelineStep> steps;
  std::map<std::string, int> occurrences;
  Compiler::runPasses(
      irfunc,
      config,
      [&](hir::Function& func, std::string_view pass_name, std::size_t) {
        int& occurrence = occurrences[std::string(pass_name)];
        ++occurrence;
        hir::HIRPrinter printer;
        steps.push_back(PipelineStep{
            static_cast<int>(steps.size()) + 1,
            fmt::format("{}#{}", pass_name, occurrence),
            normalizeToolchainPrivateSymbols(printer.ToString(func))});
      });
  return steps;
}

// Committed form: one fingerprint line per step (sequence + pass + hash
// of the post-pass HIR).  Compact by design — a drift shows up as a small,
// reviewable diff pinpointing the step, instead of thousands of dump
// lines that nobody can attribute line by line.
std::string compactTrace(const std::vector<PipelineStep>& steps) {
  std::string trace;
  for (const PipelineStep& s : steps) {
    trace += fmt::format(
        "step {:>2}: {:<32} {:016x} len={}\n",
        s.seq,
        s.label,
        fnv1a64(s.hir),
        s.hir.size());
  }
  return trace;
}

// Diagnosis form (never committed): full HIR after every pass, printed on
// demand to localize what changed behind a fingerprint diff.
std::string fullTrace(const std::vector<PipelineStep>& steps) {
  std::string trace;
  for (const PipelineStep& s : steps) {
    trace += fmt::format("### step {}: {}\n", s.seq, s.label);
    trace += s.hir;
    trace += "\n";
  }
  return trace;
}

bool fullDumpRequested() {
  const char* env = std::getenv("HIR_PIPELINE_FULL_DUMP");
  return env != nullptr && env[0] != '\0' && env != std::string_view("0");
}

class HIRPipelineGoldenTest : public RuntimeTest {
 public:
  HIRPipelineGoldenTest(const PipelineCase* c, const PipelineConfig* cfg)
      : RuntimeTest{RuntimeTest::kJit}, case_(*c), config_(*cfg) {}

  void TestBody() override {
    std::unique_ptr<hir::Function> first;
    ASSERT_NO_FATAL_FAILURE(CompileToHIR(case_.src, "test", first));
    std::vector<PipelineStep> first_steps =
        capturePipelineSteps(*first, config_.config);

    // The trace must be deterministic: building the same function twice in
    // the same process must yield an identical pass sequence and identical
    // per-pass HIR.  This pins "repeat runs diff to zero" as an assertion
    // instead of a manual CI step.
    std::unique_ptr<hir::Function> second;
    ASSERT_NO_FATAL_FAILURE(CompileToHIR(case_.src, "test", second));
    std::vector<PipelineStep> second_steps =
        capturePipelineSteps(*second, config_.config);
    ASSERT_EQ(compactTrace(first_steps), compactTrace(second_steps))
        << "pipeline trace is not deterministic for " << case_.name << "/"
        << config_.name;

    std::string actual = fmt::format(
        "# CinderX HIR pipeline golden - generated, do not hand-edit.\n"
        "# Format: step <n>: <Pass>#<occurrence> <fnv1a64 of normalized post-pass HIR> len=<bytes>\n"
        "# Diagnose a fingerprint diff: rerun with HIR_PIPELINE_FULL_DUMP=1 to print per-pass HIR.\n"
        "# Regenerate: TestScripts/update_hir_pipeline_golden.py <runtime_tests binary>\n"
        "# python: {}.{}  arch: {}  config: {}  case: {}\n",
        PY_MAJOR_VERSION,
        PY_MINOR_VERSION,
        archName(),
        config_.name,
        case_.name);
    actual += compactTrace(first_steps);

    auto path = goldenPath(case_, config_);
    if (updateGoldenRequested()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      ASSERT_FALSE(ec) << "cannot create golden dir " << path.parent_path()
                       << ": " << ec.message();
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << actual;
      out.close();
      ASSERT_FALSE(out.fail()) << "failed writing golden " << path;
      std::cout << "[golden-update] wrote " << path.string() << "\n";
      return;
    }

    if (!std::filesystem::exists(path)) {
      GTEST_SKIP() << "no golden for python "
                   << PY_MAJOR_VERSION << "." << PY_MINOR_VERSION << " / "
                   << archName() << " (" << case_.name << "__" << config_.name
                   << "); generate one on this platform with"
                   << " UPDATE_HIR_PIPELINE_GOLDEN=1";
    }
    std::ifstream in(path, std::ios::binary);
    std::string expected(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(actual, expected)
        << "HIR pipeline trace drifted from golden " << path.string()
        << " (case " << case_.name << ", config " << config_.name
        << "). If the change is intentional, regenerate with"
        << " UPDATE_HIR_PIPELINE_GOLDEN=1 and review the fingerprint diff;"
        << " rerun with HIR_PIPELINE_FULL_DUMP=1 to see per-pass HIR.";
    if (HasFailure() && fullDumpRequested()) {
      std::cout << "\n===== FULL ACTUAL TRACE (diagnosis only, never"
                << " committed) =====\n"
                << fullTrace(first_steps) << "===== END FULL TRACE =====\n";
    }
  }

 private:
  PipelineCase case_;
  PipelineConfig config_;
};

} // namespace

void registerPipelineGoldenTests() {
  for (const PipelineConfig& cfg : kPipelineConfigs) {
    for (const PipelineCase& c : kPipelineCases) {
      ::testing::RegisterTest(
          "HIRPipelineGolden",
          fmt::format("{}/{}", c.name, cfg.name).c_str(),
          nullptr,
          nullptr,
          __FILE__,
          __LINE__,
          [case_ptr = &c, cfg_ptr = &cfg]() -> ::testing::Test* {
            return new HIRPipelineGoldenTest(case_ptr, cfg_ptr);
          });
    }
  }
}
