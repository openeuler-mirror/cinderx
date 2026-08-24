// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiler.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/builtin_load_method_elimination.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/dead_code_elimination.h"
#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"
#include "cinderx/Jit/hir/float_accumulator_promotion.h"
#include "cinderx/Jit/hir/float_comparison_simplification.h"
#include "cinderx/Jit/hir/guard_removal.h"
#include "cinderx/Jit/hir/hir_stats.h"
#include "cinderx/Jit/hir/inliner.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/primitive_box_remat.h"
#include "cinderx/Jit/hir/primitive_unbox_cse.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/hir/tree_iter_state_machine_pass.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/osr.h"

#include <chrono>
#include <iostream>
#include <sstream>

#if PY_VERSION_HEX < 0x030C0000
// Records a refusal reason the scheduling gate reports; defined in
// Jit/pyjit_311_gate.cpp.
extern "C" void Ci_JitShell311_SetExecuteRefusal(const char* reason);
namespace {
void setLast311ExecuteRefusal(const char* reason) {
  Ci_JitShell311_SetExecuteRefusal(reason);
}
} // namespace
#endif

namespace jit {

template <typename T>
static void runPass(T&& pass, hir::Function& func, PostPassFunction callback) {
  COMPILE_TIMER(func.compilation_phase_timer,
                pass.name(),
                JIT_LOGIF(
                    getConfig().log.dump_hir_passes,
                    "HIR for {} before pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                Timer timer;
                pass.Run(func);
                std::size_t time_ns = timer.finish().count();
                callback(func, pass.name(), time_ns);

                JIT_LOGIF(
                    getConfig().log.dump_hir_passes,
                    "HIR for {} after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                JIT_DCHECK(
                    checkFunc(func, std::cerr),
                    "Function {} failed verification after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                JIT_DCHECK(
                    funcTypeChecks(func, std::cerr),
                    "Function {} failed type checking after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);)
}

void Compiler::runPasses(jit::hir::Function& irfunc, PassConfig config) {
  PostPassFunction callback =
      [](hir::Function&, std::string_view, std::size_t) {};
  runPasses(irfunc, config, callback);
}

void Compiler::runPasses(
    jit::hir::Function& irfunc,
    PassConfig config,
    PostPassFunction callback) {
  // SSAify must come first; nothing but SSAify should ever see non-SSA HIR.
  runPass(jit::hir::SSAify{}, irfunc, callback);

  // Written this way because it's hard to forward the P type variable down to
  // runPass if it's not tied to one of the lambda's arguments.
  auto runPassIf = [&]<typename P>(P&& pass, PassConfig bit) {
    if (config & bit) {
      runPass(pass, irfunc, callback);
    }
  };
  auto runSimplifyAndFloatAccumulatorPromotion = [&]() {
    runPassIf(hir::Simplify{}, PassConfig::kSimplify);
    runPassIf(
        hir::FloatAccumulatorPromotion{},
        PassConfig::kFloatAccumulatorPromotion);
  };

  runSimplifyAndFloatAccumulatorPromotion();
  runPassIf(
      hir::DynamicComparisonElimination{}, PassConfig::kDynamicComparisonElim);
  runPassIf(hir::GuardTypeRemoval{}, PassConfig::kGuardTypeRemoval);
  runPassIf(hir::PhiElimination{}, PassConfig::kPhiElim);

  if (config & PassConfig::kInliner) {
    runPass(jit::hir::InlineFunctionCalls{}, irfunc, callback);

    runSimplifyAndFloatAccumulatorPromotion();
    runPassIf(
        hir::BeginInlinedFunctionElimination{},
        PassConfig::kBeginInlinedFunctionElim);
  }

  runPassIf(
      hir::BuiltinLoadMethodElimination{}, PassConfig::kBuiltinLoadMethodElim);
  runSimplifyAndFloatAccumulatorPromotion();
  // Simplify creates the CDouble PrimitiveBox shapes this pass rematerializes.
  // PrimitiveUnboxCSE makes more of those boxes frame-state-only by merging
  // repeated unboxes before remat looks at direct uses.
  runPassIf(hir::PrimitiveUnboxCSE{}, PassConfig::kPrimitiveUnboxCSE);
#if defined(CINDER_AARCH64)
  // ARM-only: relies on fcmp + signed GT/GE for correct NaN semantics.
  runPassIf(
      hir::FloatComparisonSimplification{},
      PassConfig::kFloatComparisonSimplification);
#endif
  // Deopt correctness also requires the backend trampoline to save FP
  // registers in regs[]; that support is currently implemented for AArch64.
#if defined(CINDER_AARCH64)
  runPassIf(hir::PrimitiveBoxRemat{}, PassConfig::kPrimitiveBoxRemat);
#endif
  runPassIf(hir::CleanCFG{}, PassConfig::kCleanCFG);
  runPassIf(hir::DeadCodeElimination{}, PassConfig::kDeadCodeElim);
  runPassIf(hir::CleanCFG{}, PassConfig::kCleanCFG);
  runPassIf(
      hir::TreeIterStateMachinePass{}, PassConfig::kTreeIterStateMachine);

  runPass(jit::hir::RefcountInsertion{}, irfunc, callback);

  if (getConfig().dump_hir_stats) {
    jit::hir::HIRStats stats;
    runPass(stats, irfunc, callback);
    stats.dump(irfunc.fullname);
  }

  runPassIf(
      jit::hir::InsertUpdatePrevInstr{}, PassConfig::kInsertUpdatePrevInstr);

  JIT_LOGIF(
      getConfig().log.dump_hir_final,
      "Optimized HIR for {}:\n{}",
      irfunc.fullname,
      irfunc);
}

#if PY_VERSION_HEX < 0x030C0000
// Mid-flight tracing/profile transitions follow the RFC's versioned
// compatibility semantics (3.3.4.5): a JIT frame already on the stack
// keeps running natively to its natural return and its remaining
// trace/profile events are not delivered; only NEW calls observe the
// activation, at the guarded entry, and fall back to the interpreter.
// No safepoint stack-level deoptimization exists, so the executing mode
// inserts no bytecode-boundary instrumentation polls.  A frame that
// deopts mid-function for an ordinary reason (guard, exception) while
// tracing is already active gets its f_trace/f_trace_lines set at the
// resume (RFC item 7) -- see setupTraceForDeoptedFrame.

PassConfig createConfig();

std::unique_ptr<hir::Function> compileToFinalHIRForTest(
    BorrowedRef<PyFunctionObject> func) {
  std::unique_ptr<hir::Preloader> preloader =
      hir::Preloader::make(func, makeFrameReifier(func->func_code));
  if (preloader == nullptr) {
    return nullptr;
  }
  std::unique_ptr<hir::Function> irfunc(hir::buildHIR(*preloader));
  if (irfunc == nullptr) {
    return nullptr;
  }
  Compiler::runPasses(*irfunc, createConfig());
  return irfunc;
}
#endif

std::optional<CompiledFunctionData> Compiler::Compile(
    BorrowedRef<PyFunctionObject> func) {
  JIT_CHECK(PyFunction_Check(func), "Expected PyFunctionObject");
  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "multi-thread compile must preload first");
  std::unique_ptr<hir::Preloader> preloader =
      hir::Preloader::make(func, makeFrameReifier(func->func_code));
  return preloader ? Compile(*preloader) : std::nullopt;
}

std::optional<ShadowCompileResult> Compiler::CompileShadow(
    BorrowedRef<PyFunctionObject> func) {
  ShadowCompileScope shadow_scope;
  JIT_CHECK(PyFunction_Check(func), "Expected PyFunctionObject");
  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "multi-thread compile must preload first");
  std::unique_ptr<hir::Preloader> preloader =
      hir::Preloader::make(func, makeFrameReifier(func->func_code));
  if (preloader == nullptr) {
    const char* name = "<unknown>";
    if (func->func_qualname != nullptr) {
      if (const char* utf8 = PyUnicode_AsUTF8(func->func_qualname)) {
        name = utf8;
      }
    }
    JIT_THROW("shadow preload failed for {}", name);
  }
  return CompileShadow(*preloader);
}

PassConfig createConfig() {
  auto result = static_cast<uint64_t>(PassConfig::kMinimal);

  auto set = [&](bool global, PassConfig pass) {
    if (global) {
      result |= pass;
    }
  };

  auto const& hir_opts = getConfig().hir_opts;
  set(hir_opts.begin_inlined_function_elim,
      PassConfig::kBeginInlinedFunctionElim);
  set(hir_opts.builtin_load_method_elim, PassConfig::kBuiltinLoadMethodElim);
  set(hir_opts.clean_cfg, PassConfig::kCleanCFG);
  set(hir_opts.dynamic_comparison_elim, PassConfig::kDynamicComparisonElim);
  set(hir_opts.float_accumulator_promotion,
      PassConfig::kFloatAccumulatorPromotion);
  set(hir_opts.guard_type_removal, PassConfig::kGuardTypeRemoval);
  // Inliner currently depends on code objects being stable.
  set(hir_opts.inliner && getConfig().stable_frame, PassConfig::kInliner);
  set(hir_opts.insert_update_prev_instr, PassConfig::kInsertUpdatePrevInstr);
  set(hir_opts.phi_elim, PassConfig::kPhiElim);
  set(hir_opts.primitive_box_remat, PassConfig::kPrimitiveBoxRemat);
  set(hir_opts.primitive_unbox_cse, PassConfig::kPrimitiveUnboxCSE);
  set(hir_opts.float_comparison_simplification,
      PassConfig::kFloatComparisonSimplification);
  set(hir_opts.simplify, PassConfig::kSimplify);
  set(hir_opts.tree_iter_state_machine, PassConfig::kTreeIterStateMachine);

#if PY_VERSION_HEX < 0x030C0000
  // FloatAccumulatorPromotion still stays off in the executing mode: it
  // is a speculative rewrite whose deopt metadata is not yet the MR-07
  // subject.  Simplify returns so compact-long / float / x**2 guards
  // ship with stable site ids.
  if (getConfig().state == State::kRunning) {
    result &= ~static_cast<uint64_t>(PassConfig::kFloatAccumulatorPromotion);
  }
#endif

  return static_cast<PassConfig>(result);
}

std::optional<ShadowCompileResult> Compiler::CompileShadow(
    const jit::hir::Preloader& preloader) {
  ShadowCompileScope shadow_scope;
  const std::string& fullname = preloader.fullname();
  if (!PyDict_CheckExact(preloader.globals()) ||
      !PyDict_CheckExact(preloader.builtins())) {
    JIT_DLOG(
        "Refusing shadow compilation for {}: globals and builtins must be "
        "exact dicts",
        fullname);
    return std::nullopt;
  }

  std::unique_ptr<hir::Function> irfunc(hir::buildHIR(preloader));
  irfunc->reifier = ThreadedRef<>::create(preloader.reifier());
  Compiler::runPasses(*irfunc, createConfig());

  // Unlike the per-pass debug checks, the shadow verifier is part of the
  // release gate: malformed CFG, missing terminators/definitions, bad phi
  // edges, and illegal typed operands must become a stable compile failure.
  std::ostringstream verifier_errors;
  if (!hir::checkFunc(*irfunc, verifier_errors) ||
      !hir::funcTypeChecks(*irfunc, verifier_errors)) {
    JIT_DLOG(
        "Shadow HIR for {} failed verification:\n{}\n{}",
        fullname,
        verifier_errors.str(),
        *irfunc);
    JIT_THROW(
        "Shadow HIR for {} failed verification: {}",
        fullname,
        verifier_errors.str());
  }

  ShadowCompileResult result;
  result.hir_opcode_counts = hir::count_opcodes(*irfunc);
  for (const auto& instr : BytecodeInstructionBlock{preloader.code()}) {
    int physical_opcode = instr.specializedOpcode();
    if (physical_opcode != unspecialize(physical_opcode)) {
      result.specialized_opcodes++;
    }
  }

  auto ngen = ngen_factory_(irfunc.get());
  if (ngen == nullptr) {
    JIT_THROW("shadow native generator missing for {}", fullname);
  }
  result.code_size = ngen->getShadowCodeSize();
  if (result.code_size == 0) {
    JIT_THROW("shadow codegen produced no bytes for {}", fullname);
  }
  JIT_DLOG(
      "Finished shadow compilation for {}, code size: {} bytes",
      fullname,
      result.code_size);
  return result;
}

std::optional<CompiledFunctionData> Compiler::Compile(
    const jit::hir::Preloader& preloader) {
  const std::string& fullname = preloader.fullname();
  if (!PyDict_CheckExact(preloader.globals())) {
    JIT_DLOG(
        "Refusing to compile {}: globals is a {:.200}, not a dict",
        fullname,
        Py_TYPE(preloader.globals())->tp_name);
    return std::nullopt;
  }

  PyObject* builtins = preloader.builtins();
  if (!PyDict_CheckExact(builtins)) {
    JIT_DLOG(
        "Refusing to compile {}: builtins is a {:.200}, not a dict",
        fullname,
        Py_TYPE(builtins)->tp_name);
    return std::nullopt;
  }
  JIT_DLOG("Compiling {}", fullname);

  std::unique_ptr<CompilationPhaseTimer> compilation_phase_timer{nullptr};

  if (captureCompilationTimeFor(fullname)) {
    compilation_phase_timer = std::make_unique<CompilationPhaseTimer>(fullname);
    compilation_phase_timer->start("Overall compilation");
    compilation_phase_timer->start("Lowering into HIR");
  }

  Timer timer;
  std::unique_ptr<hir::Function> irfunc(hir::buildHIR(preloader));
  irfunc->reifier = ThreadedRef<>::create(preloader.reifier());
  const bool compile_osr_entries =
      getConfig().osr_enabled && !preloader.osrEntryTargetOffsets().empty();
  if (compile_osr_entries) {
    irfunc->markOSREntries(preloader.osrEntryTargetOffsets(), preloader.code());
  }
  if (nullptr != compilation_phase_timer) {
    compilation_phase_timer->end();
  }

  if (getConfig().log.dump_hir_initial) {
    JIT_LOG("Initial HIR for {}:\n{}", fullname, *irfunc);
  }

  if (nullptr != compilation_phase_timer) {
    irfunc->setCompilationPhaseTimer(std::move(compilation_phase_timer));
  }

  PassConfig config = createConfig();
  COMPILE_TIMER(
      irfunc->compilation_phase_timer,
      "HIR transformations",
      Compiler::runPasses(*irfunc, config))
  if (compile_osr_entries && irfunc->hasOSREntries()) {
    irfunc->extractOSRLiveIns();
  }

  hir::OpcodeCounts hir_opcode_counts = hir::count_opcodes(*irfunc);

#if PY_VERSION_HEX < 0x030C0000
  // Any reason left by an earlier attempt belongs to that attempt.
  setLast311ExecuteRefusal(nullptr);
  // Guard / GuardIs / GuardType / Deopt are the MR-07 restore surface.
  // DeoptPatchpoint is the IC / watcher patch hook and stays refused
  // until MR-09.
  if (getConfig().state == State::kRunning) {
    int count =
        hir_opcode_counts[static_cast<size_t>(hir::Opcode::kDeoptPatchpoint)];
    if (count > 0) {
      setLast311ExecuteRefusal("REFUSE_SHAPE_SPECULATIVE_GUARD");
      JIT_DLOG(
          "Refusing MR-07 execution for {}: optimized HIR holds {} "
          "DeoptPatchpoint instruction(s); IC patchpoints are MR-09 work",
          fullname,
          count);
      return std::nullopt;
    }
  }
#endif

  auto ngen = ngen_factory_(irfunc.get());
  if (ngen == nullptr) {
    return std::nullopt;
  }

  vectorcallfunc entry = nullptr;
  COMPILE_TIMER(
      irfunc->compilation_phase_timer,
      "Native code Generation",
      entry = reinterpret_cast<vectorcallfunc>(ngen->getVectorcallEntry()))
  if (entry == nullptr) {
    JIT_DLOG("Generating native code for {} failed", fullname);
    return std::nullopt;
  }

  auto compile_time =
      std::chrono::duration_cast<std::chrono::microseconds>(timer.finish());

  JIT_DLOG(
      "Finished compiling {} in {}, code size: {} bytes (code: {})",
      fullname,
      compile_time,
      ngen->getCodeBuffer().size_bytes(),
      static_cast<void*>(preloader.code()));
  if (nullptr != irfunc->compilation_phase_timer) {
    irfunc->compilation_phase_timer->end();
    irfunc->setCompilationPhaseTimer(nullptr);
  }

  int stack_size = ngen->GetCompiledFunctionStackSize();
  int spill_stack_size = ngen->GetCompiledFunctionSpillStackSize();

  // Grab some fields off of irfunc and ngen before moving them.
  hir::Function::InlineFunctionStats inline_stats =
      std::move(irfunc->inline_function_stats);
  std::span<const std::byte> code = ngen->getCodeBuffer();
  auto code_runtime = ngen->codeRuntime();

  CompiledFunctionData compiled_data;
  compiled_data.code = code;
  compiled_data.vectorcall_entry = entry;
  compiled_data.stack_size = stack_size;
  compiled_data.spill_stack_size = spill_stack_size;
  compiled_data.inline_function_stats = std::move(inline_stats);
  compiled_data.hir_opcode_counts = hir_opcode_counts;
  compiled_data.runtime = code_runtime;
  compiled_data.osr_aware = compile_osr_entries;
  compiled_data.has_osr_entries = code_runtime->hasOSREntries();
  compiled_data.compile_time = compile_time;
  compiled_data.code_patchers = std::move(irfunc->code_patchers);
  if (getConfig().log.debug) {
    irfunc->setCompilationPhaseTimer(nullptr);
    compiled_data.irfunc = std::move(irfunc);
  }
  return compiled_data;
}

} // namespace jit
