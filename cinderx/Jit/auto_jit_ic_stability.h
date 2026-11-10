// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/code.h"
#include "cinderx/Jit/behavior_classifier.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/module_state.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace jit {

inline constexpr uint64_t kICStabilitySampleInterval = 4;
inline constexpr uint8_t kICStabilityRequiredSamples = 3;
inline constexpr uint64_t kICStabilityMaxExtraCalls = 128;
inline constexpr uint16_t kICStabilityMaxSites = 32;
inline constexpr size_t kICStabilityMaxCodeUnits = 1024;

enum class ICStabilityReason : uint8_t {
  None,
  Disabled,
  NonAuto,
  Unsupported,
  NoSites,
  NoKnownSites,
  AllocationFailure,
  Explicit,
  ROI,
  Stable,
  Cap,
  Waiting,
  Unknown,
  Changed,
  Overflow,
  LoopWork,
};

inline const char* icStabilityReasonName(ICStabilityReason reason) {
  switch (reason) {
    case ICStabilityReason::None:
      return "none";
    case ICStabilityReason::Disabled:
      return "disabled";
    case ICStabilityReason::NonAuto:
      return "non_auto";
    case ICStabilityReason::Unsupported:
      return "unsupported";
    case ICStabilityReason::NoSites:
      return "no_sites";
    case ICStabilityReason::NoKnownSites:
      return "no_known_sites";
    case ICStabilityReason::AllocationFailure:
      return "allocation_failure";
    case ICStabilityReason::Explicit:
      return "explicit";
    case ICStabilityReason::ROI:
      return "roi";
    case ICStabilityReason::Stable:
      return "stable";
    case ICStabilityReason::Cap:
      return "cap";
    case ICStabilityReason::Waiting:
      return "waiting";
    case ICStabilityReason::Unknown:
      return "unknown";
    case ICStabilityReason::Changed:
      return "changed";
    case ICStabilityReason::Overflow:
      return "overflow";
    case ICStabilityReason::LoopWork:
      return "loop_work";
  }
  return "unsupported";
}

// Called only after the existing AutoJIT threshold/classifier permits
// admission. Calls do not measure work inside a loop; an extra call-count wait
// can keep a long-running loop interpreted for its entire useful lifetime.
// Preserve the classifier's loop-work policy without changing its threshold or
// bypasses.
inline ICStabilityReason icStabilityAdmissionBypassReason(
    ICStabilityReason existing,
    const std::optional<StructureKey>& key) {
  if (existing != ICStabilityReason::None) {
    return existing;
  }
  return key.has_value() && key->loop_score > 0 ? ICStabilityReason::LoopWork
                                                : ICStabilityReason::None;
}

enum class ICStabilitySnapshotStatus : uint8_t {
  Observed,
  NoSites,
  NoKnownSites,
  Unsupported,
};

struct ICStabilitySnapshot {
  ICStabilitySnapshotStatus status{ICStabilitySnapshotStatus::Unsupported};
  uint64_t fingerprint{0};
  uint16_t sites{0};
  uint16_t ready_sites{0};
  uint16_t skipped_sites{0};
};

// Owned by a separate code-extra slot. No references to runtime objects, and
// no change to the 64-byte CodeExtra shared with the interpreter.
struct ICStabilityState {
  uint64_t first_calls{0};
  uint64_t last_sample_calls{0};
  uint64_t fingerprint{0};
  uint64_t admission_calls{0};
  uint64_t last_observation_calls{0};
  uint16_t samples{0};
  uint8_t consecutive_ready{0};
  ICStabilityReason terminal_reason{ICStabilityReason::None};
  uint16_t sites{0};
  uint16_t ready_sites{0};
  ICStabilitySnapshotStatus last_status{ICStabilitySnapshotStatus::Unsupported};
  bool has_observation{false};
  bool admission_started{false};
};
static_assert(sizeof(ICStabilityState) <= 56);
static_assert(std::is_trivially_copyable_v<ICStabilityState>);
static_assert(std::is_standard_layout_v<ICStabilityState>);

struct ICStabilityDecision {
  ICStabilityReason reason{ICStabilityReason::None};
  bool defer{false};
  bool sampled{false};
  bool changed{false};
  bool unknown{false};
  bool terminal_reuse{false};
  uint16_t samples{0};
  uint64_t first_calls{0};
  uint64_t admission_calls{0};
  uint16_t sites{0};
  uint16_t ready_sites{0};
  uint16_t skipped_sites{0};
};

inline bool icStabilitySampleDue(
    const ICStabilityState& state,
    uint64_t calls,
    bool admission_ready = true) {
  if (state.terminal_reason != ICStabilityReason::None) {
    return false;
  }
  if (state.has_observation && calls <= state.last_observation_calls) {
    return false;
  }
  if (admission_ready && !state.admission_started &&
      calls >
          std::numeric_limits<uint64_t>::max() - kICStabilityMaxExtraCalls) {
    return false;
  }
  if (state.admission_started &&
      (calls < state.admission_calls ||
       calls - state.admission_calls >= kICStabilityMaxExtraCalls)) {
    return false;
  }
  // Refresh at the actual gate, even if a warmup sample was taken recently.
  // This is a validity check, not an extra interval-qualified stable sample.
  if (admission_ready && !state.admission_started) {
    return true;
  }
  if (!state.has_observation) {
    return admission_ready || calls != 0;
  }
  if (admission_ready &&
      state.consecutive_ready >= kICStabilityRequiredSamples) {
    return true;
  }
  return calls - state.last_sample_calls >= kICStabilitySampleInterval;
}

// Observe during existing warmup, then admit only after the original gate is
// ready. Unknown sites are skipped; a non-empty known subset can mature. Warmup
// never permanently decides Stable/NoKnownSites. Only post-gate calls consume
// the extra-wait budget, and a gate refresh never fabricates spaced samples.
inline ICStabilityDecision updateICStabilityState(
    ICStabilityState& state,
    uint64_t calls,
    const ICStabilitySnapshot* snapshot,
    bool admission_ready = true) {
  ICStabilityDecision result;
  auto finish = [&](ICStabilityReason reason, bool terminal = false) {
    if (terminal) {
      state.terminal_reason = reason;
    }
    result.reason = reason;
    result.samples = state.samples;
    result.first_calls = state.first_calls;
    result.admission_calls = state.admission_calls;
    result.sites = state.sites;
    result.ready_sites = state.ready_sites;
    result.skipped_sites = state.sites - state.ready_sites;
    return result;
  };
  if (state.terminal_reason != ICStabilityReason::None) {
    result.terminal_reuse = true;
    return finish(state.terminal_reason);
  }
  bool sample_due = icStabilitySampleDue(state, calls, admission_ready);
  if (state.has_observation && calls < state.last_observation_calls) {
    return finish(ICStabilityReason::Overflow, admission_ready);
  }
  if (admission_ready && !state.admission_started) {
    state.admission_started = true;
    state.admission_calls = calls;
    if (calls >
        std::numeric_limits<uint64_t>::max() - kICStabilityMaxExtraCalls) {
      return finish(ICStabilityReason::Overflow, true);
    }
  }
  if (admission_ready && state.admission_started) {
    if (calls < state.admission_calls) {
      return finish(ICStabilityReason::Overflow, true);
    }
    if (calls - state.admission_calls >= kICStabilityMaxExtraCalls) {
      return finish(ICStabilityReason::Cap, true);
    }
  }
  if (!sample_due) {
    // Share a same-call warmup observation with admission instead of scanning
    // twice. Older, incomplete windows continue at the established cadence.
    if (state.has_observation && calls == state.last_observation_calls) {
      switch (state.last_status) {
        case ICStabilitySnapshotStatus::NoSites:
          return finish(ICStabilityReason::NoSites, admission_ready);
        case ICStabilitySnapshotStatus::NoKnownSites:
          return finish(ICStabilityReason::NoKnownSites, admission_ready);
        case ICStabilitySnapshotStatus::Unsupported:
          return finish(ICStabilityReason::Unsupported, admission_ready);
        case ICStabilitySnapshotStatus::Observed:
          if (state.consecutive_ready >= kICStabilityRequiredSamples) {
            return finish(ICStabilityReason::Stable, admission_ready);
          }
          break;
      }
    }
    result.defer = admission_ready;
    return finish(ICStabilityReason::Waiting);
  }
  bool interval_elapsed = !state.has_observation ||
      calls - state.last_sample_calls >= kICStabilitySampleInterval;
  state.has_observation = true;
  state.last_observation_calls = calls;
  if (snapshot == nullptr ||
      snapshot->status == ICStabilitySnapshotStatus::Unsupported ||
      snapshot->ready_sites > kICStabilityMaxSites ||
      snapshot->ready_sites > snapshot->sites ||
      snapshot->skipped_sites > snapshot->sites ||
      static_cast<uint32_t>(snapshot->ready_sites) + snapshot->skipped_sites !=
          snapshot->sites) {
    state.last_status = ICStabilitySnapshotStatus::Unsupported;
    state.consecutive_ready = 0;
    state.last_sample_calls = calls;
    return finish(ICStabilityReason::Unsupported, admission_ready);
  }
  const uint16_t previous_ready_sites = state.ready_sites;
  state.last_status = snapshot->status;
  state.sites = snapshot->sites;
  state.ready_sites = snapshot->ready_sites;
  if (snapshot->status == ICStabilitySnapshotStatus::NoSites ||
      snapshot->sites == 0) {
    state.last_status = ICStabilitySnapshotStatus::NoSites;
    state.consecutive_ready = 0;
    state.last_sample_calls = calls;
    return finish(ICStabilityReason::NoSites, admission_ready);
  }
  if (snapshot->status == ICStabilitySnapshotStatus::NoKnownSites ||
      snapshot->ready_sites == 0) {
    state.last_status = ICStabilitySnapshotStatus::NoKnownSites;
    state.consecutive_ready = 0;
    state.last_sample_calls = calls;
    return finish(ICStabilityReason::NoKnownSites, admission_ready);
  }

  result.sampled = true;
  result.changed = state.consecutive_ready != 0 &&
      (state.fingerprint != snapshot->fingerprint ||
       previous_ready_sites != snapshot->ready_sites);
  result.unknown = snapshot->skipped_sites != 0;
  if (state.samples == 0) {
    state.first_calls = calls;
  }
  if (result.changed || state.consecutive_ready == 0) {
    state.consecutive_ready = 1;
    state.last_sample_calls = calls;
  } else if (interval_elapsed) {
    if (state.consecutive_ready < kICStabilityRequiredSamples) {
      state.consecutive_ready++;
    }
    state.last_sample_calls = calls;
  }
  state.fingerprint = snapshot->fingerprint;
  if (state.samples < std::numeric_limits<uint16_t>::max()) {
    state.samples++;
  }
  if (state.consecutive_ready >= kICStabilityRequiredSamples) {
    return finish(ICStabilityReason::Stable, admission_ready);
  }
  result.defer = admission_ready;
  return finish(
      result.unknown       ? ICStabilityReason::Unknown
          : result.changed ? ICStabilityReason::Changed
                           : ICStabilityReason::Waiting);
}

#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000 && \
    !defined(Py_GIL_DISABLED)

namespace hir {
struct LoadAttrInstanceValueCacheInfo {
  uint32_t type_version{0};
  uint16_t value_offset{0};
  Py_ssize_t valid_offset{0};
};

// Read-only wrappers around the existing builder policies, not new guards.
bool icStabilityIntGuardsUsable(PyCodeObject* code);
bool icStabilitySlotCacheUsable(
    PyCodeObject* code,
    const BytecodeInstruction& instruction,
    int name_index);
bool icStabilityInstanceValueCacheUsable(
    PyCodeObject* code,
    const BytecodeInstruction& instruction,
    int name_index,
    LoadAttrInstanceValueCacheInfo* info);
} // namespace hir

namespace ic_stability_detail {

class PreserveError {
 public:
  PreserveError() : saved_{PyErr_GetRaisedException()} {}
  ~PreserveError() {
    PyErr_SetRaisedException(saved_);
  }
  PreserveError(const PreserveError&) = delete;
  PreserveError& operator=(const PreserveError&) = delete;

 private:
  PyObject* saved_;
};

// Mix individual scalar fields; never hash counters, padding, or stale words.
inline void mix(uint64_t& hash, uint64_t value) {
  for (unsigned int i = 0; i < 8; i++) {
    hash ^= value & 0xff;
    hash *= 1099511628211ULL;
    value >>= 8;
  }
}

static_assert(INLINE_CACHE_ENTRIES_LOAD_ATTR == 9);
static_assert(INLINE_CACHE_ENTRIES_STORE_ATTR == 4);
static_assert(offsetof(_PyAttrCache, version) == sizeof(_Py_CODEUNIT));
static_assert(offsetof(_PyAttrCache, index) == 3 * sizeof(_Py_CODEUNIT));
static_assert(
    offsetof(_PyLoadMethodCache, type_version) == sizeof(_Py_CODEUNIT));
static_assert(
    offsetof(_PyLoadMethodCache, keys_version) == 3 * sizeof(_Py_CODEUNIT));

} // namespace ic_stability_detail

// A bounded snapshot of feedback the current builder can consume. Ready says
// nothing about branch frequency or unobserved receiver types. All guards and
// generic fallbacks remain in the generated code.
inline ICStabilitySnapshot observeICStability(PyCodeObject* code) {
  ICStabilitySnapshot result;
  if (code == nullptr || !PyGILState_Check() ||
      !getConfig().specialized_opcodes || code->_co_monitoring != nullptr) {
    return result;
  }
  const size_t length = countIndices(code);
  if (length > kICStabilityMaxCodeUnits) {
    return result;
  }
  uint64_t hash = 14695981039346656037ULL;
  bool checked_int_guards = false;
  bool int_guards_usable = false;
  // Validate all instruction/cache boundaries before invoking builder helpers.
  for (size_t index = 0; index < length;) {
    int raw = _Py_OPCODE(codeUnit(code)[index]);
    if (raw == EXTENDED_ARG || raw == EXTENDED_OPCODE || raw == CACHE ||
        raw == ENTER_EXECUTOR || raw == INSTRUMENTED_INSTRUCTION ||
        raw == INSTRUMENTED_LINE ||
        uninstrument(code, static_cast<int>(index)) != raw) {
      return {};
    }
    Py_ssize_t caches = inlineCacheSize(code, static_cast<int>(index));
    if (caches < 0 || static_cast<size_t>(caches) > length - index - 1) {
      return {};
    }
    index += 1 + static_cast<size_t>(caches);
  }
  for (size_t index = 0; index < length;) {
    BytecodeInstruction instruction{
        BorrowedRef<PyCodeObject>{code}, BCIndex{static_cast<int>(index)}};
    int base = instruction.opcode();
    int arg = instruction.oparg();
    int specialized = instruction.specializedOpcode();
    const size_t caches = inlineCacheSize(code, static_cast<int>(index));
    bool selected = false;
    bool ready = false;
    int category = base;
    uint32_t version = 0;
    uint32_t extra = 0;
    bool int_specialization = false;

    if (base == BINARY_OP &&
        (arg == NB_ADD || arg == NB_SUBTRACT || arg == NB_MULTIPLY ||
         arg == NB_INPLACE_ADD || arg == NB_INPLACE_SUBTRACT ||
         arg == NB_INPLACE_MULTIPLY || arg == NB_SUBSCR)) {
      selected = true;
      if (caches != INLINE_CACHE_ENTRIES_BINARY_OP) {
        return {};
      }
      switch (specialized) {
        case BINARY_OP_ADD_INT:
        case BINARY_OP_ADD_FLOAT:
        case BINARY_OP_ADD_UNICODE:
          ready = arg == NB_ADD || arg == NB_INPLACE_ADD;
          break;
        case BINARY_OP_SUBTRACT_INT:
        case BINARY_OP_SUBTRACT_FLOAT:
          ready = arg == NB_SUBTRACT || arg == NB_INPLACE_SUBTRACT;
          break;
        case BINARY_OP_MULTIPLY_INT:
        case BINARY_OP_MULTIPLY_FLOAT:
          ready = arg == NB_MULTIPLY || arg == NB_INPLACE_MULTIPLY;
          break;
        case BINARY_OP_SUBSCR_DICT:
        case BINARY_OP_SUBSCR_LIST_INT:
        case BINARY_OP_SUBSCR_TUPLE_INT:
          ready = arg == NB_SUBSCR;
          break;
        default:
          break;
      }
      int_specialization = ready &&
          (specialized == BINARY_OP_ADD_INT ||
           specialized == BINARY_OP_SUBTRACT_INT ||
           specialized == BINARY_OP_MULTIPLY_INT);
    } else if (base == LOAD_ATTR || base == STORE_ATTR) {
      selected = true;
      if (caches !=
          static_cast<size_t>(
              base == LOAD_ATTR ? INLINE_CACHE_ENTRIES_LOAD_ATTR
                                : INLINE_CACHE_ENTRIES_STORE_ATTR)) {
        return {};
      }
      int name_index = base == LOAD_ATTR ? loadAttrIndex(arg) : arg;
      if (code->co_names == nullptr || !PyTuple_Check(code->co_names) ||
          name_index < 0 || name_index >= PyTuple_GET_SIZE(code->co_names) ||
          !PyUnicode_Check(PyTuple_GET_ITEM(code->co_names, name_index))) {
        return {};
      }
      // LOAD_ATTR_INSTANCE_VALUE currently lowers through the generic path.
      // Keep it skipped: its type/layout changes do not invalidate a consumed
      // specialization, and must not delay admission of the remaining subset.
      if ((base == LOAD_ATTR && specialized == LOAD_ATTR_SLOT) ||
          (base == STORE_ATTR && specialized == STORE_ATTR_SLOT)) {
        version = instruction.attrCacheTypeVersion();
        extra = instruction.attrCacheIndex();
        ready = version != 0 && extra >= sizeof(PyObject) &&
            hir::icStabilitySlotCacheUsable(code, instruction, name_index);
      } else if (
          base == LOAD_ATTR && (arg & 1) &&
          specialized == LOAD_ATTR_METHOD_WITH_VALUES) {
        version = instruction.cacheU32(2);
        extra = instruction.cacheU32(4);
        uintptr_t descriptor = 0;
        static_assert(
            sizeof(descriptor) <=
            sizeof(((_PyLoadMethodCache*)nullptr)->descr));
        std::memcpy(
            &descriptor, &codeUnit(code)[index + 6].cache, sizeof(descriptor));
        // Only check presence; do not dereference or hash the cached pointer.
        ready = version != 0 && extra != 0 && descriptor != 0;
      } else if (
          base == LOAD_ATTR && !(arg & 1) && specialized == LOAD_ATTR_MODULE) {
        // The builder consumes the module category, not the cache's keys.
        ready = true;
      }
    }

    if (selected) {
      result.sites++;
      if (ready && int_specialization) {
        if (!checked_int_guards) {
          int_guards_usable = hir::icStabilityIntGuardsUsable(code);
          checked_int_guards = true;
        }
        ready = int_guards_usable;
      }
      if (ready) {
        category = specialized;
        if (++result.ready_sites > kICStabilityMaxSites) {
          return {};
        }
        ic_stability_detail::mix(hash, index);
        ic_stability_detail::mix(hash, arg);
        ic_stability_detail::mix(hash, category);
        ic_stability_detail::mix(hash, version);
        ic_stability_detail::mix(hash, extra);
      } else {
        result.skipped_sites++;
      }
    }
    index += caches + 1;
  }
  ic_stability_detail::mix(hash, result.ready_sites);
  result.fingerprint = hash;
  result.status = result.sites == 0 ? ICStabilitySnapshotStatus::NoSites
      : result.ready_sites == 0     ? ICStabilitySnapshotStatus::NoKnownSites
                                    : ICStabilitySnapshotStatus::Observed;
  return result;
}

inline void initAutoJitStabilityCodeExtraIndex() {
  auto* module = cinderx::getModuleState();
  if (module == nullptr || module->auto_jit_ic_stability_extra_index != -1) {
    return;
  }
  ic_stability_detail::PreserveError preserve;
  module->auto_jit_ic_stability_extra_index =
      PyUnstable_Eval_RequestCodeExtraIndex(PyMem_Free);
}

inline void finiAutoJitStabilityCodeExtraIndex() {
  if (auto* module = cinderx::getModuleState()) {
    // Existing code objects retain CPython's freefunc; never free their data
    // through a second owner, including during module teardown.
    module->auto_jit_ic_stability_extra_index = -1;
  }
}

inline ICStabilityDecision
runICStabilityCheck(PyCodeObject* code, uint64_t calls, bool admission_ready) {
  ICStabilityDecision failed;
  failed.reason = ICStabilityReason::AllocationFailure;
  failed.first_calls = calls;
  failed.admission_calls = admission_ready ? calls : 0;
  if (code == nullptr || !PyGILState_Check()) {
    failed.reason = ICStabilityReason::Unsupported;
    return failed;
  }
  auto* module = cinderx::getModuleState();
  if (module == nullptr || module->auto_jit_ic_stability_extra_index < 0) {
    return failed;
  }
  ic_stability_detail::PreserveError preserve;
  Py_ssize_t slot = module->auto_jit_ic_stability_extra_index;
  void* data = nullptr;
  auto* object = reinterpret_cast<PyObject*>(code);
  if (PyUnstable_Code_GetExtra(object, slot, &data) < 0) {
    return failed;
  }
  ICStabilityState initial;
  auto* state =
      data == nullptr ? &initial : static_cast<ICStabilityState*>(data);
  ICStabilitySnapshot snapshot;
  bool sample_due = icStabilitySampleDue(*state, calls, admission_ready);
  if (sample_due) {
    snapshot = observeICStability(code);
  }
  auto decision = updateICStabilityState(
      *state, calls, sample_due ? &snapshot : nullptr, admission_ready);
  if (data == nullptr &&
      (decision.defer || (!admission_ready && state->has_observation))) {
    auto* owned = static_cast<ICStabilityState*>(
        PyMem_Calloc(1, sizeof(ICStabilityState)));
    if (owned == nullptr) {
      decision.reason = ICStabilityReason::AllocationFailure;
      decision.defer = false;
      return decision;
    }
    *owned = initial;
    if (PyUnstable_Code_SetExtra(object, slot, owned) < 0) {
      PyMem_Free(owned);
      decision.reason = ICStabilityReason::AllocationFailure;
      decision.defer = false;
      return decision;
    }
  }
  return decision;
}

inline ICStabilityDecision observeICStabilityWarmup(
    PyCodeObject* code,
    uint64_t calls) {
  return runICStabilityCheck(code, calls, false);
}

inline ICStabilityDecision checkICStabilityAdmission(
    PyCodeObject* code,
    uint64_t calls) {
  return runICStabilityCheck(code, calls, true);
}

#else

inline ICStabilitySnapshot observeICStability(PyCodeObject*) {
  return {};
}
inline void initAutoJitStabilityCodeExtraIndex() {}
inline void finiAutoJitStabilityCodeExtraIndex() {}
inline ICStabilityDecision observeICStabilityWarmup(PyCodeObject*, uint64_t) {
  ICStabilityDecision result;
  result.reason = ICStabilityReason::Unsupported;
  return result;
}
inline ICStabilityDecision checkICStabilityAdmission(
    PyCodeObject*,
    uint64_t calls) {
  ICStabilityDecision result;
  result.reason = ICStabilityReason::Unsupported;
  result.first_calls = calls;
  result.admission_calls = calls;
  return result;
}

#endif

} // namespace jit
