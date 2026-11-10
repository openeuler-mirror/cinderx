// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

#include <memory>

namespace jit::hir {

// SPIKE(PHIUNBOX): turn the object Phis that carry an int local around a loop
// (a `for` loop variable, an index recomputed in the body) into CInt64 Phis,
// so the PrimitiveBox<CInt64> feeding the back edge is left with only
// frame-state uses and PrimitiveBoxRemat removes it.  A Phi whose local may
// still be unbound (the first entry of a loop) gets a 0/1 flag Phi that the
// deopt metadata carries next to the local (FrameState::local_bound).
class LoopPhiUnbox : public Pass {
 public:
  LoopPhiUnbox() : Pass("LoopPhiUnbox") {}

  void Run(Function& irfunc) override;

  static std::unique_ptr<LoopPhiUnbox> Factory() {
    return std::make_unique<LoopPhiUnbox>();
  }
};

} // namespace jit::hir
