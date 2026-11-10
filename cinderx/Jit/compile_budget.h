// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdexcept>
#include <string>

#if PY_VERSION_HEX < 0x030C0000
// Records the refusal reason the CPython 3.11 scheduling gate reports; defined
// in Jit/pyjit_311_gate.cpp.
extern "C" void Ci_JitShell311_SetExecuteRefusal(const char* reason);
#endif

namespace jit {

// Stable names of the CPython 3.11 refusal reasons for a function that exceeds
// the per-function size budgets (jit-max-hir-* and jit-max-lir-*).  They are
// registered in Interpreter/3.11/bytecode_support.toml.
inline constexpr const char* kRefuseHirSizeLimit =
    "REFUSE_SHAPE_HIR_SIZE_LIMIT";
inline constexpr const char* kRefuseLirSizeLimit =
    "REFUSE_SHAPE_LIR_SIZE_LIMIT";

// Thrown when a function is too large to be worth compiling.  It is an
// ordinary std::runtime_error to every caller that does not care why a compile
// was abandoned; callers that do care can catch it by type.
class CompileBudgetExceeded : public std::runtime_error {
 public:
  CompileBudgetExceeded(const char* reason, const std::string& what)
      : std::runtime_error(what), reason_(reason) {}

  // The CPython 3.11 refusal reason (one of the kRefuse*SizeLimit names).
  const char* reason() const noexcept {
    return reason_;
  }

 private:
  const char* reason_;
};

// Abandon the compile of a function that is over budget.  On CPython 3.11 the
// refusal reason is recorded first, so that the scheduling gate and
// _jit311_compile_diagnostic() can report it instead of a generic failure.
[[noreturn]] inline void throwCompileBudgetExceeded(
    const char* reason,
    const std::string& what) {
#if PY_VERSION_HEX < 0x030C0000
  Ci_JitShell311_SetExecuteRefusal(reason);
#endif
  throw CompileBudgetExceeded{reason, what};
}

} // namespace jit
