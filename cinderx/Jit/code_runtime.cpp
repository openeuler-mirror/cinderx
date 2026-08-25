// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_runtime.h"

#include "cinderx/Common/util.h"

namespace jit {

GenYieldPoint::GenYieldPoint(std::size_t deopt_idx, ptrdiff_t yield_from_offset)
    : deopt_idx_{deopt_idx}, yield_from_offset_{yield_from_offset} {}

void GenYieldPoint::setResumeTarget(uintptr_t resume_target) {
  resume_target_ = resume_target;
}

uintptr_t GenYieldPoint::resumeTarget() const {
  return resume_target_;
}

std::size_t GenYieldPoint::deoptIdx() const {
  return deopt_idx_;
}

bool GenYieldPoint::isYieldFrom() const {
  return yield_from_offset_ != kInvalidYieldFromOffset;
}

ptrdiff_t GenYieldPoint::yieldFromOffset() const {
  return yield_from_offset_;
}

bool CodeRuntime::isGen() const {
  return code()->co_flags & kCoFlagsAnyGenerator;
}

BorrowedRef<PyCodeObject> CodeRuntime::code() const {
  return code_;
}

BorrowedRef<PyDictObject> CodeRuntime::builtins() const {
  return builtins_;
}

BorrowedRef<PyDictObject> CodeRuntime::globals() const {
  return globals_;
}

CodeRuntime::CodeRuntime(BorrowedRef<PyFunctionObject> func)
    : CodeRuntime{
          BorrowedRef<PyCodeObject>{func->func_code},
          func->func_builtins,
          func->func_globals} {}

CodeRuntime::CodeRuntime(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals)
    : code_{code}, builtins_{builtins}, globals_{globals} {
  // Ensure code, globals, and builtins objects live as long as their compiled
  // functions.
  addReference(code);
  addReference(builtins);
  addReference(globals);
}

void CodeRuntime::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

void CodeRuntime::releaseReferences() {
  // We want to be careful here with the freeing of these references. Freeing
  // the objects could cause our CompiledFunction to be freed as well so first
  // we grab the references and then clear them.
  std::unordered_set<ThreadedRef<>> refs;
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  ThreadedRef<> tmp;
#endif
  {
    ThreadedCompileSerialize guard;
    refs = std::move(references_);
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    tmp = std::move(reifier_);
#endif
  }
  // and then we let the dtors clean everything up
}

GenYieldPoint* CodeRuntime::addGenYieldPoint(GenYieldPoint&& gen_yield_point) {
  gen_yield_points_.emplace_back(std::move(gen_yield_point));
  return &gen_yield_points_.back();
}

std::size_t CodeRuntime::addDeoptMetadata(DeoptMetadata&& deopt_meta) {
  if (!deopt_meta.frame_meta.empty()) {
    uint32_t seq = 0;
    for (const DeoptMetadata& existing : deopt_metadatas_) {
      if (existing.frame_meta.empty()) {
        continue;
      }
      if (existing.code() == deopt_meta.code() &&
          existing.innermostFrame().cause_instr_idx ==
              deopt_meta.innermostFrame().cause_instr_idx &&
          existing.reason == deopt_meta.reason &&
          existing.inline_depth() == deopt_meta.inline_depth()) {
        ++seq;
      }
    }
    deopt_meta.site_id = computeDeoptSiteId(
        deopt_meta.code(),
        deopt_meta.innermostFrame().cause_instr_idx.asOffset(),
        deopt_meta.reason,
        deopt_meta.inline_depth(),
        seq);
  }
  deopt_metadatas_.emplace_back(std::move(deopt_meta));
  return deopt_metadatas_.size() - 1;
}

DeoptMetadata& CodeRuntime::getDeoptMetadata(std::size_t id) {
  return deopt_metadatas_[id];
}

const DeoptMetadata& CodeRuntime::getDeoptMetadata(std::size_t id) const {
  return deopt_metadatas_[id];
}

const std::vector<DeoptMetadata>& CodeRuntime::deoptMetadatas() const {
  return deopt_metadatas_;
}

bool CodeRuntime::armForcedDeopt(uint64_t site_id, int n, bool at_or_after) {
  if (n < 1) {
    return false;
  }
  bool armed = false;
  for (DeoptMetadata& meta : deopt_metadatas_) {
    if (meta.frame_meta.empty() || meta.site_id != site_id) {
      continue;
    }
    if (!meta.forceable) {
      continue;
    }
    meta.force_mode = at_or_after ? 2 : 1;
    meta.force_countdown = n;
    meta.consumed_forced = false;
    armed = true;
  }
  if (armed) {
    forced_deopt_armed_ = 1;
  }
  return armed;
}

bool CodeRuntime::consumeForcedDeopt(std::size_t deopt_id) {
  if (deopt_id >= deopt_metadatas_.size()) {
    return false;
  }
  DeoptMetadata& meta = deopt_metadatas_[deopt_id];
  if (meta.force_mode == 0) {
    return false;
  }
  if (meta.force_countdown > 0) {
    --meta.force_countdown;
  }
  if (meta.force_countdown > 0) {
    return false;
  }
  if (meta.force_mode == 1) {
    meta.force_mode = 0;
    forced_deopt_armed_ = 0;
    for (const DeoptMetadata& other : deopt_metadatas_) {
      if (other.force_mode != 0) {
        forced_deopt_armed_ = 1;
        break;
      }
    }
  }
  meta.consumed_forced = true;
  return true;
}

std::size_t CodeRuntime::addOSRMetadata(OSRMetadata&& osr_meta) {
  ThreadedCompileSerialize guard;
  osr_metadatas_.emplace_back(std::move(osr_meta));
  return osr_metadatas_.size() - 1;
}

OSRMetadata& CodeRuntime::getOSRMetadata(std::size_t id) {
  return osr_metadatas_[id];
}

const OSRMetadata& CodeRuntime::getOSRMetadata(std::size_t id) const {
  return osr_metadatas_[id];
}

std::vector<OSRMetadata>& CodeRuntime::osrMetadatas() {
  return osr_metadatas_;
}

const std::vector<OSRMetadata>& CodeRuntime::osrMetadatas() const {
  return osr_metadatas_;
}

bool CodeRuntime::hasOSREntries() const {
  return !osr_metadatas_.empty();
}

int CodeRuntime::frameSize() const {
  return frame_size_;
}

void CodeRuntime::setFrameSize(int size) {
  frame_size_ = size;
}

uint32_t CodeRuntime::spillWords() const {
  return spill_words_;
}

void CodeRuntime::setSpillWords(uint32_t words) {
  spill_words_ = words;
}

DebugInfo* CodeRuntime::debugInfo() {
  return &debug_info_;
}

bool CodeRuntime::isCleared() const {
  // We always add some references when we first create the CodeRuntime, so we
  // know if no references are left we've been cleared.
  return references_.empty();
}

int CodeRuntime::traverse(visitproc visit, void* arg) {
  // Only traverse objects that this CodeRuntime owns strong references to.
  // The references_ set contains ThreadedRef which hold strong references.
  // code_, builtins_, globals_ are BorrowedRef pointing to the same objects
  // already in references_ - don't double-count.
  for (const auto& ref : references_) {
    Py_VISIT(ref.get());
  }

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  if (reifier_ != nullptr) {
    Py_VISIT(reifier_.get());
  }
#endif

  return 0;
}

std::optional<UnitCallStack> CodeRuntime::getUnitCallStackFromDeoptIdx(
    std::size_t deopt_idx) const {
  if (deopt_idx >= deopt_metadatas_.size()) {
    return std::nullopt;
  }
  const DeoptMetadata& meta = deopt_metadatas_[deopt_idx];
  UnitCallStack stack;
  stack.reserve(meta.frame_meta.size());
  for (const auto& frame : meta.frame_meta) {
    stack.emplace_back(frame.code, frame.cause_instr_idx);
  }
  return stack;
}

} // namespace jit
