// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/pyjit.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Jit/trigger_stats.h"
#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp_structs.h"
#endif

#include "cinderx/Common/audit.h"
#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/import.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/string.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/autojit_import.h"
#include "cinderx/Jit/behavior_classifier.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/Jit/codegen/tls.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/annotation_index.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_flag_processor.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_list.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/mmap_file.h"
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/module_state.h"

#ifndef WIN32
#include <dlfcn.h>
#endif
#include <fmt/std.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
// The MR-04 execute surface, defined in Jit/pyjit_311_gate.cpp.
#include "cinderx/Interpreter/3.11/eval_hook.h"
#include "cinderx/Interpreter/3.11/observe.h"
#endif

using namespace jit;

namespace {

constexpr uint32_t kAutoJitInterpretOnlyThreshold = 65536;
constexpr uint32_t kAutoJitLongLowRoiFreezeThreshold = 1000;
constexpr uint32_t kAutoJitClassifyDefaultThreshold = 2;

struct AutoJitGateStats {
  std::atomic<uint64_t> jit_vectorcall{0};
  std::atomic<uint64_t> global_threshold_return{0};
  std::atomic<uint64_t> classified_schedule_cold_skip{0};
  std::atomic<uint64_t> classified_warmup_return{0};
  std::atomic<uint64_t> classified_defer_freeze{0};
  std::atomic<uint64_t> forced_compile{0};
  std::atomic<uint64_t> forced_compile_ok{0};
  std::atomic<uint64_t> forced_compile_fallback{0};
  std::atomic<uint64_t> roi_uncompile{0};
  std::atomic<uint64_t> roi_recompile{0};
  std::atomic<uint64_t> roi_frozen{0};
};

struct AutoJitGateState {
  CodeExtra* extra{nullptr};
  uint64_t calls{0};
  GateContext context;
};

AutoJitGateStats g_auto_jit_gate_stats;
std::atomic<bool> g_auto_jit_gate_stats_enabled{false};

void incAutoJitGateStat(std::atomic<uint64_t>& stat) {
  if (g_auto_jit_gate_stats_enabled.load(std::memory_order_relaxed)) {
    stat.fetch_add(1, std::memory_order_relaxed);
  }
}

void clearAutoJitGateStats() {
  g_auto_jit_gate_stats.jit_vectorcall.store(0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.global_threshold_return.store(
      0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.classified_schedule_cold_skip.store(
      0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.classified_warmup_return.store(
      0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.classified_defer_freeze.store(
      0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.forced_compile.store(0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.forced_compile_ok.store(0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.forced_compile_fallback.store(
      0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.roi_uncompile.store(0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.roi_recompile.store(0, std::memory_order_relaxed);
  g_auto_jit_gate_stats.roi_frozen.store(0, std::memory_order_relaxed);
}

int setAutoJitGateStat(
    PyObject* dict,
    const char* name,
    const std::atomic<uint64_t>& stat) {
  auto value = Ref<>::steal(
      PyLong_FromUnsignedLongLong(stat.load(std::memory_order_relaxed)));
  if (value == nullptr) {
    return -1;
  }
  return PyDict_SetItemString(dict, name, value);
}

const char* familyName(Family family) {
  switch (family) {
    case Family::NumericLoop:
      return "NumericLoop";
    case Family::BranchFSM:
      return "BranchFSM";
    case Family::ObjectManipulator:
      return "ObjectManipulator";
    case Family::CallDispatcher:
      return "CallDispatcher";
    case Family::AsyncStateMachine:
      return "AsyncStateMachine";
    case Family::ReflectionMeta:
      return "ReflectionMeta";
    case Family::Trivial:
      return "Trivial";
    case Family::Mixed:
      return "Mixed";
    case Family::kCount:
      break;
  }
  return "Unknown";
}

const char* branchReasonName(BranchReason reason) {
  switch (reason) {
    case BranchReason::None:
      return "None";
    case BranchReason::LowRoi:
      return "LowRoi";
    case BranchReason::StartupInit:
      return "StartupInit";
    case BranchReason::RiskDefer:
      return "RiskDefer";
    case BranchReason::RoiBackoff:
      return "RoiBackoff";
    case BranchReason::FallbackInvalid:
      return "FallbackInvalid";
  }
  return "Unknown";
}

std::string jsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (unsigned char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          fmt::format_to(std::back_inserter(escaped), "\\u{:04x}", ch);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
        break;
    }
  }
  return escaped;
}

std::string pyStringOrInvalid(BorrowedRef<PyObject> obj) {
  if (obj == nullptr || !PyUnicode_Check(obj)) {
    return "<invalid>";
  }
  const char* utf8 = PyUnicode_AsUTF8(obj);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return "<invalid>";
  }
  return utf8;
}

const char* currentAutoJitPhaseName(const GateContext& context) {
  const char* phase = std::getenv("CINDERX_AUTOJIT_PHASE");
  if (phase != nullptr && phase[0] != '\0') {
    return phase;
  }
  if (context.import_phase && context.setup_phase) {
    return "import_setup";
  }
  if (context.import_phase) {
    return "import";
  }
  if (context.setup_phase) {
    return "setup";
  }
  if (context.startup_phase) {
    return "startup";
  }
  return "steady";
}

void writeAutoJitCompileEvent(
    BorrowedRef<PyFunctionObject> func,
    const AutoJitGateState& state,
    const std::optional<StructureKey>& key,
    const ThresholdDecision& decision,
    uint32_t effective_limit) {
  const char* path = std::getenv("CINDERX_AUTOJIT_COMPILE_EVENTS_FILE");
  if (path == nullptr || path[0] == '\0') {
    return;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
  std::ofstream out(path, std::ios::app);
  if (!out) {
    return;
  }

  out << "{\"event\":\"forced_compile\""
      << ",\"ts_ns\":" << now_ns << ",\"pid\":" << getpid() << ",\"phase\":\""
      << jsonEscape(currentAutoJitPhaseName(state.context)) << "\""
      << ",\"fullname\":\"" << jsonEscape(funcFullname(func)) << "\""
      << ",\"filename\":\"" << jsonEscape(pyStringOrInvalid(code->co_filename))
      << "\""
      << ",\"qualname\":\"" << jsonEscape(pyStringOrInvalid(code->co_qualname))
      << "\""
      << ",\"firstlineno\":" << code->co_firstlineno
      << ",\"calls\":" << state.calls
      << ",\"effective_limit\":" << effective_limit << ",\"startup_phase\":"
      << (state.context.startup_phase ? "true" : "false")
      << ",\"import_phase\":" << (state.context.import_phase ? "true" : "false")
      << ",\"setup_phase\":" << (state.context.setup_phase ? "true" : "false")
      << ",\"branch_reason\":\"" << branchReasonName(decision.branch_reason)
      << "\"";
  if (key.has_value()) {
    out << ",\"shape\":{"
        << "\"family\":\"" << familyName(key->family) << "\""
        << ",\"mixed_shape\":" << static_cast<int>(key->mixed_shape)
        << ",\"loop_score\":" << static_cast<int>(key->loop_score)
        << ",\"is_suspendable\":" << (key->is_suspendable ? "true" : "false")
        << ",\"is_static\":" << (key->is_static ? "true" : "false")
        << ",\"risk_reason\":" << static_cast<int>(key->risk_reason)
        << ",\"code_size_bucket\":" << static_cast<int>(key->code_size_bucket)
        << ",\"active_dim_mask\":" << static_cast<int>(key->active_dim_mask)
        << "}";
  } else {
    out << ",\"shape\":null";
  }
  out << "}\n";
}

// RAII device for disabling GIL checking.
class DisableGilCheck {
 public:
  DisableGilCheck() : old_check_enabled_{_PyRuntime.gilstate.check_enabled} {
    _PyRuntime.gilstate.check_enabled = 0;
  }

  ~DisableGilCheck() {
    _PyRuntime.gilstate.check_enabled = old_check_enabled_;
  }

 private:
  int old_check_enabled_;
};

CompilerContext<Compiler>* jitCtx() {
  auto state = cinderx::getModuleState();
  if (state != nullptr) {
    return static_cast<CompilerContext<Compiler>*>(state->jit_context.get());
  }
  return nullptr;
}

bool isLightweightFramesCompiledIn();
int validateFrameModeConfig();

// Don't care flags: CO_NOFREE, CO_FUTURE_* (the only still-relevant future is
// "annotations" which doesn't impact bytecode execution.)
constexpr int required_code_flags = CO_OPTIMIZED | CO_NEWLOCALS;
bool hasRequiredFlags(BorrowedRef<PyCodeObject> code) {
  return (code->co_flags & required_code_flags) == required_code_flags;
}

GateContext readGateContext() {
  bool import_phase = autoJitImportDepth() > 0;
  bool setup_phase = autoJitSetupDepth() > 0;
  return GateContext{import_phase || setup_phase, import_phase, setup_phase};
}

AutoJitGateState readAutoJitGateState(BorrowedRef<PyCodeObject> code) {
  CodeExtra* extra = codeExtra(code);
  uint64_t calls = extra != nullptr ? Ci_code_extra_get_calls(extra) : 0;
  return {extra, calls, readGateContext()};
}

// Auto classification avoids installing Ci_EvalFrame, so the gate records the
// calls that it keeps on the interpreted path.
void recordAutoJitInterpretedCall(const AutoJitGateState& state) {
  if (!getConfig().auto_classify || state.extra == nullptr) {
    return;
  }
  uint32_t skey_word = Ci_code_extra_load_skey_acquire(state.extra);
  if (!(skey_word & kSkeyDecidedColdBit)) {
    Ci_code_extra_incr_calls(state.extra);
  }
}

bool shouldAlwaysScheduleCompile(BorrowedRef<PyCodeObject> code);

uint32_t roiBackoffRound(uint32_t ctl) {
  return (ctl & CI_CODE_EXTRA_ROI_ROUND_MASK) >> CI_CODE_EXTRA_ROI_ROUND_SHIFT;
}

uint32_t roiBackoffCtlForRound(uint32_t round) {
  constexpr uint32_t kMaxRound =
      CI_CODE_EXTRA_ROI_ROUND_MASK >> CI_CODE_EXTRA_ROI_ROUND_SHIFT;
  return (std::min(round, kMaxRound) << CI_CODE_EXTRA_ROI_ROUND_SHIFT) &
      CI_CODE_EXTRA_ROI_ROUND_MASK;
}

uint64_t saturatingAddU64(uint64_t lhs, uint64_t rhs) {
  uint64_t max = std::numeric_limits<uint64_t>::max();
  return lhs > max - rhs ? max : lhs + rhs;
}

uint64_t saturatingMulU64(uint64_t lhs, uint64_t rhs) {
  uint64_t max = std::numeric_limits<uint64_t>::max();
  if (lhs != 0 && rhs > max / lhs) {
    return max;
  }
  return lhs * rhs;
}

uint32_t roiBackoffBudgetForRound(uint32_t round) {
  size_t base = std::max<size_t>(getConfig().roi_deopt_budget_base, 1);
  uint64_t budget = static_cast<uint64_t>(std::min(
      base, static_cast<size_t>(std::numeric_limits<uint32_t>::max())));
  for (uint32_t i = 0; i < round; ++i) {
    budget = saturatingMulU64(budget, 2);
    if (budget >= std::numeric_limits<uint32_t>::max()) {
      return std::numeric_limits<uint32_t>::max();
    }
  }
  return static_cast<uint32_t>(budget);
}

uint64_t roiBackoffRecompileFloor(uint64_t calls, uint32_t round) {
  uint32_t threshold = getConfig().compile_after_n_calls.value_or(
      kAutoJitClassifyDefaultThreshold);
  uint64_t delta = std::max<uint64_t>(threshold, 1);
  delta = saturatingMulU64(
      delta, std::max<uint64_t>(getConfig().roi_rewarm_factor, 1));
  for (uint32_t i = 0; i < round; ++i) {
    delta = saturatingMulU64(delta, 2);
  }
  return saturatingAddU64(calls, delta);
}

bool roiBackoffReasonCounts(DeoptReason reason, bool is_instrumentation_deopt) {
  if (is_instrumentation_deopt) {
    return false;
  }
  return reason != DeoptReason::kPeriodicTaskFailure;
}

bool roiBackoffCtlFrozen(uint32_t ctl) {
  return (ctl & CI_CODE_EXTRA_ROI_FROZEN_BIT) != 0;
}

bool roiBackoffCtlPending(uint32_t ctl) {
  return (ctl & CI_CODE_EXTRA_ROI_PENDING_BIT) != 0;
}

bool roiBackoffStateAllowsCompile(CodeExtra* extra) {
  if (!getConfig().roi_backoff_enabled || extra == nullptr) {
    return true;
  }

  uint32_t ctl = Ci_code_extra_load_roi_ctl_relaxed(extra);
  if (roiBackoffCtlFrozen(ctl)) {
    return false;
  }

  uint64_t floor = Ci_code_extra_load_roi_recompile_floor_relaxed(extra);
  if (floor == 0) {
    return true;
  }

  uint64_t calls = Ci_code_extra_get_calls(extra);
  if (calls < floor) {
    return false;
  }

  Ci_code_extra_store_roi_recompile_floor_release(extra, 0);
  incAutoJitGateStat(g_auto_jit_gate_stats.roi_recompile);
  return true;
}

bool shouldSkipAutoJitScheduleForRoiBackoffFrozen(
    BorrowedRef<PyFunctionObject> func) {
  if (!getConfig().roi_backoff_enabled ||
      cinderx::getModuleState()->jit_list != nullptr) {
    return false;
  }
  if (shouldAlwaysScheduleCompile(BorrowedRef<PyCodeObject>{func->func_code})) {
    return false;
  }
  CodeExtra* extra =
      codeExtraIfExists(reinterpret_cast<PyCodeObject*>(func->func_code));
  if (extra == nullptr) {
    return false;
  }
  return roiBackoffCtlFrozen(Ci_code_extra_load_roi_ctl_relaxed(extra));
}

void setInterpreterJitFlag([[maybe_unused]] bool enabled) {
#if PY_VERSION_HEX >= 0x030D0000
  // PyInterpreterState.jit is the upstream tier-2 interpreter's own flag,
  // added in 3.13; it does not exist earlier.  On 3.11 the JIT never
  // initializes, so there is no flag to set.
  PyThreadState* tstate = _PyThreadState_UncheckedGet();
  if (tstate != nullptr && tstate->interp != nullptr) {
    tstate->interp->jit = enabled;
  }
#endif
}

// If functions in the cinderx module get compiled, they will somehow keep the
// module alive forever and the module will never get finalized on shutdown.
// This breaks many assumptions and has a high chance of use-after-frees or ASAN
// errors on shutdown.
//
// This is a hack around that by preventing the JIT from compiling anything in
// cinderx.
bool isCinderModule(BorrowedRef<> module_name) {
  if (module_name == nullptr || !PyUnicode_Check(module_name)) {
    return false;
  }
  std::string_view name = PyUnicode_AsUTF8(module_name);
  return name == "cinderx";
}

#if PY_VERSION_HEX < 0x030C0000
// On the CPython 3.11 delivery, import machinery functions are kept out of
// the JIT wholesale: compiling them buys no steady-state time and taxes
// startup.  3.14 keeps its own startup-cost mechanisms unchanged.
bool isImportlibBootstrapModule(BorrowedRef<> module_name) {
  if (module_name == nullptr || !PyUnicode_Check(module_name)) {
    return false;
  }
  std::string_view name = PyUnicode_AsUTF8(module_name);
  return name == "_frozen_importlib" || name == "_frozen_importlib_external" ||
      name == "importlib._bootstrap" || name == "importlib._bootstrap_external";
}
#endif

bool shouldAlwaysScheduleCompile(BorrowedRef<PyCodeObject> code) {
  // There's a config option for forcing all Static Python functions to be
  // compiled.
  bool is_static = code->co_flags & CI_CO_STATICALLY_COMPILED;
  return is_static && getConfig().compile_all_static_functions;
}

// Check if a function has been preloaded.
bool isPreloaded(BorrowedRef<PyFunctionObject> func) {
  return hir::preloaderManager().find(func) != nullptr;
}

// Like jitVectorcall(), but ignores any call count requirements.
PyObject* forcedJitVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called JIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};

  // Compile the function.
  Result result;
  try {
    result = compileFunction(func);
  } catch (const std::exception& exn) {
    // Gently fall back to the interpreter when C++ exceptions happen.
    JIT_DLOG("{}", exn.what());
    PyErr_Clear();
    result = Result::UNKNOWN_ERROR;
  }

  if (result == Result::OK) {
    incAutoJitGateStat(g_auto_jit_gate_stats.forced_compile_ok);
    JIT_DCHECK(
        isJitCompiled(func),
        "JIT succeeded for function {} but it is not recognized as compiled",
        funcFullname(func));
    return func->vectorcall(func_obj, stack, nargsf, kwnames);
  }

  auto interp_entry = getInterpretedVectorcall(func);

  // Python errors shouldn't happen during compilation, but if they do, bubble
  // them up without calling the function.
  if (result == Result::PYTHON_EXCEPTION) {
    incAutoJitGateStat(g_auto_jit_gate_stats.forced_compile_fallback);
    setVectorcall(func, interp_entry);
    return nullptr;
  }

  // Reset the function's entrypoint if it doesn't seem like there's a chance
  // compilation will work "soon".
  if (result != Result::ALREADY_SCHEDULED && result != Result::PAUSED) {
    setVectorcall(func, interp_entry);
  }

  // There's been some kind of compilation error, explicitly call the
  // interpreted entrypoint instead.
  incAutoJitGateStat(g_auto_jit_gate_stats.forced_compile_fallback);
  return interp_entry(func_obj, stack, nargsf, kwnames);
}

// Python function entry point when the JIT is enabled.
PyObject* jitVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called JIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};

  incAutoJitGateStat(g_auto_jit_gate_stats.jit_vectorcall);

  // If there's a call count limit, interpret the function as usual until the
  // limit is reached.
  if (auto limit = getConfig().compile_after_n_calls; limit.has_value()) {
    AutoJitGateState state = readAutoJitGateState(code);
    if (state.calls < *limit) {
      incAutoJitGateStat(g_auto_jit_gate_stats.global_threshold_return);
      auto entry = getInterpretedVectorcall(func);
      recordAutoJitInterpretedCall(state);
      return entry(func_obj, stack, nargsf, kwnames);
    }

    if (getConfig().roi_backoff_enabled && state.extra != nullptr) {
      uint32_t roi_ctl = Ci_code_extra_load_roi_ctl_relaxed(state.extra);
      if (roiBackoffCtlFrozen(roi_ctl)) {
        auto entry = getInterpretedVectorcall(func);
        setVectorcall(func, entry);
        return entry(func_obj, stack, nargsf, kwnames);
      }
      if (!roiBackoffStateAllowsCompile(state.extra)) {
        auto entry = getInterpretedVectorcall(func);
        recordAutoJitInterpretedCall(state);
        return entry(func_obj, stack, nargsf, kwnames);
      }
    }

    uint32_t effective_limit = *limit;
    std::optional<StructureKey> key;
    ThresholdDecision decision{*limit, BranchReason::None};
    if (getConfig().auto_classify) {
      if (shouldDeferSuspendableAutoJitWithoutStructureKey(
              code, state.context)) {
        effective_limit = kAutoJitInterpretOnlyThreshold;
      } else if (auto computed_key =
                     getOrComputeStructureKey(code, state.extra);
                 computed_key.has_value()) {
        decision =
            computeThresholdForCode(code, *computed_key, state.context, *limit);
        effective_limit = decision.limit;
        key = *computed_key;
      }
    }
    if (state.calls < effective_limit) {
      auto entry = getInterpretedVectorcall(func);
      bool freeze_low_roi = decision.branch_reason == BranchReason::LowRoi &&
          (effective_limit >= kAutoJitLongLowRoiFreezeThreshold ||
           (key.has_value() && key->family == Family::Trivial));
      if (getConfig().auto_classify &&
          (effective_limit >= kAutoJitInterpretOnlyThreshold ||
           freeze_low_roi)) {
        incAutoJitGateStat(g_auto_jit_gate_stats.classified_defer_freeze);
        if (state.extra != nullptr) {
          Ci_code_extra_or_skey_release(state.extra, kSkeyDecidedColdBit);
        }
        setVectorcall(func, entry);
      } else {
        incAutoJitGateStat(g_auto_jit_gate_stats.classified_warmup_return);
        recordAutoJitInterpretedCall(state);
      }
      return entry(func_obj, stack, nargsf, kwnames);
    }

    writeAutoJitCompileEvent(func, state, key, decision, effective_limit);
  }

  incAutoJitGateStat(g_auto_jit_gate_stats.forced_compile);
  return forcedJitVectorcall(func_obj, stack, nargsf, kwnames);
}

void setJitLogFile(const std::string& log_filename) {
  // Redirect logging to a file if configured.
  const char* kPidMarker = "{pid}";
  std::string pid_filename = log_filename;
  auto marker_pos = pid_filename.find(kPidMarker);
  if (marker_pos != std::string::npos) {
    pid_filename.replace(
        marker_pos, std::strlen(kPidMarker), fmt::format("{}", getpid()));
  }
  FILE* file = fopen(pid_filename.c_str(), "w");
  if (file == nullptr) {
    JIT_LOG(
        "Couldn't open log file {} ({}), logging to stderr",
        pid_filename,
        strerror(errno));
  } else {
    getMutableConfig().log.output_file = file;
  }
}

void setASMSyntax(const std::string& asm_syntax) {
  if (asm_syntax.compare("intel") == 0) {
    getMutableConfig().asm_syntax = AsmSyntax::Intel;
  } else if (asm_syntax.compare("att") == 0) {
    getMutableConfig().asm_syntax = AsmSyntax::ATT;
  } else {
    JIT_ABORT("Unknown asm syntax '{}'", asm_syntax);
  }
}

size_t parse_sized_argument(const std::string& val) {
  std::string parsed;
  // " 1024 k" should parse OK - so remove the space.
  std::remove_copy_if(
      val.begin(), val.end(), std::back_inserter(parsed), ::isspace);
  JIT_CHECK(!parsed.empty(), "Input string is empty");
  static_assert(
      sizeof(decltype(std::stoull(parsed))) == sizeof(size_t),
      "stoull parses to size_t size");
  size_t scale = 1;
  // "1024k" and "1024K" are the same - so upper case.
  char lastChar = std::toupper(parsed.back());
  switch (lastChar) {
    case 'K':
      scale = 1024;
      parsed.pop_back();
      break;
    case 'M':
      scale = 1024 * 1024;
      parsed.pop_back();
      break;
    case 'G':
      scale = 1024 * 1024 * 1024;
      parsed.pop_back();
      break;
    default:
      JIT_CHECK(
          std::isdigit(lastChar), "Invalid character in input string: {}", val);
  }
  size_t ret_value{0};
  auto p_last = parsed.data() + parsed.size();
  auto int_ok = std::from_chars(parsed.data(), p_last, ret_value);
  JIT_CHECK(
      int_ok.ec == std::errc() && int_ok.ptr == p_last,
      "Invalid unsigned integer in input string: '{}'",
      val);
  JIT_CHECK(
      ret_value <= (std::numeric_limits<size_t>::max() / scale),
      "Unsigned Integer overflow in input string: '{}'",
      val);
  return ret_value * scale;
}

bool parse_uint32_arg(std::string_view value, uint32_t* parsed) {
  if (value.empty()) {
    return false;
  }
  uint64_t tmp = 0;
  auto first = value.data();
  auto last = value.data() + value.size();
  auto result = std::from_chars(first, last, tmp);
  if (result.ec != std::errc() || result.ptr != last ||
      tmp > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *parsed = static_cast<uint32_t>(tmp);
  return true;
}

void configureCompileAfterNCalls(uint32_t calls, bool auto_classify) {
  getMutableConfig().compile_after_n_calls = calls;
  getMutableConfig().auto_classify = auto_classify;
  getMutableConfig().enable_startup_init_policy =
      auto_classify && autoJitImportProviderEnabledFromEnv();
}

void parseAutoJitOption(const std::string& value) {
  if (value.empty()) {
    configureCompileAfterNCalls(1, false);
    return;
  }

  uint32_t threshold = 0;
  if (value == "auto") {
    configureCompileAfterNCalls(kAutoJitClassifyDefaultThreshold, true);
    return;
  }
  constexpr std::string_view kAutoPrefix{"auto:"};
  if (value.starts_with(kAutoPrefix)) {
    std::string_view threshold_text{
        value.data() + kAutoPrefix.size(), value.size() - kAutoPrefix.size()};
    if (parse_uint32_arg(threshold_text, &threshold)) {
      configureCompileAfterNCalls(threshold, true);
    } else {
      JIT_LOG("Invalid value for jit-auto/PYTHONJITAUTO: {}", value);
    }
    return;
  }
  if (parse_uint32_arg(value, &threshold)) {
    configureCompileAfterNCalls(threshold, false);
  } else {
    JIT_LOG("Invalid value for jit-auto/PYTHONJITAUTO: {}", value);
  }
}

FlagProcessor initFlagProcessor() {
  FlagProcessor flag_processor;

  // Flags are inspected in order of definition below.

  flag_processor.addOption(
      "jit-dump-hir-stats",
      "PYTHONJITDUMPHIRSTATS",
      getMutableConfig().dump_hir_stats,
      "Dump counts of instructions and types per function");

  flag_processor.addOption(
      "jit-all",
      "PYTHONJITALL",
      [](uint32_t) { configureCompileAfterNCalls(0, false); },
      "Enable the JIT and set it to compile all functions as soon as they are "
      "called");

  flag_processor.addOption(
      "jit-auto",
      "PYTHONJITAUTO",
      [](const std::string& val) { parseAutoJitOption(val); },
      "Enable auto-JIT mode, which compiles functions after the given "
      "threshold");

  flag_processor.addOption(
      "jit-auto-roi-backoff",
      "CINDERX_AUTOJIT_ROI_BACKOFF",
      getMutableConfig().roi_backoff_enabled,
      "Enable or disable AutoJIT dynamic negative-ROI backoff after "
      "repeated deopts");

  flag_processor.addOption(
      "jit-auto-roi-backoff-budget",
      "CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET",
      getMutableConfig().roi_deopt_budget_base,
      "Base deopt budget before AutoJIT ROI backoff uncompile");

  flag_processor.addOption(
      "jit-auto-roi-backoff-max-rounds",
      "CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS",
      getMutableConfig().roi_backoff_max_rounds,
      "Maximum AutoJIT ROI backoff rounds before freezing");

  flag_processor.addOption(
      "jit-auto-roi-rewarm-factor",
      "CINDERX_AUTOJIT_ROI_REWARM_FACTOR",
      getMutableConfig().roi_rewarm_factor,
      "Multiplier for AutoJIT ROI backoff recompile floor");

  flag_processor.addOption(
      "jit-auto-code-dedup",
      "CINDERX_AUTOJIT_CODE_DEDUP",
      getMutableConfig().auto_code_twin_dedup,
      "Canonicalize exec-generated namespace-free content-twin code objects "
      "onto one identity at compile time; set to 0 to disable when isolating "
      "A/B");

  flag_processor.addOption(
      "jit-auto-lowroi-warm-calls",
      "CINDERX_AUTOJIT_LOWROI_WARM_CALLS",
      getMutableConfig().auto_classify_low_roi_warm_calls,
      "Held calls a process must accumulate before steady-state LowRoi "
      "shapes stop being deferred; 0 releases them immediately");

  flag_processor.addOption(
      "jit-debug",
      "PYTHONJITDEBUG",
      getMutableConfig().log.debug,
      "JIT debug and extra logging");

  flag_processor
      .addOption(
          "jit-log-file",
          "PYTHONJITLOGFILE",
          [](const std::string& log_filename) { setJitLogFile(log_filename); },
          "write log entries to <filename> rather than stderr")
      .withFlagParamName("filename");

  flag_processor
      .addOption(
          "jit-asm-syntax",
          "PYTHONJITASMSYNTAX",
          [](const std::string& asm_syntax) { setASMSyntax(asm_syntax); },
          "set the assembly syntax used in log files")
      .withFlagParamName("intel|att")
      .withDebugMessageOverride("Sets the assembly syntax used in log files");

  flag_processor
      .addOption(
          "jit-debug-refcount",
          "PYTHONJITDEBUGREFCOUNT",
          getMutableConfig().log.debug_refcount,
          "JIT refcount insertion debug mode")
      .withDebugMessageOverride("Enabling");

  flag_processor.addOption(
      "jit-debug-regalloc",
      "PYTHONJITDEBUGREGALLOC",
      getMutableConfig().log.debug_regalloc,
      "Enable or disable debug logging for the register allocator");

  flag_processor.addOption(
      "jit-debug-inliner",
      "PYTHONJITDEBUGINLINER",
      getMutableConfig().log.debug_inliner,
      "Enable or disable debug logging for the JIT's HIR inliner");

  flag_processor
      .addOption(
          "jit-dump-hir",
          "PYTHONJITDUMPHIR",
          getMutableConfig().log.dump_hir_initial,
          "Log the HIR representation of all functions after initial "
          "lowering from bytecode")
      .withDebugMessageOverride("Dump initial HIR of JITed functions");

  flag_processor
      .addOption(
          "jit-dump-hir-passes",
          "PYTHONJITDUMPHIRPASSES",
          getMutableConfig().log.dump_hir_passes,
          "Log the HIR after each optimization pass")
      .withDebugMessageOverride(
          "Dump HIR of JITed functions after each individual optimization "
          "pass");

  flag_processor
      .addOption(
          "jit-dump-final-hir",
          "PYTHONJITDUMPFINALHIR",
          getMutableConfig().log.dump_hir_final,
          "Log the HIR after all optimizations")
      .withDebugMessageOverride(
          "Dump final HIR of JITed functions after all optimizations");

  flag_processor
      .addOption(
          "jit-dump-lir",
          "PYTHONJITDUMPLIR",
          getMutableConfig().log.dump_lir,
          "Log the LIR representation of functions after lowering from HIR")
      .withDebugMessageOverride("Dump initial LIR of JITed functions");

  flag_processor.addOption(
      "jit-dump-lir-origin",
      "PYTHONJITDUMPLIRORIGIN",
      [](bool value) {
        getMutableConfig().log.dump_lir = true;
        getMutableConfig().log.lir_origin = value;
      },
      "Enable or disable whether LIR is displayed with HIR origin data");

  flag_processor.addOption(
      "jit-symbolize",
      "PYTHONJITSYMBOLIZE",
      getMutableConfig().log.symbolize_funcs,
      "Enable or disable symbolization of functions called by JIT code");

  flag_processor
      .addOption(
          "jit-dump-asm",
          "PYTHONJITDUMPASM",
          [](bool value) {
#ifndef ENABLE_DISASSEMBLER
            if (value) {
              JIT_LOG(
                  "Cannot use PYTHONJITDUMPASM, disassembler not supported by "
                  "this build");
              return;
            }
#endif
            getMutableConfig().log.dump_asm = value;
          },
          "Log the final compiled code, annotated with HIR instructions")
      .withDebugMessageOverride("Dump asm of JITed functions");

  flag_processor.addOption(
      "jit-enable-inline-cache-stats-collection",
      "PYTHONJITCOLLECTINLINECACHESTATS",
      getMutableConfig().collect_attr_cache_stats,
      "Collect inline cache stats (supported stats are cache misses for load "
      "method inline caches");

  flag_processor.addOption(
      "jit-gdb-support",
      "PYTHONJITGDBSUPPORT",
      [](bool value) {
        getMutableConfig().log.debug = value;
        getMutableConfig().gdb.supported = value;
      },
      "Enable or disable GDB support and JIT debug mode");

  flag_processor.addOption(
      "jit-gdb-write-elf",
      "PYTHONJITGDBWRITEELF",
      [](bool value) {
        getMutableConfig().log.debug = value;
        getMutableConfig().gdb.supported = value;
        getMutableConfig().gdb.write_elf_objects = value;
      },
      "Debugging aid, GDB support with ELF output");

  flag_processor.addOption(
      "jit-dump-stats",
      "PYTHONJITDUMPSTATS",
      getMutableConfig().log.dump_stats,
      "Dump JIT runtime stats at shutdown");

  flag_processor.addOption(
      "jit-huge-pages",
      "PYTHONJITHUGEPAGES",
      getMutableConfig().use_huge_pages,
      "Enable or disable huge pages for compiled functions");

  flag_processor.addOption(
      "jit-enable-jit-list-wildcards",
      "PYTHONJITENABLEJITLISTWILDCARDS",
      getMutableConfig().allow_jit_list_wildcards,
      "allow wildcards in JIT list");

  flag_processor.addOption(
      "jit-all-static-functions",
      "PYTHONJITALLSTATICFUNCTIONS",
      getMutableConfig().compile_all_static_functions,
      "JIT-compile all static functions");

  flag_processor
      .addOption(
          "jit-list-file",
          "PYTHONJITLISTFILE",
          getMutableConfig().jit_list.filename,
          "Load list of functions to compile from <filename>")
      .withFlagParamName("filename");

  flag_processor.addOption(
      "jit-list-fail-on-parse-error",
      "PYTHONJITLISTFAILONPARSEERROR",
      getMutableConfig().jit_list.error_on_parse,
      "Raise a Python exception when a JIT list fails to parse");

  flag_processor.addOption(
      "jit-disable",
      "PYTHONJITDISABLE",
      [](int val) {
        // Only update force_init if it wasn't already set.
        if (val && !getConfig().force_init.has_value()) {
          getMutableConfig().force_init = false;
        }
      },
      "disable the JIT");

  flag_processor.addOption(
      "jit-lightweight-frame",
      "PYTHONJITLIGHTWEIGHTFRAME",
      [](int val) {
        getMutableConfig().frame_mode =
            val ? FrameMode::kLightweight : FrameMode::kNormal;
      },
      "Enable/disable JIT lightweight frames");

  flag_processor.addOption(
      "jit-stable-frame",
      "PYTHONJITSTABLEFRAME",
      getMutableConfig().stable_frame,
      "Assume that data found in the Python frame is unchanged across "
      "function calls");

  flag_processor.addOption(
      "jit-preload-dependent-limit",
      "PYTHONJITPRELOADDEPENDENTLIMIT",
      getMutableConfig().preload_dependent_limit,
      "When compiling a function, set the number of dependent functions that "
      "can be compiled along with it.");

  // HIR optimizations.

#define HIR_OPTIMIZATION_OPTION(NAME, OPT, CLI, ENV) \
  flag_processor.addOption(                          \
      (CLI),                                         \
      (ENV),                                         \
      getMutableConfig().hir_opts.OPT,               \
      "Enable the HIR " NAME " optimization pass")

  HIR_OPTIMIZATION_OPTION(
      "BeginInlinedFunction elimination",
      begin_inlined_function_elim,
      "jit-begin-inlined-function-elim",
      "PYTHONJITBEGININLINEDFUNCTIONELIM");
  HIR_OPTIMIZATION_OPTION(
      "builtin LoadMethod elimination",
      builtin_load_method_elim,
      "jit-builtin-load-method-elim",
      "PYTHONJITBUILTINLOADMETHODELIM");
  HIR_OPTIMIZATION_OPTION(
      "CFG cleaning", clean_cfg, "jit-clean-cfg", "PYTHONJITCLEANCFG");
  HIR_OPTIMIZATION_OPTION(
      "dead code elimination",
      dead_code_elim,
      "jit-dead-code-elim",
      "PYTHONJITDEADCODEELIM");
  HIR_OPTIMIZATION_OPTION(
      "dynamic comparison elimination",
      dynamic_comparison_elim,
      "jit-dynamic-comparison-elim",
      "PYTHONJITDYNAMICCOMPARISIONELIM");
  HIR_OPTIMIZATION_OPTION(
      "float accumulator promotion",
      float_accumulator_promotion,
      "jit-float-accumulator-promotion",
      "PYTHONJITFLOATACCUMULATORPROMOTION");
  HIR_OPTIMIZATION_OPTION(
      "guard type removal",
      guard_type_removal,
      "jit-guard-type-removal",
      "PYTHONJITGUARDTYPEREMOVAL");
  HIR_OPTIMIZATION_OPTION(
      "inliner",
      inliner,
      "jit-enable-hir-inliner",
      "PYTHONJITENABLEHIRINLINER");
  HIR_OPTIMIZATION_OPTION(
      "list prefix reverse assign",
      list_prefix_reverse_assign,
      "jit-list-prefix-reverse-assign",
      "PYTHONJITLISTPREFIXREVERSEASSIGN");
  HIR_OPTIMIZATION_OPTION(
      "phi elimination", phi_elim, "jit-phi-elim", "PYTHONJITPHIELIM");
  HIR_OPTIMIZATION_OPTION(
      "TreeIter state machine",
      tree_iter_state_machine,
      "jit-tree-iter-state-machine",
      "PYTHONJITTREEITERSTATEMACHINE");
  HIR_OPTIMIZATION_OPTION(
      "PrimitiveBox rematerialization",
      primitive_box_remat,
      "jit-primitive-box-remat",
      "PYTHONJITPRIMITIVEBOXREMAT");
  HIR_OPTIMIZATION_OPTION(
      "PrimitiveUnbox CSE",
      primitive_unbox_cse,
      "jit-primitive-unbox-cse",
      "PYTHONJITPRIMITIVEUNBOXCSE");
  HIR_OPTIMIZATION_OPTION(
      "simplify", simplify, "jit-simplify", "PYTHONJITSIMPLIFY");

  flag_processor.addOption(
      "jit-simplify-iteration-limit",
      "PYTHONJITSIMPLIFYITERATIONLIMIT",
      getMutableConfig().simplifier.iteration_limit,
      "Set the maximum number of times the simplifier can run over a "
      "function");
  flag_processor.addOption(
      "jit-simplify-new-block-limit",
      "PYTHONJITSIMPLIFYNEWBLOCKLIMIT",
      getMutableConfig().simplifier.new_block_limit,
      "Set the maximum number of blocks that can be added by the simplifier "
      "to a function");
  flag_processor.addOption(
      "jit-hir-inliner-cost-limit",
      "PYTHONJITHIRINLINERCOSTLIMIT",
      getMutableConfig().inliner_cost_limit,
      "Limit how much the inliner is able to inline. The number's definition "
      "is only relevant to the inliner itself.");
  flag_processor.addOption(
      "jit-hir-inliner-cold-call-threshold",
      "PYTHONJITHIRINLINERCOLDCALLTHRESHOLD",
      getMutableConfig().inliner_cold_call_threshold,
      "Calls below this threshold are considered cold and will not be "
      "inlined.  Setting to 0 disables the check.");

  flag_processor.addOption(
      "jit-lir-inliner",
      "PYTHONJITLIRINLINER",
      getMutableConfig().lir_opts.inliner,
      "Enable the LIR inliner");

  flag_processor
      .addOption(
          "jit-batch-compile-workers",
          "PYTHONJITBATCHCOMPILEWORKERS",
          getMutableConfig().batch_compile_workers,
          "set the number of batch compile workers to <COUNT>")
      .withFlagParamName("COUNT");

  flag_processor
      .addOption(
          "jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          getMutableConfig().multithreaded_compile_test,
          "JIT multithreaded compile test")
      .isHiddenFlag(true);

  flag_processor.addOption(
      "jit-list-match-line-numbers",
      "PYTHONJITLISTMATCHLINENUMBERS",
      getMutableConfig().jit_list.match_line_numbers,
      "JIT list match line numbers");

  flag_processor
      .addOption(
          "jit-time",
          "",
          [](const std::string& flag_value) {
            parseAndSetFuncList(flag_value);
          },
          "Measure time taken in compilation phases and output summary to "
          "stderr or approperiate logfile. Only functions in comma separated "
          "<function_list> list will be included. Comma separated list may "
          "include wildcards, * and ?. Wildcards are processed in glob "
          "fashion and not as regex.")
      .withFlagParamName("function_list")
      .withDebugMessageOverride(
          "Will capture time taken in compilation phases and output summary");

  flag_processor.addOption(
      "jit-multiple-code-sections",
      "PYTHONJITMULTIPLECODESECTIONS",
      getMutableConfig().multiple_code_sections,
      "Enable emitting code into multiple code sections.");

  flag_processor.addOption(
      "jit-cold-code-huge-pages",
      "PYTHONJITCOLDCODEHUGEPAGES",
      getMutableConfig().cold_code_huge_pages,
      "Use huge pages for cold code sections.");

  flag_processor.addOption(
      "jit-attr-caches",
      "PYTHONJITATTRCACHES",
      getMutableConfig().attr_caches,
      "Use inline caches for attribute access instructions");

  flag_processor.addOption(
      "jit-attr-cache-size",
      "PYTHONJITATTRCACHESIZE",
      [](uint32_t entries) {
        JIT_CHECK(
            entries > 0 && entries <= 16,
            "Using {} entries for attribute access inline "
            "caches is not within the appropriate range",
            entries);
        getMutableConfig().attr_cache_size = entries;
      },
      "Set the number of entries in the JIT's attribute access inline "
      "caches");

  flag_processor.addOption(
      "jit-refine-static-python",
      "PYTHONJITREFINESTATICPYTHON",
      getMutableConfig().refine_static_python,
      "Add RefineType instructions to coerce Static Python types to be "
      "valid");

#ifndef WIN32
  flag_processor.addOption(
      "jit-perfmap",
      "JIT_PERFMAP",
      perf::jit_perfmap,
      "write out /tmp/perf-<pid>.map for JIT symbols");

  flag_processor
      .addOption(
          "jit-perf-dumpdir",
          "JIT_DUMPDIR",
          perf::perf_jitdump_dir,
          "absolute path to a <DIRECTORY> that exists. A perf jitdump file "
          "will be written to this directory")
      .withFlagParamName("DIRECTORY");
#endif

  flag_processor.addOption(
      "jit-help", "", [] {}, "print all available JIT flags and exits");

  flag_processor.addOption(
      "perf-trampoline-prefork-compilation",
      "PERFTRAMPOLINEPREFORKCOMPILATION",
      getMutableConfig().compile_perf_trampoline_prefork,
      "Compile perf trampoline pre-fork");

  flag_processor.addOption(
      "jit-immortalize-compiled-functions",
      "PYTHONJITIMMORTALIZECOMPILEDFUNCTIONS",
      getMutableConfig().immortalize_compiled_functions,
      "Always immortalize CompiledFunction objects");

  flag_processor.addOption(
      "jit-max-code-size",
      "PYTHONJITMAXCODESIZE",
      [](const std::string& val) {
        getMutableConfig().max_code_size = parse_sized_argument(val);
      },
      "Set the maximum code size for JIT in bytes (no suffix). For kilobytes "
      "use k or K as a suffix. "
      "Megabytes is m or M and gigabytes is g or G. 0 implies no limit.");

  flag_processor.addOption(
      "jit-emit-type-annotation-guards",
      "PYTHONJITTYPEANNOTATIONGUARDS",
      getMutableConfig().emit_type_annotation_guards,
      "Generate runtime checks that validate type annotations to specialize "
      "generated code.");

  flag_processor.addOption(
      "jit-specialized-opcodes",
      "PYTHONJITSPECIALIZEDOPCODES",
      getMutableConfig().specialized_opcodes,
      "JIT specialized opcodes or to fall back to their generic counterparts.");

  flag_processor.addOption(
      "osr-enabled",
      "CINDERX_OSR_ENABLED",
      getMutableConfig().osr_enabled,
      "Enable OSR hot-loop detection");

  flag_processor.addOption(
      "osr-backedge-threshold",
      "CINDERX_OSR_BACKEDGE_THRESHOLD",
      [](int val) {
        getMutableConfig().osr_backedge_threshold =
            static_cast<uint32_t>(val);
      },
      "Set the per-backedge OSR trigger threshold");

  flag_processor.addOption(
      "osr-compile-budget",
      "CINDERX_OSR_COMPILE_BUDGET",
      [](int val) {
        getMutableConfig().osr_compile_budget_code_units =
            static_cast<uint32_t>(val);
      },
      "Set the OSR compile budget in code units");

  flag_processor.addOption(
      "jit-backedge-gated-int-guards",
      "PYTHONJITBACKEDGEGATEDINTGUARDS",
      getMutableConfig().backedge_gated_int_guards,
      "Only emit exact-int guards for specialized numeric opcodes in code "
      "objects that contain a loop backedge.");

  flag_processor.addOption(
      "jit-support-instrumentation",
      "PYTHONJITSUPPORTINSTRUMENTATION",
      getMutableConfig().support_instrumentation,
      "Support instrumentation (e.g. monitoring/tracing/profiling)");

  flag_processor.setFlags(PySys_GetXOptions());

  // Inlining relies on lightweight-frame reification support.  Keep the
  // inliner disabled for normal-frame runs so tests and explicit normal-mode
  // configurations do not build inline frames that cannot be safely unlinked.
  bool force_disable_inliner_for_normal_frame =
      getConfig().frame_mode != FrameMode::kLightweight;
  if (force_disable_inliner_for_normal_frame) {
    getMutableConfig().hir_opts.inliner = false;
  }

  // If the inliner is off and the user hasn't explicitly set the preload
  // dependent limit, set it to zero.  Nothing is going to be inlined so there's
  // no need to aggressively preload.
  //
  // This will reduce the chance that Static Python functions can natively call
  // each other though.
  if (!force_disable_inliner_for_normal_frame &&
      !getConfig().hir_opts.inliner &&
      !flag_processor.hasHandled("jit-preload-dependent-limit")) {
    getMutableConfig().preload_dependent_limit = 0;
  }

  return flag_processor;
}

/*
 * Re-optimize a function by setting it to use JIT-compiled code if there's a
 * matching compiled code object.
 *
 * Intended for functions that have been explicitly deopted and for nested
 * functions.  Nested functions are created and destroyed multiple times but
 * have the same underlying code object.
 *
 * Return true if the function was successfully reopted, false if nothing
 * happened.
 */
bool reoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (jitCtx() == nullptr) {
    return false;
  } else if (jitCtx()->didCompile(func)) {
    return true;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return false;
  }

  if (CompiledFunction* compiled = jitCtx()->lookupFunc(func)) {
#if PY_VERSION_HEX < 0x030C0000
    // finalizeFunc() reports a refusal as "nothing to do", which is right
    // for it -- nothing was installed and nothing is half-built -- but
    // wrong to pass upward as "reopted".  Ask first, so the answer here
    // describes what happened.  A refusal can be transient -- tracing
    // active while enable() runs is the concrete case -- so a refused
    // function stays parked for the next enable() instead of being
    // forgotten here.
    if (Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
      return false;
    }
#endif
    // finalizeFunc() unparks on success; a failed publication leaves the
    // entry parked so a later enable() can retry it.
    return jitCtx()->finalizeFunc(func, compiled);
  }
  // No artifact remains for this function, so nothing will ever reattach
  // it: drop the parked entry (a no-op for a nested function that was
  // never explicitly deopted).
  jitCtx()->removeDeoptedFunc(func);
  return false;
}

// Check if we have exceeded the max code size limit.
bool isOverMaxCodeSize() {
  auto max_code_size = getConfig().max_code_size;
  ICodeAllocator* code_allocator =
      cinderx::getModuleState()->code_allocator.get();
  return max_code_size && code_allocator->usedBytes() >= max_code_size;
}

Result compilePreloader(
    const hir::Preloader& preloader,
    BorrowedRef<PyFunctionObject> func) {
  if (isOverMaxCodeSize()) {
    return Result::OVER_MAX_CODE_SIZE;
  }

  return compilePreloaderImpl(jitCtx(), preloader, func);
}

// Convert a registered translation unit into a pair of a Python function and
// its code object.  When the translation unit only refers to a code object
// (e.g. it's a nested function), the function will be a nullptr.
std::pair<BorrowedRef<PyFunctionObject>, BorrowedRef<PyCodeObject>> splitUnit(
    BorrowedRef<> unit) {
  if (PyFunction_Check(unit)) {
    BorrowedRef<PyFunctionObject> func{unit};
    BorrowedRef<PyCodeObject> code{func->func_code};
    return {func, code};
  }
  JIT_CHECK(
      PyCode_Check(unit),
      "Translation units must be functions or code objects, got '{}'",
      Py_TYPE(unit)->tp_name);

  BorrowedRef<PyCodeObject> code{unit};
  return {nullptr, code};
}

std::string unitFullname(BorrowedRef<> unit) {
  if (unit == nullptr) {
    return "<nullptr>";
  }
  auto [func, code] = splitUnit(unit);
  if (func != nullptr) {
    return funcFullname(func);
  }
  auto& jit_code_outer_funcs = jitCtx()->codeOuterFunctions();
  auto iter = jit_code_outer_funcs.find(code);
  if (iter == jit_code_outer_funcs.end()) {
    return fmt::format(
        "<Unknown code object {}>", static_cast<void*>(code.get()));
  }
  return codeFullname(iter->second->func_module, code);
}

// Load the preloader for a given function or code object.  If it doesn't exist
// yet, then preload the function and return the new preloader.
//
// Can potentially hit a Python exception, if so, will forward that along and
// return nullptr.
hir::Preloader* preload(BorrowedRef<> unit) {
  auto [func, code] = splitUnit(unit);
  if (hir::Preloader* existing = hir::preloaderManager().find(code)) {
    return existing;
  }

  // Make a new preloader. Note that this will run Python code so a lot of
  // assumptions are broken after this.
  std::unique_ptr<hir::Preloader> preloader;
  if (func != nullptr) {
    preloader = hir::Preloader::make(func, makeFrameReifier(func->func_code));
  } else {
    auto& jit_code_outer_funcs = jitCtx()->codeOuterFunctions();
    auto it = jit_code_outer_funcs.find(code);
    if (it == jit_code_outer_funcs.end()) {
      PyErr_Format(
          PyExc_RuntimeError,
          "failed to find code object for preloading: %U",
          code->co_qualname);
      return nullptr;
    }
    BorrowedRef<PyFunctionObject>& outer_func = it->second;
    // Assuming the builtins + globals will always be a dictionary goes way back
    // in the JIT's history. I'm not sure what guarantees this though. Tread
    // carefully but try not to blow things up if this happens in production
    // code.
    JIT_DCHECK(
        PyDict_CheckExact(outer_func->func_builtins),
        "Unexpected type for builtins ({}) on function {}",
        Py_TYPE(outer_func->func_builtins)->tp_name,
        funcFullname(outer_func));
    JIT_DCHECK(
        PyDict_CheckExact(outer_func->func_globals),
        "Unexpected type for globals ({}) on function {}",
        Py_TYPE(outer_func->func_globals)->tp_name,
        funcFullname(outer_func));
    preloader = hir::Preloader::make(
        code,
        outer_func->func_builtins,
        outer_func->func_globals,
        nullptr,
        codeFullname(outer_func->func_module, code),
        makeFrameReifier(code));
  }

  if (preloader == nullptr) {
    JIT_CHECK(
        PyErr_Occurred(), "Expect a Python exception when preloading fails");
    return nullptr;
  }

  // Have to check again for an existing preloader, because the preloader might
  // have re-entered itself when running Python code.
  if (hir::Preloader* existing = hir::preloaderManager().find(code)) {
    return existing;
  }

  // Grab a copy of the raw pointer before it gets moved away.
  auto copy = preloader.get();
  hir::preloaderManager().add(code, std::move(preloader));
  return copy;
}

using UnitDeletedCallback = std::function<void(BorrowedRef<>)>;

// Preloading can execute Python and re-enter the JIT. Preserve and chain the
// callback so nested preloads report deletions to both scopes, then restore the
// callback that was active on entry on every exit path.
hir::Preloader* preloadWithUnitDeletedCallback(
    BorrowedRef<> unit,
    UnitDeletedCallback current) {
  auto* state = cinderx::getModuleState();
  UnitDeletedCallback previous = std::move(state->unit_deleted_during_preload);
  SCOPE_EXIT(state->unit_deleted_during_preload = std::move(previous));

  state->unit_deleted_during_preload = [&](BorrowedRef<> deleted_unit) {
    current(deleted_unit);
    if (previous) {
      previous(deleted_unit);
    }
  };
  return preload(unit);
}

// JIT compile func or code object, only if a preloader is available.
//
// Re-entrant compile that is safe to call from within compilation, because it
// will only use an already-created preloader, it will not preload, and
// therefore it cannot raise a Python exception.
//
// Returns Result::NO_PRELOADER if no preloader is available.
Result tryCompilePreloaded(BorrowedRef<> unit) {
  // func may be null here if we're just compiling a code object for a nested
  // function
  auto [func, code] = splitUnit(unit);
  hir::Preloader* preloader = hir::preloaderManager().find(code);
  return preloader ? compilePreloader(*preloader, func) : Result::NO_PRELOADER;
}

void compile_worker_thread() {
  JIT_DLOG("Started compile worker in thread {}", std::this_thread::get_id());

  int attempts = 0;
  int retries = 0;

  while (BorrowedRef<> unit = getThreadedCompileContext().nextUnit()) {
    attempts++;
    auto result = tryCompilePreloaded(unit);
    if (result == Result::ALREADY_SCHEDULED) {
      retries++;
      getThreadedCompileContext().retryUnit(unit);
    }
    JIT_CHECK(
        result != Result::NO_PRELOADER,
        "Cannot find a JIT preloader for {}",
        unitFullname(unit));
  }

  cinderx::getModuleState()->compile_workers_attempted.fetch_add(attempts);
  cinderx::getModuleState()->compile_workers_retries.fetch_add(retries);

  JIT_DLOG(
      "Finished compile worker in thread {}. Compile attempts: {}, scheduled "
      "retries: {}",
      std::this_thread::get_id(),
      attempts,
      retries);
}

void compile_units_preloaded(std::vector<BorrowedRef<>>&& units) {
  for (auto unit : units) {
    tryCompilePreloaded(unit);
  }
}

void multithread_compile_units_preloaded(
    std::vector<BorrowedRef<>>&& units,
    size_t worker_count) {
  JIT_CHECK(worker_count > 1, "Expecting >1 workers but got {}", worker_count);

  JIT_DLOG(
      "Running multithread_compile_units_preloaded for {} units with {} "
      "workers",
      units.size(),
      worker_count);

  // Disable checks for using GIL protected data across threads.
  // Conceptually what we're doing here is saying we're taking our own
  // responsibility for managing locking of CPython runtime data structures.
  // Instead of holding the GIL to serialize execution to one thread, we're
  // holding the GIL for a group of co-operating threads which are aware of each
  // other. We still need the GIL as this protects the cooperating threads from
  // unknown other threads. Within our group of cooperating threads we can
  // safely do any read-only operations in parallel, but we grab our own lock if
  // we do a write (e.g. an incref).
  DisableGilCheck gil_check_guard;

  getThreadedCompileContext().startCompile(std::move(units));
  std::vector<std::thread> worker_threads;
  {
    // Ensure that no worker threads start compiling until they are all created,
    // in case something else in the process has hooked thread creation to run
    // arbitrary code.
    ThreadedCompileSerialize guard;
    for (size_t i = 0; i < worker_count; i++) {
      worker_threads.emplace_back(compile_worker_thread);
    }
  }
  for (std::thread& worker_thread : worker_threads) {
    worker_thread.join();
  }

  auto retry_list = getThreadedCompileContext().endCompile();

  jitCtx()->finalizeMultiThreadedCompile();

  JIT_DLOG(
      "multithread_compile_units_preloaded retrying {} units serially",
      retry_list.size());
  compile_units_preloaded(std::move(retry_list));
}

// Compile all functions registered via a JIT list that haven't been executed
// yet.
bool compile_all(size_t workers = 0) {
  FreeThreadedJITEntrypointGuard guard;
  JIT_CHECK(jitCtx(), "JIT not initialized");

  try {
    SCOPE_EXIT(hir::preloaderManager().clear());

    if (workers == 0) {
      workers = std::max<size_t>(getConfig().batch_compile_workers, 1);
    }

    std::vector<BorrowedRef<>> compilation_units;
    // units that were deleted during preloading
    std::unordered_set<BorrowedRef<>> deleted_units;
    // The deletion record is taken inside a death callback, where nothing
    // may throw; a failed record means the deleted-units view is
    // incomplete and this batch cannot be trusted.
    bool deletion_record_lost = false;

    auto& jit_reg_units =
        cinderx::getModuleState()->registered_compilation_units;
    JIT_DLOG(
        "Starting compile_all with {} workers for {} registered units",
        workers,
        jit_reg_units.size());

    // First we have to preload everything we are going to compile.
    while (jit_reg_units.size() > 0) {
      auto preload_units = std::move(jit_reg_units);
      jit_reg_units.clear();
      JIT_DLOG(
          "compile_all preloading a batch of {} units", preload_units.size());

      for (auto unit : preload_units) {
        if (deleted_units.contains(unit)) {
          continue;
        }
        hir::Preloader* preloader = preloadWithUnitDeletedCallback(
            unit, [&](BorrowedRef<> deleted_unit) {
              try {
                throwIfJitPublishStepArmedForTest(9);
                deleted_units.emplace(deleted_unit);
              } catch (const std::bad_alloc&) {
                deletion_record_lost = true;
              }
            });
        if (!preloader) {
          return false;
        }
        compilation_units.push_back(unit);
      }
    }

    if (deletion_record_lost || consumeUnitDeletionTrackingPoison()) {
      // A unit died during preloading and its record was lost; any entry
      // below may be dead.  Refuse the whole batch rather than execute a
      // guess.
      PyErr_NoMemory();
      return false;
    }

    // Filter out any units that were deleted as a side effect of preloading.
    std::erase_if(compilation_units, [&](BorrowedRef<> unit) {
      return deleted_units.contains(unit);
    });

    JIT_DLOG(
        "compile_all finished preloading {} units, {} were deleted",
        compilation_units.size(),
        deleted_units.size());

    if (workers > 1) {
      multithread_compile_units_preloaded(
          std::move(compilation_units), workers);
    } else {
      compile_units_preloaded(std::move(compilation_units));
    }
  } catch (const std::exception& exn) {
    setRuntimeError(exn);
    return false;
  }

  return true;
}

// Gets the eligibility for code or a function to be compiled. A function
// can be ineligible, eligible due to the JIT list, or if there's no
// jit list then just eligible. This is used to support handling nested
// functions in the cases of multi-threaded compile / JIT list and without.
//
// In multi-threaded compile w/ a JIT list: We need to track the nested code
// objects in jit_reg_units for when the multi-threaded compile kicks in and we
// may not have created any functions yet. But we don't need that if we're not
// doing multi-threaded compile, we'll only compile nested functions when a
// function gets called. So that's why we track this as an extra state.
//
// In both cases we always need to track the outer function so that we don't
// repeatedly re-compile nested functions - which is the big change here. That's
// the processing that we were previously only doing when we had a JIT list so
// now we're just skipping the jit_reg_units case when we're doing this for the
// non-JIT list/multi-threaded compile case.
enum class JitEligibility { Ineligible, JitListEligible, Eligible };

/*
 * Check for a functions eligibility to be compiled.
 *
 * This is the most broad definition of eligibility - that is it will only
 * return Ineligible for functions which are specifically not allowed to
 * be compiled for one reason or another.
 *
 * This doesn't guarantee that the function can or will be compiled, it just
 * checks if the JIT has been configured in such a way that compilation is
 * possible.
 */
JitEligibility getCompilationEligibility(BorrowedRef<PyFunctionObject> func) {
  // Can be called after the module has been finalized, due to function events.
  if (jitCtx() == nullptr || isCinderModule(func->func_module)) {
    return JitEligibility::Ineligible;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (isImportlibBootstrapModule(func->func_module)) {
    return JitEligibility::Ineligible;
  }
#endif

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (!hasRequiredFlags(code)) {
    return JitEligibility::Ineligible;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return JitEligibility::Ineligible;
  }
#endif

  // Note: This is not the same as fetching the function's code object and
  // checking its module and qualname, as functions can be renamed after they
  // are created.  Code objects cannot.
  if (auto jit_list = cinderx::getModuleState()->jit_list.get()) {
    if (jit_list->lookupFunc(func) == 1) {
      return JitEligibility::JitListEligible;
    }
    return JitEligibility::Ineligible;
  }

  return JitEligibility::Eligible;
}

/*
 * Variant of getCompilationEligibility() for nested code objects.
 */
JitEligibility getCompilationEligibility(
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code) {
  // Can be called after the module has been finalized, due to function events.
  if (jitCtx() == nullptr) {
    return JitEligibility::Ineligible;
  }

  if (isCinderModule(module_name)) {
    return JitEligibility::Ineligible;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (isImportlibBootstrapModule(module_name)) {
    return JitEligibility::Ineligible;
  }
#endif

  if (!hasRequiredFlags(code)) {
    return JitEligibility::Ineligible;
  }
#if PY_VERSION_HEX < 0x030C0000
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return JitEligibility::Ineligible;
  }
#endif

  if (auto jit_list = cinderx::getModuleState()->jit_list.get()) {
    if (jit_list->lookupCode(code) == 1 ||
        jit_list->lookupName(module_name, code->co_qualname) == 1) {
      return JitEligibility::JitListEligible;
    }
    return JitEligibility::Ineligible;
  }

  return JitEligibility::Eligible;
}

// Recursively search the given co_consts tuple for any code objects that are
// on the current jit-list, using the given module name to form a
// fully-qualified function name.
std::vector<BorrowedRef<PyCodeObject>> findNestedCodes(
    BorrowedRef<> module,
    BorrowedRef<> root_consts) {
  std::queue<PyObject*> consts_tuples;
  std::unordered_set<PyCodeObject*> visited;
  std::vector<BorrowedRef<PyCodeObject>> result;

  consts_tuples.push(root_consts);
  while (!consts_tuples.empty()) {
    PyObject* consts = consts_tuples.front();
    consts_tuples.pop();

    for (size_t i = 0, size = PyTuple_GET_SIZE(consts); i < size; ++i) {
      BorrowedRef<PyCodeObject> code = PyTuple_GET_ITEM(consts, i);
      if (!PyCode_Check(code) || !visited.insert(code).second ||
          code->co_qualname == nullptr ||
          getCompilationEligibility(module, code) ==
              JitEligibility::Ineligible) {
        continue;
      }

      result.emplace_back(code);
      consts_tuples.emplace(code->co_consts);
    }
  }

  return result;
}

// Register a function with the JIT to be compiled in the future.
//
// The JIT will run compileFunction() before the function executes on its next
// call.  The JIT can still choose to **not** compile the function at that
// point.
//
// The JIT will not keep the function alive, instead it will be informed that
// the function is being de-allocated via funcDestroyed() before the function
// goes away.
//
// Return true if the function is registered with JIT or is already compiled,
// and false otherwise.
bool registerFunction(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;

  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized.
  if (reoptFunc(func)) {
    return true;
  }

  if (!isJitUsable()) {
    return false;
  }

  if (isOverMaxCodeSize()) {
    return false;
  }

  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "Not intended for using during threaded compilation");
#if PY_VERSION_HEX < 0x030C0000
  // Borrowed pointer: arm the death watch BEFORE recording it (same
  // discipline as the installed/parked registries), or a function dying
  // registered-but-never-compiled leaves a dangling key for batch compile.
  if (!jitCtx()->watchFunctionDeath(func)) {
    return false;
  }
#endif
  auto& jit_reg_units = cinderx::getModuleState()->registered_compilation_units;
  try {
    jit_reg_units.emplace(func.getObj());
  } catch (const std::bad_alloc&) {
    // Report not-registered rather than letting the exception cross the
    // C API.  The watch stays armed: a watch without registry entries is
    // a benign no-op at delivery, and disarming here could withdraw a
    // watch an earlier publication armed.
    return false;
  }

  return true;
}

PyObject* multithreaded_compile_test(PyObject*, PyObject*) {
  FreeThreadedJITEntrypointGuard guard;
  if (!getConfig().multithreaded_compile_test) {
    PyErr_SetString(
        PyExc_NotImplementedError, "multithreaded_compile_test not enabled");
    return nullptr;
  }
  cinderx::getModuleState()->compile_workers_attempted = 0;
  cinderx::getModuleState()->compile_workers_retries = 0;
  auto& jit_reg_units = cinderx::getModuleState()->registered_compilation_units;
  JIT_LOG("(Re)compiling {} units compiles", jit_reg_units.size());
  jitCtx()->clearForMultithreadedCompileTest();

  std::chrono::time_point start = std::chrono::steady_clock::now();
  if (!compile_all()) {
    return nullptr;
  }
  std::chrono::time_point end = std::chrono::steady_clock::now();
  auto batch_compilation_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  JIT_LOG(
      "Took {} ms, compiles attempted: {}, compiles retried: {}",
      batch_compilation_time.count(),
      cinderx::getModuleState()->compile_workers_attempted.load(),
      cinderx::getModuleState()->compile_workers_retries.load());
  Py_RETURN_NONE;
}

PyObject* is_multithreaded_compile_test_enabled(PyObject*, PyObject*) {
  if (getConfig().multithreaded_compile_test) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func) {
  // There appear to be instances where the runtime is finalizing and goes to
  // destroy the cinderjit module and deopt all compiled functions, only to find
  // that some of the compiled functions have already been zeroed out and
  // possibly deallocated.  In theory this should be covered by funcDestroyed()
  // but somewhere that isn't being triggered.  This is not a good solution but
  // it fixes some shutdown crashes for now.
  if (func->func_module == nullptr && func->func_qualname == nullptr) {
    JIT_CHECK(
        Py_IsFinalizing(),
        "Trying to deopt destroyed function at {} when runtime is not "
        "finalizing",
        reinterpret_cast<void*>(func.get()));
    return false;
  }

  if (!jitCtx()->removeCompiledFunc(func)) {
    return false;
  }
  setVectorcall(func, getInterpretedVectorcall(func));
  return true;
}

void uncompileImpl(BorrowedRef<PyFunctionObject> func) {
  deoptFuncImpl(func);
  jitCtx()->forgetCode(func);
}

void uncompile(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;
  uncompileImpl(func);
}

/*
 * De-optimize a function by setting it to run through the interpreter if it
 * had been previously JIT-compiled.
 *
 * Return true if the function was previously JIT-compiled, false otherwise.
 */
bool deoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (jitCtx() && deoptFuncImpl(func)) {
    jitCtx()->addDeoptedFunc(func);
    return true;
  }
  return false;
}

void disable_jit_impl(bool deopt_all) {
  FreeThreadedJITEntrypointGuard guard;
  if (jitCtx() == nullptr) {
    return;
  }

  if (deopt_all) {
    auto& funcs = jitCtx()->compiledFuncs();
    JIT_DLOG("Deopting {} compiled functions", funcs.size());
    size_t success = 0;
    for (auto it = funcs.begin(); it != funcs.end();) {
      BorrowedRef<PyFunctionObject> func = it->first;
      // Advance before deoptFunc() which erases func from funcs,
      // invalidating the iterator pointing to it.
      ++it;
#if PY_VERSION_HEX < 0x030C0000
      // This walk is reachable from a user weak-reference callback while
      // the function it watches is mid-death (weak references clear before
      // the JIT's own callback runs).  Deopting would park a pointer whose
      // owner is already being destroyed; its callback owns the cleanup.
      if (jitCtx()->isFunctionDeathPending(func)) {
        continue;
      }
#endif
      if (deoptFunc(func)) {
        success++;
      } else {
        JIT_DLOG("Failed to deopt compiled function '{}'", funcFullname(func));
      }
    }
    JIT_DLOG("Deopted {} compiled functions", success);
  }

  if (isJitUsable()) {
    getMutableConfig().state = State::kPaused;
    getMutableConfig().osr_capable = false;
    setInterpreterJitFlag(false);
    syncOSRFlags();
    JIT_DLOG("Disabled the JIT");
  }
}

PyObject* disable_jit(PyObject* /* self */, PyObject* args, PyObject* kwargs) {
  int deopt_all = 0;

  const char* keywords[] = {"deopt_all", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|p", const_cast<char**>(keywords), &deopt_all)) {
    return nullptr;
  }

  disable_jit_impl(deopt_all);

  Py_RETURN_NONE;
}

// Walk the parked set and republish every function in it.  Returns false
// with the error pending when a republication fails (a MemoryError,
// typically): the failure must neither be swallowed nor leak into further
// C-API calls, and everything not yet reattached -- the failed function
// included -- stays parked for the next walk to retry.  Refusals report no
// error, stay parked, and do not stop the walk.
static bool reattachParkedFuncs() {
  size_t count = 0;
  // Walk a snapshot, never the live set.  reoptFunc() erases entries on
  // success, and a reattachment that takes over a prior artifact queues
  // that artifact's displaced anchor -- whose eventual release runs
  // arbitrary Python that can call disable() and INSERT into the set.  The
  // strong references keep entries alive across the walk (the set holds
  // by borrow); one that dies between snapshot and visit is skipped by the
  // still-parked check, its death having erased it from the set.
  std::vector<Ref<PyFunctionObject>> parked;
  {
    auto& funcs = jitCtx()->deoptedFuncs();
    parked.reserve(funcs.size());
    for (BorrowedRef<PyFunctionObject> func : funcs) {
#if PY_VERSION_HEX < 0x030C0000
      // A parked entry whose death is already pending -- its weak
      // reference cleared, its callback batch running, this walk reached
      // from a user callback in that batch -- must not be resurrected by
      // the snapshot's strong reference: an INCREF here revives an object
      // mid-deallocation.  Its own callback erases the entry; skip it.
      if (jitCtx()->isFunctionDeathPending(func)) {
        continue;
      }
#endif
      parked.emplace_back(Ref<PyFunctionObject>::create(func));
    }
  }
  bool ok = true;
  for (auto& func : parked) {
    if (jitCtx()->deoptedFuncs().count(func) == 0) {
      // An earlier iteration's side effects already served or removed it.
      continue;
    }
    if (reoptFunc(func)) {
      count++;
      continue;
    }
    if (PyErr_Occurred()) {
      ok = false;
      break;
    }
  }
#if PY_VERSION_HEX < 0x030C0000
  // Anchors displaced by takeovers drain only now, with no iteration
  // active.  A release can run disable() reentrantly; the caller re-checks
  // the state generation before writing any state after this returns.
  jitCtx()->drainDeferredAnchorReleases();
#endif
  JIT_DLOG("Re-optimized {} parked functions", count);
  return ok;
}

bool enable_jit_impl() {
  FreeThreadedJITEntrypointGuard guard;
  // Shadow compilation is a terminal, non-executing state on CPython 3.11.
  // It is intentionally not a paused executing JIT and must never be promoted
  // to kRunning through an internal re-enable path.
  if (isJitShadow()) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Trying to enable machine-code execution from shadow JIT mode");
    return false;
  }
  if (jitCtx() == nullptr) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Trying to re-enable the JIT but the JIT context is missing");
    return false;
  }
  // Finalization releases the registries this function walks, and it does so
  // by dropping references -- which runs destructors, which run arbitrary
  // Python.  A __del__ reaching enable() would re-arm the execute surface
  // mid-teardown and iterate a container that is being emptied underneath
  // it.  Teardown is one-way.
  if (getConfig().state == State::kFinalizing) {
    PyErr_SetString(
        PyExc_RuntimeError, "Trying to enable the JIT while it is finalizing");
    return false;
  }
#if PY_VERSION_HEX < 0x030C0000
  // The evaluator is part of what "enabled" means.  If it was handed back
  // to stock, enabling reclaims it; if a third party holds the entry
  // point, the install refuses and so does enable() -- reporting success
  // while every call runs interpreted would be the global version of the
  // per-function lie the installed-artifact predicate already refuses to
  // tell.
  if ((getConfig().state == State::kRunning ||
       getConfig().state == State::kPaused) &&
      !Ci_EvalHook311_IsInstalled() && Ci_InitFrameEvalFunc() < 0) {
    return false;
  }
#endif
  if (isJitUsable()) {
    // Already enabled -- but reattachment is still this call's job.  A
    // failed retry and a single-function deopt both leave a running JIT
    // with functions parked; without this walk they would be unreachable
    // forever, since only enable() walks the parked set.
    if (!reattachParkedFuncs()) {
      return false;
    }
#if PY_VERSION_HEX < 0x030C0000
    if (!isJitUsable()) {
      // The walk's anchor drain ran a reentrant disable().  That decision
      // is newer than this call: leave the state it wrote alone, and do
      // not report an enabled JIT that is not.
      PyErr_SetString(
          PyExc_RuntimeError,
          "a reentrant disable() during enable() left the JIT paused");
      return false;
    }
#endif
    return true;
  }

#if PY_VERSION_HEX < 0x030C0000
  // Re-arm before re-attaching, not after.  On 3.11 the execute surface is
  // only open while the state says kRunning, so a reopt loop that ran first
  // was refused for every function and enable() merely drained the parked
  // set -- pause() became a one-way door.  The flip is ordered this way
  // only here; other versions keep the order they had.
  const State state_before_flip = getConfig().state;
  setInterpreterJitFlag(true);
  getMutableConfig().state = State::kRunning;
  syncOSRFlags();
  if (!reattachParkedFuncs()) {
    // enable() either completes or leaves the world as it found it.  The
    // walk keeps whatever it could not republish parked, and the flip this
    // call performed is handed back -- flag, state and OSR wiring -- so a
    // caller that saw the exception does not find is_enabled() answering
    // true.  Functions the walk did reattach stay attached: paused with
    // installed entries is the disable(deopt_all=False) world, and the
    // next enable() finishes the remainder.  Only this call's own flip is
    // unwound; the already-running retry path above performs none, and a
    // state some newer decision wrote -- a reentrant disable() inside the
    // walk's anchor drain -- is not overwritten either.
    if (isJitUsable()) {
      setInterpreterJitFlag(false);
      getMutableConfig().state = state_before_flip;
      syncOSRFlags();
    }
    return false;
  }
  if (!isJitUsable()) {
    // The walk succeeded, but its anchor drain ran a reentrant disable().
    // That decision is newer than this call: leave the state it wrote
    // alone, and do not report an enabled JIT that is not.
    PyErr_SetString(
        PyExc_RuntimeError,
        "a reentrant disable() during enable() left the JIT paused");
    return false;
  }
#else
  // The failure precedes the flip below, so a failed enable() never turns
  // the JIT on.
  if (!reattachParkedFuncs()) {
    return false;
  }
#endif

  setInterpreterJitFlag(true);
  getMutableConfig().state = State::kRunning;
  syncOSRFlags();

  JIT_DLOG("Re-enabled the JIT");

  return true;
}

PyObject* enable_jit(PyObject* /* self */, PyObject* /* arg */) {
  if (!enable_jit_impl()) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

// Check if there are any active callback registered through
// sys.monitoring.register_callback()
bool hasRegisteredMonitoringCallbacks() {
#if PY_VERSION_HEX < 0x030C0000
  return false;
#else
  auto is = PyInterpreterState_Get();
  for (int tool_id = 0; tool_id < PY_MONITORING_TOOL_IDS; ++tool_id) {
    // Skip the internal Python tool IDs used by sys.setprofile and
    // sys.settrace as these are internal (users can't call register_callback()
    // with these tool IDs) and their registered callbacks are never cleared
    if (tool_id == PY_MONITORING_SYS_PROFILE_ID ||
        tool_id == PY_MONITORING_SYS_TRACE_ID) {
      continue;
    }
    // Skip tool IDs that have been freed via sys.monitoring.free_tool_id().
    // CPython's free_tool_id only clears monitoring_tool_names but leaves
    // stale entries in monitoring_callables, so we must check the tool name
    // to avoid treating orphaned callbacks as active instrumentation.
    if (is->monitoring_tool_names[tool_id] == nullptr) {
      continue;
    }
    for (int event_id = 0; event_id < _PY_MONITORING_EVENTS; ++event_id) {
      BorrowedRef<> entry = is->monitoring_callables[tool_id][event_id];
      if (entry != nullptr && !Py_IsNone(entry)) {
        return true;
      }
    }
  }
  return false;
#endif
}

// Check if sys.setprofile or sys.settrace have active callbacks registered.
bool hasActiveLegacyTracing() {
#if PY_VERSION_HEX < 0x030C0000
  PyThreadState* tstate = PyThreadState_Get();
  return tstate->c_profilefunc != nullptr || tstate->c_tracefunc != nullptr;
#else
  auto is = PyInterpreterState_Get();
  return is->sys_profiling_threads > 0 || is->sys_tracing_threads > 0;
#endif
}

bool isInstrumentationActive() {
  return hasRegisteredMonitoringCallbacks() || hasActiveLegacyTracing();
}

// Returns false only if enable_jit_impl() fails (with Python exception set).
bool toggleJitBasedOnInstrumentationState() {
  if (isInstrumentationActive()) {
    if (!isJitPaused()) {
      disable_jit_impl(true /* deopt_all */);
      patchJitGenAmSendForDeopt();
      deoptAllJitFramesOnStack();
    }
    return true;
  }
  unpatchJitGenAmSendForDeopt();
  return enable_jit_impl();
}

// Patched version of sys.monitoring.register_callback().
// Intercepts callback registration/deregistration to disable/enable the JIT
// This is to handle debuggers/profilers attaching or detaching.
PyObject* patched_sys_monitoring_register_callback(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->orig_sys_monitoring_register_callback;
  JIT_CHECK(
      original != nullptr,
      "Expecting to have sys.monitoring.register_callback already saved");

  // Run the original function first
  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

// Patched version of sys.monitoring.free_tool_id().
// Intercepts tool ID deallocation to re-enable the JIT when there are no more
// active monitoring callbacks. Without this, stale entries left in
// monitoring_callables by free_tool_id would cause the JIT to remain paused.
PyObject* patched_sys_monitoring_free_tool_id(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->orig_sys_monitoring_free_tool_id;
  JIT_CHECK(
      original != nullptr,
      "Expecting to have sys.monitoring.free_tool_id already saved");

  // Run the original function first
  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

// Patched version of sys.setprofile().
// Intercepts profiler registration/deregistration to disable/enable the JIT.
PyObject* patched_sys_setprofile(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->orig_sys_setprofile;
  JIT_CHECK(
      original != nullptr, "Expecting to have sys.setprofile already saved");

  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

// Patched version of sys.settrace().
// Intercepts tracer registration/deregistration to disable/enable the JIT.
PyObject* patched_sys_settrace(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->orig_sys_settrace;
  JIT_CHECK(
      original != nullptr, "Expecting to have sys.settrace already saved");

  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

void schedule_existing_functions_for_jit(uint32_t calls) {
  // Schedule all pre-existing functions for compilation.
  walkFunctionObjects(
      [](BorrowedRef<PyFunctionObject> func) { scheduleJitCompile(func); });

  JIT_DLOG("Configuring JIT to compile functions after {} calls", calls);
}

int compile_after_n_calls_impl(uint32_t calls) {
  if (Ci_InitFrameEvalFunc() < 0) {
    return -1;
  }
  configureCompileAfterNCalls(calls, false);
  schedule_existing_functions_for_jit(calls);
  return 0;
}

PyObject* compile_after_n_calls(PyObject* /* self */, PyObject* arg) {
  Py_ssize_t calls = -1;
  if (!PyArg_Parse(arg, "n:compile_after_n_calls", &calls)) {
    return nullptr;
  }
  if (calls < 0 || calls > std::numeric_limits<uint32_t>::max()) {
    PyErr_Format(
        PyExc_ValueError,
        "Cannot configure JIT to compile functions after '%zd' calls",
        calls);
    return nullptr;
  }

  if (compile_after_n_calls_impl(calls) < 0) {
    return nullptr;
  }

  Py_RETURN_NONE;
}

PyObject* auto_jit(PyObject* /* self */, PyObject* /* arg */) {
  // Default value that works well for most applications.
  if (compile_after_n_calls_impl(1000) < 0) {
    return nullptr;
  }

  Py_RETURN_NONE;
}

BorrowedRef<PyFunctionObject> get_func_arg(
    const char* method_name,
    BorrowedRef<> arg) {
  if (PyFunction_Check(arg)) {
    return BorrowedRef<PyFunctionObject>{arg};
  }
  PyErr_Format(
      PyExc_TypeError,
      "%s expected a Python function, received '%s' object",
      method_name,
      Py_TYPE(arg)->tp_name);
  return nullptr;
}

PyObject*
precompile_all(PyObject* /* self */, PyObject* args, PyObject* kwargs) {
  if (!isJitUsable()) {
    Py_RETURN_FALSE;
  }

  // Default value of 0 means to read the value from the jit::Config if it
  // exists, otherwise do it all inline on the same thread.
  Py_ssize_t workers = 0;
  const char* keywords[] = {"workers", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|n", const_cast<char**>(keywords), &workers)) {
    return nullptr;
  }
  if (workers < 0) {
    PyErr_Format(
        PyExc_ValueError,
        "Cannot call precompile_all with %ld workers",
        workers);
    return nullptr;
  }
  if (workers > 1000) {
    PyErr_Format(
        PyExc_ValueError,
        "Trying to call precompile_all with %ld workers which seems like too "
        "much",
        workers);
    return nullptr;
  }

  if (!compile_all(workers)) {
    return nullptr;
  }

  Py_RETURN_TRUE;
}

PyObject* force_compile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("force_compile", arg);
  if (func == nullptr) {
    return nullptr;
  }
  if (!isJitUsable() || isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  if (Ci_InitFrameEvalFunc() < 0) {
    return nullptr;
  }

  // Compile the function.
  Result result;
  try {
    result = compileFunction(func);
  } catch (const std::exception& exn) {
    // Surface C++ exceptions to the caller immediately.
    setRuntimeError(exn);
    return nullptr;
  }

  switch (result) {
    case Result::OK:
      Py_RETURN_TRUE;
    case Result::ALREADY_SCHEDULED:
      // Strange case, the function is being compiled by a different thread.
      // Shouldn't happen, but don't die if it does.
      Py_RETURN_FALSE;
    case Result::PAUSED:
      PyErr_SetString(
          PyExc_RuntimeError,
          "Compilation failed because the JIT was paused, but that shouldn't "
          "be possible as this case was already checked");
      return nullptr;
    case Result::CANNOT_SPECIALIZE:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_CANNOT_SPECIALIZE");
      return nullptr;
    case Result::NOT_ON_JITLIST:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_NOT_ON_JITLIST");
      return nullptr;
    case Result::UNKNOWN_ERROR:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_UNKNOWN_ERROR");
      return nullptr;
    case Result::NOT_INITIALIZED:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_NOT_INITIALIZED");
      return nullptr;
    case Result::NO_PRELOADER:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_NO_PRELOADER");
      return nullptr;
    case Result::OVER_MAX_CODE_SIZE:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_OVER_MAX_CODE_SIZE");
      return nullptr;
    case Result::PYTHON_EXCEPTION:
      return nullptr;
  }
  PyErr_Format(
      PyExc_RuntimeError,
      "Unhandled compilation result: %d",
      static_cast<int>(result));
  return nullptr;
}

PyObject* lazy_compile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("lazy_compile", arg);
  if (func == nullptr) {
    return nullptr;
  }
  FreeThreadedJITEntrypointGuard guard;

  if (!isJitUsable() || isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  if (Ci_InitFrameEvalFunc() < 0) {
    return nullptr;
  }

  setVectorcall(func, forcedJitVectorcall);
  if (!registerFunction(func)) {
    setVectorcall(func, getInterpretedVectorcall(func));
    Py_RETURN_FALSE;
  }

  Py_RETURN_TRUE;
}

namespace {
void (*s_uncompile_midpoint_hook_for_test)() = nullptr;
} // namespace

// Uncompile a function by returning it to its non-jitted version.
PyObject* force_uncompile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("force_uncompile", arg);
  if (func == nullptr) {
    return nullptr;
  }
  FreeThreadedJITEntrypointGuard guard;

#if PY_VERSION_HEX < 0x030C0000
  // Gate on retained state, not the call predicate: isJitCompiled() is
  // deliberately false while parked / after a __code__ swap / with the
  // evaluator away, yet those states retain exactly what uncompile must
  // remove -- gated on it, the artifact revived on the way back.
  if (jitCtx() == nullptr || !jitCtx()->hasCompilationState(func)) {
    Py_RETURN_FALSE;
  }
#else
  if (!isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }
#endif

  // Pin the function for the duration: a last external reference dropped
  // mid-operation must not free what the remaining steps read.
  Ref<PyFunctionObject> keep = Ref<PyFunctionObject>::create(func);

#if PY_VERSION_HEX < 0x030C0000
  // Capture the claimed artifact BEFORE unpublication severs the
  // association -- the association is authoritative; func_code is mutable
  // and after a swap names another function's code (retiring by that key
  // cleared the donor's artifact).  Doubles as the retirement pin.
  Ref<jit::CompiledFunction> claimed;
  {
    BorrowedRef<jit::CompiledFunction> assoc = jitCtx()->findAssociated(func);
    if (assoc != nullptr) {
      claimed = Ref<jit::CompiledFunction>::create(assoc);
    }
  }
#endif

  // Replace the function entrypoint with the interpreter entrypoint, so that it
  // can properly be called again.
  setVectorcall(func, getInterpretedVectorcall(func));

  // Unpublish the function: erase the JIT's records of it.  This is not a
  // death and must not count as one.
  funcUnpublished(func);

  if (s_uncompile_midpoint_hook_for_test != nullptr) {
    s_uncompile_midpoint_hook_for_test();
  }

  if (jitCtx() != nullptr) {
#if PY_VERSION_HEX < 0x030C0000
    // Retire the dictionary anchor (what keeps the artifact resident),
    // detained in a local pin: released in place it would run arbitrary
    // Python mid-operation.  A local needs no allocation, so no failure.
    Ref<> detained_anchor;
    if (func->func_dict != nullptr) {
      BorrowedRef<> anchored =
          PyDict_GetItemWithError(func->func_dict, jit::kCompiledFunctionKey);
      if (anchored == nullptr && PyErr_Occurred()) {
        PyErr_Clear();
      }
      // Only the claimed artifact's own anchor is ours: the key is
      // user-writable state, and a forged foreign value may be the last
      // reference to ANOTHER function's artifact -- releasing it would
      // uncompile that function.  Anything else stays as the user wrote
      // it (retirement goes by identity, not by this key).
      if (anchored != nullptr && claimed != nullptr &&
          anchored.get() == reinterpret_cast<PyObject*>(claimed.get())) {
        detained_anchor = Ref<>::create(anchored);
        if (PyDict_DelItem(func->func_dict, jit::kCompiledFunctionKey) < 0) {
          PyErr_Clear();
        }
      }
    }
    // Retire by artifact identity: the entry-point bookkeeping, then the
    // compiled-codes entry of the artifact the association CLAIMED --
    // keyed from the artifact's own runtime, never rebuilt from the
    // function's mutable func_code.
    deoptFuncImpl(func);
    if (claimed != nullptr) {
      jitCtx()->forgetCodeForArtifact(func, claimed);
    }
    // Settlement: every structure is consistent.  The pins release here --
    // either can be the artifact's last reference, and whatever their
    // destruction runs sees the finished uncompilation.  Residue from the
    // destruction drains with it.
    claimed.reset();
    detained_anchor.reset();
    jitCtx()->drainDeferredAnchorReleases();
    // Verdict AFTER the releases: a __del__ there can reenter
    // force_compile(), and a rebuilt compilation state is a newer decision
    // this operation must not report over (symmetric to force_compile's
    // post-drain re-verification).
    if (jitCtx()->hasCompilationState(func)) {
      PyErr_SetString(
          PyExc_RuntimeError, "function was recompiled during force_uncompile");
      return nullptr;
    }
#else
    uncompileImpl(func);
#endif
  }

  Py_RETURN_TRUE;
}

int aot_func_visitor(PyObject* obj, void* arg) {
  constexpr int kGcVisitContinue = 1;

  auto aot_ctx = reinterpret_cast<AotContext*>(arg);
  if (!PyFunction_Check(obj)) {
    return kGcVisitContinue;
  }

  BorrowedRef<PyFunctionObject> func{obj};
  auto func_state = aot_ctx->lookupFuncState(func);
  if (func_state != nullptr) {
    setVectorcall(func, func_state->normalEntry());
  }
  return kGcVisitContinue;
}

#ifndef WIN32
PyObject* load_aot_bundle(PyObject* /* self */, PyObject* arg) {
  JIT_CHECK(
      jitCtx() != nullptr,
      "Loading an AOT bundle currently requires the JIT to be enabled");

  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(
        PyExc_ValueError, "load_aot_bundle expects a filename string");
    return nullptr;
  }

  const char* filename = PyUnicode_AsUTF8(arg);

  // TASK(T193992967): Verify these options are what we want.
  auto handle = dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    std::string msg = fmt::format(
        "Failed to dlopen() the AOT bundle at {}\n{}", filename, dlerror());
    PyErr_SetString(PyExc_RuntimeError, msg.c_str());
    return nullptr;
  }

  g_aot_ctx.init(handle);

  // TASK(T193608222): It would be great to do something other than mmapping the
  // entire file into memory, especially since we just loaded it in via
  // dlopen().
  MmapFile file;
  try {
    file.open(filename);
  } catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }

  // Find the function metadata section.
  std::span<const std::byte> note_span;
  try {
    note_span = elf::findSection(file.data(), elf::kFuncNoteSectionName);
  } catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }
  if (note_span.empty()) {
    PyErr_SetString(
        PyExc_RuntimeError, "Cannot find note section for function metadata");
    return nullptr;
  }

  elf::NoteArray note_array = elf::readNoteSection(note_span);

  // Populate AotContext with data from the note section.
  for (const elf::Note& note : note_array.notes()) {
    g_aot_ctx.registerFunc(note);
  }

  // Now map compiled functions to existing PyFunctionObjects.
  //
  // TASK(T193992967): This is terrible and we should be going the other way,
  // mapping read notes over to function objects.
  PyUnstable_GC_VisitObjects(aot_func_visitor, &g_aot_ctx);

  Py_RETURN_NONE;
}
#endif

PyObject* get_compile_after_n_calls(PyObject* /* self */, PyObject*) {
  auto limit = getConfig().compile_after_n_calls;
  if (limit.has_value()) {
    return PyLong_FromLong(*limit);
  }
  Py_RETURN_NONE;
}

PyObject* is_enabled(PyObject* /* self */, PyObject* /* args */) {
#if PY_VERSION_HEX < 0x030C0000
  // Execution-usable, not merely configured: without the frame evaluator
  // no call reaches machine code, and the per-function predicate already
  // answers false for every function.  The global answer must not
  // disagree with all of its parts -- pause() and the test harnesses use
  // it to decide whether a real JIT capability is present.
  return PyBool_FromLong(isJitUsable() && Ci_EvalHook311_IsInstalled());
#else
  return PyBool_FromLong(isJitUsable());
#endif
}

PyObject* is_attr_caches_enabled(PyObject* /* self */, PyObject* /* args */) {
  // Read-only config export so the MR-04 acceptance ("the 3.11 default
  // keeps inline attribute caches off until MR-09") is attested inside
  // the child process rather than assumed.
  return PyBool_FromLong(getConfig().attr_caches);
}

PyObject* count_interpreted_calls(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func =
      get_func_arg("count_interpreted_calls", arg);
  if (func == nullptr) {
    return nullptr;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  return PyLong_FromLong(static_cast<long>(codeCallCount(code)));
}

PyObject* is_jit_compiled(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("is_jit_compiled", arg);
  return func != nullptr ? PyBool_FromLong(isJitCompiled(func)) : nullptr;
}

PyObject* set_max_code_size(PyObject* /* self */, PyObject* arg) {
  Py_ssize_t new_size;
  if (!PyArg_Parse(arg, "n:set_max_code_size", &new_size)) {
    return nullptr;
  }
  if (new_size < 0) {
    PyErr_Format(
        PyExc_ValueError, "max_code_size cannot be negative: %zd", new_size);
    return nullptr;
  }
  getMutableConfig().max_code_size = static_cast<size_t>(new_size);
  Py_RETURN_NONE;
}

PyObject* print_hir(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->printHIR();
  Py_RETURN_NONE;
}

PyObject* disassemble(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->disassemble();
  Py_RETURN_NONE;
}

PyObject* test_parse_thread_state_prologue(PyObject* /* self */, PyObject* arg) {
  Ref<> seq = Ref<>::steal(PySequence_Fast(
      arg, "_test_parse_thread_state_prologue expects an iterable"));
  if (seq == nullptr) {
    return nullptr;
  }

  Py_ssize_t size = PySequence_Fast_GET_SIZE(seq.get());
  if (size <= 0) {
    PyErr_SetString(PyExc_ValueError, "instruction sequence cannot be empty");
    return nullptr;
  }

  std::vector<uint32_t> insns;
  insns.reserve(size);
  PyObject** items = PySequence_Fast_ITEMS(seq.get());
  for (Py_ssize_t i = 0; i < size; i++) {
    unsigned long value = PyLong_AsUnsignedLong(items[i]);
    if (PyErr_Occurred()) {
      return nullptr;
    }
    if (value > std::numeric_limits<uint32_t>::max()) {
      PyErr_SetString(PyExc_ValueError, "instruction does not fit in uint32_t");
      return nullptr;
    }
    insns.push_back(static_cast<uint32_t>(value));
  }

#if defined(CINDER_AARCH64)
  auto offset =
      jit::codegen::parseThreadStatePrologue(insns.data(), insns.size());
  if (!offset.has_value()) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLong(*offset);
#else
  Py_RETURN_NONE;
#endif
}

PyObject* test_set_thread_state_offset(PyObject* /* self */, PyObject* arg) {
  long offset = PyLong_AsLong(arg);
  if (PyErr_Occurred()) {
    return nullptr;
  }
  if (offset < std::numeric_limits<int32_t>::min() ||
      offset > std::numeric_limits<int32_t>::max()) {
    PyErr_SetString(PyExc_ValueError, "offset does not fit in int32_t");
    return nullptr;
  }
  auto module_state = cinderx::getModuleState();
  module_state->tstate_offset = static_cast<int32_t>(offset);
  module_state->tstate_offset_inited = true;
  Py_RETURN_NONE;
}

#ifndef WIN32
PyObject* dump_elf(PyObject* /* self */, PyObject* arg) {
  JIT_CHECK(
      jitCtx() != nullptr,
      "JIT context not initialized despite cinderjit module having been "
      "loaded");
  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(PyExc_ValueError, "dump_elf expects a filename string");
    return nullptr;
  }

  Py_ssize_t filename_size = 0;
  const char* filename = PyUnicode_AsUTF8AndSize(arg, &filename_size);

  std::vector<elf::CodeEntry> entries;
  for (auto func_and_compiled : jitCtx()->compiledFuncs()) {
    auto func = func_and_compiled.first;
    BorrowedRef<PyCodeObject> code{func->func_code};
    CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);

    elf::CodeEntry entry;
    entry.code = code;
    entry.compiled_code = compiled_func->codeBuffer();
    entry.normal_entry =
        reinterpret_cast<void*>(compiled_func->vectorcallEntry());
    entry.static_entry = compiled_func->staticEntry();
    entry.func_name = funcFullname(func);
    if (code->co_filename != nullptr && PyUnicode_Check(code->co_filename)) {
      entry.file_name = unicodeAsString(code->co_filename);
    }
    entry.lineno = code->co_firstlineno;

    entries.emplace_back(std::move(entry));
  }

  std::ofstream out{filename};
  elf::writeEntries(out, entries);

  Py_RETURN_NONE;
}
#endif

PyObject* get_jit_list(PyObject* /* self */, PyObject*) {
  if (auto jit_list = cinderx::getModuleState()->jit_list.get()) {
    return jit_list->getList().release();
  }

  Py_RETURN_NONE;
}

// Create a new JIT list if one doesn't exist yet, returning true if a new list
// was made.
bool ensureJitList() {
  if (cinderx::getModuleState()->jit_list.get() != nullptr) {
    return false;
  }
  std::unique_ptr<JITList> jit_list;
  if (getConfig().allow_jit_list_wildcards) {
    jit_list = WildcardJITList::create();
  } else {
    jit_list = JITList::create();
  }
  cinderx::getModuleState()->jit_list = std::move(jit_list);
  return true;
}

void deleteJitList() {
  cinderx::getModuleState()->jit_list = nullptr;
}

// Reschedule all functions on the JIT list for compilation.  Run when the JIT
// list is modified.
int rescheduleJitList() {
  if (Ci_InitFrameEvalFunc() < 0) {
    return -1;
  }

  walkFunctionObjects([](BorrowedRef<PyFunctionObject> func) {
    auto jit_list = cinderx::getModuleState()->jit_list.get();
    if (jit_list->lookupFunc(func)) {
      scheduleJitCompile(func);
    }
  });

  return 0;
}

PyObject* append_jit_list(PyObject* /* self */, PyObject* arg) {
  if (!PyUnicode_Check(arg)) {
    PyErr_Format(
        PyExc_TypeError,
        "append_jit_list expected a file path string, received '%s' object",
        Py_TYPE(arg)->tp_name);
    return nullptr;
  }

  Py_ssize_t line_len;
  const char* line_str = PyUnicode_AsUTF8AndSize(arg, &line_len);
  if (line_str == nullptr) {
    return nullptr;
  }
  std::string_view line{
      line_str, static_cast<std::string::size_type>(line_len)};

  bool new_list = ensureJitList();

  // Parse in the new line.  If that fails and a new list was created, delete
  // it.
  auto jit_list = cinderx::getModuleState()->jit_list.get();
  if (!jit_list->parseLine(line)) {
    if (new_list) {
      deleteJitList();
    }
    PyErr_Format(
        PyExc_RuntimeError, "Failed to parse new JIT list line %U", arg);
    return nullptr;
  }

  if (rescheduleJitList() < 0) {
    return nullptr;
  }

  Py_RETURN_NONE;
}

PyObject* read_jit_list(PyObject* /* self */, PyObject* arg) {
  if (!PyUnicode_Check(arg)) {
    PyErr_Format(
        PyExc_TypeError,
        "read_jit_list expected a file path string, received '%s' object",
        Py_TYPE(arg)->tp_name);
    return nullptr;
  }

  const char* path = PyUnicode_AsUTF8(arg);
  if (path == nullptr) {
    return nullptr;
  }

  // Create a new JIT list if one doesn't exist yet.
  bool new_list = ensureJitList();

  // Parse in the new file.  If that fails and a new list was created, delete
  // it.
  auto jit_list = cinderx::getModuleState()->jit_list.get();
  try {
    jit_list->parseFile(path);
  } catch (const std::exception& exn) {
    if (new_list) {
      deleteJitList();
    }

    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }

  if (rescheduleJitList() < 0) {
    return nullptr;
  }

  Py_RETURN_NONE;
}

// The registry as it stands, unfiltered.  get_compiled_functions() answers
// the logical question -- will a call run machine code -- and a function
// that temporarily fell back (its code or globals changed, say) drops out
// of it while its artifact and code buffer are still very much resident.
// Telling the two apart is what makes a lifecycle report mean anything.
PyObject* get_resident_compiled_functions(PyObject* /* self */, PyObject*) {
  // A physical measurement, so it must not depend on the JIT's current
  // state OR its registries: pausing does not release a code buffer, and
  // neither does force_uncompile() while an external reference pins the
  // artifact -- the registry entry is gone, the machine code is not.  The
  // gauge is maintained on the buffer's real lifetime (acquired with the
  // artifact, released in its destructor), so it answers "how much
  // executable memory is still alive", not "which functions would run"
  // and not "what does the registry remember".
  return PyLong_FromUnsignedLongLong(
      jit::triggerStatsSnapshot().resident_code_buffers);
}

PyObject* get_compiled_functions(PyObject* /* self */, PyObject*) {
  auto funcs = Ref<>::steal(PyList_New(0));
  if (funcs == nullptr) {
    return nullptr;
  }
  for (auto func_and_compiled : jitCtx()->compiledFuncs()) {
#if PY_VERSION_HEX < 0x030C0000
    // Never hand out a function that is already being destroyed.  This can
    // run from a weak-reference callback, which CPython invokes from inside
    // func_dealloc after temporarily resurrecting the object to a reference
    // count of one; appending it here would take that count to two, and the
    // restore that follows the callbacks frees it anyway -- leaving this
    // list holding freed memory.  func_dealloc untracks before it runs the
    // callbacks, so "not tracked" is exactly "being destroyed" for a type
    // that is otherwise always tracked.
    if (!PyObject_GC_IsTracked(func_and_compiled.first)) {
      continue;
    }
    // The untracked check catches an ordinary dealloc, where func_dealloc
    // untracks before it runs the weak-reference callbacks.  A cyclic
    // collection is the other way around: the collector clears the weak
    // references of the doomed, and runs the externally rooted callbacks,
    // before anything is untracked -- so a query from such a callback sees
    // a condemned function still tracked, and appending it here would
    // resurrect it out of the garbage set.  The JIT's own death watch is
    // the oracle for that state: its weak reference is cleared, the
    // callback has not landed, the death is already in flight.
    if (jitCtx()->isFunctionDeathPending(func_and_compiled.first)) {
      continue;
    }
    // Report what is actually installed.  A function whose code or globals
    // changed since it compiled still holds a registry entry, but every
    // call to it now goes to the interpreter, so listing it here would
    // contradict is_jit_compiled() and inflate the installed count.
    if (!isJitCompiled(func_and_compiled.first)) {
      continue;
    }
#endif
    if (PyList_Append(funcs, func_and_compiled.first) < 0) {
      return nullptr;
    }
  }
  return funcs.release();
}

PyObject* get_compilation_time(PyObject* /* self */, PyObject*) {
  auto time = jitCtx() != nullptr ? jitCtx()->totalCompileTime()
                                  : std::chrono::milliseconds::zero();
  return PyLong_FromLong(time.count());
}

PyObject* get_function_compilation_time(PyObject* /* self */, PyObject* arg) {
  if (arg == nullptr || !PyFunction_Check(arg) || jitCtx() == nullptr) {
    Py_RETURN_NONE;
  }

  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  auto compile_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      compiled_func->compileTime());
  return PyLong_FromLong(compile_time.count());
}

PyObject* get_inlined_functions_stats(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  auto const& stats = compiled_func->inlinedFunctionsStats();
  auto py_stats = Ref<>::steal(PyDict_New());
  if (py_stats == nullptr) {
    return nullptr;
  }
  auto num_inlined_functions =
      Ref<>::steal(PyLong_FromSize_t(stats.num_inlined_functions));
  if (num_inlined_functions == nullptr) {
    return nullptr;
  }
  if (PyDict_SetItemString(
          py_stats, "num_inlined_functions", num_inlined_functions) < 0) {
    return nullptr;
  }
  auto failure_stats = Ref<>::steal(PyDict_New());
  if (failure_stats == nullptr) {
    return nullptr;
  }
  for (const auto& [reason, functions] : stats.failure_stats) {
    auto py_failure_reason =
        Ref<>::steal(PyUnicode_InternFromString(getInlineFailureName(reason)));
    if (py_failure_reason == nullptr) {
      return nullptr;
    }
    auto py_functions_set = Ref<>::steal(PySet_New(nullptr));
    if (py_functions_set == nullptr) {
      return nullptr;
    }
    if (PyDict_SetItem(failure_stats, py_failure_reason, py_functions_set) <
        0) {
      return nullptr;
    }
    for (const auto& function : functions) {
      auto py_function = Ref<>::steal(PyUnicode_FromString(function.c_str()));
      if (PySet_Add(py_functions_set, py_function) < 0) {
        return nullptr;
      }
    }
  }
  if (PyDict_SetItemString(py_stats, "failure_stats", failure_stats) < 0) {
    return nullptr;
  }
  return py_stats.release();
}

PyObject* get_num_inlined_functions(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    return PyLong_FromLong(0);
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr
      ? compiled_func->inlinedFunctionsStats().num_inlined_functions
      : 0;
  return PyLong_FromLong(size);
}

PyObject* get_function_hir_opcode_counts(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  const hir::OpcodeCounts& counts = compiled_func->hirOpcodeCounts();
  Ref<> dict = Ref<>::steal(PyDict_New());
  if (dict == nullptr) {
    return nullptr;
  }
#define HIR_OP(OPNAME)                                                 \
  {                                                                    \
    const size_t idx = static_cast<size_t>(hir::Opcode::k##OPNAME);    \
    if (int count = counts.at(idx); count != 0) {                      \
      Ref<> count_obj = Ref<>::steal(PyLong_FromLong(count));          \
      if (count_obj == nullptr) {                                      \
        return nullptr;                                                \
      }                                                                \
      auto opname = Ref<>::steal(PyUnicode_InternFromString(#OPNAME)); \
      if (opname == nullptr) {                                         \
        return nullptr;                                                \
      }                                                                \
      if (PyDict_SetItem(dict, opname, count_obj) < 0) {               \
        return nullptr;                                                \
      }                                                                \
    }                                                                  \
  }
  FOREACH_OPCODE(HIR_OP)
#undef HIR_OP
  return dict.release();
}

PyObject* mlock_profiler_dependencies(PyObject* /* self */, PyObject*) {
  if (jitCtx() == nullptr) {
    Py_RETURN_NONE;
  }
  jitCtx()->mlockProfilerDependencies();
  Py_RETURN_NONE;
}

PyObject* page_in_profiler_dependencies(PyObject*, PyObject*) {
  Ref<> qualnames = jitCtx()->pageInProfilerDependencies();
  return qualnames.release();
}

// Simple wrapper functions to turn nullptr or -1 return values from C-API
// functions into a thrown exception. Meant for repetitive runs of C-API calls
// and not intended for use in public APIs.
class CAPIError : public std::exception {};

PyObject* check(PyObject* obj) {
  if (obj == nullptr) {
    throw CAPIError();
  }
  return obj;
}

int check(int ret) {
  if (ret < 0) {
    throw CAPIError();
  }
  return ret;
}

// Appends deopt event dicts to stats: one per guilty type if profiled,
// or a single "<none>" event otherwise.
void collect_deopt_stat(
    const DeoptStat& stat,
    const DeoptMetadata& meta,
    std::size_t deopt_idx,
    BorrowedRef<> stats) {
  DEFINE_STATIC_STRING(bc_offset);
  DEFINE_STATIC_STRING(count);
  DEFINE_STATIC_STRING(deopt_idx);
  DEFINE_STATIC_STRING(description);
  DEFINE_STATIC_STRING(filename);
  DEFINE_STATIC_STRING(func_qualname);
  DEFINE_STATIC_STRING(guilty_type);
  DEFINE_STATIC_STRING(lineno);
  DEFINE_STATIC_STRING(normal);
  DEFINE_STATIC_STRING(int);
  DEFINE_STATIC_STRING(nonce);
  DEFINE_STATIC_STRING(opcode);
  DEFINE_STATIC_STRING(reason);
  DEFINE_STATIC_STRING(specialized_opcode);

  const DeoptFrameMetadata& frame_meta = meta.innermostFrame();
  BorrowedRef<PyCodeObject> code = frame_meta.code;

  auto func_qualname = code->co_qualname;
  BCOffset line_offset = frame_meta.cause_instr_idx;
  int opcode_raw = -1;
  int specialized_opcode_raw = -1;
  if (line_offset.value() >= 0) {
    BytecodeInstruction instr{code, line_offset};
    opcode_raw = instr.opcode();
    specialized_opcode_raw = instr.specializedOpcode();
  }
  int lineno_raw = code->co_linetable != nullptr
      ? PyCode_Addr2Line(code, line_offset.value())
      : -1;
  auto lineno = Ref<>::steal(check(PyLong_FromLong(lineno_raw)));
  auto bc_offset = Ref<>::steal(check(PyLong_FromLong(line_offset.value())));
  auto deopt_idx_obj = Ref<>::steal(check(PyLong_FromSize_t(deopt_idx)));
  auto nonce = Ref<>::steal(check(PyLong_FromSize_t(meta.nonce)));
  auto opcode = Ref<>::steal(check(PyLong_FromLong(opcode_raw)));
  auto specialized_opcode =
      Ref<>::steal(check(PyLong_FromLong(specialized_opcode_raw)));
  auto reason =
      Ref<>::steal(check(PyUnicode_FromString(deoptReasonName(meta.reason))));
  auto description = Ref<>::steal(check(PyUnicode_FromString(meta.descr)));

  // Helper to create an event dict with a given count value.
  auto append_event = [&](size_t count_raw, const char* type_name) {
    auto event = Ref<>::steal(check(PyDict_New()));
    auto normals = Ref<>::steal(check(PyDict_New()));
    auto ints = Ref<>::steal(check(PyDict_New()));

    check(PyDict_SetItem(event, s_normal, normals));
    check(PyDict_SetItem(event, s_int, ints));
    check(PyDict_SetItem(normals, s_func_qualname, func_qualname));
    check(PyDict_SetItem(normals, s_filename, code->co_filename));
    check(PyDict_SetItem(ints, s_lineno, lineno));
    check(PyDict_SetItem(ints, s_bc_offset, bc_offset));
    check(PyDict_SetItem(ints, s_deopt_idx, deopt_idx_obj));
    check(PyDict_SetItem(ints, s_nonce, nonce));
    check(PyDict_SetItem(ints, s_opcode, opcode));
    check(PyDict_SetItem(ints, s_specialized_opcode, specialized_opcode));
    check(PyDict_SetItem(normals, s_reason, reason));
    check(PyDict_SetItem(normals, s_description, description));

    auto count = Ref<>::steal(check(PyLong_FromSize_t(count_raw)));
    check(PyDict_SetItem(ints, s_count, count));
    auto type_str = Ref<>::steal(check(PyUnicode_InternFromString(type_name)));
    check(PyDict_SetItem(normals, s_guilty_type, type_str));
    check(PyList_Append(stats, event));
  };

  // For deopts with type profiles, add a copy of the dict with counts for
  // each type, including "other".
  if (!stat.types.empty()) {
    for (size_t i = 0; i < stat.types.size && stat.types.types[i] != nullptr;
         ++i) {
      append_event(
          stat.types.counts[i], typeFullname(stat.types.types[i]).c_str());
    }
    if (stat.types.other > 0) {
      append_event(stat.types.other, "<other>");
    }
  } else {
    append_event(stat.count, "<none>");
  }
}

Ref<> make_deopt_stats() {
  CompilerContext<Compiler>* ctx = jitCtx();
  auto stats = Ref<>::steal(check(PyList_New(0)));

  for (auto& pair : jitCtx()->compiledCodes()) {
    const BorrowedRef<CompiledFunction> compiled_func = pair.second;
    const CodeRuntime* code_runtime = compiled_func->runtime();

    auto const& deopt_metadatas = code_runtime->deoptMetadatas();
    for (size_t deopt_idx = 0; deopt_idx < deopt_metadatas.size();
         ++deopt_idx) {
      const DeoptMetadata& meta = deopt_metadatas[deopt_idx];

      ctx->ifDeoptStat(code_runtime, deopt_idx, [&](const auto& stat) {
        collect_deopt_stat(stat, meta, deopt_idx, stats);
      });
    }
  }

  ctx->clearDeoptStats();

  return stats;
}

PyObject* get_and_clear_runtime_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  try {
    Ref<> deopt_stats = make_deopt_stats();
    check(PyDict_SetItemString(stats, "deopt", deopt_stats));
  } catch (const CAPIError&) {
    return nullptr;
  }

  return stats.release();
}

PyObject* clear_runtime_stats(PyObject* /* self */, PyObject*) {
  jitCtx()->clearDeoptStats();
  Py_RETURN_NONE;
}

PyObject* autojit_gate_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  if (setAutoJitGateStat(
          stats, "jit_vectorcall", g_auto_jit_gate_stats.jit_vectorcall) != 0 ||
      setAutoJitGateStat(
          stats,
          "global_threshold_return",
          g_auto_jit_gate_stats.global_threshold_return) != 0 ||
      setAutoJitGateStat(
          stats,
          "classified_schedule_cold_skip",
          g_auto_jit_gate_stats.classified_schedule_cold_skip) != 0 ||
      setAutoJitGateStat(
          stats,
          "classified_warmup_return",
          g_auto_jit_gate_stats.classified_warmup_return) != 0 ||
      setAutoJitGateStat(
          stats,
          "classified_defer_freeze",
          g_auto_jit_gate_stats.classified_defer_freeze) != 0 ||
      setAutoJitGateStat(
          stats, "forced_compile", g_auto_jit_gate_stats.forced_compile) != 0 ||
      setAutoJitGateStat(
          stats,
          "forced_compile_ok",
          g_auto_jit_gate_stats.forced_compile_ok) != 0 ||
      setAutoJitGateStat(
          stats,
          "forced_compile_fallback",
          g_auto_jit_gate_stats.forced_compile_fallback) != 0 ||
      setAutoJitGateStat(
          stats, "roi_uncompile", g_auto_jit_gate_stats.roi_uncompile) != 0 ||
      setAutoJitGateStat(
          stats, "roi_recompile", g_auto_jit_gate_stats.roi_recompile) != 0 ||
      setAutoJitGateStat(
          stats, "roi_frozen", g_auto_jit_gate_stats.roi_frozen) != 0) {
    return nullptr;
  }

  return stats.release();
}

PyObject* clear_autojit_gate_stats(PyObject* /* self */, PyObject*) {
  clearAutoJitGateStats();
  g_auto_jit_gate_stats_enabled.store(true, std::memory_order_relaxed);
  Py_RETURN_NONE;
}

PyObject* get_compiled_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->codeSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_stack_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->stackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_spill_stack_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->spillStackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* jit_frame_mode(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(static_cast<int>(getConfig().frame_mode));
}

PyObject* is_lightweight_frames_enabled(PyObject* /* self */, PyObject*) {
  return PyBool_FromLong(isLightweightFramesCompiledIn());
}

PyObject* get_and_clear_inline_cache_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  auto make_inline_cache_stats = [](PyObject* stats, CacheStats& cache_stats) {
    auto result = Ref<>::steal(check(PyDict_New()));
    auto filename = Ref<>::steal(
        check(PyUnicode_InternFromString(cache_stats.filename.c_str())));
    check(PyDict_SetItemString(result, "filename", filename));
    auto method = Ref<>::steal(
        check(PyUnicode_InternFromString(cache_stats.method_name.c_str())));
    check(PyDict_SetItemString(result, "method", method));
    auto cache_misses_dict = Ref<>::steal(check(PyDict_New()));
    check(PyDict_SetItemString(result, "cache_misses", cache_misses_dict));
    for (auto& [key, miss] : cache_stats.misses) {
      auto py_key = Ref<>::steal(check(PyUnicode_FromString(key.c_str())));
      auto miss_dict = Ref<>::steal(check(PyDict_New()));
      auto count = Ref<>::steal(check(PyLong_FromLong(miss.count)));
      check(PyDict_SetItemString(miss_dict, "count", count));
      auto reason = Ref<>::steal(check(PyUnicode_InternFromString(
          std::string(cacheMissReason(miss.reason)).c_str())));
      check(PyDict_SetItemString(miss_dict, "reason", reason));

      check(PyDict_SetItem(cache_misses_dict, py_key, miss_dict));
    }
    check(PyList_Append(stats, result));
  };
  auto load_method_stats = Ref<>::steal(check(PyList_New(0)));
  check(PyDict_SetItemString(stats, "load_method_stats", load_method_stats));
  for (auto& cache_stats : jitCtx()->getAndClearLoadMethodCacheStats()) {
    make_inline_cache_stats(load_method_stats, cache_stats);
  }

  auto load_type_method_stats = Ref<>::steal(check(PyList_New(0)));
  check(PyDict_SetItemString(
      stats, "load_type_method_stats", load_type_method_stats));
  for (auto& cache_stats : jitCtx()->getAndClearLoadTypeMethodCacheStats()) {
    make_inline_cache_stats(load_type_method_stats, cache_stats);
  }

  return stats.release();
}

PyObject* jit_suppress(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("jit_suppress", arg);
  if (func == nullptr) {
    return nullptr;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  code->co_flags |= CI_CO_SUPPRESS_JIT;

  Py_INCREF(arg);
  return arg;
}

// Unsuppress a function that was suppressed by jit_suppress. This will allow it
// to be compiled in the future.
PyObject* jit_unsuppress(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("jit_unsuppress", arg);
  if (func == nullptr) {
    return nullptr;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  code->co_flags &= ~CI_CO_SUPPRESS_JIT;

  Py_INCREF(arg);
  return arg;
}

PyObject* get_allocator_stats(PyObject*, PyObject*) {
  auto base_allocator = cinderx::getModuleState()->code_allocator.get();
  if (base_allocator == nullptr) {
    Py_RETURN_NONE;
  }

  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  auto used_bytes =
      Ref<>::steal(PyLong_FromSize_t(base_allocator->usedBytes()));
  if (used_bytes == nullptr ||
      PyDict_SetItemString(stats, "used_bytes", used_bytes) < 0) {
    return nullptr;
  }
  auto max_bytes = Ref<>::steal(PyLong_FromSize_t(getConfig().max_code_size));
  if (max_bytes == nullptr ||
      PyDict_SetItemString(stats, "max_bytes", max_bytes) < 0) {
    return nullptr;
  }

  auto allocator = dynamic_cast<CodeAllocatorCinder*>(base_allocator);
  if (allocator == nullptr) {
    return stats.release();
  }

  auto lost_bytes = Ref<>::steal(PyLong_FromSize_t(allocator->lostBytes()));
  if (lost_bytes == nullptr ||
      PyDict_SetItemString(stats, "lost_bytes", lost_bytes) < 0) {
    return nullptr;
  }
  auto fragmented_allocs =
      Ref<>::steal(PyLong_FromSize_t(allocator->fragmentedAllocs()));
  if (fragmented_allocs == nullptr ||
      PyDict_SetItemString(stats, "fragmented_allocs", fragmented_allocs) < 0) {
    return nullptr;
  }
  auto huge_allocs = Ref<>::steal(PyLong_FromSize_t(allocator->hugeAllocs()));
  if (huge_allocs == nullptr ||
      PyDict_SetItemString(stats, "huge_allocs", huge_allocs) < 0) {
    return nullptr;
  }
  return stats.release();
}

PyObject* is_hir_inliner_enabled(PyObject* /* self */, PyObject*) {
  if (getConfig().hir_opts.inliner) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* is_inline_cache_stats_collection_enabled(
    PyObject* /* self */,
    PyObject* /* arg */) {
  return PyBool_FromLong(getConfig().collect_attr_cache_stats);
}

PyObject* enable_hir_inliner(PyObject* /* self */, PyObject*) {
  getMutableConfig().hir_opts.inliner = true;
  Py_RETURN_NONE;
}

PyObject* disable_hir_inliner(PyObject* /* self */, PyObject*) {
  getMutableConfig().hir_opts.inliner = false;
  Py_RETURN_NONE;
}

// Enable the emit_type_annotation_guards configuration option.
PyObject* enable_emit_type_annotation_guards(PyObject* /* self */, PyObject*) {
  getMutableConfig().emit_type_annotation_guards = true;
  Py_RETURN_NONE;
}

// Disable the emit_type_annotation_guards configuration option.
PyObject* disable_emit_type_annotation_guards(PyObject* /* self */, PyObject*) {
  getMutableConfig().emit_type_annotation_guards = false;
  Py_RETURN_NONE;
}

PyObject* enable_specialized_opcodes(PyObject* /* self */, PyObject*) {
  getMutableConfig().specialized_opcodes = true;
  Py_RETURN_NONE;
}

PyObject* disable_specialized_opcodes(PyObject* /* self */, PyObject*) {
  getMutableConfig().specialized_opcodes = false;
  Py_RETURN_NONE;
}

// If the given generator-like object is a suspended JIT generator, deopt it
// and return 1. Otherwise, return 0.
int deopt_gen_impl(PyGenObject* gen) {
  // deopt_jit_gen optimistically succeeds when the generator isn't a JIT
  // generator.
  return JitGenObject::cast(gen) != nullptr && deopt_jit_gen(gen);
}

PyObject* deopt_gen(PyObject*, PyObject* op) {
  if (!PyGen_Check(op) && !PyCoro_CheckExact(op) &&
      !PyAsyncGen_CheckExact(op) && !JitGen_CheckAny(op)) {
    PyErr_Format(
        PyExc_TypeError,
        "Exected generator-like object, got %.200s",
        Py_TYPE(op)->tp_name);
    return nullptr;
  }
  auto gen = reinterpret_cast<PyGenObject*>(op);
  if (gen->gi_frame_state == FRAME_EXECUTING) {
    PyErr_SetString(PyExc_RuntimeError, "generator is executing");
    return nullptr;
  }
  if (deopt_gen_impl(gen)) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

int deopt_gen_visitor(PyObject* obj, void*) {
  if (PyGen_Check(obj) || PyCoro_CheckExact(obj) ||
      PyAsyncGen_CheckExact(obj) || JitGen_CheckAny(obj)) {
    deopt_gen_impl(reinterpret_cast<PyGenObject*>(obj));
  }
  return 1;
}

PyObject* after_fork_child(PyObject*, PyObject*) {
#ifndef WIN32
  perf::afterForkChild();
#endif
  Py_RETURN_NONE;
}

// Patch sys.monitoring.register_callback to intercept debugger/profiler
// attachment.
void patchSysMonitoringFunctions(PyObject* cinderjit_module) {
  BorrowedRef<> monitoring = PySys_GetObject("monitoring");
  if (monitoring == nullptr) {
    JIT_DLOG("sys.monitoring not found, skipping JIT monitoring integration");
    return;
  }

  Ref<> original =
      Ref<>::steal(PyObject_GetAttrString(monitoring, "register_callback"));
  if (original == nullptr) {
    PyErr_Clear();
    JIT_DLOG(
        "sys.monitoring.register_callback not found, skipping JIT monitoring "
        "integration");
    return;
  }

  auto mod_state = cinderx::getModuleState();
  mod_state->orig_sys_monitoring_register_callback = Ref<>::create(original);

  Ref<> patched_func = Ref<>::steal(PyObject_GetAttrString(
      cinderjit_module, "patched_sys_monitoring_register_callback"));
  if (patched_func == nullptr) {
    JIT_LOG(
        "Failed to get patched_sys_monitoring_register_callback from cinderjit "
        "module");
    PyErr_Clear();
    return;
  }

  if (PyObject_SetAttrString(monitoring, "register_callback", patched_func) <
      0) {
    JIT_LOG("Failed to patch sys.monitoring.register_callback");
    PyErr_Clear();
    return;
  }

  JIT_DLOG("Successfully patched sys.monitoring.register_callback");

  // Also patch free_tool_id to trigger a JIT state re-evaluation when tools
  // are freed. Without this, the JIT would remain paused until some other
  // event happens to call `toggleJitBasedOnInstrumentationState()`.
  Ref<> orig_free_tool_id =
      Ref<>::steal(PyObject_GetAttrString(monitoring, "free_tool_id"));
  if (orig_free_tool_id != nullptr) {
    mod_state->orig_sys_monitoring_free_tool_id =
        Ref<>::create(orig_free_tool_id);

    Ref<> patched_free_tool_id = Ref<>::steal(PyObject_GetAttrString(
        cinderjit_module, "patched_sys_monitoring_free_tool_id"));
    if (patched_free_tool_id != nullptr) {
      if (PyObject_SetAttrString(
              monitoring, "free_tool_id", patched_free_tool_id) < 0) {
        JIT_LOG("Failed to patch sys.monitoring.free_tool_id");
        PyErr_Clear();
      } else {
        JIT_DLOG("Successfully patched sys.monitoring.free_tool_id");
      }
    } else {
      JIT_LOG(
          "Failed to get patched_sys_monitoring_free_tool_id from cinderjit "
          "module");
      PyErr_Clear();
    }
  } else {
    PyErr_Clear();
    JIT_DLOG(
        "sys.monitoring.free_tool_id not found, skipping free_tool_id "
        "patching");
  }
}

// Patch sys.setprofile and sys.settrace to intercept profiler/debugger
// attachment.
void patchSysSetProfileAndSetTrace(PyObject* cinderjit_module) {
  Ref<> sys = Ref<>::steal(PyImport_ImportModule("sys"));
  if (sys == nullptr) {
    PyErr_Clear();
    JIT_DLOG("sys module not found, skipping sys.setprofile/settrace patching");
    return;
  }

  auto mod_state = cinderx::getModuleState();

  auto patchSysFunc =
      [&](const char* attr_name,
          const char* patched_attr_name,
          const std::function<void(BorrowedRef<>)>& storeOriginal) {
        Ref<> original = Ref<>::steal(PyObject_GetAttrString(sys, attr_name));
        if (original == nullptr) {
          JIT_DLOG("sys.{} not found, skipping patching", attr_name);
          PyErr_Clear();
          return;
        }
        storeOriginal(original);

        Ref<> patched = Ref<>::steal(
            PyObject_GetAttrString(cinderjit_module, patched_attr_name));
        if (patched == nullptr) {
          JIT_LOG("Failed to get {} from cinderjit module", patched_attr_name);
          PyErr_Clear();
          return;
        }
        if (PyObject_SetAttrString(sys, attr_name, patched) < 0) {
          JIT_LOG("Failed to patch sys.{}", attr_name);
          PyErr_Clear();
        } else {
          JIT_DLOG("Successfully patched sys.{}", attr_name);
        }
      };

  patchSysFunc("setprofile", "patched_sys_setprofile", [&](BorrowedRef<> func) {
    mod_state->orig_sys_setprofile = Ref<>::create(func);
  });
  patchSysFunc("settrace", "patched_sys_settrace", [&](BorrowedRef<> func) {
    mod_state->orig_sys_settrace = Ref<>::create(func);
  });
}

void restoreSysMonitoringRegisterCallback() {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> monitoring = PySys_GetObject("monitoring");
  if (monitoring == nullptr) {
    return;
  }

  BorrowedRef<> orig_register_callback =
      mod_state->orig_sys_monitoring_register_callback;
  if (orig_register_callback != nullptr) {
    if (PyObject_SetAttrString(
            monitoring, "register_callback", orig_register_callback) < 0) {
      PyErr_Clear();
    }
  }

  BorrowedRef<> orig_free_tool_id = mod_state->orig_sys_monitoring_free_tool_id;
  if (orig_free_tool_id != nullptr) {
    if (PyObject_SetAttrString(monitoring, "free_tool_id", orig_free_tool_id) <
        0) {
      PyErr_Clear();
    }
  }
}

void restoreSysSetProfileAndSetTrace() {
  auto mod_state = cinderx::getModuleState();

  if (BorrowedRef<> original_setprofile = mod_state->orig_sys_setprofile) {
    if (PySys_SetObject("setprofile", original_setprofile) < 0) {
      PyErr_Clear();
    }
  }

  if (BorrowedRef<> original_settrace = mod_state->orig_sys_settrace) {
    if (PySys_SetObject("settrace", original_settrace) < 0) {
      PyErr_Clear();
    }
  }
}

// Referenced by CompiledFunction.__reduce__ (Jit/compiled_function.cpp) as the
// picklable-by-reference target that rebuilds a CompiledFunction dropped during
// pickling as None.
PyObject* reconstruct_pickled_compiled_function(
    PyObject* /* self */,
    PyObject* /* ignored */) {
  Py_RETURN_NONE;
}

PyMethodDef jit_methods[] = {
    {"disable",
     reinterpret_cast<PyCFunction>(disable_jit),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Compile all functions that are pending compilation and then "
               "disable the JIT.")},
    {"enable",
     enable_jit,
     METH_NOARGS,
     PyDoc_STR("Re-enable the JIT and re-attach compiled onto previously "
               "JIT-compiled functions.")},
    {"patched_sys_monitoring_register_callback",
     _PyCFunction_CAST(patched_sys_monitoring_register_callback),
     METH_FASTCALL,
     PyDoc_STR(
         "Patched version of sys.monitoring.register_callback that "
         "disables/enables the JIT when debuggers/profilers attach/detach.")},
    {"patched_sys_monitoring_free_tool_id",
     _PyCFunction_CAST(patched_sys_monitoring_free_tool_id),
     METH_FASTCALL,
     PyDoc_STR("Patched version of sys.monitoring.free_tool_id that "
               "re-enables the JIT when monitoring tools are freed.")},
    {"patched_sys_setprofile",
     _PyCFunction_CAST(patched_sys_setprofile),
     METH_FASTCALL,
     PyDoc_STR("Patched version of sys.setprofile that "
               "disables/enables the JIT when profilers attach/detach.")},
    {"patched_sys_settrace",
     _PyCFunction_CAST(patched_sys_settrace),
     METH_FASTCALL,
     PyDoc_STR(
         "Patched version of sys.settrace that "
         "disables/enables the JIT when debuggers/profilers attach/detach.")},
    {"auto",
     auto_jit,
     METH_NOARGS,
     PyDoc_STR("Configure the JIT to automatically compile functions, using "
               "default settings")},
    {"compile_after_n_calls",
     compile_after_n_calls,
     METH_O,
     PyDoc_STR("Configure the JIT to automatically compile functions after "
               "they are called a set number of times.")},
    {"disassemble", disassemble, METH_O, "Disassemble JIT compiled functions."},
    {"_test_parse_thread_state_prologue",
     test_parse_thread_state_prologue,
     METH_O,
     PyDoc_STR("Test helper for AArch64 thread-state TLS prologue parsing.")},
    {"_test_set_thread_state_offset",
     test_set_thread_state_offset,
     METH_O,
     PyDoc_STR("Test helper for forcing the cached thread-state TLS offset.")},
#ifndef WIN32
    {"dump_elf",
     dump_elf,
     METH_O,
     PyDoc_STR(
         "Write out all generated code into an ELF file, whose filepath "
         "is passed as the first argument. This is currently intended for "
         "debugging purposes.")},
    {"load_aot_bundle",
     load_aot_bundle,
     METH_O,
     PyDoc_STR("Load a bundle of ahead-of-time generated code from an ELF "
               "file, whose filepath is passed as the first argument. Note: "
               "This does not actually work yet, it's being used for debugging "
               "purposes.")},
#endif
    {"get_compile_after_n_calls",
     get_compile_after_n_calls,
     METH_NOARGS,
     PyDoc_STR("Get the current number of calls needed before a function is "
               "automatically compiled.")},
    {"is_attr_caches_enabled",
     is_attr_caches_enabled,
     METH_NOARGS,
     PyDoc_STR("Whether inline attribute caches are enabled (3.11 default: "
               "off until MR-09 acceptance).")},
    {"is_enabled",
     is_enabled,
     METH_NOARGS,
     PyDoc_STR("Check whether the JIT is enabled and usable")},
    {"count_interpreted_calls",
     count_interpreted_calls,
     METH_O,
     PyDoc_STR("Get the number of times a function has been executed in the "
               "interpreter since cinderx has been initialized")},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     PyDoc_STR("Check if a function is jit compiled.")},
    {"set_max_code_size",
     set_max_code_size,
     METH_O,
     PyDoc_STR(
         "Set the maximum amount of memory (in bytes) the JIT is allowed to "
         "write")},
    {"precompile_all",
     reinterpret_cast<PyCFunction>(precompile_all),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("If the JIT is enabled, compile all functions registered for "
               "future compilation and return True, otherwise return False. "
               "This is not meant for general use, it has the potential to "
               "compile many unneeded functions. Use wisely.")},
    {"force_compile",
     force_compile,
     METH_O,
     PyDoc_STR("Force a function to be JIT compiled if it hasn't yet.")},
    {"force_uncompile",
     force_uncompile,
     METH_O,
     PyDoc_STR("Uncompile a function that has been JIT compiled.")},
    {"_reconstruct_pickled_compiled_function",
     reconstruct_pickled_compiled_function,
     METH_NOARGS,
     PyDoc_STR(
         "Internal helper referenced by CompiledFunction.__reduce__ to rebuild "
         "a CompiledFunction dropped during pickling as None.")},
    {"lazy_compile",
     lazy_compile,
     METH_O,
     PyDoc_STR("Set a function to be JIT compiled the first time it is run.")},
    {"jit_frame_mode",
     jit_frame_mode,
     METH_NOARGS,
     PyDoc_STR(
         "Get JIT frame mode (0 = normal frames, 1 = lightweight frames).")},
    {"is_lightweight_frames_enabled",
     is_lightweight_frames_enabled,
     METH_NOARGS,
     PyDoc_STR("Return True when JIT lightweight frames are compiled in.")},
    {"get_jit_list",
     get_jit_list,
     METH_NOARGS,
     PyDoc_STR("Get the list of functions to JIT compile.")},
    {"append_jit_list",
     append_jit_list,
     METH_O,
     PyDoc_STR("Parse a JIT-list line and append it.")},
    {"read_jit_list",
     read_jit_list,
     METH_O,
     PyDoc_STR("Read a JIT list file and apply it.")},
    {"print_hir",
     print_hir,
     METH_O,
     PyDoc_STR("Print the HIR for a jitted function to stdout.")},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     PyDoc_STR("Return a list of functions that are currently JIT-compiled.")},
    {"get_compilation_time",
     get_compilation_time,
     METH_NOARGS,
     PyDoc_STR("Return the total time used for JIT compiling functions in "
               "milliseconds.")},
    {"get_function_compilation_time",
     get_function_compilation_time,
     METH_O,
     PyDoc_STR("Return the time used for JIT compiling a given function in "
               "milliseconds.")},
    {"get_and_clear_runtime_stats",
     get_and_clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR("Returns information about the runtime behavior of JIT-compiled "
               "code.")},
    {"clear_runtime_stats",
     clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR("Clears runtime stats about JIT-compiled code without returning "
               "a value.")},
    {"_autojit_gate_stats",
     autojit_gate_stats,
     METH_NOARGS,
     PyDoc_STR("Return AutoJIT gate path counters.")},
    {"_clear_autojit_gate_stats",
     clear_autojit_gate_stats,
     METH_NOARGS,
     PyDoc_STR("Clear and enable AutoJIT gate path counters.")},
    {"get_and_clear_inline_cache_stats",
     get_and_clear_inline_cache_stats,
     METH_NOARGS,
     PyDoc_STR(
         "Returns and clears information about the runtime inline cache stats "
         "behavior of JIT-compiled code. Stats will only be collected with X "
         "flag jit-enable-inline-cache-stats-collection.")},
    {"is_inline_cache_stats_collection_enabled",
     is_inline_cache_stats_collection_enabled,
     METH_NOARGS,
     PyDoc_STR("Return True if jit-enable-inline-cache-stats-collection is on "
               "and False otherwise.")},
    {"get_compiled_size",
     get_compiled_size,
     METH_O,
     PyDoc_STR("Return code size in bytes for a JIT-compiled function.")},
    {"get_compiled_stack_size",
     get_compiled_stack_size,
     METH_O,
     PyDoc_STR("Return stack size in bytes for a JIT-compiled function.")},
    {"get_compiled_spill_stack_size",
     get_compiled_spill_stack_size,
     METH_O,
     PyDoc_STR("Return stack size in bytes used for register spills for a "
               "JIT-compiled function.")},
    {"jit_suppress",
     jit_suppress,
     METH_O,
     PyDoc_STR("Decorator to prevent the JIT from running on a function.")},
    {"jit_unsuppress",
     jit_unsuppress,
     METH_O,
     PyDoc_STR("Decorator to allow the JIT to run on a function.")},
    {"multithreaded_compile_test",
     multithreaded_compile_test,
     METH_NOARGS,
     PyDoc_STR("Force multi-threaded recompile of still existing JIT functions "
               "for testing.")},
    {"is_multithreaded_compile_test_enabled",
     is_multithreaded_compile_test_enabled,
     METH_NOARGS,
     PyDoc_STR("Return True if multithreaded_compile_test mode is enabled.")},
    {"get_allocator_stats",
     get_allocator_stats,
     METH_NOARGS,
     PyDoc_STR("Return stats from the code allocator as a dictionary.")},
    {"is_hir_inliner_enabled",
     is_hir_inliner_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "Return True if the HIR inliner is enabled and False otherwise.")},
    {"enable_hir_inliner",
     enable_hir_inliner,
     METH_NOARGS,
     PyDoc_STR("Enable the HIR inliner.")},
    {"disable_hir_inliner",
     disable_hir_inliner,
     METH_NOARGS,
     PyDoc_STR("Disable the HIR inliner.")},
    {"enable_emit_type_annotation_guards",
     enable_emit_type_annotation_guards,
     METH_NOARGS,
     PyDoc_STR("Enable the emit_type_annotation_guards configuration option.")},
    {"disable_emit_type_annotation_guards",
     disable_emit_type_annotation_guards,
     METH_NOARGS,
     PyDoc_STR(
         "Disable the emit_type_annotation_guards configuration option.")},
    {"enable_specialized_opcodes",
     enable_specialized_opcodes,
     METH_NOARGS,
     PyDoc_STR("Enable compiling specialized opcodes.")},
    {"disable_specialized_opcodes",
     disable_specialized_opcodes,
     METH_NOARGS,
     PyDoc_STR("Disable compiling specialized opcodes.")},
    {"get_inlined_functions_stats",
     get_inlined_functions_stats,
     METH_O,
     PyDoc_STR("Return a dict containing function inlining stats with the the "
               "following structure: {'num_inlined_functions' => int, "
               "'failure_stats' => { "
               "failure_reason => set of function names}} ).")},
    {"get_num_inlined_functions",
     get_num_inlined_functions,
     METH_O,
     PyDoc_STR("Return the number of inline sites in this function.")},
    {"get_function_hir_opcode_counts",
     get_function_hir_opcode_counts,
     METH_O,
     PyDoc_STR(
         "Return a map from HIR opcode name to the count of that opcode in the "
         "JIT-compiled version of this function.")},
    {"mlock_profiler_dependencies",
     mlock_profiler_dependencies,
     METH_NOARGS,
     PyDoc_STR("Keep profiler dependencies paged in.")},
    {"page_in_profiler_dependencies",
     page_in_profiler_dependencies,
     METH_NOARGS,
     PyDoc_STR("Read the memory needed by ebpf-based profilers.")},
    {"after_fork_child",
     after_fork_child,
     METH_NOARGS,
     PyDoc_STR("Callback to be invoked by the runtime after fork().")},
    {"_deopt_gen",
     deopt_gen,
     METH_O,
     PyDoc_STR(
         "Argument must be a suspended generator, coroutine, or async "
         "generator. If it is a JIT generator, deopt it, so it will resume in "
         "the interpreter the next time it executes, and return True. "
         "Otherwise, return False. Intended only for use in tests.")},
    {nullptr, nullptr, 0, nullptr},
};

int jit_exec(PyObject*) {
  return 0;
}

PyModuleDef_Slot jit_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(jit_exec)},
    {}};

#if PY_VERSION_HEX < 0x030C0000
// The 3.11 canary control plane.
//
// The full method table is a control surface for capabilities this port
// does not yet have: precompile_all() and lazy_compile() install machine
// code through the batch and re-optimization paths, the jit-list mutators
// belong to a later milestone, and the guard and specialization setters can
// re-open exactly the speculation MR-04 excludes.
// Exposing them would make the execute surface a matter of which entry a
// caller picked.  Canary therefore publishes only what its own evidence
// needs, and each later milestone adds back what its acceptance covers.
PyMethodDef jit_methods_311_canary[] = {
    {"is_enabled",
     is_enabled,
     METH_NOARGS,
     PyDoc_STR("Check whether the JIT is enabled and usable")},
    {"is_attr_caches_enabled",
     is_attr_caches_enabled,
     METH_NOARGS,
     PyDoc_STR("Whether inline attribute caches are enabled (3.11 default: "
               "off until MR-09 acceptance).")},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     PyDoc_STR("Check if a function is jit compiled.")},
    {"force_compile",
     force_compile,
     METH_O,
     PyDoc_STR("Force a function to be JIT compiled if it hasn't yet.")},
    // MR-05: the inverse of force_compile, and the only published way to
    // take a function back off machine code.  A call already inside the
    // artifact keeps running it -- the guarded entry pins it for the
    // duration -- so this affects subsequent calls only.
    {"force_uncompile",
     force_uncompile,
     METH_O,
     PyDoc_STR("Take a function off JIT-compiled code; later calls run in "
               "the interpreter.")},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     PyDoc_STR("Return a list of functions that are currently JIT-compiled.")},
    {"get_and_clear_runtime_stats",
     get_and_clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR("Returns information about the runtime behavior of JIT-compiled "
               "code.")},
    // Restrictions, not capabilities.  Withholding them is what made the
    // cinderx.jit wrapper keep a no-op stub for @jit_suppress, so a
    // function marked "do not compile" was compiled and executed anyway,
    // walking around the CI_CO_SUPPRESS_JIT gate; and pause() disabled
    // nothing.  A milestone may withhold what it cannot do, never what
    // stops it doing something.
    {"jit_suppress",
     jit_suppress,
     METH_O,
     PyDoc_STR("Decorator to prevent the JIT from running on a function.")},
    {"jit_unsuppress",
     jit_unsuppress,
     METH_O,
     PyDoc_STR("Decorator to allow the JIT to run on a function.")},
    {"disable",
     reinterpret_cast<PyCFunction>(disable_jit),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Compile all functions that are pending compilation and then "
               "disable the JIT.")},
    {"enable",
     enable_jit,
     METH_NOARGS,
     PyDoc_STR("Re-enable the JIT and re-attach compiled onto previously "
               "JIT-compiled functions.")},
    // The physical half of the lifecycle: every function the registry still
    // holds an artifact for, whether or not a call would currently enter it.
    {"_get_resident_compiled_functions",
     get_resident_compiled_functions,
     METH_NOARGS,
     PyDoc_STR("Functions the registry still holds a compiled artifact for, "
               "including those temporarily falling back to the "
               "interpreter.")},
    // Not a capability: CompiledFunction.__reduce__ looks this up by name in
    // the cinderjit module, so pickling or deep-copying anything holding a
    // compiled artifact needs it present.
    {"_reconstruct_pickled_compiled_function",
     reconstruct_pickled_compiled_function,
     METH_NOARGS,
     PyDoc_STR("Internal helper for unpickling a compiled function.")},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef jit_module_311_canary = {
    PyModuleDef_HEAD_INIT,
    "cinderjit", /* m_name */
    PyDoc_STR("The CPython 3.11 canary control plane: the MR-04 execute "
              "surface and nothing beyond it."),
    0, /* m_size */
    jit_methods_311_canary, /* m_methods */
    jit_slots, /* m_slots */
    nullptr, /* m_traverse */
    nullptr, /* m_clear */
    nullptr, /* m_free */
};
#endif

PyModuleDef jit_module = {
    PyModuleDef_HEAD_INIT,
    "cinderjit", /* m_name */
    PyDoc_STR(
        "Control the Cinder JIT compiler. Only available when the JIT "
        "has been enabled."),
    0, /* m_size */
    jit_methods, /* m_methods */
    jit_slots, /* m_slots */
    nullptr, /* m_traverse */
    nullptr, /* m_clear */
    nullptr, /* m_free */
};

void trackEligibleCodeObjects(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyCodeObject> func_code,
    JitEligibility eligibility = JitEligibility::Eligible) {
#if PY_VERSION_HEX < 0x030C0000
  // This table maps a code object to the function it was first seen on,
  // and both halves are borrowed; its only consumer is the batch path the
  // canary does not publish, so on 3.11 it stays empty (wired in MR-11).
  (void)func;
  (void)func_code;
  (void)eligibility;
  return;
#endif
  // We need to maintain a mapping for all functions which are
  // eligible for compilation at some point - we track the code
  // object and their parent function. If we have a JIT list we
  // also track the registered units.
  // Map this function's code object to itself.
  auto& jit_code_outer_funcs = jitCtx()->codeOuterFunctions();
  if (jit_code_outer_funcs.contains(func_code)) {
    // already registered this code
    return;
  }

  auto& jit_reg_units = cinderx::getModuleState()->registered_compilation_units;

  jit_code_outer_funcs.try_emplace(func_code, func);

  // Scan this function's code object for any nested functions that
  // might be compiled
  PyObject* module = func->func_module;
  BorrowedRef<> top_consts{func_code->co_consts};
  for (BorrowedRef<PyCodeObject> code : findNestedCodes(module, top_consts)) {
    if (jit_code_outer_funcs.contains(code)) {
      continue;
    }
    jit_code_outer_funcs.emplace(code, func);
    if (eligibility == JitEligibility::JitListEligible) {
      jit_reg_units.emplace(code.getObj());
    }
  }
}

// Preload a function and its dependencies, then compile them all.
//
// Failing to compile a dependent function is a soft failure, and is ignored.
Result compile_func(BorrowedRef<PyFunctionObject> func) {
  // Content-keyed reuse for exec-generated namespace-free code: if an
  // identical code object was compiled before, attach this function to the
  // existing artifact instead of compiling again.
  if (getConfig().auto_code_twin_dedup && jitCtx() != nullptr &&
      jitCtx()->reuseDedupedCompiled(func)) {
    return Result::OK;
  }

  // isolate preloaders state since batch preloading might trigger a call to a
  // jitable function, resulting in a single-function compile
  hir::IsolatedPreloaders ip;

  // We generally track function objects when they are created. But we may need
  // to re-track here. A function can have nested functions and those nested
  // functions can out-live the function that created them. When the outer
  // function is destroyed we need to remove the dangling registrations in
  // codeOuterFunctions. We will treat whatever remains as new top-level
  // functions.
  trackEligibleCodeObjects(func, func->func_code);

  // Collect a list of functions to compile.  If it's empty then there must have
  // been a Python error during preloading.
  std::vector<BorrowedRef<PyFunctionObject>> targets = preloadFuncAndDeps(func);
  if (targets.empty()) {
    JIT_CHECK(
        PyErr_Occurred(), "Expect a Python exception when preloading fails");
    return Result::PYTHON_EXCEPTION;
  }

  if (targets.size() > 1) {
    JIT_DLOG(
        "Compiling {} along with {} functions it calls",
        funcFullname(func),
        targets.size() - 1);
  }

  // Will return unknown error if none of the targets can find a matching
  // preloader.
  auto result = Result::UNKNOWN_ERROR;

  for (BorrowedRef<PyFunctionObject> target : targets) {
    auto preloader = hir::preloaderManager().find(target);
    if (preloader == nullptr) {
      continue;
    }

    // Don't compile functions that were preloaded purely for inlining.
    bool is_static = preloader->code()->co_flags & CI_CO_STATICALLY_COMPILED;
    if (target != func && !is_static) {
      continue;
    }

    result = compilePreloader(*preloader, target);
    JIT_CHECK(
        result != Result::PYTHON_EXCEPTION,
        "Raised a Python exception while JIT-compiling function {}, which is "
        "not allowed",
        funcFullname(target));
    JIT_CHECK(
        result != Result::NO_PRELOADER,
        "Cannot find a preloader for function {}, despite it just being "
        "preloaded",
        funcFullname(target));

    // If we hit the max code size limit, stop compiling further functions
    if (result == Result::OVER_MAX_CODE_SIZE) {
      break;
    }
  }

  // This is the common case, where the original function is compiled last.
  // Return its compilation result.
  BorrowedRef<PyFunctionObject> last_func = targets.back();
  if (last_func == func) {
    return result;
  }

  // Otherwise the original function was destroyed during preloading, which is
  // rare but can happen with nested functions.  In that case, we're just going
  // to pretend everything went okay.  It doesn't make sense to return the
  // results of any of the other preloaded functions, as the caller never asked
  // for them in the first place.
  return Result::OK;
}

// Call posix.register_at_fork(None, None, cinderjit.after_fork_child), if it
// exists. Returns 0 on success or if the module/function doesn't exist, and -1
// on any other errors.
int register_fork_callback(BorrowedRef<> cinderjit_module) {
  auto os_module = Ref<>::steal(
      PyImport_ImportModuleLevel("posix", nullptr, nullptr, nullptr, 0));
  if (os_module == nullptr) {
    PyErr_Clear();
    return 0;
  }
  auto register_at_fork =
      Ref<>::steal(PyObject_GetAttrString(os_module, "register_at_fork"));
  if (register_at_fork == nullptr) {
    PyErr_Clear();
    return 0;
  }
  auto callback = Ref<>::steal(
      PyObject_GetAttrString(cinderjit_module, "after_fork_child"));
  if (callback == nullptr) {
    return -1;
  }
  auto args = Ref<>::steal(PyTuple_New(0));
  if (args == nullptr) {
    return -1;
  }
  auto kwargs = Ref<>::steal(PyDict_New());
  if (kwargs == nullptr ||
      PyDict_SetItemString(kwargs, "after_in_child", callback) < 0 ||
      PyObject_Call(register_at_fork, args, kwargs) == nullptr) {
    return -1;
  }
  return 0;
}

// Informs the JIT that an instance has had an assignment to its __class__
// field.
void instanceTypeAssigned(PyTypeObject* old_ty, PyTypeObject* new_ty) {
  if (CompilerContext<Compiler>* ctx = jitCtx()) {
    ctx->notifyTypeModified(old_ty, new_ty);
  }
}

// JIT audit event callback. For now, we only pay attention to when an object's
// __class__ is assigned to.
int jit_audit_hook(const char* event, PyObject* args, void* /* data */) {
  if (strcmp(event, "object.__setattr__") != 0 || PyTuple_GET_SIZE(args) != 3) {
    return 0;
  }
  BorrowedRef<> name(PyTuple_GET_ITEM(args, 1));
  if (!PyUnicode_Check(name) ||
      PyUnicode_CompareWithASCIIString(name, "__class__") != 0) {
    return 0;
  }

  BorrowedRef<> object(PyTuple_GET_ITEM(args, 0));
  BorrowedRef<PyTypeObject> new_type(PyTuple_GET_ITEM(args, 2));
  instanceTypeAssigned(Py_TYPE(object), new_type);
  return 0;
}

int install_jit_audit_hook() {
  void* kData = nullptr;
  if (!installAuditHook(jit_audit_hook, kData)) {
    PyErr_SetString(PyExc_RuntimeError, "Could not install JIT audit hook");
    return -1;
  }
  return 0;
}

void dump_jit_stats() {
  auto stats = Ref<>::steal(get_and_clear_runtime_stats(nullptr, nullptr));
  if (stats == nullptr) {
    return;
  }
  auto stats_str = Ref<>::steal(PyObject_Str(stats));
  if (!stats_str) {
    return;
  }

  JIT_LOG("JIT runtime stats:\n{}", PyUnicode_AsUTF8(stats_str.get()));
}

constexpr std::string_view getCpuArchName() {
#if defined(__x86_64__)
  return "x86-64";
#elif defined(__i386__)
  return "x86 (32-bit)";
#elif defined(__aarch64__)
  return "arm64";
#elif defined(__arm__)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

bool isLightweightFramesCompiledIn() {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  return true;
#else
  return false;
#endif
}

int validateFrameModeConfig() {
  if (getConfig().frame_mode == FrameMode::kLightweight &&
      !isLightweightFramesCompiledIn()) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "PYTHONJITLIGHTWEIGHTFRAME requires ENABLE_LIGHTWEIGHT_FRAMES");
    return -1;
  }
  if (getConfig().frame_mode == FrameMode::kLightweight &&
      getConfig().osr_enabled) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "PYTHONJITLIGHTWEIGHTFRAME is mutually exclusive with "
        "CINDERX_OSR_ENABLED");
    return -1;
  }
  return 0;
}

void notifyUnitDeletedDuringPreload(
    cinderx::ModuleState* state,
    BorrowedRef<> unit) {
  if (state->unit_deleted_during_preload) {
    state->unit_deleted_during_preload(unit);
  }
}

// Unregister a function and its nested code objects from jit_reg_units and
// jit_code_outer_funcs. Called when a function is destroyed or its code object
// is being replaced.
void unregisterFunctionCodes(BorrowedRef<PyFunctionObject> func) {
  if (!jitCtx()) {
    return;
  }
  auto mod_state = cinderx::getModuleState();
  if (!mod_state) {
    return;
  }

  auto& jit_reg_units = mod_state->registered_compilation_units;
  auto& jit_code_outer_funcs = jitCtx()->codeOuterFunctions();

  BorrowedRef<PyCodeObject> top_code{func->func_code};
  // The unconditional erases come first: this often runs under a C
  // destructor, and erasing cannot fail while the nested walk below
  // allocates.
  jit_reg_units.erase(func);
  jit_reg_units.erase(top_code);

  auto it = jit_code_outer_funcs.find(top_code);
  if (it != jit_code_outer_funcs.end() && it->second == func) {
    jit_code_outer_funcs.erase(it);
    PyObject* module = func->func_module;
    BorrowedRef<> top_consts{top_code->co_consts};
    try {
      for (BorrowedRef<PyCodeObject> code :
           findNestedCodes(module, top_consts)) {
        jit_reg_units.erase(code);
        auto existing = jit_code_outer_funcs.find(code);
        if (existing != jit_code_outer_funcs.end() &&
            existing->second == func) {
          jit_code_outer_funcs.erase(code);
        }
        notifyUnitDeletedDuringPreload(mod_state, code.getObj());
      }
    } catch (const std::bad_alloc&) {
      // The nested registrations could not be enumerated: some may remain,
      // and their deletions were never announced, so no batch may trust
      // its deleted-units view.
      jit::poisonUnitDeletionTracking();
    }
  }

  notifyUnitDeletedDuringPreload(mod_state, func.getObj());
}

} // namespace

namespace jit {

void setUncompileMidpointHookForTest(void (*hook)()) {
  s_uncompile_midpoint_hook_for_test = hook;
}

bool registerFunctionForTest(BorrowedRef<PyFunctionObject> func) {
  return registerFunction(func);
}

void poisonUnitDeletionTracking() {
  if (auto* state = cinderx::getModuleState()) {
    state->unit_deletion_tracking_failed = true;
  }
}

bool consumeUnitDeletionTrackingPoison() {
  auto* state = cinderx::getModuleState();
  if (state == nullptr || !state->unit_deletion_tracking_failed) {
    return false;
  }
  state->unit_deletion_tracking_failed = false;
  return true;
}

bool roiBackoffAllowsCompile(BorrowedRef<PyCodeObject> code) {
  if (!getConfig().roi_backoff_enabled) {
    return true;
  }
  CodeExtra* extra = codeExtraIfExists(code);
  return roiBackoffStateAllowsCompile(extra);
}

namespace {

class PreservePythonError {
 public:
  PreservePythonError() {
    PyErr_Fetch(&type_, &value_, &traceback_);
  }

  ~PreservePythonError() {
    PyErr_Restore(type_, value_, traceback_);
  }

  PreservePythonError(const PreservePythonError&) = delete;
  PreservePythonError& operator=(const PreservePythonError&) = delete;

 private:
  PyObject* type_{nullptr};
  PyObject* value_{nullptr};
  PyObject* traceback_{nullptr};
};

void triggerRoiBackoff(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* extra,
    uint32_t ctl) {
  uint32_t expected = ctl;
  uint32_t pending =
      (ctl & CI_CODE_EXTRA_ROI_ROUND_MASK) | CI_CODE_EXTRA_ROI_PENDING_BIT;
  if (!Ci_code_extra_cas_roi_ctl_release(extra, &expected, pending)) {
    return;
  }

  FreeThreadedJITEntrypointGuard guard;

  std::vector<Ref<PyFunctionObject>> funcs;
  if (jitCtx() != nullptr) {
    for (auto& entry : jitCtx()->compiledFuncs()) {
      BorrowedRef<PyFunctionObject> func = entry.first;
      if (reinterpret_cast<PyCodeObject*>(func->func_code) == code.get()) {
        funcs.emplace_back(Ref<PyFunctionObject>::create(func.get()));
      }
    }
  }

  if (funcs.empty()) {
    Ci_code_extra_store_roi_ctl_release(extra, ctl);
    return;
  }

  uint32_t round = roiBackoffRound(ctl);
  uint32_t next_round = round + 1;
  bool frozen = getConfig().roi_backoff_max_rounds == 0 ||
      next_round >= getConfig().roi_backoff_max_rounds;

  for (auto& func : funcs) {
    ::uncompileImpl(func);
  }

  Ci_code_extra_store_roi_deopt_count_relaxed(extra, 0);
  if (frozen) {
    Ci_code_extra_store_roi_recompile_floor_release(extra, 0);
    Ci_code_extra_or_skey_release(extra, kSkeyDecidedColdBit);
    Ci_code_extra_store_roi_ctl_release(
        extra,
        CI_CODE_EXTRA_ROI_FROZEN_BIT | roiBackoffCtlForRound(next_round));
    incAutoJitGateStat(g_auto_jit_gate_stats.roi_frozen);
    return;
  }

  uint64_t calls = Ci_code_extra_get_calls(extra);
  Ci_code_extra_store_roi_recompile_floor_release(
      extra, roiBackoffRecompileFloor(calls, round));
  Ci_code_extra_store_roi_ctl_release(extra, roiBackoffCtlForRound(next_round));
  incAutoJitGateStat(g_auto_jit_gate_stats.roi_uncompile);

  for (auto& func : funcs) {
    scheduleJitCompile(func);
  }
}

} // namespace

void recordDeoptForRoiBackoff(
    CodeRuntime* code_runtime,
    DeoptReason reason,
    bool is_instrumentation_deopt) {
  if (!getConfig().roi_backoff_enabled ||
      !getConfig().compile_after_n_calls.has_value() ||
      code_runtime == nullptr ||
      !roiBackoffReasonCounts(reason, is_instrumentation_deopt)) {
    return;
  }

  BorrowedRef<PyCodeObject> code = code_runtime->code();
  CodeExtra* extra = codeExtra(code);
  if (extra == nullptr) {
    return;
  }

  uint32_t ctl = Ci_code_extra_load_roi_ctl_relaxed(extra);
  if (roiBackoffCtlFrozen(ctl) || roiBackoffCtlPending(ctl)) {
    return;
  }

  uint32_t round = roiBackoffRound(ctl);
  uint32_t budget = roiBackoffBudgetForRound(round);
  uint32_t count = Ci_code_extra_incr_roi_deopt_count(extra);
  if (count < budget) {
    return;
  }

  PreservePythonError preserve_error;
  triggerRoiBackoff(code, extra, ctl);
}

int initialize() {
  JIT_CHECK(
      getConfig().state != State::kFinalizing,
      "Trying to re-initialize the JIT as it is finalizing");
  if (isJitInitialized()) {
    return 0;
  }

  // Save fields that might have been set by test code before jit::initialize()
  // is called.
  auto force_init = getConfig().force_init;
  auto use_stable_pointers = getConfig().use_stable_pointers;

#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 only initializes the compiler for explicit shadow mode (or
  // RuntimeTests' force-init hook). Observe and off stay capability-gated
  // before a compiler context or code allocator is created.
  const char* runtime_mode = std::getenv("CINDERX_JIT_MODE");
  bool shadow_requested =
      runtime_mode != nullptr && std::strcmp(runtime_mode, "shadow") == 0;
  bool canary_requested =
      runtime_mode != nullptr && std::strcmp(runtime_mode, "canary") == 0;
  if (!shadow_requested && !canary_requested &&
      force_init != std::make_optional(true)) {
    return 0;
  }
#endif

  getMutableConfig() = Config{};
  if (force_init.has_value()) {
    getMutableConfig().force_init = force_init;
  }
  getMutableConfig().use_stable_pointers = use_stable_pointers;

  FlagProcessor flag_processor = initFlagProcessor();
  if (flag_processor.hasHandled("jit-help")) {
    std::cout << flag_processor.jitXOptionHelpMessage() << '\n';
    // Return rather than exit here for arg printing test doesn't end early.
    return -2;
  }
  if (validateFrameModeConfig() < 0) {
    return -1;
  }

  // Handle force_init = false case only after parsing all flags.
  if (getConfig().force_init == std::make_optional(false)) {
    return 0;
  }

  // Do this check after config is initialized, so we can use JIT_DLOG().
#if !defined(__x86_64__) && !defined(__aarch64__)
  JIT_DLOG(
      "JIT only supported x86-64 or aarch64 platforms, detected current "
      "architecture as '{}'. Disabling the JIT.",
      getCpuArchName());
  return 0;
#endif

#if defined(CINDER_AARCH64)
  if (getConfig().cold_code_huge_pages) {
    JIT_LOG(
        "cold_code_huge_pages is not supported on ARM64 (hot and cold code "
        "must share a contiguous allocation to stay within branch range). "
        "The flag will be ignored.");
  }
#endif

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 has no code watcher: the code-extra free function delivers the
  // watcher-equivalent notification (both modes; shadow populates too).
  // The hook is invoked from inside code_dealloc, so no C++ exception may
  // cross it; a caught failure means a deletion may have gone unrecorded.
  setCodeDestroyedHook([](PyCodeObject* code) {
    try {
      codeDestroyed(code);
      // The bookkeeping above is no-throw by construction; the injection
      // models a fault at the boundary's edge, after cleanup has run.
      throwIfJitPublishStepArmedForTest(11);
    } catch (...) {
      poisonUnitDeletionTracking();
    }
  });

  // The 3.11 attribute-cache default is explicitly OFF until the MR-09
  // pull-based invalidation acceptance; neither shadow nor canary may walk
  // an unaccepted IC arm (dev plan MR-04).
  getMutableConfig().attr_caches = false;
  // Artifact sharing across functions stays off as policy: the death
  // watch removed the old safety hazard, but twin adoption is scheduling
  // policy with its own acceptance, outside this milestone.
  getMutableConfig().auto_code_twin_dedup = false;
  if (!canary_requested) {
    // Shadow owns the same front-end/compiler context as the executing JIT
    // so HIR, optimization, LIR, register allocation and target relocation
    // all run normally. It deliberately omits CompiledFunction/cinderjit
    // initialization, interpreter entry installation, generator types,
    // audit hooks and OSR.
    cinderx::ModuleState* shadow_mod_state = cinderx::getModuleState();
    // Construct all throwing state locally and publish it only after every
    // constructor succeeds. A failed import must not leave a
    // half-initialized allocator or context behind while Config still says
    // kNotInitialized.
    std::unique_ptr<ICodeAllocator> code_allocator{CodeAllocator::make()};
    auto jit_context = std::make_unique<CompilerContext<Compiler>>();
    jit::codegen::initThreadStateOffset();
    shadow_mod_state->code_allocator = std::move(code_allocator);
    shadow_mod_state->jit_context = std::move(jit_context);
    getMutableConfig().state = State::kShadow;
    return 0;
  }

  // canary (MR-04): the test-only execution mode.  Machine code compiles,
  // installs and executes for functions inside the strict execute surface;
  // everything the plan defers stays off -- no generator types, no OSR, no
  // audit instrumentation, no JIT list, no automatic scheduling.  The
  // product auto-JIT remains unavailable.
  //
  // Speculative type guards are MR-07 work: the plan's MR-04 eligibility
  // excludes them, and consuming the interpreter's specialized forms is
  // what produces them.  Only the executing mode compiles the unspecialized
  // forms; shadow keeps its accepted MR-03 behaviour and RuntimeTests keep
  // the build default.  Warm and organic timings still exercise real
  // interpreter cache state -- the quickening happens either way -- without
  // putting a guard-and-deopt arm under machine code.
  getMutableConfig().specialized_opcodes = false;
  // Refuse, do not silently clear: an immortal artifact skips the
  // dictionary anchor and the function association, so the guarded entry
  // would refuse a function the registry calls compiled -- and the death
  // watch cannot reconcile that, because an immortalized function never
  // dies.  The configuration is simply incompatible with this milestone's
  // ownership model, and saying so beats behaving as if it were honoured.
  if (getConfig().immortalize_compiled_functions) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "PYTHONJITIMMORTALIZECOMPILEDFUNCTIONS is not supported by the "
        "CPython 3.11 canary: an immortal artifact anchors no function, and "
        "without function watchers the registry would keep dangling "
        "entries");
    return -1;
  }
  // Same doctrine for the instrumentation toggle.  The flag patches
  // sys.setprofile/settrace/monitoring to pause the whole JIT while any
  // instrumentation is active and re-enable it afterwards; this mode
  // already delivers tracing correctness through the per-boundary polls
  // and the guarded entry's per-call check, and the toggle's disable arm
  // (generator send patching, on-stack frame deopt) has no audited 3.11
  // story.  Refuse the configuration instead of running an unaudited
  // pause-the-world path alongside the audited polling one.
  if (getConfig().support_instrumentation) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "PYTHONJITSUPPORTINSTRUMENTATION is not supported by the CPython "
        "3.11 canary: tracing is handled by bytecode-boundary polls and "
        "the guarded entry, not by pausing the JIT");
    return -1;
  }
  // The executing mode publishes its ledger through co_extra, and CPython
  // 3.11's code_dealloc walks every registered slot from zero up to
  // ce_size, calling each freefunc whether or not this code object ever
  // populated the slot.  Capping our writes protects the slots ABOVE our
  // index; nothing can protect a foreign slot BELOW it, because
  // publishing the ledger forces ce_size past it -- and if that foreign
  // freefunc dies before the last code object does (a ctypes callback at
  // interpreter shutdown), the walk calls freed memory.  Refuse the mode
  // up front when the registration order puts a foreign user below us;
  // moving the ledger off co_extra is the long-term answer.
  {
    Py_ssize_t extra_index = cinderx::getModuleState()->code_extra_index;
    if (extra_index != 0) {
      PyErr_Format(
          PyExc_RuntimeError,
          "the CPython 3.11 canary requires the first code-extra slot, but "
          "another component registered %zd slot(s) before CinderX loaded; "
          "interpreter shutdown would walk that component's freefunc on "
          "every code object the JIT touches",
          extra_index);
      return -1;
    }
  }
  if (jit::initCompiledFunctionType() < 0) {
    return -1;
  }
  {
    cinderx::ModuleState* canary_mod_state = cinderx::getModuleState();
    std::unique_ptr<ICodeAllocator> code_allocator{CodeAllocator::make()};
    auto jit_context = std::make_unique<CompilerContext<Compiler>>();
    jit::codegen::initThreadStateOffset();
    canary_mod_state->code_allocator = std::move(code_allocator);
    canary_mod_state->jit_context = std::move(jit_context);
  }
  {
    PyObject* canary_mod =
        _Ci_CreateBuiltinModule(&jit_module_311_canary, "cinderjit");
    if (canary_mod == nullptr) {
      return -1;
    }
    jitCtx()->setCinderJitModule(Ref<>::steal(canary_mod));
  }
  // The canary is an execution mode, not a library a harness assembles:
  // without the frame evaluator, the interpreter's specialized CALL pushes
  // the callee frame inline and never consults the vectorcall entry, so a
  // function the control plane calls compiled runs interpreted on the
  // hottest path there is.  The compile entries install the evaluator
  // lazily as a backstop; the mode's own initialization installs it up
  // front and verifies the installation took, so is_jit_compiled() means
  // what it says from the first call on.
  if (Ci_InitFrameEvalFunc() < 0) {
    return -1;
  }
  if (!Ci_EvalHook311_IsInstalled()) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "the CPython 3.11 canary could not take the frame-evaluator entry "
        "point; another component holds it");
    return -1;
  }
  getMutableConfig().state = State::kRunning;
  return 0;
#endif

  std::unique_ptr<JITList> jit_list;
  if (!getConfig().jit_list.filename.empty()) {
    if (getConfig().allow_jit_list_wildcards) {
      jit_list = jit::WildcardJITList::create();
    } else {
      jit_list = jit::JITList::create();
    }
    if (jit_list == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate JIT list");
      return -1;
    }
    jit_list->parseFile(getConfig().jit_list.filename.c_str());
  }

  jit::init_jit_genobject_type();

  // Initialize the CompiledFunction type.
  if (jit::initCompiledFunctionType() < 0) {
    return -1;
  }

  // Create code allocator after jit::Config has been filled out.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  mod_state->code_allocator.reset(CodeAllocator::make());

  // Discover the TLS offset for PyThreadState so the JIT can load tstate
  // directly from the thread-local segment register instead of calling
  // _PyThreadState_GetCurrent().
  jit::codegen::initThreadStateOffset();

  // Initialize the main compiler object and its context.  This will throw if
  // asmjit cannot initialize.
  cinderx::getModuleState()->jit_context.reset(new CompilerContext<Compiler>());

  PyObject* mod = _Ci_CreateBuiltinModule(&jit_module, "cinderjit");
  if (mod == nullptr) {
    return -1;
  }

  jitCtx()->setCinderJitModule(Ref<>::steal(mod));

  if (install_jit_audit_hook() < 0 || register_fork_callback(mod) < 0) {
    return -1;
  }

  if (getConfig().support_instrumentation) {
    patchSysMonitoringFunctions(mod);
    patchSysSetProfileAndSetTrace(mod);
  }

  setInterpreterJitFlag(true);
  getMutableConfig().osr_capable = true;
  getMutableConfig().state = State::kRunning;
  syncOSRFlags();

  mod_state->jit_list = std::move(jit_list);

  // JIT is now fully initialized.  If it was configured to run automatically on
  // startup, start scheduling functions for compilation now.
  if (auto compile_n = getConfig().compile_after_n_calls;
      compile_n.has_value()) {
    if (getConfig().auto_classify) {
      JIT_DLOG(
          "Configuring AutoJIT to classify new functions after {} calls",
          *compile_n);
    } else {
      if (Ci_InitFrameEvalFunc() < 0) {
        return -1;
      }
      schedule_existing_functions_for_jit(*compile_n);
    }
  } else if (mod_state->jit_list.get() != nullptr) {
    if (rescheduleJitList() < 0) {
      return -1;
    }
  }

  return 0;
}

void finalize() {
  FreeThreadedJITEntrypointGuard guard;
  if (!isJitInitialized()) {
    return;
  }

  if (isJitShadow()) {
    getMutableConfig().state = State::kFinalizing;

    auto mod_state = cinderx::getModuleState();
    auto* context = static_cast<Context*>(mod_state->jit_context.get());
    if (context != nullptr) {
      context->clearDeoptStats();
      context->releaseReferences();
      context->codeOuterFunctions().clear();
    }
    mod_state->registered_compilation_units.clear();
    JIT_CHECK(
        hir::preloaderManager().empty(),
        "Shadow JIT cannot be finalized while compilation is active size:{} "
        "is_global:{}",
        hir::preloaderManager().size(),
        hir::preloaderManager().isGlobalManager());
    if (mod_state->cache_manager != nullptr) {
      mod_state->cache_manager->clear();
    }
    mod_state->jit_context.reset();
    mod_state->code_allocator.reset();
    setCodeDestroyedHook(nullptr);
    getMutableConfig().state = State::kNotInitialized;
    return;
  }

  // Disable the JIT first so nothing we do in here ends up attempting to
  // invoke the JIT while we're finalizing our data structures.
  getMutableConfig().state = State::kFinalizing;
  setInterpreterJitFlag(false);
  syncOSRFlags();

  // Deopt all JIT generators, since JIT generators reference code and other
  // metadata that we will be freeing later in this function.
  PyUnstable_GC_VisitObjects(deopt_gen_visitor, nullptr);

  JIT_DLOG(
      "CinderX JIT Total Compilation Time: {}", jitCtx()->totalCompileTime());

  if (getConfig().log.dump_stats) {
    dump_jit_stats();
  }

  // Deopt all compiled functions before releasing references. This ensures
  // that if any JIT Python functions are invoked as side-effects during the
  // remainder of shutdown, they will go through the interpreter.
  auto& shutdown_funcs = jitCtx()->compiledFuncs();
  for (auto it = shutdown_funcs.begin(); it != shutdown_funcs.end();) {
    BorrowedRef<PyFunctionObject> func = it->first;
    // Advance before deoptFuncImpl() which erases func from funcs,
    // invalidating the iterator pointing to it.
    ++it;
    deoptFuncImpl(func);
  }

#if PY_VERSION_HEX < 0x030C0000
  // On 3.11 the registry must be empty once the loop above has run: a
  // function still holding a machine-code entrypoint here would be called
  // into freed code during the rest of shutdown.  The emptiness is not
  // observable from Python -- the module is being torn down -- so the
  // invariant is enforced where it is knowable.
  JIT_CHECK(
      jitCtx()->compiledFuncs().empty(),
      "JIT finalized with {} function(s) still compiled",
      jitCtx()->compiledFuncs().size());
#endif

  // Always release references from Context objects: C++ clients may have
  // invoked the JIT directly without initializing a full jit::Context.
  jitCtx()->clearDeoptStats();
  jitCtx()->releaseReferences();

#if PY_VERSION_HEX < 0x030C0000
  // Deferred displaced anchors hold the last references to superseded
  // artifacts; release them at this controlled point -- the state is
  // kFinalizing, so nothing their destructors run can re-arm the JIT --
  // before the registries they reach back into are torn down.
  jitCtx()->drainDeferredAnchorReleases();
  // The deopted set holds borrowed entries on this branch; empty it
  // before the context goes so nothing walks it during teardown.
  jitCtx()->clearDeoptedFuncs();
#endif

  deleteJitList();

  // Clear some global maps that reference Python data.
  auto mod_state = cinderx::getModuleState();
  auto& jit_code_outer_funcs = jitCtx()->codeOuterFunctions();
  auto& jit_reg_units = mod_state->registered_compilation_units;
  jit_code_outer_funcs.clear();
  jit_reg_units.clear();
  JIT_CHECK(
      hir::preloaderManager().empty(),
      "JIT cannot be finalized while batch compilation is active size:{} "
      "is_global:{}",
      hir::preloaderManager().size(),
      hir::preloaderManager().isGlobalManager());

  mod_state->jit_context.reset();
  mod_state->code_allocator.reset();

#ifndef WIN32
  g_aot_ctx.destroy();
#endif

  restoreSysMonitoringRegisterCallback();
  restoreSysSetProfileAndSetTrace();

  // Past this point nothing can service a code-death notification.
  setCodeDestroyedHook(nullptr);

  getMutableConfig().state = State::kNotInitialized;
  getMutableConfig().osr_capable = false;
  syncOSRFlags();
}

bool shouldScheduleCompile(BorrowedRef<PyFunctionObject> func) {
  BorrowedRef<PyCodeObject> code{func->func_code};
  return shouldAlwaysScheduleCompile(code) ||
      getConfig().compile_after_n_calls.has_value();
}

bool shouldSkipAutoJitScheduleForSteadyColdCode(
    BorrowedRef<PyFunctionObject> func) {
  if (!getConfig().auto_classify ||
      !getConfig().compile_after_n_calls.has_value() ||
      cinderx::getModuleState()->jit_list != nullptr) {
    return false;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (shouldAlwaysScheduleCompile(code)) {
    return false;
  }
  CodeExtra* extra = codeExtraIfExists(code);
  if (extra == nullptr) {
    return false;
  }

  uint32_t skey_word = Ci_code_extra_load_skey_acquire(extra);
  if ((skey_word & kSkeyDecidedColdBit) == 0 ||
      (skey_word & kSkeyValidBit) == 0) {
    return false;
  }

  StructureKey key = StructureKey::unpack(skey_word & kSkeyPayloadMask);
  GateContext steady_state{};
  ThresholdDecision decision = computeThresholdForCode(
      code, key, steady_state, *getConfig().compile_after_n_calls);
  bool freeze_low_roi = decision.branch_reason == BranchReason::LowRoi &&
      (decision.limit >= kAutoJitLongLowRoiFreezeThreshold ||
       key.family == Family::Trivial);
  return decision.limit >= kAutoJitInterpretOnlyThreshold || freeze_low_roi;
}

// Fast path for creating a function whose code object has already been
// JIT-compiled with the same globals/builtins (e.g. a closure or generator
// expression recreated each iteration of a hot loop). It skips the
// eligibility re-checks and the compiled_codes_ hashmap lookup + lock, then
// hands off to the normal finalizeFunc() so the function is fully tracked for
// deopt and the CompiledFunction's lifetime is anchored exactly as on the slow
// path. Returns true if the function was attached.
bool tryAttachCachedCompiledEntry(BorrowedRef<PyFunctionObject> func) {
  if (jitCtx() == nullptr) {
    return false;
  }
#if PY_VERSION_HEX < 0x030C0000
  // This path exists to hand an already-compiled artifact to a freshly
  // created function object.  Fresh attachment is scheduled with auto-JIT
  // (MR-11); the path stays closed here.
  (void)func;
  return false;
#else
  // When an explicit JIT list is active, eligibility is per-function: it
  // depends on the function's (possibly renamed) module/qualname, not just its
  // code object (see getCompilationEligibility -> jit_list->lookupFunc). The
  // fast path skips that check, so bail out and let the slow path enforce the
  // JIT list exactly. Auto-JIT (no list) is unaffected.
  if (cinderx::getModuleState()->jit_list != nullptr) {
    return false;
  }
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  CodeExtra* extra = codeExtra(code);
  if (extra == nullptr) {
    return false;
  }
  auto* compiled = reinterpret_cast<CompiledFunction*>(
      _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (compiled == nullptr) {
    return false;
  }
  // Re-attaching compiled code during active instrumentation would bypass
  // monitoring events; mirror the guard the slow reoptFunc() path uses.
  if (isInstrumentationActive()) {
    return false;
  }
  // The cached entry is only valid for the exact (code, globals, builtins)
  // tuple it was compiled under -- see jit::CompilationKey. The cached
  // CompiledFunction was already produced by the normal eligibility path, and
  // code flags are immutable, so the original module/suppress checks still
  // apply. Re-attachment is intentionally independent of scheduling thresholds.
  if (extra->jit_globals != func->func_globals ||
      extra->jit_builtins != func->func_builtins) {
    return false;
  }
  // finalizeFunc() does the full association (compiled_funcs_ tracking,
  // CompiledFunction function set, func_dict strong ref, vectorcall + static
  // entry), so deopt and GC behave identically to the slow path.
  return jitCtx()->finalizeFunc(func, compiled);
#endif
}

bool scheduleJitCompile(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;

  // CPython 3.11 shadow requests are dispatched synchronously by the observe
  // gate. Never attach entries, retain scheduling metadata, or route them into
  // the executing JIT scheduler.
  if (isJitShadow()) {
    return false;
  }

  if (shouldSkipAutoJitScheduleForRoiBackoffFrozen(func)) {
    return true;
  }

  if (tryAttachCachedCompiledEntry(func)) {
    return true;
  }

  if (shouldSkipAutoJitScheduleForSteadyColdCode(func)) {
    incAutoJitGateStat(g_auto_jit_gate_stats.classified_schedule_cold_skip);
    return true;
  }

  auto eligible = getCompilationEligibility(func);
  if (eligible == JitEligibility::Ineligible) {
    return false;
  }
  trackEligibleCodeObjects(func, func->func_code, eligible);

  // If we're not eligible due to the JIT list check if we have config (e.g.
  // auto jit, jit all, or jit all static methods) that makes compilation happen
  // automatically.
  if (eligible == JitEligibility::Eligible && !shouldScheduleCompile(func)) {
    return false;
  }

  // Could be creating an inner function with an already-compiled code object.
  if (isJitCompiled(func)) {
    return true;
  }

  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized and instrumentation isn't active.
  // Reopting during active instrumentation would bypass monitoring events.
  //
  // Without this, nested code objects would almost never run their compiled
  // functions if the user had disabled the JIT without selecting to deopt
  // everything.  This is a weird behavior though, to have "new" functions get
  // JIT-compiled code despite the JIT being disabled.
  if (!isInstrumentationActive() && reoptFunc(func)) {
    return true;
  }

  if (!isJitUsable()) {
    return false;
  }

  setVectorcall(func, jitVectorcall);
  if (!registerFunction(func)) {
    setVectorcall(func, getInterpretedVectorcall(func));
    return false;
  }

  return true;
}

Result compileFunction(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;
  if (!isJitInitialized()) {
    return Result::NOT_INITIALIZED;
  }
#if PY_VERSION_HEX < 0x030C0000
  // Every 3.11 compile-and-install request funnels through the execute
  // surface: force_compile and the observe dispatch obey the same strict
  // eligibility, so nothing outside the MR-04 surface can reach machine
  // code no matter which door it came in.
  if (Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
    return Result::CANNOT_SPECIALIZE;
  }
#endif
  if (isJitPaused()) {
    return Result::PAUSED;
  }
  if (!isJitUsable()) {
    return Result::UNKNOWN_ERROR;
  }

  auto& jit_reg_units = cinderx::getModuleState()->registered_compilation_units;
  jit_reg_units.erase(func);
  return compile_func(func);
}

void uncompile(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;
  ::uncompileImpl(func);
}

Result compileFunctionWithOSR(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;
  if (!isJitInitialized()) {
    return Result::NOT_INITIALIZED;
  }
  if (isJitPaused()) {
    return Result::PAUSED;
  }
  if (!isJitUsable()) {
    return Result::UNKNOWN_ERROR;
  }

  JIT_CHECK(func != nullptr, "OSR only supports function frames");
  BorrowedRef<PyCodeObject> pinned_code{func->func_code};
  if (!osrCompileBudgetCheck(pinned_code)) {
    return Result::CANNOT_SPECIALIZE;
  }

  try {
    hir::IsolatedPreloaders isolated_preloaders;
    hir::Preloader* preloader = preload(func);
    if (preloader == nullptr || func->func_code != pinned_code) {
      PyErr_Clear();
      return Result::CANNOT_SPECIALIZE;
    }

    Result result = compilePreloader(*preloader, func);
    if (result != Result::OK && PyErr_Occurred()) {
      PyErr_Clear();
    }
    return result;
  } catch (const std::exception& exn) {
    JIT_DLOG("{}", exn.what());
    PyErr_Clear();
    return Result::UNKNOWN_ERROR;
  }
}

std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func,
    bool forcePreload) {
  // Add one for the original function itself.  When forcePreload is set the
  // caller wants all dependents regardless of the configured limit.
  size_t limit = forcePreload ? std::numeric_limits<size_t>::max()
                              : getConfig().preload_dependent_limit + 1;

  std::deque<BorrowedRef<PyFunctionObject>> worklist;
  std::vector<BorrowedRef<PyFunctionObject>> result;

  // Track units that are deleted while preloading.
  std::unordered_set<PyObject*> deleted_units;
  // The deletion record is taken inside a death callback, where nothing may
  // throw; losing one makes the pruning below unsound.
  bool deletion_record_lost = false;

  worklist.push_back(func);

  auto shouldPreload = [&](BorrowedRef<PyFunctionObject> f) {
    return !isPreloaded(f) &&
        (forcePreload ||
         getCompilationEligibility(f) != JitEligibility::Ineligible);
  };

  while (worklist.size() > 0 && result.size() < limit) {
    BorrowedRef<PyFunctionObject> f = worklist.front();
    worklist.pop_front();

    hir::Preloader* preloader =
        preloadWithUnitDeletedCallback(f, [&](BorrowedRef<> deleted_unit) {
          try {
            throwIfJitPublishStepArmedForTest(9);
            deleted_units.emplace(deleted_unit);
          } catch (const std::bad_alloc&) {
            deletion_record_lost = true;
          }
        });

    if (preloader == nullptr) {
      return {};
    }
    result.emplace_back(f);

    // Preload all invoked Static Python functions because then the JIT can
    // compile them and emit direct calls to them from the original function.
    for (const auto& [descr, target] : preloader->invokeFunctionTargets()) {
      if (!target->isFunction() || !target->is_statically_typed) {
        continue;
      }
      BorrowedRef<PyFunctionObject> target_func = target->func();
      if (shouldPreload(target_func)) {
        worklist.push_back(target_func);
      }
    }

    // Preload any used functions in case the JIT might want to inline them.
    for (const auto& [idx, name] : preloader->globalNames()) {
      BorrowedRef<> obj = preloader->global(idx);
      if (!obj || !PyFunction_Check(obj)) {
        continue;
      }
      BorrowedRef<PyFunctionObject> target_func = obj.get();
      if (shouldPreload(target_func)) {
        worklist.push_back(target_func);
      }
    }
  }

  if (deletion_record_lost || consumeUnitDeletionTrackingPoison()) {
    // A unit died during preloading and its record was lost; the pruning
    // below could keep a dead function.  Fail the whole preload
    // conservatively -- the caller's contract is an empty result with a
    // Python error set.
    PyErr_NoMemory();
    return {};
  }

  // Prune out all functions that are no longer alive / allocated.
  result.erase(
      std::remove_if(
          result.begin(),
          result.end(),
          [&](BorrowedRef<PyFunctionObject> func) {
            return deleted_units.contains(func.getObj()) ||
                deleted_units.contains(func->func_code);
          }),
      result.end());

  std::reverse(result.begin(), result.end());
  return result;
}

void codeDestroyed(BorrowedRef<PyCodeObject> code) {
  FreeThreadedJITEntrypointGuard guard;
  triggerStatsOnCodeDestroyed();
#if PY_VERSION_HEX < 0x030C0000
  // The notification comes from the code-extra free function (no watcher)
  // and shadow populates the registries too: gate on "initialized".
  const bool deliverable = isJitInitialized();
#else
  const bool deliverable = isJitUsable();
#endif
  if (deliverable) {
    auto mod_state = cinderx::getModuleState();
    if (!mod_state) {
      return;
    }
    auto& jit_reg_units = mod_state->registered_compilation_units;
    jit_reg_units.erase(code.getObj());
    if (auto* ctx = jitCtx()) {
      ctx->codeOuterFunctions().erase(code);
    }
    notifyUnitDeletedDuringPreload(mod_state, code.getObj());
  }
}

void funcUnpublishedInContext(
    Context* ctx,
    BorrowedRef<PyFunctionObject> func) {
  auto mod_state = cinderx::getModuleState();
  if (!mod_state) {
    return;
  }
  FreeThreadedJITEntrypointGuard guard;

  unregisterFunctionCodes(func);

  // The registries and entry caches to clean belong to the context whose
  // watch (or caller) delivered the event; the module-state bookkeeping
  // above is process-wide either way.  A null context means the event
  // arrived after jit::finalize() took the context down.
  if (ctx != nullptr) {
    ctx->funcDestroyed(func);
    ctx->clearFunctionEntryCache(func);
  }
}

void funcUnpublished(BorrowedRef<PyFunctionObject> func) {
  funcUnpublishedInContext(jitCtx(), func);
}

void funcDestroyedInContext(Context* ctx, BorrowedRef<PyFunctionObject> func) {
  if (cinderx::getModuleState() == nullptr) {
    return;
  }
  // The counter is the proof that death notifications are delivered at
  // all, so only the real sources may move it: the function watcher on
  // 3.12+ and the weak-reference death watch on 3.11.  Administrative
  // unpublication goes through funcUnpublished() and manufactures no
  // death.
  triggerStatsOnFunctionDestroyed();
  funcUnpublishedInContext(ctx, func);
}

void funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  funcDestroyedInContext(jitCtx(), func);
}

void funcModified(BorrowedRef<PyFunctionObject> func) {
  FreeThreadedJITEntrypointGuard guard;
  deoptFunc(func);
  // Clean up registrations for the old code object. At this point
  // func->func_code still refers to the old code. The caller will update
  // func->func_code and call scheduleCompile() to re-register with the new
  // code.
  unregisterFunctionCodes(func);
  // Reset OSR state for the old code — must happen before func_code is
  // updated by the caller.
  resetOSRState(reinterpret_cast<PyCodeObject*>(func->func_code));
}

void typeDestroyed(BorrowedRef<PyTypeObject> type) {
  if (CompilerContext<Compiler>* ctx = jitCtx()) {
    ctx->notifyTypeModified(type, nullptr);
  }
}

void typeModified(BorrowedRef<PyTypeObject> type) {
  if (CompilerContext<Compiler>* ctx = jitCtx()) {
    ctx->notifyTypeModified(type, type);
  }
}

void typeNameModified(BorrowedRef<PyTypeObject> type) {
  // We assume that this is a very rare case, and simply give up on tracking
  // the type if it happens.
  if (CompilerContext<Compiler>* ctx = jitCtx()) {
    ctx->notifyTypeModified(type, type);
  }
}

Result compilePreloaderImpl(
    jit::CompilerContext<Compiler>* jit_ctx,
    const hir::Preloader& preloader,
    BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 execution goes through the canary mode only (MR-04):
  // outside it -- shadow, observe, or an internal caller sneaking past the
  // mode plumbing -- refuse before anything can allocate or install a
  // CompiledFunction.  The canary requests themselves have already passed
  // the execute-surface choke in compileFunction().
  if (getConfig().state != State::kRunning) {
    return Result::CANNOT_SPECIALIZE;
  }
#endif

  // We are compiling the code stored in the preloader. Includes an optional
  // function if we have the function for which we're currently compiling. We
  // could just be compiling a code object for a nested function in which case
  // the outer owning function should be registered in
  // jitCtx()->codeOuterFunctions()
  JIT_CHECK(
      func != nullptr ||
          jitCtx()->codeOuterFunctions().contains(preloader.code()),
      "expected function or outer function to be registered");
  BorrowedRef<PyCodeObject> code = preloader.code();

  if (code == nullptr) {
    JIT_DLOG("Can't compile {} as it has no code object", preloader.fullname());
    return Result::CANNOT_SPECIALIZE;
  }

  BorrowedRef<PyDictObject> builtins = preloader.builtins();
  BorrowedRef<PyDictObject> globals = preloader.globals();

  if (!hasRequiredFlags(code)) {
    JIT_DLOG(
        "Can't compile {} due to missing required code flags",
        preloader.fullname());
    return Result::CANNOT_SPECIALIZE;
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    JIT_DLOG(
        "Can't compile {} as it has had the JIT suppressed",
        preloader.fullname());
    return Result::CANNOT_SPECIALIZE;
  }
  constexpr int forbidden_flags = CO_ASYNC_GENERATOR;
  if (code->co_flags & forbidden_flags) {
    JIT_DLOG(
        "Cannot JIT compile {} as it has prohibited code flags: 0x{:x}",
        preloader.fullname(),
        code->co_flags & forbidden_flags);
    return Result::CANNOT_SPECIALIZE;
  }

  CompilationKey key{code, builtins, globals};
  {
    // Attempt to atomically transition the code from "not compiled" to "in
    // progress".
    ThreadedCompileSerialize guard;
    auto compiled = jit_ctx->lookupCode(code, builtins, globals);
    if (compiled != nullptr) {
      // The code is already compiled and we have a CompiledFunction object.
      // Just finalize the code.
      if (func != nullptr) {
#if PY_VERSION_HEX < 0x030C0000
        // Same reason as reoptFunc(): a refusal here must not come back as
        // Result::OK, which the caller would report as installed.
        if (Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
          return Result::CANNOT_SPECIALIZE;
        }
#endif
        if (getThreadedCompileContext().compileRunning()) {
          // Can't call finalizeFunc on a worker thread - it does Python
          // allocations (PyDict_New, etc.) which require the GIL. Defer
          // finalization to after multi-threaded compile completes.
          jit_ctx->addDeferredFinalization(func, compiled);
        } else if (!jit_ctx->finalizeFunc(func, compiled)) {
          JIT_CHECK(PyErr_Occurred(), "should have set an error");
          // Failed to finalize, probably due to failure to allocate
          return Result::PYTHON_EXCEPTION;
        }
      }
      return Result::OK;
    } else if (jit_ctx->hasCompletedCompile(key)) {
      // We're in the multi-threaded scenario we've created the
      // CompiledFunctionData and will create the CompiledFunction at the end
      return Result::OK;
    } else if (!jit_ctx->addActiveCompile(key)) {
      // The compilation is in-flight on another thread
      return Result::ALREADY_SCHEDULED;
    }
  }

  std::optional<CompiledFunctionData> compiled_func;
  try {
    compiled_func = jit_ctx->compiler().Compile(preloader);
  } catch (const std::exception& exn) {
    JIT_DLOG("{}", exn.what());
  }

  ThreadedCompileSerialize guard;
  jit_ctx->removeActiveCompile(key);
  if (!compiled_func.has_value()) {
    return Result::UNKNOWN_ERROR;
  }

  register_pycode_debug_symbol(
      preloader.code(), preloader.fullname().c_str(), *compiled_func);

#if PY_VERSION_HEX < 0x030C0000
  // The compile succeeded; publication can still fail -- an allocation
  // inside CompiledFunction::create() or the code-extra reservation, or
  // the post-compile artifact refusal.  Every deterministic refusal is
  // answered identically by the pre-compile choke, so this is the honesty
  // path for the failures only the install can see: without it,
  // force_compile() reported OK for a function whose every call runs
  // interpreted.  The mapping mirrors the established refusal surface:
  // an exception propagates, anything else is CANNOT_SPECIALIZE.
  if (!jit_ctx->codeCompiled(func, key, std::move(*compiled_func))) {
    return PyErr_Occurred() ? Result::PYTHON_EXCEPTION
                            : Result::CANNOT_SPECIALIZE;
  }
  // The publication queued the anchor it displaced; drain it before the
  // verdict, not after.  The release runs arbitrary Python -- a __del__
  // can call disable() or uncompile the function -- and a verdict computed
  // before that ran would report an installation that no longer exists.
  // Re-verify after the drain, so OK still means what the control plane
  // defines it to mean: this function's calls execute machine code as
  // this returns.
  jit_ctx->drainDeferredAnchorReleases();
  if (func != nullptr && !isJitCompiled(func)) {
    return Result::CANNOT_SPECIALIZE;
  }
#else
  jit_ctx->codeCompiled(func, key, std::move(*compiled_func));
#endif

  return Result::OK;
}

} // namespace jit
