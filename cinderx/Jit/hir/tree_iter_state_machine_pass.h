// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/type.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>

namespace jit::hir {

// TreeIterPhase encodes the current state of the state machine for each
// resume.
enum class TreeIterPhase : int32_t {
  kLeft = 0,
  kYield = 1,
  kRight = 2,
  kBacktrack = 3,
  kExit = 4,
};

// ---------------------------------------------------------------------------
// Field-access proof types (GJIT-SM-009)
//
// These structures capture how a tree-node field is accessed (slot/member vs.
// dict-backed split-dict) and what action to take if the proof is invalidated
// at runtime.  Production guard/layout tracking fields (GuardSource,
// DependencySource, FallbackShape) are stub types in the experimental first
// version; they will be populated when the production gate is opened.
// ---------------------------------------------------------------------------

struct GuardSource {};       // production: exactness/type-version deps
struct DependencySource {};  // production: layout invalidation hook
struct FallbackShape {};     // production: LoadAttr fallback semantics

enum class FieldAccessKind {
  kSlotOrMember,  // direct offset load from __slots__ or C member
  kSplitDict,     // dict-backed heap object: fast inline-values path + fallback
};

enum class ChildGuardKind {
  kNoneGuard,               // explicit `is not None` / PrimitiveCompare/GuardIs
  kDefaultTruthinessGuard,  // `if child:` with proven default truthiness
};

enum class MatchRejectReason {
  kUnsupportedShape,
  kMissingGuard,
  kMissingFieldProof,
  kMissingIteratorIdentity,
  kMissingRuntimeFailureAction,
};

enum class RuntimeFailureAction {
  kExperimentalFailClosed,  // experimental path: clear state + bail
  kInvalidate,              // production: invalidate compiled code
  kExactDeoptReify,         // production: deopt + reify original generator
};

// Describes how to read one named field from a tree node and what to do if
// the proof is invalidated at runtime.
struct FieldAccessProof {
  FieldAccessKind kind{FieldAccessKind::kSlotOrMember};
  PyTypeObject* owner_type{nullptr};  // exact node type at match time
  std::string field_name;
  int name_idx{-1};
  intptr_t value_offset{0};
  std::optional<intptr_t> valid_offset;  // split-dict inline-values valid slot
  GuardSource guard_source;
  DependencySource layout_dependency;
  FallbackShape fallback_shape;
  RuntimeFailureAction runtime_failure_action{
      RuntimeFailureAction::kExperimentalFailClosed};
};

// Describes the child-skip guard that was found for one yield-from arm.
struct ChildGuardProof {
  ChildGuardKind kind{ChildGuardKind::kNoneGuard};
  Instr* guard_instr{nullptr};          // the guard instruction in the HIR
  bool requires_default_truthiness{false};
};

// Holds the results of TreeIter pattern matching.  All pointers are borrowed
// from the Function being matched — they are invalidated once the CFG is
// rewritten.
struct TreeIterMatch {
  Register* self_reg{nullptr};
  Register* original_self_reg{nullptr};
  Instr* initial_yield{nullptr};
  FrameState* yield_frame_state{nullptr};
  Type exact_node_type{TTop};

  FieldAccessProof left_field;
  FieldAccessProof right_field;
  FieldAccessProof value_field;

  ChildGuardProof left_guard;
  ChildGuardProof right_guard;
  const YieldValue* left_yield_from{nullptr};
  const YieldValue* value_yield{nullptr};
  const YieldValue* right_yield_from{nullptr};

  // Production capability flags — all false in the experimental first version.
  bool production_admission_manifest_verified{false};
  bool can_exact_reify_yield_from_protocol{false};
  bool can_deopt_state_machine{false};
  bool can_enforce_active_path{false};
  bool can_enforce_depth_budget{false};
};

// TreeIterStateMachinePass converts tree-traversal generators that match the
// canonical in-order pattern:
//
//   def __iter__(self):
//       if self.left is not None:
//           yield from self.left
//       yield self.value
//       if self.right is not None:
//           yield from self.right
//
// into an explicit state machine stored in a heap-backed TreeIterState object
// attached to GenDataFooter.  This eliminates the recursive generator frame
// overhead for each left/right child.
//
// The pass is gated behind the `tree_iter_state_machine` config option
// (env var PYTHONJITTREEITERSTATEMACHINE). It defaults to enabled and can be
// explicitly disabled with PYTHONJITTREEITERSTATEMACHINE=0.
class TreeIterStateMachinePass : public Pass {
 public:
  TreeIterStateMachinePass() : Pass("TreeIterStateMachinePass") {}
  void Run(Function& func) override;

 private:
  // Try to match the canonical TreeIter in-order pattern in func.
  // Returns a populated TreeIterMatch on success, std::nullopt if the
  // function does not match.
  std::optional<TreeIterMatch> matchTreeIter(const Function& func) const;

  // Trace iter_reg back through Phi/Assign/GetIter chains to find the
  // LoadField instruction that produces the iterable.  Returns nullptr if
  // the chain does not end at a LoadField.
  const LoadField* traceYieldFromIterable(const Register* iter_reg) const;

  // Same, but shares an on-path set of Phi registers across recursive
  // calls so a Phi cycle (HIR loop back-edge between mutually-referencing
  // Phis) terminates with a conservative nullptr instead of recursing
  // forever.
  const LoadField* traceYieldFromIterable(
      const Register* iter_reg,
      std::unordered_set<const Register*>& on_path) const;

  // Rewrite func's CFG to implement the in-order state machine described by
  // match.  The original recursive yield-from body becomes unreachable and is
  // removed by CleanCFG before this method returns.
  void buildTreeIterStateMachine(
      Function& func,
      const TreeIterMatch& match);
};

} // namespace jit::hir
