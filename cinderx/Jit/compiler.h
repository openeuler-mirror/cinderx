// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/preload.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace jit {

using PostPassFunction = std::function<
    void(hir::Function& func, std::string_view pass_name, std::size_t time_ns)>;

struct ShadowCompileResult {
  size_t code_size{0};
  hir::OpcodeCounts hir_opcode_counts{};
  uint64_t specialized_opcodes{0};
};

// Controls what compiler passes are run.
enum PassConfig : uint64_t {
  // Only run compiler passes that are necessary for correctness, e.g. SSAify.
  kMinimal = 0,

  // Bits to toggle individual optimization passes.

  kBeginInlinedFunctionElim = 1 << 0,
  kBuiltinLoadMethodElim = 1 << 1,
  kCleanCFG = 1 << 2,
  kDeadCodeElim = 1 << 3,
  kDynamicComparisonElim = 1 << 4,
  kGuardTypeRemoval = 1 << 5,
  kInliner = 1 << 6,
  kPhiElim = 1 << 7,
  kSimplify = 1 << 8,
  kInsertUpdatePrevInstr = 1 << 9,
  kFloatAccumulatorPromotion = 1 << 10,
  kPrimitiveBoxRemat = 1 << 11,
  kPrimitiveUnboxCSE = 1 << 12,
  kFloatComparisonSimplification = 1 << 13,
  kTreeIterStateMachine = 1 << 14,

  // Run all the passes.
  kAll = ~uint64_t{0},

  // Run all the passes except for inlining.
  kAllExceptInliner = kAll & ~kInliner,
};

// The high-level interface for translating Python functions into native code.
class Compiler {
 public:
  Compiler() = default;

  // Compile the function / code object preloaded by the given Preloader.
  // Returns the compiled function data, or nullptr on failure.
  std::optional<CompiledFunctionData> Compile(const hir::Preloader& preloader);

  // Convenience wrapper to create and compile a preloader from a
  // PyFunctionObject.
  std::optional<CompiledFunctionData> Compile(
      BorrowedRef<PyFunctionObject> func);

  // Validate the entire compilation pipeline without allocating executable
  // memory, creating a CompiledFunction, or installing an entry point.
  std::optional<ShadowCompileResult> CompileShadow(
      const hir::Preloader& preloader);
  std::optional<ShadowCompileResult> CompileShadow(
      BorrowedRef<PyFunctionObject> func);

  // Runs all the compiler passes on the HIR function.
  static void runPasses(hir::Function&, PassConfig config);

  // Runs the compiler passes, calling callback on the HIR function after each
  // pass.
  static void runPasses(
      hir::Function& irfunc,
      PassConfig config,
      PostPassFunction callback);

 private:
  DISALLOW_COPY_AND_ASSIGN(Compiler);
  codegen::NativeGeneratorFactory ngen_factory_;
};

#if PY_VERSION_HEX < 0x030C0000
// Test-only: run the executing mode's exact HIR pipeline -- preload, build,
// instrumentation polls, passes -- and return the final HIR, so RuntimeTests
// can assert structural invariants (e.g. every Decref precedes the next
// boundary poll) on what codegen would actually consume.
std::unique_ptr<hir::Function> compileToFinalHIRForTest(
    BorrowedRef<PyFunctionObject> func);
#endif

} // namespace jit
