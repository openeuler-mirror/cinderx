// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <cstdint>

// CPython 3.11 direct-entry gate, read by the generated entry guard through
// one materialized address: the evaluator our frames expect in the
// interpreter's PEP 523 slot (compared live) and the usable byte (JIT state).
// Field offsets are hardcoded by codegen (see the static_assert).
struct Ci_JitDirectEntryGate311Layout {
  void* eval_frame;
  uint8_t usable;
};
static_assert(
    offsetof(Ci_JitDirectEntryGate311Layout, eval_frame) == 0 &&
        offsetof(Ci_JitDirectEntryGate311Layout, usable) == 8,
    "the entry guard hardcodes the gate layout");

extern "C" Ci_JitDirectEntryGate311Layout Ci_JitDirectEntryGate311;
