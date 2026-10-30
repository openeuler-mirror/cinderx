// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/hir/cfg.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/osr.h"
#include "cinderx/Jit/type_deopt_patchers.h"
#include "cinderx/StaticPython/typed-args-info.h"

namespace jit::hir {

// True when the bytecode offset lies inside a range covered by the code
// object's exception table (a handler or cleanup would run for an exception
// raised there).
bool isInExceptionProtectedRange(
    BorrowedRef<PyCodeObject> code,
    BCOffset target_offset);

class Function {
 public:
  using InlineFailureStats =
      UnorderedMap<InlineFailureType, UnorderedSet<std::string>>;
  Function();
  ~Function();

  ThreadedRef<PyCodeObject> code;
  ThreadedRef<PyDictObject> builtins;
  ThreadedRef<PyDictObject> globals;

  // for primitive args only, null if there are none
  ThreadedRef<_PyTypedArgsInfo> prim_args_info;

  // Fully-qualified name of the function
  std::string fullname;

  // Does this function have primitive args?
  bool has_primitive_args{false};

  struct InlineFunctionStats {
    int num_inlined_functions{0};
    // map of {inline_failure_type -> function_names}
    InlineFailureStats failure_stats;
  } inline_function_stats;

  // vector of {locals_idx, type, optional}
  // in argument order, may have gaps for unchecked args
  std::vector<TypedArgument> typed_args;

  // Return type
  Type return_type{TObject};

  FrameMode frameMode{FrameMode::kNormal};
  // CPython 3.11: the builder writes every local-variable store through to
  // the materialized frame (MR-08 frame observability), so the frame's
  // localsplus hold owned references while the body runs and the exit must
  // release them (the inline unlink assumes empty slots).
  bool frame_locals_written_through_311{false};

#if PY_VERSION_HEX < 0x030C0000
  // Builder analysis for the root code, indexed by code unit. Reused by
  // Simplify so each float divide does not rescan the exception table and
  // bytecode. Empty when no handler is eligible; inlined bodies do not use it.
  std::vector<bool> exc_edge_capable_311;

  bool excEdgeCapable311(BCOffset offset) const {
    int index = offset.asIndex().value();
    return index >= 0 &&
        static_cast<size_t>(index) < exc_edge_capable_311.size() &&
        exc_edge_capable_311[index];
  }
#endif

  CFG cfg;

  Environment env;

  // All the code patchers pointing to patch points in this function.
  //
  // These will be moved over to the CompiledFunction after compilation is
  // complete.
  std::vector<std::unique_ptr<CodePatcher>> code_patchers;

  // Optional property used to track time taken for individual compilation
  // phases
  std::unique_ptr<CompilationPhaseTimer> compilation_phase_timer;

  // Return the total number of arguments (positional + kwonly + varargs +
  // varkeywords)
  int numArgs() const;

  // Return the number of locals + cellvars + freevars
  Py_ssize_t numVars() const;

  // Set code and a number of other members that are derived from it.
  void setCode(BorrowedRef<PyCodeObject> code);

  // Total number of HIR basic blocks in the function.
  std::size_t numBlocks() const;

  // Total number of HIR instructions in the function.
  std::size_t numInstrs() const;

  // Count the number of instructions that match the predicate
  std::size_t CountInstrs(InstrPredicate pred) const;

  // Does this function return a primitive type?
  bool returnsPrimitive() const;

  // Does this function return a primitive double?
  bool returnsPrimitiveDouble() const;

  void setCompilationPhaseTimer(std::unique_ptr<CompilationPhaseTimer> cpt);

  bool canDeopt() const;

  void markOSREntries(
      const std::vector<BCOffset>& offsets,
      BorrowedRef<PyCodeObject> code);
  bool hasOSREntries() const;
  void extractOSRLiveIns();
  const OSRMetadata* osrMetadataFor(const OSREntry& entry) const;

  template <typename T, typename... Args>
  T* allocateCodePatcher(Args&&... args) {
    code_patchers.emplace_back(
        std::make_unique<T>(std::forward<Args>(args)...));
    return static_cast<T*>(code_patchers.back().get());
  }

  // Get the code object for the given instruction.  Handles inlined functions
  // but assumes that inlined functions have a dominating FrameState from
  // BeginInlinedFunction to use.  If we start optimizing that out for inlined
  // functions that cannot deopt, we will have to do something different.
  //
  // The instruction must be part of this function.
  BorrowedRef<PyCodeObject> codeFor(const Instr& instr) const;

  ThreadedRef<> reifier;

 private:
  bool isEligibleOSREntry(
      const FrameState& fs,
      BorrowedRef<PyCodeObject> code) const;
  void refreshOSREntries();

  UnorderedMap<BCOffset, OSREntry*> osr_entries_;
  UnorderedMap<const OSREntry*, OSRMetadata> osr_metadata_;

  DISALLOW_COPY_AND_ASSIGN(Function);
};

using OpcodeCounts = std::array<int, kNumOpcodes>;
OpcodeCounts count_opcodes(const Function& func);

} // namespace jit::hir
