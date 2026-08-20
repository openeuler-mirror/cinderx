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
// CPython 3.11 has no hook that reports PyEval_SetTrace()/SetProfile() to
// the JIT -- no monitoring events, no watchers -- and the entry check only
// answers for the moment of the call.  Stock reacts to a mid-frame
// transition immediately: the running frame's remaining events, above all
// its PyTrace_RETURN, are delivered under the new state.  So the executing
// mode polls, and the poll is a BOUNDARY INVARIANT, not an enumeration of
// instructions that look reentrant: anything can flip the state --
// operator protocol, truthiness, iteration, a pending call at the back
// edge, and reference counting itself, whose Decrefs are generated by a
// later pass and can run arbitrary __del__ code.  An enumeration is a
// second, narrower definition of "arbitrary execution" that drifts from
// the one the codebase already maintains; the invariant subsumes it.
//
// Concretely: a poll follows every per-bytecode snapshot (the last of a
// consecutive run), carrying that snapshot's frame state -- the state in
// which the previous bytecode's work is complete and resumable.  A
// transition consumed by a block terminator (truthiness feeding a
// conditional branch, an iterator step feeding the loop exit) is observed
// at the successor's entry boundary, whose snapshot the builder always
// emits; a transition made by a Decref's __del__ is observed at the next
// boundary after the Decref, because the refcount pass places releases
// between the boundary that last names the value and the next one.  The
// back edge's eval-breaker service keeps its dedicated poll, which is
// where pending calls execute.  DeoptReason::kInstrumentation resumes AT
// the frame state's instruction, so completed work is neither replayed
// nor skipped.
//
// This runs before the pass pipeline, on the builder's snapshots, so the
// polls are renamed by SSA and annotated by the refcount pass like any
// other deopt exit; being effectful deopt carriers, no pass moves or
// removes them.  The cost -- two loads and two branches per bytecode
// boundary -- is paid only by the executing mode.
void insertInstrumentationPolls311(hir::Function& irfunc) {
  for (auto& block : irfunc.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      hir::Instr& instr = *it;
      ++it;
      if (instr.opcode() == hir::Opcode::kSnapshot) {
        if (it == block.end()) {
          continue;
        }
        // Consecutive snapshots are consecutive bytecode boundaries --
        // an instruction like POP_TOP emits nothing between them -- and
        // every one of them gets its poll.  Skipping to the last of a run
        // was measurably wrong: a value that dies at the earlier boundary
        // is then anchored by no poll, and the refcount pass places its
        // release after the only poll left, where a __del__ can flip the
        // instrumentation state with no boundary check remaining before
        // the return.  The poll at the earlier boundary is what pins the
        // release between two checks.
        auto& snapshot = static_cast<hir::Snapshot&>(instr);
        hir::FrameState* state = snapshot.frameState();
        if (state == nullptr) {
          continue;
        }
        hir::Instr* check = hir::CheckInstrumentation::create(*state);
        check->copyBytecodeOffset(instr);
        check->InsertBefore(*it);
      } else if (instr.opcode() == hir::Opcode::kRunPeriodicTasks) {
        auto& periodic = static_cast<hir::DeoptBase&>(instr);
        hir::FrameState* state = periodic.frameState();
        // A snapshot right after the service carries the boundary's own
        // frame state and receives its poll above; adding one here with
        // the service's state would be wrong for the back-edge polls,
        // whose service deliberately carries the state AT the backward
        // jump (for traceback parity) while the boundary around it
        // resumes at the jump's target.
        if (state != nullptr && it != block.end() &&
            it->opcode() != hir::Opcode::kSnapshot) {
          hir::Instr* check = hir::CheckInstrumentation::create(*state);
          check->copyBytecodeOffset(instr);
          check->InsertBefore(*it);
        }
      }
    }
  }
}

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
  if (getConfig().state == State::kRunning) {
    insertInstrumentationPolls311(*irfunc);
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
  // MR-04 excludes speculative guards, and Simplify is where they come
  // from once quickened opcodes are already out of the picture: the
  // compact-long comparison and arithmetic fast paths, float division and
  // the bounds checks all install a Guard with a deopt behind it.
  //
  // Refusing every function that would get one would empty the execute
  // surface -- a plain `while i < b` loop is exactly the shape that picks
  // up the compact-long compare guard -- so the executing mode compiles
  // without the pass instead, and the artifact scan downstream stays as
  // the backstop for anything that still slips through.  Generated code is
  // slower; that is the right trade for a milestone whose subject is
  // correctness, and Simplify returns with the guard metadata in MR-07.
  if (getConfig().state == State::kRunning) {
    result &= ~static_cast<uint64_t>(PassConfig::kSimplify);
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

#if PY_VERSION_HEX < 0x030C0000
  // Before the pass pipeline: the polls ride through SSA renaming like
  // every other instruction, and the refcount pass annotates their live
  // registers exactly as it does for the error exits -- metadata the
  // deopt machinery cannot reconstruct for an instruction added later.
  // They are deopt-carrying instructions with side effects, so no pass
  // moves or removes them.
  if (getConfig().state == State::kRunning) {
    insertInstrumentationPolls311(*irfunc);
  }
#endif

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
  // MR-04 excludes speculative guards, and an eligibility check on the
  // bytecode cannot see them: the optimizer introduces its own.  Simplify
  // rewrites `x ** 2` into a float multiply behind a GuardType, for one,
  // and that is a deopt point on an unaudited path just as much as a
  // quickened opcode's guard would be.  So the rule is stated where it can
  // actually be checked -- on the artifact about to be emitted -- and a
  // violation refuses the compile rather than shipping the guard.
  if (getConfig().state == State::kRunning) {
    // All three of HIR's deopt guards, not just the typed ones: Simplify
    // emits the untyped Guard directly for compact-long comparisons,
    // float division and the in-place long paths, and that is a deopt on
    // an unaudited path exactly like the others.
    // Every opcode that installs a deopt exit of its own, not just the
    // three reachable today.  Deopt and DeoptPatchpoint are currently
    // unreachable because the whitelist excludes the opcodes that emit
    // them and Simplify is off -- but a backstop resting on someone
    // else's invariant is not a backstop.  Widen the surface or re-enable
    // the pass and this still holds.
    // CheckInstrumentation is deliberately not in this list: it is an
    // audited-path exit like the error checks -- it asserts nothing
    // about values, and the instrumentation poll above inserts it on
    // purpose.  The refusal is about SPECULATIVE deopt points.
    static constexpr hir::Opcode kSpeculativeGuards[] = {
        hir::Opcode::kDeopt,
        hir::Opcode::kDeoptPatchpoint,
        hir::Opcode::kGuard,
        hir::Opcode::kGuardIs,
        hir::Opcode::kGuardType,
    };
    for (hir::Opcode op : kSpeculativeGuards) {
      int count = hir_opcode_counts[static_cast<size_t>(op)];
      if (count > 0) {
        setLast311ExecuteRefusal("REFUSE_SHAPE_SPECULATIVE_GUARD");
        JIT_DLOG(
            "Refusing MR-04 execution for {}: optimized HIR holds {} {} "
            "instruction(s); speculative guards are MR-07 work",
            fullname,
            count,
            hir::hirOpcodeName(op));
        return std::nullopt;
      }
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
