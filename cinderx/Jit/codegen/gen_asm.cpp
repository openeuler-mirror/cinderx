// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/gen_asm.h"
#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/interpreter_contract.h"
#endif
#include "internal/pycore_ceval.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/codegen/gen_asm_utils.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/printer.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/Jit/lir/target_select.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace asmjit;
using namespace jit;
using namespace jit::hir;
using namespace jit::lir;
using namespace jit::util;

namespace jit::codegen {

namespace {

// Return type for prepareForDeopt: the reified frame and whether this deopt
// was triggered by instrumentation (setIP patch) rather than a guard failure.
struct DeoptResult {
  CiPyFrameObjType* frame;
  bool is_instrumentation_deopt;
};

#define ASM_CHECK_THROW(exp)                         \
  {                                                  \
    auto err = (exp);                                \
    if (err != kErrorOk) {                           \
      auto message = DebugUtils::errorAsString(err); \
      throw AsmJitException(err, #exp, message);     \
    }                                                \
  }

#define ASM_CHECK(exp, what)             \
  {                                      \
    auto err = (exp);                    \
    JIT_CHECK(                           \
        err == kErrorOk,                 \
        "Failed generating {}: {}",      \
        (what),                          \
        DebugUtils::errorAsString(err)); \
  }

// Scratch register used by the various deopt trampolines.
[[maybe_unused]] const auto deopt_scratch_reg = arch::reg_scratch_deopt;

void raiseUnboundLocalError(BorrowedRef<> name) {
  // name is converted into a `char*` in format_exc_check_arg

  const char* msg =
      "cannot access local variable '%.200s' where it is not associated with "
      "a value";

  _PyEval_FormatExcCheckArg(
      _PyThreadState_GET(), PyExc_UnboundLocalError, msg, name);
}

void raiseUnboundFreevarError(BorrowedRef<> name) {
  // name is converted into a `char*` in format_exc_check_arg

  const char* msg =
      "cannot access free variable '%.200s' where it is not associated with a"
      " value in enclosing scope";

  _PyEval_FormatExcCheckArg(_PyThreadState_GET(), PyExc_NameError, msg, name);
}

void raiseAttributeError(BorrowedRef<> receiver, BorrowedRef<> name) {
  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      Py_TYPE(receiver)->tp_name,
      name);
}

// Helper to recursively reify the lightweight frames. We need to reify the
// outermost lightweight frame first and work inwards to have the frames
// allocated correctly on the slab. We then need to update the inner functions
// previous to point at any updated outer frames. So we recurse to the inner
// most frame, convert it, return the new frame, and continue converting as
// we unwind.
_PyInterpreterFrame* reifyLightweightFrames(
    PyThreadState* tstate,
    const DeoptMetadata& deopt_meta,
    size_t depth,
    _PyInterpreterFrame* cur_frame) {
  _PyInterpreterFrame* prev = nullptr;
  if (depth > 0) {
    prev = reifyLightweightFrames(
        tstate, deopt_meta, depth - 1, cur_frame->previous);
  }
  if (!(_PyFrame_GetCode(cur_frame)->co_flags & kCoFlagsAnyGenerator)) {
    cur_frame = convertInterpreterFrameFromStackToSlab(tstate, cur_frame);
    if (cur_frame == nullptr) {
      return nullptr;
    }
  } else {
    jitFramePopulateFrame(cur_frame);
    jitFrameRemoveReifier(cur_frame);
  }
  if (prev) {
    cur_frame->previous = prev;
  }
  return cur_frame;
}

DeoptResult prepareForDeopt(
    const uint64_t* regs,
    CodeRuntime* code_runtime,
    std::size_t deopt_idx) {
  JIT_CHECK(deopt_idx != -1ull, "deopt_idx must be valid");
  DeoptMetadata& deopt_meta = code_runtime->getDeoptMetadata(deopt_idx);
  bool is_forced_deopt = false;
#if PY_VERSION_HEX < 0x030C0000
  is_forced_deopt = deopt_meta.consumed_forced;
  deopt_meta.consumed_forced = false;
  if (is_forced_deopt) {
    triggerStatsOnForcedDeopt();
  } else {
    triggerStatsOnOrganicDeopt();
  }
#endif
  PyThreadState* tstate = _PyThreadState_UncheckedGet();
  bool is_patched_instrumentation = false;
  _PyInterpreterFrame* frame = interpFrameFromThreadState(tstate);

  // Check JIT_FRAME_DEOPT_PATCHED on the outermost frame's header before
  // reification destroys it. Walk past inlined frames to find the outer one.
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  {
    if (getConfig().frame_mode == FrameMode::kLightweight) {
      _PyInterpreterFrame* outer = frame;
      for (size_t i = 0; i < deopt_meta.inline_depth(); i++) {
        outer = outer->previous;
      }
      is_patched_instrumentation = (jitFrameGetHeader(outer)->frame_status &
                                    JIT_FRAME_DEOPT_PATCHED) != 0;
    }
  }
#endif

  // The patched flavor above stops a frame at its next deopt exit with the
  // PRE-instruction state, leaving the completed call's return value in a
  // register to be recovered below.  The polled flavor -- a
  // CheckInstrumentation exit -- carries the complete post-instruction
  // frame state instead: the result is already in the reified stack, and
  // there is nothing to recover from registers.  The two flavors resume
  // differently -- patched advances past the completed instruction, polled
  // resumes at its own frame state -- so only the patched flag travels into
  // frame reification; the polled and periodic-task cases are recognized
  // there by their deopt reasons.  A single "is instrumentation" bit was
  // tried as the resume selector and is exactly wrong: it made the polled
  // flavor advance, skipping the instruction the frame state names.  The
  // combined bit survives below only for what it is right about --
  // statistics, backoff accounting and the resume-target routing, which
  // treat both flavors alike.
  const bool is_polled_instrumentation =
      deopt_meta.reason == DeoptReason::kInstrumentation;
  const bool is_instrumentation_deopt =
      is_patched_instrumentation || is_polled_instrumentation;

  if (getConfig().frame_mode == FrameMode::kLightweight) {
    frame = reifyLightweightFrames(
        tstate, deopt_meta, deopt_meta.inline_depth(), frame);
    if (frame == nullptr) {
      Py_FatalError("Cannot recover from OOM");
    }
    setCurrentFrame(tstate, frame);
  }

  _PyInterpreterFrame* frame_iter = frame;

  // Iterate one past the inline depth because that is the caller frame.
  for (int i = deopt_meta.inline_depth(); i >= 0; i--) {
    // Transfer ownership of a light weight frame to the interpreter. The
    // associated Python frame will be ignored during future attempts to
    // materialize the stack.
    reifyFrame(
        frame_iter,
        deopt_meta,
        deopt_meta.frame_meta.at(i),
        regs,
        is_patched_instrumentation);
    frame_iter = frame_iter->previous;
  }

  // For instrumentation deopts where the bytecode's C call completed
  // (reason != kPeriodicTaskFailure), push its return value onto the
  // operand stack on top of the pre-instruction state restored by reifyStack.
  if (is_patched_instrumentation) {
    if (deopt_meta.reason != DeoptReason::kPeriodicTaskFailure &&
        !PyErr_Occurred()) {
      PyObject* retval = reinterpret_cast<PyObject*>(
          regs[codegen::arch::reg_general_return_loc.loc]);
      if (retval != nullptr) {
#if PY_VERSION_HEX >= 0x030E0000
        *(frame->stackpointer) = PyStackRef_FromPyObjectSteal(retval);
        frame->stackpointer++;
#else
        frame->localsplus[frame->stacktop] = Ci_STACK_STEAL(retval);
        frame->stacktop++;
#endif
      } else {
        PyErr_SetString(
            PyExc_SystemError,
            "JIT instrumentation deopt: call returned NULL without "
            "setting an exception");
      }
    }
  }
  // Clear our references now that we've transferred them to the frame
  MemoryView mem{regs};
  Ref<> deopt_obj;
  if (!is_instrumentation_deopt) {
    // TODO(T262342844): Add USDT support for instrumentation-related deopts.
    deopt_obj = profileDeopt(deopt_meta, mem);
  }
  auto ctx = getContext();
  ctx->recordDeopt(code_runtime, deopt_idx, deopt_obj);
  releaseRefs(deopt_meta, mem);
  if (_PyFrame_GetCode(frame)->co_flags & kCoFlagsAnyGenerator) {
    BorrowedRef<PyGenObject> base_gen = _PyGen_GetGeneratorFromFrame(frame);
    JitGenObject* gen = JitGenObject::cast(base_gen.get());
    if (gen != nullptr) {
      deopt_jit_gen_object_only(gen);
    }
  }
  if (!PyErr_Occurred() && !is_instrumentation_deopt && !is_forced_deopt) {
    auto reason = deopt_meta.reason;
    switch (reason) {
      case DeoptReason::kGuardFailure: {
        ctx->guardFailed(deopt_meta);
        break;
      }
      case DeoptReason::kRaise:
      case DeoptReason::kYieldFrom:
      case DeoptReason::kInstrumentation: {
        break;
      }
      case DeoptReason::kUnhandledNullField:
        raiseAttributeError(deopt_obj, deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledUnboundLocal:
        raiseUnboundLocalError(deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledUnboundFreevar:
        raiseUnboundFreevarError(deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledException:
      case DeoptReason::kPeriodicTaskFailure:
        JIT_ABORT("unhandled exception without error set");
      case DeoptReason::kRaiseStatic:
        JIT_ABORT("Lost exception when raising static exception");
    }
  }
  if (!is_forced_deopt) {
    jit::recordDeoptForRoiBackoff(
        code_runtime, deopt_meta.reason, is_instrumentation_deopt);
  }
  return {frame, is_instrumentation_deopt};
}

// Set up f_trace/f_trace_lines for sys.settrace compatibility on deopted
// frames. CPython normally sets these during the RESUME opcode at function
// entry, but deopted frames resume mid-function and skip RESUME.
void setupTraceForDeoptedFrame(
    _PyInterpreterFrame* frame,
    PyThreadState* tstate) {
  if (tstate->c_tracefunc != nullptr &&
      frame->owner != FRAME_OWNED_BY_GENERATOR) {
    PyFrameObject* fobj = _PyFrame_GetFrameObject(frame);
    if (fobj != nullptr) {
      fobj->f_trace_lines = 1;
      if (fobj->f_trace == nullptr && tstate->c_traceobj != nullptr) {
        fobj->f_trace = Py_NewRef(tstate->c_traceobj);
      }
    }
  }
}

PyObject* resumeInInterpreter(
    _PyInterpreterFrame* frame,
    CodeRuntime* code_runtime,
    std::size_t deopt_idx,
    bool is_instrumentation_deopt) {
  JIT_CHECK(code_runtime != nullptr, "CodeRuntime cannot be a nullptr");

  PyThreadState* tstate = PyThreadState_Get();

  const DeoptMetadata& deopt_meta = code_runtime->getDeoptMetadata(deopt_idx);

  // For instrumentation deopts, only enter error handler if actually excepted.
  int err_occurred;
  if (is_instrumentation_deopt) {
    err_occurred = PyErr_Occurred() ? 1 : 0;
  } else {
    err_occurred = shouldResumeInterpreterInErrorHandler(deopt_meta.reason);
  }

  PyObject* result = nullptr;
  // Resume all of the inlined frames and the caller
  int inline_depth = deopt_meta.inline_depth();
  while (inline_depth >= 0) {
    // TODO(emacs): Investigate skipping resuming frames that do not have
    // try/catch. Will require re-adding _PyShadowFrame_Pop back for
    // non-generators and unlinking the frame manually.

    // Resume one frame.
    _PyInterpreterFrame* prev_frame = frame->previous;
    // Delegate management of `tstate->frame` to the interpreter loop. On
    // entry, it expects that tstate->frame points to the frame for the calling
    // function.
    JIT_CHECK(
        currentFrame(tstate) == frame, "unexpected frame at top of stack");
    setCurrentFrame(tstate, frame->previous);
    // The interpreter calls _Py_Instrument() on the initial RESUME opcode in a
    // function, but we don't do this in the JIT. Calling it now is a bit
    // dubious, because we are splitting execution of the same function between
    // instrumented and not. However, if we don't do this and instrumentation
    // is enabled we might hit assertions in the deopted execution because the
    // code's instrumentation version doesn't match the interpreter's.
    JIT_CHECK(
        _Py_Instrument(frameCode(frame), tstate->interp) == 0,
        "Failed to instrument code on deopt");

    // For generator frames, ensure the exception state is properly linked
    // before resuming in the interpreter. In Python 3.15+, generator
    // expressions may raise exceptions before RETURN_GENERATOR (e.g., GET_ITER
    // on a non-iterable). The interpreter expects tstate->exc_info to point to
    // the generator's exception state for generator frames.
    // The interpreter's clear_gen_frame() will handle restoring the previous
    // exception state, so we don't need to do any cleanup after
    // _PyEval_EvalFrame. Note: We only set this up if it's not already set
    // (e.g., jitgen_am_send may have already set it up before we got here).
    //
    // Additionally, if the generator was never returned to the caller (i.e.,
    // exception occurred before RETURN_GENERATOR), we need to decref the
    // generator since nobody owns the reference. We detect this by checking
    // if gi_frame_state was FRAME_CREATED before executing.
    PyGenObject* gen_to_cleanup = nullptr;
    if (frame->owner == FRAME_OWNED_BY_GENERATOR) {
      PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
      if (gen->gi_frame_state == FRAME_CREATED) {
        // This generator was never returned to the caller (before
        // RETURN_GENERATOR). If an exception occurs, we need to clean it up.
        gen_to_cleanup = gen;
      }
      if (tstate->exc_info != &gen->gi_exc_state) {
        gen->gi_exc_state.previous_item = tstate->exc_info;
        tstate->exc_info = &gen->gi_exc_state;
      }
    }

    setupTraceForDeoptedFrame(frame, tstate);

#if PY_VERSION_HEX < 0x030C0000
    // Pin the continuation to the evaluator that started this frame.
    // _PyEval_EvalFrame() dispatches through interp->eval_frame at CALL
    // time, so a PEP 523 client installed while machine code was running
    // would retroactively receive the reified mid-frame -- and with it
    // this branch's entry-frame clear-and-pop contract, which is anchored
    // to the vendored 3.11 evaluator's return protocol.  Changing the
    // evaluator affects future entries; a frame already in flight is
    // finished by the interpreter that launched it.
    result = Ci_EvalFrameDefault_311(tstate, frame, err_occurred);
#else
    result = _PyEval_EvalFrame(tstate, frame, err_occurred);
#endif

    // If exception occurred before RETURN_GENERATOR, the generator was never
    // returned to anyone. The JIT created the generator early, but the caller
    // never received it. We need to decref it to avoid a memory leak.
    if (result == nullptr && gen_to_cleanup != nullptr) {
      Py_DECREF(gen_to_cleanup);
    }

#if PY_VERSION_HEX < 0x030C0000
    // Stock 3.11 leaves entry-frame ownership with the caller: eval's
    // exit paths return without clearing or popping the frame they were
    // entered with (is_entry), and _PyEval_Vector performs the
    // clear-and-pop afterwards.  Do that caller duty here, mirroring
    // _PyEvalFrameClearAndPop (including the recursion_remaining dance so
    // destructors run under the same headroom); without it every
    // exception deopt leaks the reified frame's references and later
    // datastack pushes reuse the never-cleared slots.  3.12+ eval pops
    // the entry frame itself, so this block must not exist there.
    if (frame->owner == FRAME_OWNED_BY_THREAD) {
      tstate->recursion_remaining--;
      _PyFrame_Clear(frame);
      tstate->recursion_remaining++;
      _PyThreadState_PopFrame(tstate, frame);
    }
#endif

    frame = prev_frame;

    err_occurred = result == nullptr;
    // Push the previous frame's result onto the value stack. We can't push
    // after resuming because f_stacktop is nullptr during execution of a frame.
    if (!err_occurred && inline_depth > 0) {
      // The caller is at inline depth 0, so we only attempt to push the
      // result onto the stack in the deeper (> 0) frames. Otherwise, we
      // should just return the value from the native code in the way our
      // native calling convention requires.
#if PY_VERSION_HEX >= 0x030E0000
      _PyFrame_StackPush(frame, Ci_STACK_STEAL(result));
#else
      frame->localsplus[frame->stacktop++] = result;
#endif
    }
    inline_depth--;
  }
  return result;
}

void* finalizeCode(arch::Builder& builder, std::string_view name) {
  if (auto err = builder.finalize(); err != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to finalize asmjit builder for {}, got error code {}",
        name,
        DebugUtils::errorAsString(err))};
  }

  ICodeAllocator* code_allocator =
      cinderx::getModuleState()->code_allocator.get();
  AllocateResult result = code_allocator->addCode(builder.code());
  if (result.error != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to add generated code for {} to asmjit runtime, got error code "
        "{}",
        name,
        DebugUtils::errorAsString(result.error))};
  }
  return result.addr;
}

size_t finalizeShadowCode(arch::Builder& builder, std::string_view name) {
  if (auto err = builder.finalize(); err != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to finalize shadow asmjit builder for {}, got error code {}",
        name,
        DebugUtils::errorAsString(err))};
  }

  CodeHolder* code = builder.code();
  bool validate_split_layout = false;
#if defined(CINDER_AARCH64)
  validate_split_layout = getConfig().multiple_code_sections &&
      dynamic_cast<CodeAllocatorCinder*>(
          cinderx::getModuleState()->code_allocator.get()) != nullptr;
#endif
  if (validate_split_layout) {
    // Model a fresh split allocation without touching the allocator, using the
    // production layout calculation so AArch64 alignment and displacement
    // constraints cannot drift between the two paths.
    size_t hot_size = 0;
    size_t cold_size = 0;
    for (Section* section : code->sections()) {
      if (codeSectionFromName(section->name()) == CodeSection::kCold) {
        cold_size += section->realSize();
      } else {
        hot_size += section->realSize();
      }
    }
    SplitAllocationLayout layout =
        computeSplitAllocationLayout(hot_size, cold_size);
    size_t hot_offset = 0;
    size_t cold_offset = layout.hot_capacity;
    for (Section* section : code->sections()) {
      if (codeSectionFromName(section->name()) == CodeSection::kCold) {
        section->setOffset(cold_offset);
        cold_offset += section->realSize();
      } else {
        section->setOffset(hot_offset);
        hot_offset += section->realSize();
      }
    }
  } else if (auto err = code->flatten(); err != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to flatten shadow code for {}, got error code {}",
        name,
        DebugUtils::errorAsString(err))};
  }
  if (auto err = code->resolveUnresolvedLinks(); err != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to resolve shadow code links for {}, got error code {}",
        name,
        DebugUtils::errorAsString(err))};
  }

  // Relocate against a representative address in the JIT image. This checks
  // target displacement constraints without allocating executable memory.
  constexpr uintptr_t kPageMask = ~uintptr_t{0xfff};
  uintptr_t validation_base =
      reinterpret_cast<uintptr_t>(&finalizeShadowCode) & kPageMask;
  if (auto err = code->relocateToBase(validation_base); err != kErrorOk) {
    throw std::runtime_error{fmt::format(
        "Failed to relocate shadow code for {}, got error code {}",
        name,
        DebugUtils::errorAsString(err))};
  }
  size_t code_size = 0;
  for (Section* section : code->sections()) {
    code_size += section->realSize();
  }
  return code_size;
}

// Emit machine code from LIR blocks by translating each instruction via the
// AutoTranslator.  Populates env->block_label_map and records annotations.
//
// When |code| and |metadata| are non-null, CodeSectionOverride is applied per
// block (for multi-section support in normal JIT functions).  When they are
// null the section override is skipped (standalone trampolines).
void flushAarch64NearDeoptBranches(Environ* env, bool has_fallthrough) {
#if defined(CINDER_AARCH64)
  if (env->aarch64_near_deopt_branches.empty()) {
    return;
  }

  auto* as = env->as;
  Label continuation;
  if (has_fallthrough) {
    continuation = as->newLabel();
    as->b(continuation);
  }

  auto cursor = as->cursor();
  for (const auto& branch : env->aarch64_near_deopt_branches) {
    as->bind(branch.near_label);
    as->b(branch.deopt_label);
  }
  env->aarch64_near_deopt_branches.clear();
  env->addAnnotation("AArch64 near deopt branches", cursor);

  if (has_fallthrough) {
    as->bind(continuation);
  }
#else
  (void)env;
  (void)has_fallthrough;
#endif
}

void emitLIRBlocks(
    Environ* env,
    lir::Function* lir_func,
    const asmjit::CodeHolder* code = nullptr,
    CodeHolderMetadata* metadata = nullptr) {
  auto* as = env->as;
  auto& blocks = lir_func->basicblocks();

  for (auto& basicblock : blocks) {
    env->block_label_map.emplace(basicblock, as->newLabel());
  }

  std::string pending_annotation;
  asmjit::BaseNode* annotation_cursor = nullptr;

  for (size_t i = 0; i < blocks.size(); ++i) {
    lir::BasicBlock* basicblock = blocks[i];
    // Optional section override for multi-section code layout.
    std::optional<CodeSectionOverride> section_override;
    if (code != nullptr && metadata != nullptr) {
      section_override.emplace(as, code, metadata, basicblock->section());
    }

    as->bind(env->block_label_map[basicblock]);
    for (auto& instr : basicblock->instructions()) {
      asmjit::BaseNode* cursor = as->cursor();

      // Check for annotation BEFORE translating so cursor captures the
      // position before the instruction's code is emitted.
      auto* annot_text = lir_func->getAnnotation(instr.get());
      if (annot_text) {
        // Close any previous pending annotation.
        if (!pending_annotation.empty()) {
          JIT_DCHECK(annotation_cursor != nullptr, "should be set");
          env->addAnnotation(std::move(pending_annotation), annotation_cursor);
        }
        // Start new annotation range from current cursor position.
        pending_annotation = *annot_text;
        annotation_cursor = cursor;
      }

      env->suppress_annotations = !pending_annotation.empty();
      autogen::AutoTranslator::getInstance().translateInstr(env, instr.get());
      env->suppress_annotations = false;

      if (!pending_annotation.empty()) {
        // Under an active annotation — don't emit per-instruction annotations.
      } else if (instr->origin() != nullptr) {
        env->addAnnotation(instr.get(), cursor);
      }
    }
    // Close pending annotation at block boundary.
    if (!pending_annotation.empty()) {
      env->addAnnotation(std::move(pending_annotation), annotation_cursor);
      pending_annotation.clear();
    }

    bool has_fallthrough = false;
    if (i + 1 < blocks.size() &&
        blocks[i + 1]->section() == basicblock->section()) {
      auto& successors = basicblock->successors();
      has_fallthrough =
          std::find(successors.begin(), successors.end(), blocks[i + 1]) !=
          successors.end();
    }
    flushAarch64NearDeoptBranches(env, has_fallthrough);
  }
}

// Emit LIR blocks to machine code, finalize, register debug/perf symbols, and
// return the entry address.  Shared by all standalone trampoline generators.
static void* emitAndRegisterTrampoline(
    lir::Function* lir_func,
    const char* name) {
  auto mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    throw std::runtime_error{
        fmt::format("CinderX not initialized, cannot generate {}", name)};
  }

  CodeHolder code;
  ICodeAllocator* code_allocator = mod_state->code_allocator.get();
  ASM_CHECK(code.init(code_allocator->asmJitEnvironment()), name);
  arch::Builder a(&code);

  Environ env;
  env.as = &a;
  emitLIRBlocks(&env, lir_func);

  void* result = finalizeCode(a, name);
  JIT_LOGIF(
      getConfig().log.dump_asm,
      "Disassembly for {}\n{}",
      name,
      env.annotations.disassemble(result, code));

  auto code_size = code.codeSize();
  register_raw_debug_symbol(name, __FILE__, __LINE__, result, code_size, 0);

  std::vector<std::pair<void*, std::size_t>> code_sections;
  populateCodeSections(code_sections, code, result);
  code_sections.emplace_back(result, code_size);
#ifndef WIN32
  perf::registerFunction(code_sections, name);
#endif
  return result;
}

void* generateDeoptTrampoline(bool generator_mode) {
  lir::Function lir_func;
  lir::GenerateDeoptTrampolineBlocks(
      &lir_func,
      generator_mode,
      reinterpret_cast<void*>(prepareForDeopt),
      reinterpret_cast<void*>(resumeInInterpreter));

  return emitAndRegisterTrampoline(
      &lir_func,
      generator_mode ? "deopt_trampoline_generators" : "deopt_trampoline");
}

void* generateFailedDeferredCompileTrampoline() {
  lir::Function lir_func;
  lir::GenerateFailedDeferredCompileBlocks(
      &lir_func, reinterpret_cast<void*>(JITRT_FailedDeferredCompileShim));

  return emitAndRegisterTrampoline(
      &lir_func, "failedDeferredCompileTrampoline");
}

class AsmJitException : public std::exception {
 public:
  AsmJitException(Error err, std::string expr, std::string message) noexcept
      : err(err), expr(std::move(expr)), message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }

  Error const err;
  std::string const expr;
  std::string const message;
};

class ThrowableErrorHandler : public ErrorHandler {
 public:
  void handleError(Error err, const char* message, BaseEmitter*) override {
    throw AsmJitException(err, "<unknown>", message);
  }
};

} // namespace

NativeGenerator::NativeGenerator(
    const hir::Function* func,
    NativeGeneratorFactory& factory)
    : func_{func},
      inline_stack_size_{calcInlineStackSize(func)},
      factory_(factory) {
  env_.has_inlined_functions = inline_stack_size_ > 0;
}

#ifdef __ASM_DEBUG
extern "C" void ___debug_helper(const char* name) {
  fprintf(stderr, "Entering %s...\n", name);
}
#endif

PhyLocation get_arg_location_phy_location(int arg) {
  if (static_cast<size_t>(arg) < ARGUMENT_REGS.size()) {
    return ARGUMENT_REGS[arg];
  }

  JIT_ABORT("only six first registers should be used");
  return 0;
}

std::span<const std::byte> NativeGenerator::getCodeBuffer() const {
  return std::span{
      reinterpret_cast<const std::byte*>(code_start_), compiled_size_};
}

void* NativeGenerator::getVectorcallEntry() {
  return compile(false);
}

size_t NativeGenerator::getShadowCodeSize() {
  JIT_CHECK(
      code_start_ == nullptr && vectorcall_entry_ == nullptr,
      "NativeGenerator cannot be reused for shadow compilation");
  if (GetFunction() == nullptr) {
    return 0;
  }
  compile(true);
  return compiled_size_;
}

void* NativeGenerator::compile(bool shadow) {
  if (vectorcall_entry_ != nullptr) {
    // already compiled
    return vectorcall_entry_;
  }

  JIT_CHECK(as_ == nullptr, "Builder should not have been initialized.");

  is_shadow_compile_ = shadow;
  CodeHolder code;
  ICodeAllocator* code_allocator =
      cinderx::getModuleState()->code_allocator.get();
  if (shadow) {
    ASM_CHECK_THROW(code.init(code_allocator->asmJitEnvironment()));
  } else {
    code.init(code_allocator->asmJitEnvironment());
  }
  ThrowableErrorHandler eh;
  code.setErrorHandler(&eh);

  if (getConfig().multiple_code_sections) {
    Section* cold_text;
    ASM_CHECK_THROW(code.newSection(
        &cold_text,
        codeSectionName(CodeSection::kCold),
        SIZE_MAX,
        code.textSection()->flags(),
        code.textSection()->alignment()));
  }

  as_ = new arch::Builder(&code);

  env_.as = as_;
  env_.hard_exit_label = as_->newLabel();
  env_.gen_resume_entry_label = as_->newLabel();

  // Prepare the location for where our arguments will go.  This just
  // uses general purpose registers while available for non-floating
  // point values, and floating point values while available for fp
  // arguments.
  const std::vector<TypedArgument>& checks = GetFunction()->typed_args;

  // gp_index starts at 1 because the first argument is reserved for the
  // function
  for (size_t i = 0, check_index = 0, gp_index = 1, fp_index = 0;
       i < static_cast<size_t>(GetFunction()->numArgs());
       i++) {
    auto add_gp = [&]() {
      if (gp_index < ARGUMENT_REGS.size()) {
        env_.arg_locations.push_back(ARGUMENT_REGS[gp_index++]);
      } else {
        env_.arg_locations.emplace_back(PhyLocation::REG_INVALID);
      }
    };

    if (check_index < checks.size() &&
        checks[check_index].locals_idx == static_cast<int>(i)) {
      if (checks[check_index].jit_type <= TCDouble) {
        if (fp_index < FP_ARGUMENT_REGS.size()) {
          env_.arg_locations.push_back(FP_ARGUMENT_REGS[fp_index++]);
        } else {
          // The register will come in on the stack, and the backend
          // will access it via __asm_extra_args.
          env_.arg_locations.emplace_back(PhyLocation::REG_INVALID);
        }
      } else {
        add_gp();
      }
      check_index++;
      continue;
    }

    add_gp();
  }

  auto func = GetFunction();

  // Inline caches emitted during LIR generation must have the same lifetime as
  // the discarded shadow artifact. Declare this before shadow_code_runtime so
  // the CodeRuntime is torn down first.
  std::unique_ptr<Context> shadow_context;
  std::unique_ptr<CodeRuntime> shadow_code_runtime;
  if (shadow) {
    shadow_context = std::make_unique<Context>();
    shadow_code_runtime = std::make_unique<CodeRuntime>(
        func->code.get(), func->builtins.get(), func->globals.get());
    env_.ctx = shadow_context.get();
    env_.code_rt = shadow_code_runtime.get();
  } else {
    env_.ctx = getContext();
    env_.code_rt = env_.ctx->allocateCodeRuntime(
        func->code.get(), func->builtins.get(), func->globals.get());
  }
  SCOPE_EXIT(if (shadow) {
    if (env_.code_rt != nullptr) {
      env_.code_rt->releaseReferences();
    }
    lir_func_.reset();
    env_.function_indirections.clear();
    env_.code_rt = nullptr;
    env_.ctx = nullptr;
  });
#if defined(ENABLE_LIGHTWEIGHT_FRAMES) && PY_VERSION_HEX >= 0x030E0000
  if (func->frameMode == FrameMode::kLightweight) {
    env_.code_rt->setReifier(func->reifier);
  }
#endif

  for (auto& ref : func->env.references()) {
    env_.code_rt->addReference(ref);
  }

  lir::LIRGenerator lirgen(GetFunction(), &env_);
  std::unique_ptr<lir::Function> lir_func;

#if defined(CINDER_X86_64) && defined(_WIN32)
  {
    int fh_size = jit::frameHeaderSize(func_->code) + sizeof(void*);
    env_.win_struct_ret_offset = -(fh_size + inline_stack_size_ + 16);
  }
#endif

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Lowering into LIR",
      lir_func = lirgen.TranslateFunction())

  JIT_LOGIF(
      getConfig().log.dump_lir,
      "LIR for {} after generation:\n{}",
      GetFunction()->fullname,
      *lir_func);

  PostGenerationRewrite post_gen(lir_func.get(), &env_);
  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "LIR transformations",
      post_gen.run())

  JIT_LOGIF(
      getConfig().log.dump_lir,
      "LIR for {} after postgen rewrites:\n{}",
      GetFunction()->fullname,
      *lir_func);

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "DeadCodeElimination",
      eliminateDeadCode(lir_func.get()))

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Target Selection and Legalization",
      selectTargetOpcodes(lir_func.get()))

  JIT_LOGIF(
      getConfig().log.dump_lir,
      "LIR for {} after target selection and legalization:\n{}",
      GetFunction()->fullname,
      *lir_func);

  int frame_header_size = frameHeaderSize(func_->code);
  frame_header_size += sizeof(void*);

  int reserved_stack_space = frame_header_size + inline_stack_size_;
#if defined(CINDER_X86_64) && defined(_WIN32)
  reserved_stack_space += 16;
#endif

  LinearScanAllocator lsalloc(lir_func.get(), reserved_stack_space);

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Register Allocation",
      lsalloc.run())

  env_.shadow_frames_and_spill_size = lsalloc.getFrameSize();
  env_.changed_regs = lsalloc.getChangedRegs();
  env_.exit_label = as_->newLabel();
  env_.frame_mode = GetFunction()->frameMode;
  if (GetFunction()->code->co_flags & kCoFlagsAnyGenerator) {
    env_.initial_yield_spill_size_ = lsalloc.initialYieldSpillSize();
  }
  env_.can_deopt = GetFunction()->canDeopt();

  JIT_LOGIF(
      getConfig().log.dump_lir,
      "LIR for {} after register allocation:\n{}",
      GetFunction()->fullname,
      *lir_func);

  PostRegAllocRewrite post_rewrite(lir_func.get(), &env_);
  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Post Reg Alloc Rewrite",
      post_rewrite.run())

  JIT_LOGIF(
      getConfig().log.dump_lir,
      "LIR for {} after postalloc rewrites:\n{}",
      GetFunction()->fullname,
      *lir_func);

  std::ostringstream verifier_errors;
  if (!verifyPostRegAllocInvariants(lir_func.get(), verifier_errors)) {
    if (shadow) {
      throw std::runtime_error{fmt::format(
          "LIR for {} failed verification:\n{}\n{}",
          GetFunction()->fullname,
          verifier_errors.str(),
          *lir_func)};
    }
    JIT_ABORT(
        "LIR for {} failed verification:\n{}\n{}",
        GetFunction()->fullname,
        verifier_errors.str(),
        *lir_func);
  }

  lir_func_ = std::move(lir_func);

  try {
    COMPILE_TIMER(
        GetFunction()->compilation_phase_timer,
        "Code Generation",
        generateCode(code, lirgen.frameSetupBlock()))
  } catch (const AsmJitException& ex) {
    String s;
    FormatOptions formatOptions;
    formatOptions.setFlags(FormatFlags::kHexImms);
    Formatter::formatNodeList(s, formatOptions, as_);
    if (shadow) {
      throw std::runtime_error{fmt::format(
          "Failed to emit shadow code for '{}': '{}' failed with '{}'; "
          "builder contents on failure:\n{}",
          GetFunction()->fullname,
          ex.expr,
          ex.message,
          s.data())};
    }
    JIT_ABORT(
        "Failed to emit code for '{}': '{}' failed with '{}'\n\n"
        "Builder contents on failure:\n{}",
        GetFunction()->fullname,
        ex.expr,
        ex.message,
        s.data());
  }

  /* After code generation CodeHolder->codeSize() *should* return the actual
   * size of the generated code. This relies on the implementation of
   * JitRuntime::_add and may break in the future.
   */

  JIT_DCHECK(
      is_shadow_compile_ || code.codeSize() < INT_MAX,
      "Code size is larger than INT_MAX");
  if (!is_shadow_compile_) {
    compiled_size_ = code.codeSize();
  }
  env_.code_rt->setFrameSize(env_.stack_frame_size);
  if (GetFunction()->code->co_flags & kCoFlagsAnyGenerator) {
    JIT_DCHECK(
        env_.shadow_frames_and_spill_size % kPointerSize == 0,
        "Bad spill alignment");
    env_.code_rt->setSpillWords(
        env_.shadow_frames_and_spill_size / kPointerSize);
  }
  return shadow ? nullptr : vectorcall_entry_;
}

void* NativeGenerator::getStaticEntry() {
  if (!hasStaticEntry()) {
    return nullptr;
  }
  // Force compile, if needed.
  getVectorcallEntry();
  return reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(vectorcall_entry_) +
      JITRT_STATIC_ENTRY_OFFSET);
}

int NativeGenerator::GetCompiledFunctionStackSize() const {
  return env_.stack_frame_size;
}

int NativeGenerator::GetCompiledFunctionSpillStackSize() const {
  return spill_stack_size_;
}

void NativeGenerator::generateFunctionEntry() {
#if defined(CINDER_X86_64)
  as_->push(x86::rbp);
  as_->mov(x86::rbp, x86::rsp);
#elif defined(CINDER_AARCH64)
  as_->stp(arch::fp, arch::lr, a64::ptr_pre(a64::sp, -arch::kFrameRecordSize));
  as_->mov(arch::fp, a64::sp);
#else
  CINDER_UNSUPPORTED
#endif
}

void NativeGenerator::generateFunctionExit() {
#if defined(CINDER_X86_64)
  as_->leave();
  as_->ret();
#elif defined(CINDER_AARCH64)
  as_->mov(a64::sp, arch::fp);
  as_->ldp(arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
  as_->ret(arch::lr);
#else
  CINDER_UNSUPPORTED
#endif
}

#if defined(CINDER_AARCH64)

namespace {

uint64_t osrStackRefObjectMask() {
#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_TAG_REFCNT)
  return ~static_cast<uint64_t>(Py_TAG_REFCNT);
#else
  return ~uint64_t{0};
#endif
}

uint64_t osrStackRefNullBits() {
#if PY_VERSION_HEX >= 0x030E0000 && !defined(Py_STACKREF_DEBUG)
  return PyStackRef_NULL.bits;
#else
  return 0;
#endif
}

constexpr int osrStackSlotSize() {
#if PY_VERSION_HEX >= 0x030E0000
  return sizeof(_PyStackRef);
#else
  return sizeof(PyObject*);
#endif
}

int32_t osrLocalsplusOffset(int localsplus_index) {
  return static_cast<int32_t>(
      offsetof(_PyInterpreterFrame, localsplus) +
      localsplus_index * osrStackSlotSize());
}

} // namespace

void NativeGenerator::emitOSRFrameSetup(const jit::OSRMetadata& metadata) {
  generateFunctionEntry();
  arch::sub_immediate(
      as_,
      a64::sp,
      a64::sp,
      static_cast<uint64_t>(metadata.resume_frame_total_size));

  auto gp_regs = metadata.resume_saved_regs & ALL_GP_REGISTERS;
  auto vecd_regs = metadata.resume_saved_regs & ALL_VECD_REGISTERS;

  arch::sub_immediate(
      as_,
      a64::x9,
      arch::fp,
      static_cast<uint64_t>(metadata.resume_header_and_spill_size));

  int reg_offset = 0;
  if (!gp_regs.Empty()) {
    if (gp_regs.count() % 2 == 1) {
      as_->str(
          a64::x(gp_regs.GetFirst().loc),
          a64::ptr(a64::x9, -(reg_offset + 16)));
      gp_regs.RemoveFirst();
      reg_offset += 16;
    }
    while (!gp_regs.Empty()) {
      auto first = a64::x(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
      auto second = a64::x(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
      as_->stp(first, second, a64::ptr(a64::x9, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
  if (!vecd_regs.Empty()) {
    if (vecd_regs.count() % 2 == 1) {
      as_->str(
          a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
          a64::ptr(a64::x9, -(reg_offset + 16)));
      vecd_regs.RemoveFirst();
      reg_offset += 16;
    }
    while (!vecd_regs.Empty()) {
      auto first = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
      vecd_regs.RemoveFirst();
      auto second = a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE);
      vecd_regs.RemoveFirst();
      as_->stp(first, second, a64::ptr(a64::x9, -(reg_offset + 16)));
      reg_offset += 16;
    }
  }
}

void NativeGenerator::moveOSRValueToLocation(
    PhyLocation destination,
    asmjit::a64::Gp value,
    asmjit::a64::Gp address_scratch) {
  if (destination.loc == PhyLocation::REG_INVALID) {
    JIT_ABORT("OSR destination location is invalid");
  }
  if (destination.is_gp_register()) {
    as_->mov(a64::x(destination.loc), value);
    return;
  }
  JIT_CHECK(destination.is_memory(), "OSR destination must be GP or stack");
  as_->str(
      value,
      arch::ptr_resolve(as_, arch::fp, destination.loc, address_scratch));
}

void NativeGenerator::generateOSREntryStub(
    jit::OSRMetadata& metadata,
    lir::BasicBlock* target_block) {
  const auto kStateReg = a64::x0;
  const auto kFrameReg = a64::x9;
  const auto kValueReg = a64::x12;

  emitOSRFrameSetup(metadata);

  as_->ldr(kValueReg, a64::ptr(kStateReg, offsetof(jit::OSRState, tstate)));
  moveOSRValueToLocation(metadata.tstate_location, kValueReg, kFrameReg);

  as_->ldr(kFrameReg, a64::ptr(kStateReg, offsetof(jit::OSRState, frame)));
  moveOSRValueToLocation(metadata.frame_location, kFrameReg, kValueReg);

  as_->ldr(
      kValueReg, a64::ptr(kFrameReg, offsetof(_PyInterpreterFrame, f_funcobj)));
#if PY_VERSION_HEX >= 0x030E0000
  as_->and_(kValueReg, kValueReg, osrStackRefObjectMask());
#endif
  moveOSRValueToLocation(metadata.func_location, kValueReg, kFrameReg);

  for (const jit::OSRLiveIn& live_in : metadata.live_ins) {
    JIT_CHECK(
        live_in.localsplus_index >= 0,
        "OSR live-in must have a localsplus source");
    int32_t slot_offset = osrLocalsplusOffset(live_in.localsplus_index);

    as_->ldr(kFrameReg, a64::ptr(kStateReg, offsetof(jit::OSRState, frame)));
    as_->ldr(kValueReg, a64::ptr(kFrameReg, slot_offset));
#if PY_VERSION_HEX >= 0x030E0000
    as_->and_(kValueReg, kValueReg, osrStackRefObjectMask());
#endif
    moveOSRValueToLocation(live_in.destination, kValueReg, kFrameReg);

    as_->ldr(kFrameReg, a64::ptr(kStateReg, offsetof(jit::OSRState, frame)));
    as_->mov(kValueReg, osrStackRefNullBits());
    as_->str(kValueReg, a64::ptr(kFrameReg, slot_offset));
  }

  as_->b(map_get(env_.block_label_map, target_block));
}

#endif

NativeGenerator::FrameInfo NativeGenerator::computeFrameInfo() {
  // During execution, the stack looks like the diagram below. The column to
  // left indicates how many words on the stack each line occupies.
  //
  // Legend:
  //  - <empty> - 1 word
  //  - N       - A fixed number of words > 1
  //  - *       - 0 or more words
  //  - ?       - 0 or 1 words
  //
  // +-----------------------+
  // | * memory arguments    |
  // |   return address      |
  // |   saved rbp           | <-- rbp
  // | N frame header        | See frame.h
  // | * inl. shad. frame 0  |
  // | * inl. shad. frame 1  |
  // | * inl. shad. frame .  |
  // | * inl. shad. frame N  |
  // | * spilled values      |
  // | ? alignment padding   |
  // | * callee-saved regs   |
  // | ? call arg buffer     | <-- rsp
  // +-----------------------+
  FrameInfo info{
      // The frame header size and inlined shadow frames are already included in
      // env_.shadow_frames_and_spill_size.
      // Make sure we have at least one word for scratch in the epilogue.
      .header_and_spill_size =
          std::max(env_.shadow_frames_and_spill_size, kPointerSize),
      .saved_regs = env_.changed_regs & CALLEE_SAVE_REGS,
      .arg_buffer_size = env_.max_arg_buffer_size + env_.reserve_stack_size,
  };
  if ((info.header_and_spill_size + info.saved_regs_size() +
       info.arg_buffer_size) %
      kStackAlign) {
    info.header_and_spill_size += kPointerSize;
  }
  spill_stack_size_ = env_.shadow_frames_and_spill_size;
  env_.last_callee_saved_reg_off =
      info.header_and_spill_size + info.saved_regs_size();
  env_.stack_frame_size = info.size();
  return info;
}

arch::Gp get_arg_location(int arg) {
#if defined(CINDER_X86_64)
  auto phyloc = get_arg_location_phy_location(arg);

  if (phyloc.is_register()) {
    return x86::gpq(phyloc.loc);
  }
#elif defined(CINDER_AARCH64)
  auto phyloc = get_arg_location_phy_location(arg);

  if (phyloc.is_register()) {
    return a64::x(phyloc.loc);
  }
#else
  CINDER_UNSUPPORTED
#endif

  JIT_ABORT("should only be used with first six args");
}

void NativeGenerator::generateDeoptExits(const asmjit::CodeHolder& code) {
  if (env_.deopt_exits.empty()) {
    return;
  }

#if defined(CINDER_X86_64)
  // Always place the deopt exit call to the cold section, and revert to the
  // previous section at the end of this scope.
  CodeSectionOverride override{as_, &code, &metadata_, CodeSection::kCold};

  auto& deopt_exits = env_.deopt_exits;

  auto deopt_cursor = as_->cursor();
  auto deopt_exit = as_->newLabel();
  std::sort(deopt_exits.begin(), deopt_exits.end(), [](auto& a, auto& b) {
    return a.deopt_meta_index < b.deopt_meta_index;
  });
  // Generate stage 1 trampolines (one per guard). These push the index of the
  // appropriate `DeoptMetadata` and then jump to the stage 2 trampoline.
  for (const auto& exit : deopt_exits) {
    as_->bind(exit.label);
    as_->push(exit.deopt_meta_index);
    emitCall(env_, deopt_exit, exit.instr);
  }
  // Generate the stage 2 trampoline (one per function). This saves the address
  // of the final part of the JIT-epilogue that is responsible for restoring
  // callee-saved registers and returning, our scratch register, whose original
  // contents may be needed during frame reification, and jumps to the final
  // trampoline.
  //
  // Right now the top of the stack looks like:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // +-------------------------+
  //
  // and we need to pass our scratch register and the address of the epilogue
  // to the global deopt trampoline. The code below leaves the stack with the
  // following layout:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // | padding                 |
  // | padding                 |
  // | address of CodeRuntime  |
  // | address of epilogue     |
  // | r15                     |
  // +-------------------------+
  //
  // The global deopt trampoline expects that our scratch register is at the
  // top of the stack so that it can save the remaining registers immediately
  // after it, forming a contiguous array of all registers.
  //
  // If you change this make sure you update that code!
  as_->bind(deopt_exit);

  // Two slots for padding.  One of them will get the deopt metadata index
  // shuffled in, making space to save RBP before calling prepareForDeopt.
  as_->push(deopt_scratch_reg);
  as_->push(deopt_scratch_reg);

  // Save space for the CodeRuntime.
  as_->push(deopt_scratch_reg);

  // Save space for the epilogue.
  as_->push(deopt_scratch_reg);

  // Save our scratch register.
  as_->push(deopt_scratch_reg);

  // Save the address of the CodeRuntime.
  as_->mov(deopt_scratch_reg, reinterpret_cast<uintptr_t>(env_.code_rt));
  as_->mov(x86::ptr(x86::rsp, kPointerSize * 2), deopt_scratch_reg);

  // Save the address of the epilogue.
  as_->lea(deopt_scratch_reg, x86::ptr(env_.hard_exit_label));
  as_->mov(x86::ptr(x86::rsp, kPointerSize), deopt_scratch_reg);

  void* trampoline = reinterpret_cast<void*>(1);
  if (!is_shadow_compile_) {
    trampoline = GetFunction()->code->co_flags & kCoFlagsAnyGenerator
        ? factory_.deoptTrampolineGenerators()
        : factory_.deoptTrampoline();
  }
  as_->mov(deopt_scratch_reg, reinterpret_cast<uint64_t>(trampoline));
  as_->jmp(deopt_scratch_reg);

  env_.addAnnotation("Deoptimization exits", deopt_cursor);
#elif defined(CINDER_AARCH64)
  // Always place the deopt exit call to the cold section, and revert to the
  // previous section at the end of this scope.
  CodeSectionOverride override{as_, &code, &metadata_, CodeSection::kCold};

  auto& deopt_exits = env_.deopt_exits;

  auto deopt_cursor = as_->cursor();
  auto deopt_exit = as_->newLabel();
  std::sort(deopt_exits.begin(), deopt_exits.end(), [](auto& a, auto& b) {
    return a.deopt_meta_index < b.deopt_meta_index;
  });

  // Generate stage 1 trampolines (one per guard). These push the index of the
  // appropriate `DeoptMetadata` and then jump to the stage 2 trampoline.
  for (const auto& exit : deopt_exits) {
    // On x86-64, when you emit a call instruction to a label, it pushes the
    // return address onto the stack. In order to replicate that behavior on
    // aarch64, we manually add a label and use adr to determine its offset.
    auto after = as_->newLabel();

    as_->bind(exit.label);
    as_->mov(arch::reg_scratch_0, exit.deopt_meta_index);
    as_->adr(arch::reg_scratch_1, after);
    as_->stp(
        arch::reg_scratch_1, arch::reg_scratch_0, a64::ptr_pre(a64::sp, -16));
    emitCall(env_, deopt_exit, exit.instr);
    as_->bind(after);
  }
  // Generate the stage 2 trampoline (one per function). This saves the address
  // of the final part of the JIT-epilogue that is responsible for restoring
  // callee-saved registers and returning, our scratch register, whose original
  // contents may be needed during frame reification, and jumps to the final
  // trampoline.
  //
  // Right now the top of the stack looks like:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved pc                | <-- sp
  // +-------------------------+
  //
  // and we need to pass our scratch register and the address of the epilogue
  // to the global deopt trampoline. The code below leaves the stack with the
  // following layout:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved pc                |
  // | padding (8 bytes)       |
  // | padding (8 bytes)       |
  // | address of CodeRuntime  |
  // | address of epilogue     |
  // | fp                      |
  // | x28                     | <-- sp
  // +-------------------------+
  //
  // The global deopt trampoline expects that our scratch register is at the
  // top of the stack so that it can save the remaining registers immediately
  // after it, forming a contiguous array of all registers.
  //
  // If you change this make sure you update that code!
  as_->bind(deopt_exit);

  // Two slots for padding, then the address of the CodeRuntime and the address
  // of the epilogue, then the first of the registers to get stored (fp and
  // x28). One of the slots of padding will get the deopt metadata index
  // shuffled in, making space to save fp before calling prepareForDeopt.
  as_->stp(deopt_scratch_reg, arch::fp, a64::ptr_pre(a64::sp, -0x30));

  // Save the address of the CodeRuntime.
  as_->mov(deopt_scratch_reg, reinterpret_cast<uintptr_t>(env_.code_rt));
  as_->str(
      deopt_scratch_reg,
      arch::ptr_resolve(as_, a64::sp, kPointerSize * 3, arch::reg_scratch_0));

  // Save the address of the epilogue.
  as_->adr(deopt_scratch_reg, env_.hard_exit_label);
  as_->str(
      deopt_scratch_reg,
      arch::ptr_resolve(as_, a64::sp, kPointerSize * 2, arch::reg_scratch_0));

  void* trampoline = reinterpret_cast<void*>(1);
  if (!is_shadow_compile_) {
    trampoline = GetFunction()->code->co_flags & kCoFlagsAnyGenerator
        ? factory_.deoptTrampolineGenerators()
        : factory_.deoptTrampoline();
  }
  as_->mov(deopt_scratch_reg, trampoline);
  as_->br(deopt_scratch_reg);

  env_.addAnnotation("Deoptimization exits", deopt_cursor);
#else
  CINDER_UNSUPPORTED
#endif
}

void NativeGenerator::emitAarch64LoadAttrInvokeStub(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && PY_VERSION_HEX >= 0x030E0000 && \
    !defined(Py_GIL_DISABLED)
  if (!env_.load_attr_invoke_stub.isValid()) {
    return;
  }

  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};

  Label slow_path = as_->newLabel();
  Label return_value = as_->newLabel();
  Label incref_done = as_->newLabel();

  constexpr int kEntryTypeOffset = static_cast<int>(
      jit::AttributeCache::entriesOffset() +
      jit::AttributeMutator::typeOffset());
  constexpr int kValOffsetOffset = static_cast<int>(
      jit::AttributeCache::entriesOffset() +
      jit::AttributeMutator::splitValOffsetOffset());
  constexpr int kEntrySize = sizeof(jit::AttributeMutator);
  constexpr uint64_t kKindMask = jit::AttributeMutator::kindMask();
  constexpr uint64_t kSplitInlineKnownOffsetKind =
      jit::AttributeMutator::splitInlineKnownOffsetKind();
  constexpr int kObTypeOffset = offsetof(PyObject, ob_type);
  constexpr int kTpBasicSizeOffset = offsetof(PyTypeObject, tp_basicsize);
  constexpr int kInlineValuesValidOffset = offsetof(PyDictValues, valid);
  constexpr uint64_t kInlineValuesValuesOffset = offsetof(PyDictValues, values);
  constexpr int kRefcountOffset = offsetof(PyObject, ob_refcnt);

  if (is_shadow_compile_) {
    ASM_CHECK_THROW(as_->align(AlignMode::kCode, 8));
  } else {
    ASM_CHECK(as_->align(AlignMode::kCode, 8), GetFunction()->fullname);
  }
  as_->bind(env_.load_attr_invoke_stub);

  // x0=cache, x1=obj, x2=name
  as_->ldr(a64::x11, arch::ptr_offset(a64::x1, kObTypeOffset));

  auto emit_load_attr_entry = [&](uint32_t entry_index, Label next_entry) {
    const int entry_offset = static_cast<int>(entry_index) * kEntrySize;
    as_->ldr(
        a64::x12,
        arch::ptr_offset(a64::x0, kEntryTypeOffset + entry_offset));
    as_->cbz(a64::x12, next_entry);

    as_->mov(a64::x13, ~kKindMask);
    as_->and_(a64::x13, a64::x12, a64::x13);
    as_->cmp(a64::x13, a64::x11);
    as_->b_ne(next_entry);

    as_->and_(a64::x14, a64::x12, kKindMask);
    arch::cmp_immediate(as_, a64::x14, kSplitInlineKnownOffsetKind);
    as_->b_ne(slow_path);

    as_->ldr(a64::x14, arch::ptr_offset(a64::x13, kTpBasicSizeOffset));
    as_->add(a64::x15, a64::x1, a64::x14);
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(
            a64::x15, kInlineValuesValidOffset, arch::AccessSize::k8));
    as_->cbz(a64::w14, slow_path);

    as_->ldr(
        a64::x14,
        arch::ptr_offset(a64::x0, kValOffsetOffset + entry_offset));
    arch::add_immediate(as_, a64::x15, a64::x15, kInlineValuesValuesOffset);
    as_->add(a64::x15, a64::x15, a64::x14, a64::lsl(3));
    as_->ldr(a64::x9, a64::ptr(a64::x15));
    as_->cbz(a64::x9, slow_path);
    as_->b(return_value);
  };

  constexpr uint32_t kMaxStubEntries = 4;
  const uint32_t stub_entries =
      std::min<uint32_t>(jit::getConfig().attr_cache_size, kMaxStubEntries);
  if (stub_entries == 0) {
    as_->b(slow_path);
  }
  for (uint32_t entry_index = 0; entry_index < stub_entries; ++entry_index) {
    const bool has_next_entry = entry_index + 1 != stub_entries;
    Label next_entry = has_next_entry ? as_->newLabel() : slow_path;
    emit_load_attr_entry(entry_index, next_entry);
    if (has_next_entry) {
      as_->bind(next_entry);
    }
  }

  as_->bind(return_value);
  as_->ldr(
      a64::w12,
      arch::ptr_offset(a64::x9, kRefcountOffset, arch::AccessSize::k32));
  as_->adds(a64::w12, a64::w12, 1);
  as_->b_mi(incref_done);
  as_->str(
      a64::w12,
      arch::ptr_offset(a64::x9, kRefcountOffset, arch::AccessSize::k32));
  as_->bind(incref_done);
  as_->mov(a64::x0, a64::x9);
  as_->ret(arch::lr);

  as_->bind(slow_path);
  as_->mov(
      arch::reg_scratch_br,
      reinterpret_cast<uint64_t>(jit::LoadAttrCache::invoke));
  as_->br(arch::reg_scratch_br);
#else
  (void)code;
#endif
}

void NativeGenerator::emitAarch64ExactLongAddSubStubs(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && PY_VERSION_HEX >= 0x030E0000 &&  \
    PY_VERSION_HEX < 0x030F0000 && !defined(Py_GIL_DISABLED) && \
    !defined(Py_REF_DEBUG) && !defined(Py_STATS)
  if (env_.exact_long_add_sub_stubs.empty()) {
    return;
  }

  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};
  constexpr int kObTypeOffset = offsetof(PyObject, ob_type);
  const uint64_t exact_long_type = reinterpret_cast<uint64_t>(&PyLong_Type);

  for (const auto& stub : env_.exact_long_add_sub_stubs) {
    Label generic_path = as_->newLabel();

    if (is_shadow_compile_) {
      ASM_CHECK_THROW(as_->align(AlignMode::kCode, 8));
    } else {
      ASM_CHECK(as_->align(AlignMode::kCode, 8), GetFunction()->fullname);
    }
    as_->bind(stub.entry);

    // x0=left, x1=right. Exact checks exclude bool and int subclasses, whose
    // reflected methods remain observable through PyNumber_Add/Subtract.
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, kObTypeOffset));
    as_->mov(a64::x10, exact_long_type);
    as_->cmp(a64::x9, a64::x10);
    as_->b_ne(generic_path);
    as_->ldr(a64::x9, arch::ptr_offset(a64::x1, kObTypeOffset));
    as_->cmp(a64::x9, a64::x10);
    as_->b_ne(generic_path);

    // BR preserves the LR set by the JIT's BL to this stub, so either helper
    // returns directly to the original post-call guard/callsite.
    as_->mov(arch::reg_scratch_br, stub.exact_target);
    as_->br(arch::reg_scratch_br);

    as_->bind(generic_path);
    as_->mov(arch::reg_scratch_br, stub.generic_target);
    as_->br(arch::reg_scratch_br);
  }
#else
  (void)code;
#endif
}

void NativeGenerator::linkDeoptPatchers(const asmjit::CodeHolder& code) {
  JIT_CHECK(code.hasBaseAddress(), "code not generated!");
  uint64_t base = code.baseAddress();
  for (const auto& udp : env_.pending_deopt_patchers) {
    uint64_t patchpoint = base + code.labelOffsetFromBase(udp.patchpoint);
    uint64_t deopt_exit = base + code.labelOffsetFromBase(udp.deopt_exit);
    udp.patcher->linkJump(patchpoint, deopt_exit);

    // Register patcher with the runtime if it is type-based.
    if (auto typed_patcher = dynamic_cast<TypeDeoptPatcher*>(udp.patcher)) {
      env_.ctx->watchType(typed_patcher->type(), typed_patcher);
    }
  }

  // Any patchers that aren't linked at this point are pointing to patch points
  // that were optimized out.  It's safe to delete them.
  std::erase_if(
      const_cast<hir::Function*>(func_)->code_patchers,
      [](std::unique_ptr<CodePatcher>& patcher) {
        return !patcher->isLinked();
      });
}

Py_ssize_t NativeGenerator::giJITDataOffset() {
  Py_ssize_t python_frame_slots =
      _PyFrame_NumSlotsForCodeObject(GetFunction()->code);
  return _PyObject_VAR_SIZE(
      cinderx::getModuleState()->gen_type, python_frame_slots);
}

void NativeGenerator::generateStaticEntryPoint(
    Label finish_frame_setup,
    Label static_jmp_location) {
#if defined(CINDER_X86_64)
  auto static_entry_cursor = as_->cursor();
  as_->bind(static_jmp_location);

  generateFunctionEntry();

  size_t total_args = (size_t)GetFunction()->numArgs();

  if (total_args + 1 > ARGUMENT_REGS.size()) {
    // Capture the extra args pointer from the stack. For generators on 3.12+
    // this must happen before frame linking replaces rbp.
    as_->lea(x86::r10, x86::ptr(x86::rbp, 16));
  } else {
    for (int i = 0; i < 4; i++) {
      as_->nop();
    }
  }

  as_->jmp(finish_frame_setup);
  env_.addAnnotation("StaticEntryPoint", static_entry_cursor);
#elif defined(CINDER_AARCH64)
  // Emit the static entry point inline at the fixed-offset location.
  auto static_entry_cursor = as_->cursor();
  as_->bind(static_jmp_location);

  generateFunctionEntry();

  size_t total_args = (size_t)GetFunction()->numArgs();

  if (total_args + 1 > ARGUMENT_REGS.size()) {
    // Capture the extra args pointer from the stack. For generators on 3.12+
    // this must happen before frame linking replaces fp.
    as_->add(a64::x10, arch::fp, arch::kFrameRecordSize);
  } else {
    as_->nop();
  }

  as_->b(finish_frame_setup);
  env_.addAnnotation("StaticEntryPoint", static_entry_cursor);
#else
  CINDER_UNSUPPORTED
#endif
}

bool NativeGenerator::hasStaticEntry() const {
  PyCodeObject* code = GetFunction()->code;
  return (code->co_flags & CI_CO_STATICALLY_COMPILED);
}

void NativeGenerator::generatePrologueBlocks(lir::BasicBlock* frameSetupBlock) {
  // Pre-assign finish_frame_setup to the frame setup block so
  // generateAssemblyBody binds it at the block's start.
  env_.block_label_map[frameSetupBlock] = env_.finish_frame_setup;

  // Build pre-body LIR blocks in assembly order. Each Generate* call appends
  // blocks to the end. The body blocks (from TranslateFunction) are already
  // at the front.
  //
  // We pre-allocate the entry block (for arg loading), remove it from the
  // list, generate all other prologue blocks (which append in the correct
  // order), then push the entry block back at the end — right before the
  // body blocks. A single rotate then moves the entire prologue prefix to
  // the front.
  //
  // Final block order:
  //   [wrapper?] [func_entry] [argcount/primitive] [typechecks?]
  //   [entry(args)] [body...] [exit] [resume?] [deopt_exits]
  auto& blocks = lir_func_->basicblocks();
  size_t body_end = blocks.size();

  auto* entry_block = lir_func_->allocateBasicBlock();
  blocks.pop_back();
  env_.block_label_map[entry_block] = env_.correct_arg_count;

  // Boxed-return wrapper (if needed) — comes first so the vectorcall entry
  // falls through to it.
  Label generic_entry;
  if (func_->returnsPrimitive()) {
    generic_entry = as_->newLabel();
    env_.wrapper_exit = as_->newLabel();
    lir::GenerateBoxedReturnWrapperBlocks(
        lir_func_.get(), func_->return_type, generic_entry, env_.wrapper_exit);
  }

  // Function entry prologue (push rbp / mov rbp, rsp).
  lir::GenerateFunctionEntryBlock(lir_func_.get());
  if (generic_entry.isValid()) {
    env_.block_label_map[lir_func_->basicblocks().back()] = generic_entry;
  }

  // Argcount check or primitive-args prologue. The correct-args path
  // branches to correct_arg_count, which is assigned below to either the
  // typecheck dispatch block (if type checks exist) or the entry block.
  env_.prologue_exit = as_->newLabel();
  if (func_->has_primitive_args) {
    BorrowedRef<_PyTypedArgsInfo> info = func_->prim_args_info;
    env_.code_rt->addReference(info);

    lir::GeneratePrimitiveArgsPrologueBlock(
        lir_func_.get(),
        reinterpret_cast<PyObject*>(info.get()),
        func_->returnsPrimitiveDouble(),
        env_.prologue_exit);
  } else {
    lir::GenerateArgcountCheckBlocks(
        lir_func_.get(),
        GetFunction(),
        env_.correct_arg_count,
        env_.prologue_exit);
  }

  // Static argument type checking (if applicable).
  if (hasStaticEntry() && !func_->has_primitive_args &&
      !GetFunction()->typed_args.empty()) {
    env_.static_arg_typecheck_failed_label = as_->newLabel();
    auto jt = lir::GenerateStaticTypeCheckBlocks(
        lir_func_.get(),
        entry_block,
        GetFunction()->typed_args,
        GetFunction()->numArgs(),
        env_.code_rt,
        env_.static_arg_typecheck_failed_label);
    env_.static_typecheck_table = jt.table;
    env_.static_typecheck_jt_entries = std::move(jt.entries);
    // Reassign correct_arg_count from entry_block to the dispatch block so
    // the argcount correct path and reentry go through type checks before
    // entry(args). entry_block gets a fresh label from emitLIRBlocks.
    env_.block_label_map.erase(entry_block);
    env_.block_label_map[jt.dispatch_block] = env_.correct_arg_count;
  }

  // Push entry_block back and populate it with arg loading.
  blocks.push_back(entry_block);
  lir::PopulateEntryBlock(entry_block, env_.arg_locations);

  // Move the prologue blocks from [body_end, end) to the front.
  std::rotate(blocks.begin(), blocks.begin() + body_end, blocks.end());
}

void NativeGenerator::generateCode(
    CodeHolder& codeholder,
    lir::BasicBlock* frameSetupBlock) {
  // computeFrameInfo() is called before generateAssemblyBody() so that
  // env_.last_callee_saved_reg_off is available to the exit block's custom
  // translators. All of computeFrameInfo()'s inputs
  // (shadow_frames_and_spill_size, changed_regs, max_arg_buffer_size) are set
  // during register allocation, which completes before generateCode() is
  // called.
  auto frame_info = computeFrameInfo();

  // Populate frame layout fields on Environ for the kSetupFrame autogen
  // translator. These are computed from FrameInfo after register allocation.
  env_.resume_frame_total_size = frame_info.size();
  env_.resume_header_and_spill_size = frame_info.header_and_spill_size;
  env_.resume_saved_regs = frame_info.saved_regs;
  for (std::size_t id = 0; id < env_.code_rt->osrMetadatas().size(); ++id) {
    OSRMetadata& metadata = env_.code_rt->getOSRMetadata(id);
    metadata.resume_frame_total_size = env_.resume_frame_total_size;
    metadata.resume_header_and_spill_size = env_.resume_header_and_spill_size;
    metadata.resume_saved_regs = env_.resume_saved_regs;
  }

  // --- Emit code in final assembly order ---
  //
  // The layout is:
  //   [static entry point]        (optional, at fixed negative offset)
  //   [reentry with processed args] (at fixed negative offset)
  //   [vectorcall entry label]
  //   [LIR blocks: wrapper → func_entry → argcount → typechecks → entry(args)
  //                → body → exit → resume → deopt_exits]
  //   [exit label]
  //   [typecheck failure stub]    (optional)
  //   [prologue exit stub]        (optional)
  //   [wrapper exit stub]         (optional)
  //   [aarch64 constant pool]     (optional)

  // Create labels used by both the raw-asm entry points and the LIR
  // prologue blocks. finish_frame_setup is bound by generatePrologueBlocks
  // to the frame setup block; correct_arg_count is bound to the entry block.
  env_.finish_frame_setup = as_->newLabel();
  env_.correct_arg_count = as_->newLabel();

  Label static_jmp_location = as_->newLabel();
  bool has_static_entry = hasStaticEntry();
  if (has_static_entry) {
    generateStaticEntryPoint(env_.finish_frame_setup, static_jmp_location);
  }

  // Reentry point: dispatched to from JITRT_CallWithIncorrectArgcount and
  // JITRT_CallWithKeywordArgs after argument binding. Must be exactly
  // JITRT_CALL_REENTRY_OFFSET bytes before the vectorcall entry.
  auto arg_reentry_cursor = as_->cursor();
  Label correct_args_entry = as_->newLabel();
  as_->bind(correct_args_entry);
  generateFunctionEntry();

#if defined(CINDER_X86_64)
  as_->short_().jmp(env_.correct_arg_count);
#elif defined(CINDER_AARCH64)
  as_->b(env_.correct_arg_count);
#else
  CINDER_UNSUPPORTED
#endif

  env_.addAnnotation("Reentry with processed args", arg_reentry_cursor);

  // Generate prologue LIR blocks (wrapper, func_entry, argcount, typechecks,
  // entry with arg loading) and prepend them to the body block list.
  generatePrologueBlocks(frameSetupBlock);

  // Vectorcall entry point. The prologue LIR blocks are first in the block
  // list, so the vectorcall entry falls through to them.
  Label vectorcall_entry_label = as_->newLabel();
  as_->bind(vectorcall_entry_label);

  // Append suffix blocks to the block list. These come after the body's exit
  // block but before code emission.
  if (GetFunction()->code->co_flags & kCoFlagsAnyGenerator) {
    env_.gi_jit_data_offset = giJITDataOffset();

    auto* bb = lir_func_->resumeEntryBlock();
    JIT_CHECK(
        bb != nullptr,
        "Generator must have a resume entry block {}",
        lir_func_->hirFunc()->fullname);
    lir::PopulateResumeEntryBlock(bb, env_.gi_jit_data_offset);
    env_.block_label_map[bb] = env_.gen_resume_entry_label;
    lir_func_->basicblocks().push_back(bb);
  }

  generateAssemblyBody(codeholder);

  as_->bind(env_.exit_label);

  if (env_.static_arg_typecheck_failed_label.isValid()) {
    auto static_typecheck_cursor = as_->cursor();
    as_->bind(env_.static_arg_typecheck_failed_label);

#if defined(CINDER_X86_64)
    if (GetFunction()->returnsPrimitive()) {
      if (GetFunction()->returnsPrimitiveDouble()) {
        as_->call(
            reinterpret_cast<uint64_t>(
                JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn));
      } else {
        as_->call(
            reinterpret_cast<uint64_t>(
                JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn));
      }
    } else {
      as_->call(
          reinterpret_cast<uint64_t>(JITRT_ReportStaticArgTypecheckErrors));
    }
    as_->leave();
    as_->ret();
#elif defined(CINDER_AARCH64)
    if (GetFunction()->returnsPrimitive()) {
      if (GetFunction()->returnsPrimitiveDouble()) {
        as_->bl(JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn);
      } else {
        as_->bl(JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn);
      }
    } else {
      as_->bl(JITRT_ReportStaticArgTypecheckErrors);
    }

    // leave + ret equivalent on aarch64
    as_->mov(a64::sp, arch::fp);
    as_->ldp(
        arch::fp, arch::lr, a64::ptr_post(a64::sp, arch::kFrameRecordSize));
    as_->ret(arch::lr);
#else
    CINDER_UNSUPPORTED
#endif

    env_.addAnnotation(
        "Static argument typecheck failure stub", static_typecheck_cursor);
  }

  // Prologue exit stub: the argcount-check LIR blocks branch here after
  // calling the keyword-args or incorrect-argcount helper. The helpers
  // return the result in the ABI return register; we just tear down the
  // minimal frame (push rbp / mov rbp, rsp) and return.
  if (env_.prologue_exit.isValid()) {
    as_->bind(env_.prologue_exit);
    generateFunctionExit();
  }

  // Boxed-return wrapper exit stub: the wrapper LIR blocks branch here
  // after boxing (or on error). Tears down the wrapper's own minimal frame.
  if (env_.wrapper_exit.isValid()) {
    as_->bind(env_.wrapper_exit);
    generateFunctionExit();
  }

  generateDeoptExits(codeholder);
  emitAarch64LoadAttrInvokeStub(codeholder);
  emitAarch64ExactLongAddSubStubs(codeholder);

  for (auto& [osr_idx, block] : env_.osr_entry_blocks) {
    Label stub_label = as_->newLabel();
    env_.osr_entry_stub_labels.emplace(osr_idx, stub_label);
    as_->bind(stub_label);
#if defined(CINDER_X86_64)
    as_->jmp(map_get(env_.block_label_map, block));
#elif defined(CINDER_AARCH64)
    generateOSREntryStub(env_.code_rt->getOSRMetadata(osr_idx), block);
#else
    CINDER_UNSUPPORTED
#endif
  }

#if defined(CINDER_AARCH64)
  // Emit constant pool data for MovConstPool instructions. Each entry is an
  // 8-byte value loaded via PC-relative ldr.
  for (auto& [value, label] : env_.const_pool_labels) {
    as_->bind(label);
    as_->embed(&value, sizeof(value));
  }
#endif

  if (is_shadow_compile_) {
    compiled_size_ = finalizeShadowCode(*as_, GetFunction()->fullname);
  } else {
    code_start_ = finalizeCode(*as_, GetFunction()->fullname);
  }

  // ------------- code_start_
  // ^
  // | JITRT_STATIC_ENTRY_OFFSET
  // | JITRT_CALL_REENTRY_OFFSET (6 bytes)
  // v
  // ------------- vectorcall_entry_
  auto validate_entry_offsets = [&]() -> std::optional<std::string> {
    if (has_static_entry) {
      bool valid_static_offset =
          codeholder.labelOffsetFromBase(static_jmp_location) ==
          codeholder.labelOffsetFromBase(vectorcall_entry_label) +
              JITRT_STATIC_ENTRY_OFFSET;
      if (!valid_static_offset) {
        return fmt::format(
            "bad static-entry offset {}",
            codeholder.labelOffsetFromBase(vectorcall_entry_label) -
                codeholder.labelOffsetFromBase(static_jmp_location));
      }
    }
    bool valid_reentry_offset = codeholder.labelOffset(correct_args_entry) ==
        codeholder.labelOffset(vectorcall_entry_label) +
            JITRT_CALL_REENTRY_OFFSET;
    if (!valid_reentry_offset) {
      return fmt::format(
          "bad re-entry offset, correct_args_entry={}, vectorcall_entry={}",
          codeholder.labelOffset(correct_args_entry),
          codeholder.labelOffset(vectorcall_entry_label));
    }
    return std::nullopt;
  };
  if (auto error = validate_entry_offsets()) {
    if (is_shadow_compile_) {
      throw std::runtime_error{std::move(*error)};
    }
    JIT_ABORT("{}", *error);
  }

  if (is_shadow_compile_) {
    return;
  }

  linkDeoptPatchers(codeholder);
  env_.code_rt->debugInfo()->resolvePending(
      env_.pending_debug_locs, *GetFunction(), codeholder);

  // Resolve callsite->deopt-exit label pairs (recorded in TranslateGuard)
  // to addresses now that code is finalized.
  {
    uint64_t base = codeholder.baseAddress();
    for (const auto& entry : env_.callsite_deopt_pending) {
      uintptr_t return_addr =
          base + codeholder.labelOffsetFromBase(entry.return_addr_label);
      uintptr_t exit_addr =
          base + codeholder.labelOffsetFromBase(entry.deopt_exit_label);
      env_.code_rt->addCallsiteDeoptExit(return_addr, exit_addr);
    }
  }

  vectorcall_entry_ = static_cast<char*>(code_start_) +
      codeholder.labelOffsetFromBase(vectorcall_entry_label);

  for (auto& entry : env_.unresolved_gen_entry_labels) {
    entry.first->setResumeTarget(
        codeholder.labelOffsetFromBase(entry.second) +
        codeholder.baseAddress());
  }

  for (auto& [osr_idx, label] : env_.osr_entry_stub_labels) {
    OSRMetadata& metadata = env_.code_rt->getOSRMetadata(osr_idx);
    metadata.entry_point_offset = codeholder.labelOffsetFromBase(label);
    metadata.resume_frame_total_size = env_.resume_frame_total_size;
    metadata.resume_header_and_spill_size = env_.resume_header_and_spill_size;
    metadata.resume_saved_regs = env_.resume_saved_regs;
  }

  // Resolve the static type check jump table entries now that block labels
  // have been bound to code addresses.
  for (auto& [index, block] : env_.static_typecheck_jt_entries) {
    auto label = map_get(env_.block_label_map, block);
    env_.static_typecheck_table[index] = reinterpret_cast<void*>(
        codeholder.baseAddress() + codeholder.labelOffsetFromBase(label));
  }

  // After code generation CodeHolder->codeSize() *should* return the actual
  // size of the generated code and associated data. This relies on the
  // implementation of asmjit::JitRuntime::_add and may break in the future.
  JIT_DCHECK(
      codeholder.codeSize() < INT_MAX, "Code size is larger than INT_MAX");
  compiled_size_ = codeholder.codeSize();

  JIT_LOGIF(
      getConfig().log.dump_asm,
      "Disassembly for {}\n{}",
      GetFunction()->fullname,
      env_.annotations.disassemble(code_start_, codeholder));
  {
    ThreadedCompileSerialize guard;
    void* failed_deferred_compile_trampoline =
        factory_.failedDeferredCompileTrampoline();
    for (auto& x : env_.function_indirections) {
      *x.second.indirect = failed_deferred_compile_trampoline;
    }
  }

  const hir::Function* func = GetFunction();
  // For perf, we want only the size of the code, so we get that directly from
  // the text sections.
  std::vector<std::pair<void*, std::size_t>> code_sections;
  populateCodeSections(code_sections, codeholder, code_start_);
#ifndef WIN32
  perf::registerFunction(
      code_sections, func->fullname, perf::kFuncSymbolPrefix);
#endif
}

#ifdef __ASM_DEBUG
const char* NativeGenerator::GetPyFunctionName() const {
  return PyUnicode_AsUTF8(GetFunction()->code->co_name);
}
#endif

void NativeGenerator::generateAssemblyBody(const asmjit::CodeHolder& code) {
  emitLIRBlocks(&env_, lir_func_.get(), &code, &metadata_);
}

// calcMaxInlineDepth must work with nullptr HIR functions because it's valid
// to call NativeGenerator with only LIR (e.g., from a test). In the case of an
// LIR-only function, there is no HIR inlining.
int NativeGenerator::calcInlineStackSize(const hir::Function* func) {
  if (func == nullptr) {
    return 0;
  }
  int result = 0;
  for (const auto& block : func->cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.opcode() != Opcode::kBeginInlinedFunction) {
        continue;
      }
      auto bif = dynamic_cast<const BeginInlinedFunction*>(&instr);
      int depth = frameHeaderSize(bif->code());
      for (auto frame = bif->callerFrameState(); frame != nullptr;
           frame = frame->parent) {
        depth += frameHeaderSize(frame->code);
      }
      result = std::max(depth, result);
    }
  }
  return result;
}

NativeGeneratorFactory::NativeGeneratorFactory() {}

void* NativeGeneratorFactory::deoptTrampoline() {
  if (deopt_trampoline_ == nullptr) {
    deopt_trampoline_ = generateDeoptTrampoline(false);
  }
  return deopt_trampoline_;
}

void* NativeGeneratorFactory::deoptTrampolineGenerators() {
  if (deopt_trampoline_generators_ == nullptr) {
    deopt_trampoline_generators_ = generateDeoptTrampoline(true);
  }
  return deopt_trampoline_generators_;
}

void* NativeGeneratorFactory::failedDeferredCompileTrampoline() {
  if (failed_deferred_compile_trampoline_ == nullptr) {
    failed_deferred_compile_trampoline_ =
        generateFailedDeferredCompileTrampoline();
  }
  return failed_deferred_compile_trampoline_;
}

std::unique_ptr<NativeGenerator> NativeGeneratorFactory::operator()(
    const hir::Function* func) {
  return std::make_unique<NativeGenerator>(func, *this);
}

} // namespace jit::codegen
