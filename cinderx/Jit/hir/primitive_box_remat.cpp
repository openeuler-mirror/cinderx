// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/primitive_box_remat.h"

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

namespace {

struct RematCandidate {
  PrimitiveBox* box;
  Register* boxed;
  Register* unboxed;
  std::vector<Instr*> removable_uses;
};

bool collectRemovableUses(
    const RegUses& direct_uses,
    Register* boxed,
    std::vector<Instr*>& removable_uses) {
  auto use_it = direct_uses.find(boxed);
  if (use_it == direct_uses.end()) {
    return true;
  }

  for (Instr* use : use_it->second) {
    if (!use->IsUseType()) {
      return false;
    }
    removable_uses.push_back(use);
  }
  return true;
}

std::unordered_set<Register*> replaceInAllFrameStates(
    Function& func,
    const std::unordered_map<Register*, Register*>& replacements) {
  std::unordered_set<Register*> replaced;
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      FrameState* fs = get_frame_state(instr);
      if (fs == nullptr) {
        continue;
      }
      fs->visitUses([&](Register*& reg) {
        auto it = replacements.find(reg);
        if (it != replacements.end()) {
          replaced.insert(reg);
          reg = it->second;
        }
        return true;
      });
    }
  }
  return replaced;
}

// A box used only as the RHS of an adjacent float-inplace slow path need
// not execute on the float fast path. Keep named/aliased objects and values
// observable after the slow call out of this transformation.
bool sinkInPlaceTemporaryBoxes(Function& func) {
#if PY_VERSION_HEX < 0x030C0000
  struct StackUse {
    Instr* instr;
    Register** slot;
  };
  std::vector<PrimitiveBox*> boxes;
  std::unordered_set<Register*> named;
  std::unordered_map<Register*, std::vector<StackUse>> stack_uses;
  for (auto& block : func.cfg.blocks) {
    for (auto& instr : block) {
      if (instr.IsPrimitiveBox()) {
        auto& box = static_cast<PrimitiveBox&>(instr);
        if (box.type() == TCDouble) {
          boxes.push_back(&box);
        }
      }
      for (auto* fs = get_frame_state(instr); fs != nullptr; fs = fs->parent) {
        for (Register* reg : fs->localsplus) {
          if (reg != nullptr) {
            named.insert(modelReg(reg));
          }
        }
        for (Register*& reg : fs->stack) {
          stack_uses[reg].push_back(StackUse{&instr, &reg});
        }
      }
    }
  }

  auto direct_uses = collectDirectRegUses(func);
  std::vector<std::unique_ptr<Instr>> removed;
  bool changed = false;
  for (PrimitiveBox* box : boxes) {
    Register* boxed = box->output();
    if (named.contains(boxed)) {
      continue;
    }
    auto uses = direct_uses.find(boxed);
    if (uses == direct_uses.end()) {
      continue; // Existing rematerialization handles frame-state-only boxes.
    }
    InPlaceOp* consumer = nullptr;
    std::vector<Instr*> type_uses;
    bool valid = true;
    for (Instr* use : uses->second) {
      if (use->IsUseType()) {
        type_uses.push_back(use);
        continue;
      }
      if (!use->IsInPlaceOp() || consumer != nullptr) {
        valid = false;
        break;
      }
      consumer = static_cast<InPlaceOp*>(use);
      if (consumer->hasFloatFastPath() || consumer->right() != boxed ||
          consumer->left() == boxed ||
          (consumer->op() != InPlaceOpKind::kAdd &&
           consumer->op() != InPlaceOpKind::kSubtract)) {
        valid = false;
        break;
      }
    }
    if (!valid || consumer == nullptr) {
      continue;
    }

    BasicBlock* source = box->block();
    BasicBlock* slow = consumer->block();
    Instr* terminator = source->GetTerminator();
    if (!terminator->IsCondBranchCheckType() || source == slow ||
        slow->in_edges().size() != 1 || &slow->front() != consumer) {
      continue;
    }
    auto* branch = static_cast<CondBranchCheckType*>(terminator);
    if (branch->type() != TFloatExact || branch->false_bb() != slow ||
        branch->GetOperand(0) != consumer->left()) {
      continue;
    }

    // No calls, stores, allocation, or other potentially observable work may
    // move across the box. Instrumentation exits reify the temporary from its
    // CDouble using the existing AArch64 PrimitiveBoxRemat/deopt contract.
    std::unordered_set<Instr*> before_branch;
    auto it = source->iterator_to(*box);
    for (++it; it != source->end(); ++it) {
      Instr& instr = *it;
      before_branch.insert(&instr);
      if (&instr != terminator && !instr.IsSnapshot() &&
          !instr.IsCheckInstrumentation() && !instr.IsUpdatePrevInstr() &&
          !instr.IsUseType()) {
        valid = false;
        break;
      }
    }
    for (const StackUse& use : stack_uses[boxed]) {
      if (!before_branch.contains(use.instr)) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      continue;
    }

    for (const StackUse& use : stack_uses[boxed]) {
      *use.slot = box->value();
    }
    for (Instr* use : type_uses) {
      use->unlink();
      removed.emplace_back(use);
    }
    // Move the original allocation, retaining its bytecode offset and
    // exception FrameState. Do not clone an object that may escape on slow.
    box->unlink();
    box->InsertBefore(*consumer);
    changed = true;
  }
  return changed;
#else
  return false;
#endif
}

} // namespace

void PrimitiveBoxRemat::Run(Function& irfunc) {
  bool changed = sinkInPlaceTemporaryBoxes(irfunc);
  auto direct_uses = collectDirectRegUses(irfunc);
  std::vector<RematCandidate> candidates;
  std::unordered_map<Register*, Register*> replacements;

  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      if (!instr.IsPrimitiveBox()) {
        continue;
      }

      auto& box = static_cast<PrimitiveBox&>(instr);
      if (box.type() != TCDouble) {
        continue;
      }

      Register* boxed = box.output();
      Register* unboxed = box.value();
      if (unboxed == nullptr || !(unboxed->type() <= TCDouble)) {
        continue;
      }

      std::vector<Instr*> removable_uses;
      if (!collectRemovableUses(direct_uses, boxed, removable_uses)) {
        continue;
      }

      candidates.push_back(
          RematCandidate{&box, boxed, unboxed, std::move(removable_uses)});
      replacements.emplace(boxed, unboxed);
    }
  }

  if (candidates.empty()) {
    if (changed) {
      CopyPropagation{}.Run(irfunc);
      reflowTypes(irfunc);
    }
    return;
  }

  auto replaced = replaceInAllFrameStates(irfunc, replacements);
  std::vector<std::unique_ptr<Instr>> removed_instrs;

  for (auto& candidate : candidates) {
    if (!replaced.contains(candidate.boxed)) {
      continue;
    }

    for (Instr* use : candidate.removable_uses) {
      use->unlink();
      removed_instrs.emplace_back(use);
    }

    candidate.box->unlink();
    removed_instrs.emplace_back(candidate.box);
    changed = true;
  }

  if (changed) {
    CopyPropagation{}.Run(irfunc);
    reflowTypes(irfunc);
  }
}

} // namespace jit::hir
