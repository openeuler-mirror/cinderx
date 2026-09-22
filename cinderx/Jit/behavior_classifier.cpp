// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/behavior_classifier.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/osr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jit {

namespace {

constexpr uint32_t kDimCountFloor = 2;
constexpr uint32_t kLowCutoffPct = 10;
constexpr uint32_t kMidCutoffPct = 25;
constexpr uint32_t kHighCutoffPct = 50;
constexpr uint8_t kMixedMinBucket = 2;
constexpr uint8_t kMixedBucketDelta = 1;
constexpr uint8_t kRiskSuspendBucket = 2;
constexpr uint8_t kRiskDynamicBucket = 2;
constexpr uint32_t kRiskExceptionFloor = 2;
constexpr uint32_t kRiskEffectiveInstructionFloor = 200;
// Threshold the released shapes are held at while the process is still
// proving itself. It sits below the interpret-only freeze line and carries
// no LowRoi reason, so the code keeps counting calls without being frozen:
// whatever accumulated during the hold converts into a compile on the first
// gate check after the release, and long-lived workloads lose nothing.
// This is deliberately a second gate, not a freeze: a single shape whose
// own call count reaches the hold threshold compiles on that evidence even
// if the process-wide budget never releases -- 65535 interpreted calls of
// one function are overwhelming proof of profitability on their own.
constexpr uint32_t kLowRoiWarmHoldThreshold = 65535;
constexpr uint32_t kStartupDeferThresholdFactor = 1u << 20;
constexpr uint32_t kSteadyNonnumericWarmupThreshold = 1000;
constexpr uint8_t kWorkDimCount = static_cast<uint8_t>(WorkDim::kCount);

struct Signature {
  std::array<uint32_t, static_cast<size_t>(WorkDim::kCount)> counts{};
  uint32_t exception_control_count{0};
  uint32_t n_eff{0};
  std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES> backedges{};
  size_t stored_backedge_count{0};
  uint32_t seen_backedge_count{0};
  uint8_t loop_score{0};
};

struct RankedDim {
  WorkDim dim;
  uint8_t bucket;
  uint32_t count;
};

uint8_t dimIndex(WorkDim dim) {
  return static_cast<uint8_t>(dim);
}

bool hasRequiredFlags(BorrowedRef<PyCodeObject> code) {
  constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;
  return (code->co_flags & kRequiredCodeFlags) == kRequiredCodeFlags;
}

bool nameEquals(BorrowedRef<PyObject> name, const char* expected) {
  if (name == nullptr || !PyUnicode_Check(name)) {
    return false;
  }
  const char* utf8 = PyUnicode_AsUTF8(name);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return false;
  }
  return std::strcmp(utf8, expected) == 0;
}

const char* unicodeUtf8OrNull(BorrowedRef<PyObject> obj) {
  if (obj == nullptr || !PyUnicode_Check(obj)) {
    return nullptr;
  }
  const char* utf8 = PyUnicode_AsUTF8(obj);
  if (utf8 == nullptr) {
    PyErr_Clear();
    return nullptr;
  }
  return utf8;
}

bool contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
      value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isStdlibFilename(std::string_view filename) {
  return contains(filename, "/lib/python3.");
}

bool isStdlibAsyncioEventLoopFrameworkFilename(std::string_view filename) {
  if (!isStdlibFilename(filename)) {
    return false;
  }
  return endsWith(filename, "/asyncio/base_events.py") ||
      endsWith(filename, "/asyncio/events.py") ||
      endsWith(filename, "/asyncio/selector_events.py") ||
      endsWith(filename, "/asyncio/tasks.py") ||
      endsWith(filename, "/asyncio/unix_events.py") ||
      endsWith(filename, "/selectors.py");
}

uint8_t bucketDim(uint32_t count, uint32_t n_eff) {
  if (n_eff == 0 || count < kDimCountFloor) {
    return 0;
  }
  uint64_t scaled_count = static_cast<uint64_t>(count) * 100;
  if (scaled_count >= static_cast<uint64_t>(kHighCutoffPct) * n_eff) {
    return 3;
  }
  if (scaled_count >= static_cast<uint64_t>(kMidCutoffPct) * n_eff) {
    return 2;
  }
  if (scaled_count >= static_cast<uint64_t>(kLowCutoffPct) * n_eff) {
    return 1;
  }
  return 0;
}

std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)> bucketize(
    const Signature& sig) {
  std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)> buckets{};
  for (size_t i = 0; i < buckets.size(); ++i) {
    buckets[i] = bucketDim(sig.counts[i], sig.n_eff);
  }
  return buckets;
}

bool allBucketsZero(
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  return std::all_of(buckets.begin(), buckets.end(), [](uint8_t bucket) {
    return bucket == 0;
  });
}

uint8_t activeDimMask(
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  uint8_t mask = 0;
  for (WorkDim dim :
       {WorkDim::Compute,
        WorkDim::Control,
        WorkDim::Object,
        WorkDim::Dispatch,
        WorkDim::Dynamic}) {
    if (buckets[dimIndex(dim)] > 0) {
      mask |= activeDimMaskFor(dim);
    }
  }
  return mask;
}

uint8_t tieRank(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return 0;
    case WorkDim::Dispatch:
      return 1;
    case WorkDim::Object:
      return 2;
    case WorkDim::Control:
      return 3;
    case WorkDim::Dynamic:
      return 4;
    case WorkDim::Suspend:
      return 5;
    case WorkDim::kCount:
      break;
  }
  return 255;
}

std::array<RankedDim, static_cast<size_t>(WorkDim::kCount)> rankDims(
    const Signature& sig,
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  std::array<RankedDim, static_cast<size_t>(WorkDim::kCount)> ranked{{
      {WorkDim::Compute,
       buckets[dimIndex(WorkDim::Compute)],
       sig.counts[dimIndex(WorkDim::Compute)]},
      {WorkDim::Control,
       buckets[dimIndex(WorkDim::Control)],
       sig.counts[dimIndex(WorkDim::Control)]},
      {WorkDim::Object,
       buckets[dimIndex(WorkDim::Object)],
       sig.counts[dimIndex(WorkDim::Object)]},
      {WorkDim::Dispatch,
       buckets[dimIndex(WorkDim::Dispatch)],
       sig.counts[dimIndex(WorkDim::Dispatch)]},
      {WorkDim::Suspend,
       buckets[dimIndex(WorkDim::Suspend)],
       sig.counts[dimIndex(WorkDim::Suspend)]},
      {WorkDim::Dynamic,
       buckets[dimIndex(WorkDim::Dynamic)],
       sig.counts[dimIndex(WorkDim::Dynamic)]},
  }};

  std::sort(
      ranked.begin(), ranked.end(), [](const RankedDim& a, const RankedDim& b) {
        if (a.bucket != b.bucket) {
          return a.bucket > b.bucket;
        }
        if (a.count != b.count) {
          return a.count > b.count;
        }
        return tieRank(a.dim) < tieRank(b.dim);
      });
  return ranked;
}

Family familyForFirstDim(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return Family::NumericLoop;
    case WorkDim::Control:
      return Family::BranchFSM;
    case WorkDim::Object:
      return Family::ObjectManipulator;
    case WorkDim::Dispatch:
      return Family::CallDispatcher;
    case WorkDim::Suspend:
      return Family::AsyncStateMachine;
    case WorkDim::Dynamic:
      return Family::ReflectionMeta;
    case WorkDim::kCount:
      break;
  }
  return Family::Trivial;
}

uint8_t codeSizeBucket(uint32_t n_eff) {
  if (n_eff >= 500) {
    return 3;
  }
  if (n_eff >= 100) {
    return 2;
  }
  if (n_eff >= 50) {
    return 1;
  }
  return 0;
}

uint8_t deriveRiskReason(
    const Signature& sig,
    const std::array<uint8_t, static_cast<size_t>(WorkDim::kCount)>& buckets) {
  uint8_t reason = kRiskNone;
  if (buckets[dimIndex(WorkDim::Suspend)] >= kRiskSuspendBucket) {
    reason |= kRiskSuspend;
  }
  if (buckets[dimIndex(WorkDim::Dynamic)] >= kRiskDynamicBucket) {
    reason |= kRiskDynamic;
  }
  if (sig.exception_control_count >= kRiskExceptionFloor) {
    reason |= kRiskException;
  }
  if (sig.n_eff >= kRiskEffectiveInstructionFloor) {
    reason |= kRiskHugeCode;
  }
  return reason;
}

uint8_t loopScore(
    const std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES>& edges,
    size_t stored_edge_count,
    uint32_t seen_edge_count) {
  if (seen_edge_count == 0) {
    return 0;
  }
  if (seen_edge_count >= CI_OSR_MAX_BACKEDGES) {
    return 3;
  }

  std::array<std::pair<int, int>, CI_OSR_MAX_BACKEDGES * 2> events{};
  size_t event_count = 0;
  for (size_t i = 0; i < stored_edge_count; ++i) {
    auto [src, tgt] = edges[i];
    events[event_count++] = {tgt, 1};
    events[event_count++] = {src + 1, -1};
  }
  std::sort(events.begin(), events.begin() + event_count);

  int cur = 0;
  int depth = 0;
  for (size_t i = 0; i < event_count; ++i) {
    cur += events[i].second;
    depth = std::max(depth, cur);
  }

  uint8_t nesting_score = static_cast<uint8_t>(std::min(depth, 3));
  uint8_t count_score =
      seen_edge_count >= 4 ? 3 : (seen_edge_count >= 2 ? 2 : 1);
  return std::min<uint8_t>(3, std::max(nesting_score, count_score));
}

std::optional<Signature> scanCode(BorrowedRef<PyCodeObject> code) {
  Signature sig;
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    int opcode = instr.opcode();
    OpcodeClass cls = opcodeClassOf(opcode);
    if (cls == OpcodeClass::Invalid) {
      return std::nullopt;
    }
    if (cls == OpcodeClass::Ignored) {
      continue;
    }

    sig.n_eff++;
    if (isWorkDim(cls)) {
      sig.counts[dimIndex(toWorkDim(cls))]++;
    }
    if (isExceptionControlOpcode(opcode)) {
      sig.exception_control_count++;
    }

    if (instr.isBranch()) {
      BCIndex src = instr.opcodeIndex();
      BCIndex tgt = instr.getJumpTarget().asIndex();
      if (tgt < src) {
        if (sig.stored_backedge_count < sig.backedges.size()) {
          sig.backedges[sig.stored_backedge_count++] = {
              src.value(), tgt.value()};
        }
        sig.seen_backedge_count++;
      }
    }
  }

  sig.loop_score = loopScore(
      sig.backedges, sig.stored_backedge_count, sig.seen_backedge_count);
  return sig;
}

uint32_t saturatingMul(uint32_t value, uint32_t factor) {
  uint64_t product = static_cast<uint64_t>(value) * factor;
  return product > std::numeric_limits<uint32_t>::max()
      ? std::numeric_limits<uint32_t>::max()
      : static_cast<uint32_t>(product);
}

std::optional<std::pair<WorkDim, WorkDim>> decodeMixedShape(MixedShape shape) {
  if (shape == kMixedShapeNone) {
    return std::nullopt;
  }
  MixedShape current = 1;
  for (uint8_t i = 0; i < kWorkDimCount; ++i) {
    for (uint8_t j = i + 1; j < kWorkDimCount; ++j) {
      if (current == shape) {
        return std::make_pair(static_cast<WorkDim>(i), static_cast<WorkDim>(j));
      }
      current++;
    }
  }
  return std::nullopt;
}

bool dimInSet(WorkDim dim, std::initializer_list<WorkDim> dims) {
  return std::find(dims.begin(), dims.end(), dim) != dims.end();
}

bool mixedShapeAllIn(MixedShape shape, std::initializer_list<WorkDim> dims) {
  auto decoded = decodeMixedShape(shape);
  if (!decoded.has_value()) {
    return false;
  }
  return dimInSet(decoded->first, dims) && dimInSet(decoded->second, dims);
}

bool mixedShapeContains(MixedShape shape, WorkDim dim) {
  auto decoded = decodeMixedShape(shape);
  if (!decoded.has_value()) {
    return false;
  }
  return decoded->first == dim || decoded->second == dim;
}

bool isPureControlExceptionRisk(const StructureKey& key) {
  return key.risk_reason == kRiskException &&
      key.active_dim_mask == activeDimMaskFor(WorkDim::Control);
}

bool isLowRoiAsyncioEventLoopFrameworkShape(const StructureKey& key) {
  if (key.is_static || key.is_suspendable || key.highRisk() ||
      key.computeHint()) {
    return false;
  }
  if (key.family == Family::ObjectManipulator) {
    return key.loop_score == 0 && key.code_size_bucket == 0;
  }
  if (key.family == Family::BranchFSM) {
    return key.loop_score > 0 && key.code_size_bucket <= 1;
  }
  if (key.family == Family::ReflectionMeta) {
    return key.loop_score >= 2 && key.code_size_bucket <= 2;
  }
  return false;
}

bool isStdlibAsyncioEventLoopFrameworkHelper(
    BorrowedRef<PyCodeObject> code,
    const StructureKey& key,
    const GateContext& context) {
  if (context.startup_phase || !isLowRoiAsyncioEventLoopFrameworkShape(key)) {
    return false;
  }
  const char* filename = unicodeUtf8OrNull(code->co_filename);
  if (filename == nullptr) {
    return false;
  }
  return isStdlibAsyncioEventLoopFrameworkFilename(filename);
}

bool shouldAllowSteadyStatePlainGenerator(
    BorrowedRef<PyCodeObject> code,
    const StructureKey& key,
    const GateContext& context) {
  if (context.startup_phase || key.is_static) {
    return false;
  }
  if ((code->co_flags & CO_GENERATOR) == 0) {
    return false;
  }
  if (code->co_flags &
      (CO_COROUTINE | CO_ITERABLE_COROUTINE | CO_ASYNC_GENERATOR)) {
    return false;
  }
  return (key.risk_reason & ~(kRiskSuspend | kRiskException)) == 0;
}

bool isLoadAttrOpcode(int opcode) {
  switch (opcode) {
    case LOAD_ATTR:
    case LOAD_ATTR_CLASS:
    case LOAD_ATTR_CLASS_WITH_METACLASS_CHECK:
    case LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN:
    case LOAD_ATTR_INSTANCE_VALUE:
    case LOAD_ATTR_METHOD_LAZY_DICT:
    case LOAD_ATTR_METHOD_NO_DICT:
    case LOAD_ATTR_METHOD_WITH_VALUES:
    case LOAD_ATTR_MODULE:
    case LOAD_ATTR_NONDESCRIPTOR_NO_DICT:
    case LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES:
    case LOAD_ATTR_PROPERTY:
    case LOAD_ATTR_SLOT:
    case LOAD_ATTR_WITH_HINT:
      return true;
    default:
      return false;
  }
}

bool isStoreAttrOpcode(int opcode) {
  switch (opcode) {
    case STORE_ATTR:
    case STORE_ATTR_INSTANCE_VALUE:
    case STORE_ATTR_SLOT:
    case STORE_ATTR_WITH_HINT:
      return true;
    default:
      return false;
  }
}

bool isReturnOpcode(int opcode) {
  return opcode == RETURN_VALUE || opcode == INSTRUMENTED_RETURN_VALUE ||
      opcode == RETURN_CONST || opcode == RETURN_PRIMITIVE;
}

bool shouldDeferSteadyStateCallOnlyDispatchLoop(
    const StructureKey& key,
    const GateContext& context) {
  const uint8_t kCallOnlyLoopDims =
      activeDimMaskFor(WorkDim::Object) | activeDimMaskFor(WorkDim::Dispatch);
  return !context.startup_phase && !key.is_static && !key.is_suspendable &&
      !key.highRisk() && key.family == Family::CallDispatcher &&
      key.loop_score == 1 && key.code_size_bucket == 1 &&
      key.active_dim_mask == kCallOnlyLoopDims;
}

} // namespace

WorkDim toWorkDim(OpcodeClass cls) {
  switch (cls) {
    case OpcodeClass::Compute:
      return WorkDim::Compute;
    case OpcodeClass::Control:
      return WorkDim::Control;
    case OpcodeClass::Object:
      return WorkDim::Object;
    case OpcodeClass::Dispatch:
      return WorkDim::Dispatch;
    case OpcodeClass::Suspend:
      return WorkDim::Suspend;
    case OpcodeClass::Dynamic:
      return WorkDim::Dynamic;
    case OpcodeClass::Neutral:
    case OpcodeClass::Ignored:
    case OpcodeClass::Invalid:
      break;
  }
  return WorkDim::Compute;
}

uint8_t activeDimMaskFor(WorkDim dim) {
  switch (dim) {
    case WorkDim::Compute:
      return 1u << 0;
    case WorkDim::Control:
      return 1u << 1;
    case WorkDim::Object:
      return 1u << 2;
    case WorkDim::Dispatch:
      return 1u << 3;
    case WorkDim::Dynamic:
      return 1u << 4;
    case WorkDim::Suspend:
    case WorkDim::kCount:
      break;
  }
  return 0;
}

bool StructureKey::hasActiveDim(WorkDim dim) const {
  return (active_dim_mask & activeDimMaskFor(dim)) != 0;
}

bool StructureKey::computeHint() const {
  return hasActiveDim(WorkDim::Compute);
}

bool StructureKey::computeDominantHint() const {
  return family == Family::NumericLoop ||
      (family == Family::Mixed &&
       mixedShapeContains(mixed_shape, WorkDim::Compute));
}

uint8_t StructureKey::activeDimCount() const {
  uint8_t count = 0;
  uint8_t mask = active_dim_mask;
  while (mask != 0) {
    count += mask & 1u;
    mask >>= 1;
  }
  return count;
}

uint32_t StructureKey::pack() const {
  uint32_t payload = 0;
  payload |= (is_eafp_benign ? 1u : 0u) << 24;
  payload |= (static_cast<uint32_t>(mixed_shape) & 0xFu) << 20;
  payload |= (static_cast<uint32_t>(family) & 0xFu) << 16;
  payload |= (static_cast<uint32_t>(loop_score) & 0x3u) << 14;
  payload |= (is_suspendable ? 1u : 0u) << 13;
  payload |= (is_static ? 1u : 0u) << 12;
  payload |= (static_cast<uint32_t>(risk_reason) & 0xFu) << 7;
  payload |= (static_cast<uint32_t>(code_size_bucket) & 0x3u) << 5;
  payload |= static_cast<uint32_t>(active_dim_mask) & 0x1Fu;
  return payload & kSkeyPayloadMask;
}

StructureKey StructureKey::unpack(uint32_t payload) {
  payload &= kSkeyPayloadMask;
  StructureKey key;
  key.is_eafp_benign = ((payload >> 24) & 0x1u) != 0;
  key.mixed_shape = static_cast<MixedShape>((payload >> 20) & 0xFu);
  key.family = static_cast<Family>((payload >> 16) & 0xFu);
  key.loop_score = static_cast<uint8_t>((payload >> 14) & 0x3u);
  key.is_suspendable = ((payload >> 13) & 0x1u) != 0;
  key.is_static = ((payload >> 12) & 0x1u) != 0;
  key.risk_reason = static_cast<uint8_t>((payload >> 7) & 0xFu);
  key.code_size_bucket = static_cast<uint8_t>((payload >> 5) & 0x3u);
  key.active_dim_mask = static_cast<uint8_t>(payload & 0x1Fu);
  if (key.family != Family::Mixed) {
    key.mixed_shape = kMixedShapeNone;
  }
  return key;
}

MixedShape encodeMixedShape(WorkDim a, WorkDim b) {
  uint8_t ai = dimIndex(a);
  uint8_t bi = dimIndex(b);
  if (ai >= kWorkDimCount || bi >= kWorkDimCount || ai == bi) {
    return kMixedShapeNone;
  }
  if (ai > bi) {
    std::swap(ai, bi);
  }

  MixedShape shape = 1;
  for (uint8_t i = 0; i < kWorkDimCount; ++i) {
    for (uint8_t j = i + 1; j < kWorkDimCount; ++j) {
      if (i == ai && j == bi) {
        return shape;
      }
      shape++;
    }
  }
  return kMixedShapeNone;
}

OpcodeClass opcodeClassOf(int canonical_opcode) {
  switch (canonical_opcode) {
    case BINARY_OP:
    case BINARY_OP_ADD_FLOAT:
    case BINARY_OP_ADD_INT:
    case BINARY_OP_ADD_UNICODE:
    case BINARY_OP_EXTEND:
    case BINARY_OP_INPLACE_ADD_UNICODE:
    case BINARY_OP_MULTIPLY_FLOAT:
    case BINARY_OP_MULTIPLY_INT:
    case BINARY_OP_SUBTRACT_FLOAT:
    case BINARY_OP_SUBTRACT_INT:
    case CAST:
    case CAST_CACHED:
    case COMPARE_OP:
    case COMPARE_OP_FLOAT:
    case COMPARE_OP_INT:
    case COMPARE_OP_STR:
    case CONTAINS_OP:
    case CONTAINS_OP_DICT:
    case CONTAINS_OP_SET:
    case CONVERT_PRIMITIVE:
    case IS_OP:
    case LOAD_TYPE:
    case PRIMITIVE_BINARY_OP:
    case PRIMITIVE_BOX:
    case PRIMITIVE_COMPARE_OP:
    case PRIMITIVE_UNARY_OP:
    case PRIMITIVE_UNBOX:
    case REFINE_TYPE:
    case UNARY_INVERT:
    case UNARY_NEGATIVE:
    case UNARY_NOT:
      return OpcodeClass::Compute;

    case CHECK_EG_MATCH:
    case CHECK_EXC_MATCH:
    case CLEANUP_THROW:
    case END_FOR:
    case EXIT_INIT_CHECK:
    case FOR_ITER:
    case FOR_ITER_GEN:
    case FOR_ITER_LIST:
    case FOR_ITER_RANGE:
    case FOR_ITER_TUPLE:
    case INSTRUMENTED_END_FOR:
    case INSTRUMENTED_FOR_ITER:
    case INSTRUMENTED_JUMP_BACKWARD:
    case INSTRUMENTED_JUMP_FORWARD:
    case INSTRUMENTED_NOT_TAKEN:
    case INSTRUMENTED_POP_JUMP_IF_FALSE:
    case INSTRUMENTED_POP_JUMP_IF_NONE:
    case INSTRUMENTED_POP_JUMP_IF_NOT_NONE:
    case INSTRUMENTED_POP_JUMP_IF_TRUE:
    case INSTRUMENTED_RETURN_VALUE:
    case JUMP:
    case JUMP_BACKWARD:
    case JUMP_BACKWARD_JIT:
    case JUMP_BACKWARD_NO_INTERRUPT:
    case JUMP_BACKWARD_NO_JIT:
    case JUMP_FORWARD:
    case JUMP_IF_FALSE:
    case JUMP_IF_TRUE:
    case JUMP_NO_INTERRUPT:
    case NOT_TAKEN:
    case POP_BLOCK:
    case POP_EXCEPT:
    case POP_JUMP_IF_FALSE:
    case POP_JUMP_IF_NONE:
    case POP_JUMP_IF_NONZERO:
    case POP_JUMP_IF_NOT_NONE:
    case POP_JUMP_IF_TRUE:
    case POP_JUMP_IF_ZERO:
    case PUSH_EXC_INFO:
    case RAISE_VARARGS:
    case RERAISE:
    case RETURN_PRIMITIVE:
    case RETURN_VALUE:
    case SETUP_CLEANUP:
    case SETUP_FINALLY:
    case SETUP_WITH:
    case TO_BOOL:
    case TO_BOOL_ALWAYS_TRUE:
    case TO_BOOL_BOOL:
    case TO_BOOL_INT:
    case TO_BOOL_LIST:
    case TO_BOOL_NONE:
    case TO_BOOL_STR:
    case WITH_EXCEPT_START:
      return OpcodeClass::Control;

    case BINARY_OP_SUBSCR_DICT:
    case BINARY_OP_SUBSCR_GETITEM:
    case BINARY_OP_SUBSCR_LIST_INT:
    case BINARY_OP_SUBSCR_LIST_SLICE:
    case BINARY_OP_SUBSCR_STR_INT:
    case BINARY_OP_SUBSCR_TUPLE_INT:
    case BINARY_SLICE:
    case BUILD_CHECKED_LIST:
    case BUILD_CHECKED_LIST_CACHED:
    case BUILD_CHECKED_MAP:
    case BUILD_CHECKED_MAP_CACHED:
    case BUILD_LIST:
    case BUILD_MAP:
    case BUILD_SET:
    case BUILD_SLICE:
    case BUILD_TUPLE:
    case DELETE_ATTR:
    case DELETE_SUBSCR:
    case DICT_MERGE:
    case DICT_UPDATE:
    case FAST_LEN:
    case GET_ITER:
    case GET_LEN:
    case LIST_APPEND:
    case LIST_DEL:
    case LIST_EXTEND:
    case LOAD_ATTR:
    case LOAD_ATTR_CLASS:
    case LOAD_ATTR_CLASS_WITH_METACLASS_CHECK:
    case LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN:
    case LOAD_ATTR_INSTANCE_VALUE:
    case LOAD_ATTR_METHOD_LAZY_DICT:
    case LOAD_ATTR_METHOD_NO_DICT:
    case LOAD_ATTR_METHOD_WITH_VALUES:
    case LOAD_ATTR_MODULE:
    case LOAD_ATTR_NONDESCRIPTOR_NO_DICT:
    case LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES:
    case LOAD_ATTR_PROPERTY:
    case LOAD_ATTR_SLOT:
    case LOAD_ATTR_WITH_HINT:
    case LOAD_FIELD:
    case LOAD_ITERABLE_ARG:
    case LOAD_MAPPING_ARG:
    case LOAD_OBJ_FIELD:
    case LOAD_PRIMITIVE_FIELD:
    case MAP_ADD:
    case MATCH_CLASS:
    case MATCH_KEYS:
    case MATCH_MAPPING:
    case MATCH_SEQUENCE:
    case SEQUENCE_GET:
    case SEQUENCE_SET:
    case SET_ADD:
    case SET_UPDATE:
    case STORE_ATTR:
    case STORE_ATTR_INSTANCE_VALUE:
    case STORE_ATTR_SLOT:
    case STORE_ATTR_WITH_HINT:
    case STORE_FIELD:
    case STORE_OBJ_FIELD:
    case STORE_PRIMITIVE_FIELD:
    case STORE_SLICE:
    case STORE_SUBSCR:
    case STORE_SUBSCR_DICT:
    case STORE_SUBSCR_LIST_INT:
    case TP_ALLOC:
    case TP_ALLOC_CACHED:
    case UNPACK_EX:
    case UNPACK_SEQUENCE:
    case UNPACK_SEQUENCE_LIST:
    case UNPACK_SEQUENCE_TUPLE:
    case UNPACK_SEQUENCE_TWO_TUPLE:
      return OpcodeClass::Object;

    case CALL:
    case CALL_ALLOC_AND_ENTER_INIT:
    case CALL_BOUND_METHOD_EXACT_ARGS:
    case CALL_BOUND_METHOD_GENERAL:
    case CALL_BUILTIN_CLASS:
    case CALL_BUILTIN_FAST:
    case CALL_BUILTIN_FAST_WITH_KEYWORDS:
    case CALL_BUILTIN_O:
    case CALL_FUNCTION_EX:
    case CALL_INTRINSIC_1:
    case CALL_INTRINSIC_2:
    case CALL_ISINSTANCE:
    case CALL_KW:
    case CALL_KW_BOUND_METHOD:
    case CALL_KW_NON_PY:
    case CALL_KW_PY:
    case CALL_LEN:
    case CALL_LIST_APPEND:
    case CALL_METHOD_DESCRIPTOR_FAST:
    case CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS:
    case CALL_METHOD_DESCRIPTOR_NOARGS:
    case CALL_METHOD_DESCRIPTOR_O:
    case CALL_NON_PY_GENERAL:
    case CALL_PY_EXACT_ARGS:
    case CALL_PY_GENERAL:
    case CALL_STR_1:
    case CALL_TUPLE_1:
    case CALL_TYPE_1:
    case INSTRUMENTED_CALL:
    case INSTRUMENTED_CALL_FUNCTION_EX:
    case INSTRUMENTED_CALL_KW:
    case INSTRUMENTED_LOAD_SUPER_ATTR:
    case INVOKE_FUNCTION:
    case INVOKE_FUNCTION_CACHED:
    case INVOKE_INDIRECT_CACHED:
    case INVOKE_METHOD:
    case INVOKE_NATIVE:
    case LOAD_METHOD_STATIC:
    case LOAD_METHOD_STATIC_CACHED:
    case LOAD_SPECIAL:
    case LOAD_SUPER_ATTR:
    case LOAD_SUPER_ATTR_ATTR:
    case LOAD_SUPER_ATTR_METHOD:
    case PUSH_NULL:
      return OpcodeClass::Dispatch;

    case END_ASYNC_FOR:
    case END_SEND:
    case GET_AITER:
    case GET_ANEXT:
    case GET_AWAITABLE:
    case GET_YIELD_FROM_ITER:
    case INSTRUMENTED_END_ASYNC_FOR:
    case INSTRUMENTED_END_SEND:
    case INSTRUMENTED_YIELD_VALUE:
    case RETURN_GENERATOR:
    case SEND:
    case SEND_GEN:
    case YIELD_VALUE:
      return OpcodeClass::Suspend;

    case ANNOTATIONS_PLACEHOLDER:
    case BUILD_INTERPOLATION:
    case BUILD_STRING:
    case BUILD_TEMPLATE:
    case CONVERT_VALUE:
    case COPY_FREE_VARS:
    case DELETE_DEREF:
    case DELETE_GLOBAL:
    case DELETE_NAME:
    case EAGER_IMPORT_NAME:
    case FORMAT_SIMPLE:
    case FORMAT_WITH_SPEC:
    case IMPORT_FROM:
    case IMPORT_NAME:
    case LOAD_BUILD_CLASS:
    case LOAD_CLASS:
    case LOAD_CLOSURE:
    case LOAD_DEREF:
    case LOAD_FROM_DICT_OR_DEREF:
    case LOAD_FROM_DICT_OR_GLOBALS:
    case LOAD_GLOBAL:
    case LOAD_GLOBAL_BUILTIN:
    case LOAD_GLOBAL_MODULE:
    case LOAD_LOCALS:
    case LOAD_NAME:
    case MAKE_CELL:
    case MAKE_FUNCTION:
    case SETUP_ANNOTATIONS:
    case SET_FUNCTION_ATTRIBUTE:
    case STORE_DEREF:
    case STORE_GLOBAL:
    case STORE_NAME:
      return OpcodeClass::Dynamic;

    case COPY:
    case DELETE_FAST:
    case INSTRUMENTED_POP_ITER:
    case INTERPRETER_EXIT:
    case LOAD_COMMON_CONSTANT:
    case LOAD_CONST:
    case LOAD_CONST_IMMORTAL:
    case LOAD_CONST_MORTAL:
    case LOAD_FAST:
    case LOAD_FAST_AND_CLEAR:
    case LOAD_FAST_BORROW:
    case LOAD_FAST_BORROW_LOAD_FAST_BORROW:
    case LOAD_FAST_CHECK:
    case LOAD_FAST_LOAD_FAST:
    case LOAD_LOCAL:
    case LOAD_SMALL_INT:
    case POP_ITER:
    case POP_TOP:
    case PRIMITIVE_LOAD_CONST:
    case STORE_FAST:
    case STORE_FAST_LOAD_FAST:
    case STORE_FAST_MAYBE_NULL:
    case STORE_FAST_STORE_FAST:
    case STORE_LOCAL:
    case STORE_LOCAL_CACHED:
    case SWAP:
      return OpcodeClass::Neutral;

    case CACHE:
    case ENTER_EXECUTOR:
    case EXTENDED_ARG:
    case EXTENDED_OPCODE:
    case INSTRUMENTED_INSTRUCTION:
    case INSTRUMENTED_LINE:
    case INSTRUMENTED_RESUME:
    case NOP:
    case RESERVED:
    case RESUME:
    case RESUME_CHECK:
      return OpcodeClass::Ignored;
  }
  return OpcodeClass::Invalid;
}

bool isExceptionControlOpcode(int canonical_opcode) {
  switch (canonical_opcode) {
    case CHECK_EG_MATCH:
    case CHECK_EXC_MATCH:
    case CLEANUP_THROW:
    case POP_EXCEPT:
    case PUSH_EXC_INFO:
    case RERAISE:
    case WITH_EXCEPT_START:
      return true;
    default:
      return false;
  }
}

bool isAutoJitClassifiable(BorrowedRef<PyCodeObject> code) {
  if (code == nullptr) {
    return false;
  }
  if (!hasRequiredFlags(code)) {
    return false;
  }
  if (nameEquals(code->co_name, "<module>")) {
    return false;
  }
  if (code->co_flags & CO_ASYNC_GENERATOR) {
    return false;
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return false;
  }
  return true;
}

bool shouldDeferSuspendableAutoJitWithoutStructureKey(
    BorrowedRef<PyCodeObject> code,
    const GateContext& context) {
  if (!getConfig().enable_startup_init_policy || !context.startup_phase) {
    return false;
  }
  if (!isAutoJitClassifiable(code)) {
    return false;
  }
  if (code->co_flags & CI_CO_STATICALLY_COMPILED) {
    return false;
  }
  return (code->co_flags &
          (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) != 0;
}

namespace {

// ---- Self-contained EAFP whitelist ---------------------------------------
//
// A function qualifies when its try/except usage is limited to the cache /
// probe idiom: every typed except handler matches a benign exception type
// (KeyError/AttributeError/IndexError), every region guarded by such a
// handler contains no calls (the exception source is a local subscript or
// attribute access, never a callee), and the function contains no explicit
// raise. These functions take the exception path only on cache misses, so
// the static exception-control risk verdict misprices them. A mispredicted
// whitelist entry is still bounded by the deopt-side ROI backoff.

struct ExceptionTableEntry {
  uint32_t start; // byte offsets into co_code
  uint32_t end;
  uint32_t target;
};

bool parseExceptionTable(
    BorrowedRef<PyCodeObject> code,
    std::vector<ExceptionTableEntry>& entries) {
  PyObject* table = code->co_exceptiontable;
  if (table == nullptr || !PyBytes_Check(table)) {
    return false;
  }
  auto data = reinterpret_cast<const uint8_t*>(PyBytes_AS_STRING(table));
  Py_ssize_t size = PyBytes_GET_SIZE(table);
  Py_ssize_t pos = 0;
  auto parse_varint = [&](uint32_t& value) -> bool {
    if (pos >= size) {
      return false;
    }
    uint8_t byte = data[pos++];
    value = byte & 63;
    while (byte & 64) {
      if (pos >= size) {
        return false;
      }
      // Refuse varints that would wrap uint32_t; a well-formed
      // co_exceptiontable entry always fits comfortably in 32 bits.
      if (value > (std::numeric_limits<uint32_t>::max() >> 6)) {
        return false;
      }
      byte = data[pos++];
      value = (value << 6) | (byte & 63);
    }
    return true;
  };
  while (pos < size) {
    uint32_t start = 0, length = 0, target = 0, depth_lasti = 0;
    if (!parse_varint(start) || !parse_varint(length) ||
        !parse_varint(target) || !parse_varint(depth_lasti)) {
      return false;
    }
    // Entries store byte offsets doubled; refuse values whose doubling or
    // start+length sum would wrap.
    constexpr uint32_t kMaxOffset = std::numeric_limits<uint32_t>::max() / 2;
    if (start > kMaxOffset || target > kMaxOffset ||
        length > kMaxOffset - start) {
      return false;
    }
    entries.push_back(
        ExceptionTableEntry{start * 2, (start + length) * 2, target * 2});
  }
  return true;
}

bool isBenignEafpExceptionName(PyObject* name) {
  if (name == nullptr || !PyUnicode_Check(name)) {
    return false;
  }
  return PyUnicode_EqualToUTF8(name, "KeyError") == 1 ||
      PyUnicode_EqualToUTF8(name, "AttributeError") == 1 ||
      PyUnicode_EqualToUTF8(name, "IndexError") == 1;
}

enum class EafpHandlerKind {
  kNotTyped, // cleanup / finally-style entry: no exception type check
  kBenign, // typed handler matching a whitelisted exception
  kOther, // typed handler matching anything else
};

EafpHandlerKind classifyEafpHandler(
    BorrowedRef<PyCodeObject> code,
    BytecodeInstructionBlock& block,
    uint32_t target) {
  constexpr int kHandlerScanWindow = 5;
  bool in_handler = false;
  int remaining = kHandlerScanWindow;
  PyObject* last_global_name = nullptr;
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    uint32_t off = instr.baseIndex().value() * 2;
    if (!in_handler) {
      if (off != target) {
        continue;
      }
      if (instr.opcode() != PUSH_EXC_INFO) {
        return EafpHandlerKind::kNotTyped;
      }
      in_handler = true;
      continue;
    }
    if (remaining-- <= 0) {
      return EafpHandlerKind::kNotTyped;
    }
    switch (instr.opcode()) {
      case LOAD_GLOBAL:
        last_global_name = PyTuple_GetItem(
            code->co_names, static_cast<Py_ssize_t>(instr.oparg() >> 1));
        continue;
      case CHECK_EXC_MATCH:
        return isBenignEafpExceptionName(last_global_name)
            ? EafpHandlerKind::kBenign
            : EafpHandlerKind::kOther;
      default:
        continue;
    }
  }
  return EafpHandlerKind::kNotTyped;
}

bool regionContainsCall(
    BytecodeInstructionBlock& block,
    uint32_t begin,
    uint32_t end) {
  for (auto it = block.begin(); it != block.end(); ++it) {
    BytecodeInstruction instr = *it;
    uint32_t off = instr.baseIndex().value() * 2;
    if (off < begin || off >= end) {
      continue;
    }
    switch (instr.opcode()) {
      case CALL:
      case CALL_FUNCTION_EX:
      case CALL_INTRINSIC_1:
      case CALL_INTRINSIC_2:
      case CALL_KW:
        return true;
      default:
        break;
    }
  }
  return false;
}

bool isSelfContainedEafpCode(BorrowedRef<PyCodeObject> code) {
  std::vector<ExceptionTableEntry> entries;
  if (!parseExceptionTable(code, entries) || entries.empty()) {
    return false;
  }
  BytecodeInstructionBlock block{code};
  bool saw_benign_typed_handler = false;
  for (const auto& entry : entries) {
    switch (classifyEafpHandler(code, block, entry.target)) {
      case EafpHandlerKind::kOther:
        return false;
      case EafpHandlerKind::kBenign:
        if (regionContainsCall(block, entry.start, entry.end)) {
          return false;
        }
        saw_benign_typed_handler = true;
        break;
      case EafpHandlerKind::kNotTyped:
        // Cleanup entry (with/finally); calls on the slow path are fine.
        break;
    }
  }
  if (!saw_benign_typed_handler) {
    return false;
  }
  for (auto it = block.begin(); it != block.end(); ++it) {
    if ((*it).opcode() == RAISE_VARARGS) {
      return false;
    }
  }
  return true;
}

} // namespace

std::optional<StructureKey> deriveStructureKey(BorrowedRef<PyCodeObject> code) {
  if (!isAutoJitClassifiable(code)) {
    return std::nullopt;
  }

  auto sig = scanCode(code);
  if (!sig.has_value()) {
    return std::nullopt;
  }

  auto buckets = bucketize(*sig);
  StructureKey key;
  key.loop_score = sig->loop_score;
  key.is_static = (code->co_flags & CI_CO_STATICALLY_COMPILED) != 0;
  key.is_suspendable =
      (code->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) !=
          0 ||
      sig->counts[dimIndex(WorkDim::Suspend)] > 0;
  key.risk_reason = deriveRiskReason(*sig, buckets);
  // Record self-contained EAFP cache idioms (the handler runs only on cache
  // misses, so the static exception-risk verdict misprices them). The
  // exception risk itself stays in the key; computeThresholdForCode waives
  // it per gate check outside the startup phase, so bootstrap machinery
  // classified while imports are in flight keeps the deferral.
  if ((key.risk_reason & kRiskException) != 0 &&
      isSelfContainedEafpCode(code)) {
    key.is_eafp_benign = true;
  }
  key.code_size_bucket = codeSizeBucket(sig->n_eff);
  key.active_dim_mask = activeDimMask(buckets);

  if (allBucketsZero(buckets)) {
    key.family = Family::Trivial;
    key.mixed_shape = kMixedShapeNone;
    return key;
  }

  auto ranked = rankDims(*sig, buckets);
  const RankedDim& first = ranked[0];
  const RankedDim& second = ranked[1];
  if (first.bucket >= kMixedMinBucket && second.bucket >= kMixedMinBucket &&
      first.bucket - second.bucket <= kMixedBucketDelta) {
    key.family = Family::Mixed;
    key.mixed_shape = encodeMixedShape(first.dim, second.dim);
    return key;
  }

  key.family = familyForFirstDim(first.dim);
  key.mixed_shape = kMixedShapeNone;
  return key;
}

std::optional<StructureKey> getOrComputeStructureKey(
    BorrowedRef<PyCodeObject> code,
    CodeExtra* extra) {
  if (extra == nullptr) {
    return std::nullopt;
  }
  uint32_t word = Ci_code_extra_load_skey_acquire(extra);
  if (word & kSkeyValidBit) {
    return StructureKey::unpack(word & kSkeyPayloadMask);
  }

  auto key = deriveStructureKey(code);
  if (!key.has_value()) {
    return std::nullopt;
  }
  Ci_code_extra_store_skey_release(extra, key->pack() | kSkeyValidBit);
  return key;
}

namespace {

// Process-wide evidence that speculative compilation can amortize. Every
// increment is one interpreted execution of code this series would otherwise
// have compiled, so the budget measures forgone opportunity directly rather
// than elapsed time: call counts are properties of the program, independent
// of machine speed, architecture, and where an integration loader hooks into
// the interpreter lifecycle. A python_startup_no_site-shaped process makes a
// few hundred such calls in total and never reaches the budget; benchmark
// and application workloads reach it during their first warmup moments.
std::atomic<uint64_t> g_low_roi_held_calls{0};
std::atomic<bool> g_low_roi_release_active{false};

bool lowRoiReleaseActiveOrNote() {
  if (g_low_roi_release_active.load(std::memory_order_relaxed)) {
    return true;
  }
  size_t budget = getConfig().auto_classify_low_roi_warm_calls;
  if (budget == 0 ||
      g_low_roi_held_calls.fetch_add(1, std::memory_order_relaxed) + 1 >=
          budget) {
    g_low_roi_release_active.store(true, std::memory_order_relaxed);
    return true;
  }
  return false;
}

} // namespace

void resetLowRoiReleaseState() {
  g_low_roi_held_calls.store(0, std::memory_order_relaxed);
  g_low_roi_release_active.store(false, std::memory_order_relaxed);
}

#ifndef _WIN32
namespace {

// The budget's evidence is per-process execution, and a forked child has
// not executed anything: letting it inherit the parent's counter and sticky
// release hands it unearned proof. Disposable fork children (multiprocessing
// pool workers) then compile eagerly and pay bursts they can never amortize
// -- concurrent_imap measures 75ms with an inherited release versus 42ms
// held, against 51ms for the pre-series baseline. Already-compiled code is
// inherited as-is; only future release decisions start over. Registered at
// shared-object load so it also covers forks taken before the JIT
// initializes; resetting two relaxed atomics is async-signal-safe. If
// registration ever fails (ENOMEM), children simply skip the reset and
// inherit the parent's release state -- the pre-budget behavior, safe but
// less frugal -- so the return value is deliberately not checked.
struct LowRoiForkReset {
  LowRoiForkReset() {
    pthread_atfork(nullptr, nullptr, [] { resetLowRoiReleaseState(); });
  }
};
LowRoiForkReset s_low_roi_fork_reset;

} // namespace
#endif

ThresholdDecision computeThreshold(
    const StructureKey& key,
    const GateContext& context,
    uint32_t global) {
  bool startup_like_family = key.family == Family::CallDispatcher ||
      key.family == Family::ReflectionMeta ||
      key.family == Family::ObjectManipulator ||
      key.family == Family::BranchFSM;
  bool startup_like_mixed = key.family == Family::Mixed &&
      mixedShapeAllIn(key.mixed_shape,
                      {WorkDim::Dynamic,
                       WorkDim::Dispatch,
                       WorkDim::Object,
                       WorkDim::Control});
  bool high_cost_nonnumeric_import_candidate =
      getConfig().enable_startup_init_policy && context.startup_phase &&
      !key.is_static && key.family != Family::NumericLoop &&
      !key.computeDominantHint() &&
      (key.highRisk() || key.code_size_bucket > 0 ||
       key.family == Family::CallDispatcher ||
       key.family == Family::ReflectionMeta ||
       key.family == Family::BranchFSM || startup_like_mixed);
  bool startup_low_roi_nonnumeric_candidate =
      getConfig().enable_startup_init_policy && context.startup_phase &&
      !key.is_static &&
      (key.family == Family::Trivial ||
       (key.family == Family::ObjectManipulator && key.loop_score <= 1 &&
        !key.computeHint()));
  if (high_cost_nonnumeric_import_candidate ||
      startup_low_roi_nonnumeric_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        key.highRisk() ? BranchReason::RiskDefer : BranchReason::StartupInit};
  }

  bool steady_exception_high_cost_framework_candidate =
      !context.startup_phase && !key.is_static && !key.is_suspendable &&
      !key.computeHint() && key.code_size_bucket >= 2 &&
      (key.risk_reason & kRiskException) != 0 &&
      (startup_like_family || startup_like_mixed);
  if (steady_exception_high_cost_framework_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::RiskDefer};
  }

  bool expected_exception_loop_candidate = !context.startup_phase &&
      !key.is_static && !key.is_suspendable &&
      key.family == Family::BranchFSM && key.loop_score >= 2 &&
      key.code_size_bucket == 1 && isPureControlExceptionRisk(key);
  if (expected_exception_loop_candidate) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::RiskDefer};
  }

  // Generalized steady-state LowRoi deferral is removed: pyperformance A/B
  // showed the blanket verdicts (multidim object graphs, non-risky warmup
  // holds, Trivial/synthetic freezes) mostly suppressed profitable compiles.
  // What remains deferred below is evidence-backed or risk-gated: high-risk
  // shapes keep their prior verdicts, the call-only dispatch loop verdict
  // stays (logging_silent, negative ROI when compiled), and the asyncio
  // helper naming in computeThresholdForCode() is untouched.
  bool steady_multidim_nonnumeric_object_graph_candidate =
      !context.startup_phase && !key.is_static && !key.is_suspendable &&
      !key.computeHint() && key.activeDimCount() >= 3 &&
      (key.family == Family::ReflectionMeta ||
       key.family == Family::CallDispatcher ||
       (key.family == Family::ObjectManipulator &&
        key.hasActiveDim(WorkDim::Dynamic)) ||
       (key.family == Family::Mixed &&
        (key.hasActiveDim(WorkDim::Dispatch) ||
         key.hasActiveDim(WorkDim::Dynamic))));
  if (steady_multidim_nonnumeric_object_graph_candidate && key.highRisk()) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::LowRoi};
  }

  if (shouldDeferSteadyStateCallOnlyDispatchLoop(key, context)) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::LowRoi};
  }

  bool steady_nonnumeric_warmup_candidate = !key.is_static &&
      key.loop_score == 0 &&
      (key.is_suspendable || startup_like_family || startup_like_mixed);
  if (steady_nonnumeric_warmup_candidate && key.highRisk()) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::RiskDefer};
  }
  // Suspendable shapes keep their prior deferral: widening the compiled
  // generator surface is deliberately out of scope for this change and
  // needs its own correctness pass. The plain-generator allowance in
  // computeThresholdForCode() stays the only entry.
  if (steady_nonnumeric_warmup_candidate && key.is_suspendable) {
    if (key.code_size_bucket > 0) {
      return {
          saturatingMul(global, kStartupDeferThresholdFactor),
          BranchReason::LowRoi};
    }
    return {
        std::max(global, kSteadyNonnumericWarmupThreshold),
        BranchReason::LowRoi};
  }

  bool large_branch_warmup_candidate = !key.is_static && key.loop_score > 0 &&
      key.family == Family::BranchFSM && key.highRisk();
  if (large_branch_warmup_candidate) {
    return {
        std::max(global, kSteadyNonnumericWarmupThreshold),
        BranchReason::LowRoi};
  }

  bool low_roi_base = key.loop_score == 0 && !key.is_static &&
      !key.is_suspendable && !key.highRisk();
  bool trivial_low_roi_candidate =
      low_roi_base && key.family == Family::Trivial;

  // Everything above this point is the classifier's verdict. What follows is
  // policy: the shapes released by this series compile at the base threshold
  // only once the process has shown that speculative compilation can pay for
  // itself. Until then they are held (see kLowRoiWarmHoldThreshold), which is
  // what keeps short-lived interpreter invocations as cheap as they were
  // before the release. The hold applies in every phase, so a cold process
  // stays cheap during its imports as well.
  // Loop-bearing shapes are never held. The gate counts calls, not loop
  // iterations, and there is no on-stack replacement, so holding a function
  // whose work sits inside a loop strands that work in the interpreter for
  // as long as the hold lasts -- and a process-wide budget can outlast a
  // whole measurement window.
  bool released_low_roi_shape = key.loop_score == 0 &&
      (steady_multidim_nonnumeric_object_graph_candidate ||
       steady_nonnumeric_warmup_candidate || trivial_low_roi_candidate);
  // lowRoiReleaseActiveOrNote() has a side effect, so it is evaluated last:
  // the counter must measure forgone opportunity, i.e. only calls that the
  // release would actually have compiled.
  if (released_low_roi_shape && !lowRoiReleaseActiveOrNote()) {
    return {kLowRoiWarmHoldThreshold, BranchReason::None};
  }

  return {global, BranchReason::None};
}

ThresholdDecision computeThresholdForCode(
    BorrowedRef<PyCodeObject> code,
    const StructureKey& key,
    const GateContext& context,
    uint32_t global) {
  // Outside the startup phase, waive the exception risk for self-contained
  // EAFP cache idioms before computing the verdict. This is re-evaluated on
  // every gate check, so the same code keeps the risk deferral while
  // imports or setup are in flight.
  StructureKey effective = key;
  if (effective.is_eafp_benign && !context.startup_phase) {
    effective.risk_reason &= static_cast<uint8_t>(~kRiskException);
  }

  auto decision = computeThreshold(effective, context, global);
  if (isStdlibAsyncioEventLoopFrameworkHelper(code, effective, context)) {
    return {
        saturatingMul(global, kStartupDeferThresholdFactor),
        BranchReason::LowRoi};
  }
  if (decision.branch_reason != BranchReason::None &&
      shouldAllowSteadyStatePlainGenerator(code, effective, context)) {
    return {global, BranchReason::None};
  }
  return decision;
}

} // namespace jit
