// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/threaded_compile.h"

#include <deque>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace jit {

constexpr ptrdiff_t kInvalidYieldFromOffset =
    std::numeric_limits<ptrdiff_t>::max();

// Information about how a specific yield instruction should resume.
class GenYieldPoint {
 public:
  static constexpr int resumeTargetOffset() {
    return offsetof(GenYieldPoint, resume_target_);
  }

  GenYieldPoint(std::size_t deopt_idx, ptrdiff_t yield_from_offset);

  // Get and set what address the yield should resume from.
  uintptr_t resumeTarget() const;
  void setResumeTarget(uintptr_t resume_target);

  std::size_t deoptIdx() const;
  bool isYieldFrom() const;
  ptrdiff_t yieldFromOffset() const;

 private:
  uintptr_t resume_target_{0};
  const std::size_t deopt_idx_;
  const ptrdiff_t yield_from_offset_;
};

// Runtime data for a PyCodeObject object, containing caches and any other data
// associated with a JIT-compiled function.
class alignas(16) CodeRuntime {
 public:
  explicit CodeRuntime(BorrowedRef<PyFunctionObject> func);
  CodeRuntime(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals);

  // Ensure that this CodeRuntime owns a reference to the given borrowed
  // object, keeping it alive for use by the compiled code. Make CodeRuntime a
  // new owner of the object.
  void addReference(BorrowedRef<> obj);

  // Release any references this CodeRuntime holds to Python objects.
  void releaseReferences();

  // Store meta-data about generator yield point.
  GenYieldPoint* addGenYieldPoint(GenYieldPoint&& gen_yield_point);

  // Add metadata used during a deopt.  Return an ID that can be used to fetch
  // the metadata from generated code.
  std::size_t addDeoptMetadata(DeoptMetadata&& deopt_meta);

  // Get a reference to the DeoptMetadata with the given ID.
  DeoptMetadata& getDeoptMetadata(std::size_t id);
  const DeoptMetadata& getDeoptMetadata(std::size_t id) const;

  // Get all deopt metadatas for the given CodeRuntime.
  const std::vector<DeoptMetadata>& deoptMetadatas() const;

  // Arm the RFC site-deopt trigger on every metadata row with `site_id`.
  // `n` is the 1-based visit count; `at_or_after` keeps forcing after N.
  bool armForcedDeopt(uint64_t site_id, int n, bool at_or_after);

  // Consume one visit to a forced-deopt site.  Generated code first reads
  // forcedDeoptArmedAddress() and only calls this slow path while some site is
  // armed.
  bool consumeForcedDeopt(std::size_t deopt_id);

  uint64_t* forcedDeoptArmedAddress() {
    return &forced_deopt_armed_;
  }

  // Add OSR metadata for a loop-header secondary entry point.
  // Returns the index of the newly added metadata.
  std::size_t addOSRMetadata(OSRMetadata&& osr_meta);

  OSRMetadata& getOSRMetadata(std::size_t id);
  const OSRMetadata& getOSRMetadata(std::size_t id) const;

  // Get all OSR metadatas.
  std::vector<OSRMetadata>& osrMetadatas();
  const std::vector<OSRMetadata>& osrMetadatas() const;

  // Check if this CodeRuntime has any OSR entry stubs.
  bool hasOSREntries() const;

  // Check if this is a generator/coroutine/async generator.
  bool isGen() const;

  BorrowedRef<PyCodeObject> code() const;
  BorrowedRef<PyDictObject> builtins() const;
  BorrowedRef<PyDictObject> globals() const;

  // Get and set the total size of a stack frame for this compiled code object.
  int frameSize() const;
  void setFrameSize(int size);

  // Get and set the number of spill words for generators.
  uint32_t spillWords() const;
  void setSpillWords(uint32_t words);

  DebugInfo* debugInfo();

  // Allocate a jump table for static type check dispatch.
  // Returns a pointer to the table data (valid for the lifetime of this
  // CodeRuntime).
  void** allocateTypeCheckJumpTable(size_t num_entries) {
    type_check_jump_table_ = std::make_unique<void*[]>(num_entries);
    return type_check_jump_table_.get();
  }

  // Traverse all GC-reachable objects held by this CodeRuntime.
  int traverse(visitproc visit, void* arg);

  // True if the references have been cleared
  bool isCleared() const;

  // Get the UnitCallStack from a deopt metadata index.
  std::optional<UnitCallStack> getUnitCallStackFromDeoptIdx(
      std::size_t deopt_idx) const;

  std::optional<uintptr_t> getCallsiteDeoptExit(uintptr_t return_addr) const {
    auto it = callsite_deopt_exits_.find(return_addr);
    if (it != callsite_deopt_exits_.end()) {
      return it->second;
    }
    return std::nullopt;
  }
  void addCallsiteDeoptExit(uintptr_t return_addr, uintptr_t deopt_exit_addr) {
    callsite_deopt_exits_[return_addr] = deopt_exit_addr;
  }

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  void setReifier(BorrowedRef<> reifier) {
    ThreadedCompileSerialize guard;
    reifier_ = ThreadedRef<>::create(reifier);
  }
  BorrowedRef<> reifier() {
    return reifier_;
  }
#else
  BorrowedRef<> reifier() {
    return nullptr;
  }
#endif
 private:
  BorrowedRef<PyCodeObject> code_;
  BorrowedRef<PyDictObject> builtins_;
  BorrowedRef<PyDictObject> globals_;

  // References owned by this CodeRuntime.
  std::unordered_set<ThreadedRef<PyObject>> references_;

  // Metadata about yield points. Deque so we can have raw pointers to content.
  std::deque<GenYieldPoint> gen_yield_points_;

  // Metadata about deopt points.  Safe to use a vector as these are always
  // accessed by index.
  std::vector<DeoptMetadata> deopt_metadatas_;

  // Process-local test instrumentation flag.  It is read directly by
  // generated 3.11 code under the GIL; zero keeps the normal guard path out
  // of the C helper entirely.
  uint64_t forced_deopt_armed_{0};

  // OSR entry metadata (one per loop-header secondary entry point).
  std::vector<OSRMetadata> osr_metadatas_;

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  ThreadedRef<> reifier_;
#endif

  // Jump table for static type check dispatch (indexed by defaulted_arg_count).
  // Entries are resolved to code addresses after code generation.
  std::unique_ptr<void*[]> type_check_jump_table_;

  // Map from call return addresses to post-call guard deopt exits.
  // Built during codegen, used by deoptAllJitFramesOnStack().
  std::unordered_map<uintptr_t, uintptr_t> callsite_deopt_exits_;

  int frame_size_{-1};
  uint32_t spill_words_{0};
  DebugInfo debug_info_;
};

} // namespace jit
