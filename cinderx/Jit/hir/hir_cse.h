// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <memory>

namespace jit::hir {

// SPIKE(HIRCSE): dominance-based common-subexpression elimination for pure
// primitive instructions, plus removal of Guards whose condition an earlier
// dominating Guard already established.
//
// The 3.11 compact-long and float specialisations are emitted per arithmetic
// site by Simplify, so a function such as spectral_norm's eval_A re-derives
// IsCompactLong / CompactLongUnbox of the same immutable exact int three
// times (an unbox is a dozen machine instructions) and re-guards the same
// compactness condition.  The existing PrimitiveUnboxCSE only merges
// PrimitiveUnbox within one basic block.  This pass walks the dominator tree
// with a scoped table of available expressions keyed on (opcode, immediate
// parameters, assign-chased operands) and rewrites a dominated duplicate into
// an Assign of the earlier value; a Guard on a condition already guarded by a
// dominating Guard is deleted.  Only instructions without memory effects on
// immutable operands are eligible, so no invalidation is needed.
class HirCSE : public Pass {
 public:
  HirCSE() : Pass("HirCSE") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<HirCSE> Factory() {
    return std::make_unique<HirCSE>();
  }
};

} // namespace jit::hir
