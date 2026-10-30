// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/gen_asm.h"

#include <structmember.h>
#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/interpreter_contract.h"
#include "cinderx/Interpreter/3.11/observe.h"
#endif
#include "internal/pycore_call.h"
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
#include "cinderx/Jit/compile_budget.h"
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
#include "cinderx/Jit/threaded_compile.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Jit/direct_entry_gate_311.h"
extern "C" int32_t Ci_JitShell311_CStackSoftLimitTlsOffset(void);
extern "C" void Ci_JitShell311_InitCStackLimits(void);
extern "C" int Ci_JitShell311_CStackSoftLimitReached(void);
extern "C" PyObject*
Ci_JitShell311_CStackOverflow(PyObject*, PyObject* const*, size_t, PyObject*);
#endif
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
#include "cinderx/Jit/threaded_compile.h"
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

#if PY_VERSION_HEX < 0x030C0000
// Ci_EvalFrameDefault_311's throwflag entry normally emits PyTrace_CALL.
// A frame resumed after a mid-flight instrumentation poll is not a new call:
// its explicit local tracer is already installed, and Stock proceeds directly
// to exception delivery.  Drop exactly that synthetic entry event, then put
// the original callbacks back before the evaluator dispatches the exception.
thread_local Py_tracefunc t_resume_tracefunc = nullptr;
thread_local Py_tracefunc t_resume_profilefunc = nullptr;

int suppressResumeCallTrace(
    PyObject* object,
    PyFrameObject* frame,
    int event,
    PyObject* argument) {
  PyThreadState* tstate = PyThreadState_Get();
  Py_tracefunc original = t_resume_tracefunc;
  JIT_CHECK(original != nullptr, "missing trace callback for JIT resume");
  t_resume_tracefunc = nullptr;
  tstate->c_tracefunc = original;
  if (event == PyTrace_CALL) {
    return 0;
  }
  return original(object, frame, event, argument);
}

int suppressResumeCallProfile(
    PyObject* object,
    PyFrameObject* frame,
    int event,
    PyObject* argument) {
  PyThreadState* tstate = PyThreadState_Get();
  Py_tracefunc original = t_resume_profilefunc;
  JIT_CHECK(original != nullptr, "missing profile callback for JIT resume");
  t_resume_profilefunc = nullptr;
  tstate->c_profilefunc = original;
  if (event == PyTrace_CALL) {
    return 0;
  }
  return original(object, frame, event, argument);
}

void suppressSyntheticResumeCallEvents(PyThreadState* tstate) {
  JIT_CHECK(
      t_resume_tracefunc == nullptr && t_resume_profilefunc == nullptr,
      "nested JIT instrumentation resume callback suppression");
  if (tstate->c_tracefunc != nullptr) {
    t_resume_tracefunc = tstate->c_tracefunc;
    tstate->c_tracefunc = suppressResumeCallTrace;
  }
  if (tstate->c_profilefunc != nullptr) {
    t_resume_profilefunc = tstate->c_profilefunc;
    tstate->c_profilefunc = suppressResumeCallProfile;
  }
}

void restoreSyntheticResumeCallEvents(PyThreadState* tstate) {
  if (t_resume_tracefunc != nullptr) {
    tstate->c_tracefunc = t_resume_tracefunc;
    t_resume_tracefunc = nullptr;
  }
  if (t_resume_profilefunc != nullptr) {
    tstate->c_profilefunc = t_resume_profilefunc;
    t_resume_profilefunc = nullptr;
  }
}
#endif

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

// Raise an exception if an LIR function exceeds a reasonable size.
void checkLirSize(const lir::Function& func) {
  auto name = [&] {
    auto hir_func = func.hirFunc();
    return hir_func != nullptr ? hir_func->fullname : "<unknown func>";
  };

  auto num_blocks = func.getNumBasicBlocks();
  auto max_blocks = getConfig().max_lir_blocks;
  if (num_blocks > max_blocks) {
    throwCompileBudgetExceeded(
        kRefuseLirSizeLimit,
        fmt::format(
            "LIR function '{}' has too many basic blocks ({}, max={})",
            name(),
            num_blocks,
            max_blocks));
  }

  auto num_instrs = func.getNumInstrs();
  auto max_instrs = getConfig().max_lir_instrs;
  if (num_instrs > max_instrs) {
    throwCompileBudgetExceeded(
        kRefuseLirSizeLimit,
        fmt::format(
            "LIR function '{}' has too many instructions ({}, max={})",
            name(),
            num_instrs,
            max_instrs));
  }
}

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
#if PY_VERSION_HEX < 0x030C0000
  // Inline-recursion-ledger deopt repayment.  The deopt exit
  // bypasses the epilogue's repayment; repay here per compilation-unit
  // flag.  The interpreted resume that follows carries its own
  // Enter/Leave pair, so the two ledgers do not interfere.
  if (code_runtime->entryGuardInlined()) {
    tstate->recursion_remaining++;
  }
#endif
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

#if PY_VERSION_HEX < 0x030C0000
  // The interpreter will clear (or hand to a frame
  // object) the compiled activation's own frame with stock ownership
  // semantics; give it the references the inline link did not take.
  if (code_runtime->frameNoRef()) {
    _PyInterpreterFrame* outer = frame;
    for (size_t i = 0; i < deopt_meta.inline_depth(); i++) {
      outer = outer->previous;
    }
    Py_INCREF(outer->f_func);
    Py_INCREF(outer->f_code);
  }
#endif

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

#if PY_VERSION_HEX < 0x030C0000
  BorrowedRef<PyCodeObject> ledger_code = deopt_meta.innermostFrame().code;
  int resume_offset = static_cast<int>(
      (frame->prev_instr + 1 - _PyCode_CODE(ledger_code)) *
      sizeof(_Py_CODEUNIT));
  runtimeTransitionLedgerRecord(
      ledger_code,
      "deopt",
      deoptReasonName(deopt_meta.reason),
      deopt_meta.innermostFrame().cause_instr_idx.value(),
      resume_offset,
      is_forced_deopt,
      is_instrumentation_deopt);
#endif

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
#if PY_VERSION_HEX < 0x030C0000
    // An eligible site's exception exits are counted; at the grant the code
    // recompiles with the exit as a CFG edge (see Ci_ExcEdgeDeopt311).
    if ((deopt_meta.reason == DeoptReason::kUnhandledException ||
         deopt_meta.reason == DeoptReason::kGuardFailure) &&
        !is_instrumentation_deopt && deopt_meta.inline_depth() == 0) {
      const DeoptFrameMetadata& fm = deopt_meta.innermostFrame();
      if (fm.code != nullptr) {
        // The deopting activation's function object: a by-code artifact
        // (fresh function objects dispatched by code) has no registered
        // member to recompile through, but this one is live on the stack.
        PyObject* func = frame->f_func != nullptr &&
                reinterpret_cast<PyCodeObject*>(frame->f_func->func_code) ==
                    fm.code
            ? reinterpret_cast<PyObject*>(frame->f_func)
            : nullptr;
        Ci_ExcEdgeDeopt311(
            fm.code,
            BCOffset(fm.cause_instr_idx).value(),
            deopt_meta.reason == DeoptReason::kGuardFailure,
            func);
      }
    }
#endif
  }
  return {frame, is_instrumentation_deopt};
}

// Set up f_trace/f_trace_lines for sys.settrace compatibility on deopted
// frames. CPython normally sets these during the RESUME opcode at function
// entry, but deopted frames resume mid-function and skip RESUME.
//
// On 3.11 (RFC 3.3.4.5 item 7) the frame's tracing state is made explicit
// without FORGING any of it: tstate->c_traceobj is the GLOBAL tracer, and
// frame->f_trace is the LOCAL tracer that only a 'call' event's return
// value installs (trace_trampoline dispatches every non-call event to
// f_trace).  A mid-flight frame never had its 'call' event under the new
// tracer, so its local tracer is legitimately absent -- copying the
// global tracer in would hand 'line' events to a callable that may only
// accept 'call' (a global tracer returning a distinct local tracer, or
// returning None, is fully legal).  So: f_trace_lines is set, an f_trace
// the user installed explicitly on the frame object is preserved as-is,
// and an absent f_trace stays absent -- which also matches stock, where
// the remaining events of an already-running frame are not delivered.
// Returns false when materializing the frame object failed (the
// exception -- a MemoryError -- is left in place for the caller to route
// into the interpreter's error path; resuming "normally" with a live
// exception would hand the evaluator a stale PyErr on a non-error path).
bool setupTraceForDeoptedFrame(
    _PyInterpreterFrame* frame,
    PyThreadState* tstate) {
  if (tstate->c_tracefunc != nullptr &&
      frame->owner != FRAME_OWNED_BY_GENERATOR) {
    PyFrameObject* fobj = _PyFrame_GetFrameObject(frame);
    if (fobj == nullptr) {
      return false;
    }
    fobj->f_trace_lines = 1;
#if PY_VERSION_HEX >= 0x030C0000
    if (fobj->f_trace == nullptr && tstate->c_traceobj != nullptr) {
      fobj->f_trace = Py_NewRef(tstate->c_traceobj);
    }
#endif
  }
  return true;
}

PyObject* resumeInInterpreter(
    _PyInterpreterFrame* frame,
    CodeRuntime* code_runtime,
    std::size_t deopt_idx,
    bool is_instrumentation_deopt) {
  JIT_CHECK(code_runtime != nullptr, "CodeRuntime cannot be a nullptr");

  PyThreadState* tstate = PyThreadState_Get();

  const DeoptMetadata& deopt_meta = code_runtime->getDeoptMetadata(deopt_idx);

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 structurally forbids lightweight frames, so the patched
  // instrumentation flavor cannot occur and prepareForDeopt's combined
  // bit reduces to the polled predicate.  The trampoline forwards that
  // bit through ARGUMENT_REGS[3]; this recomputation turns a
  // mis-assembled fourth argument (historically a register residue
  // behind a >= 3.12 gate) into a deterministic failure on every deopt
  // instead of a build-dependent misroute.  A hard check, not a DCHECK:
  // the deopt path is already slow, and a corrupt trampoline argument
  // means the generated shim itself is wrong.
  JIT_CHECK(
      is_instrumentation_deopt ==
          (deopt_meta.reason == DeoptReason::kInstrumentation),
      "deopt trampoline forwarded a corrupt is_instrumentation_deopt for "
      "reason {}",
      deoptReasonName(deopt_meta.reason));
#endif

  // For instrumentation deopts, only enter error handler if actually excepted.
  int err_occurred;
  if (is_instrumentation_deopt) {
    err_occurred = PyErr_Occurred() ? 1 : 0;
  } else {
    err_occurred = shouldResumeInterpreterInErrorHandler(deopt_meta.reason);
  }

  PyObject* result = nullptr;
#if PY_VERSION_HEX < 0x030C0000
  // The generated bind wrapper entered recursion for this real JIT frame.
  // The anchored evaluator is about to Enter the same frame again. Transfer
  // ownership first so boundary deopts do not fail the duplicate Enter and
  // silently omit the deepest traceback frame.
  JITRT_TransferRecursionToInterpreter311();
#endif
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

#if PY_VERSION_HEX < 0x030C0000
    if (err_occurred == 0) {
      if (!setupTraceForDeoptedFrame(frame, tstate)) {
        // Frame-object materialization failed: the MemoryError it left
        // becomes THIS resume's live exception, entering the evaluator
        // on the error path at the deopt boundary instead of leaking a
        // stale PyErr into a normal-path opcode.
        err_occurred = 1;
      }
    }
    // When the resume already carries an exception (err_occurred == 1),
    // the setup is skipped outright: running a fallible materialization
    // on top of a live PyErr could clobber the in-flight exception, and
    // the no-forge tracing model needs nothing set for the unwind.
#else
    // 3.12+ keeps its existing unconditional shape.
    (void)setupTraceForDeoptedFrame(frame, tstate);
#endif

#if PY_VERSION_HEX < 0x030C0000
    // Pin the continuation to the evaluator that started this frame.
    // _PyEval_EvalFrame() dispatches through interp->eval_frame at CALL
    // time, so a PEP 523 client installed while machine code was running
    // would retroactively receive the reified mid-frame -- and with it
    // this branch's entry-frame clear-and-pop contract, which is anchored
    // to the vendored 3.11 evaluator's return protocol.  Changing the
    // evaluator affects future entries; a frame already in flight is
    // finished by the interpreter that launched it.
    // The trace transition can coincide with a call that raises.  That exit
    // is classified by the failing CALL (UnhandledException), not by the
    // following instrumentation poll, but it is still a continuation of an
    // already-running frame and must not synthesize a second call event.
    if (err_occurred &&
        (tstate->c_tracefunc != nullptr || tstate->c_profilefunc != nullptr)) {
      suppressSyntheticResumeCallEvents(tstate);
    }
    result = Ci_EvalFrameDefault_311(tstate, frame, err_occurred);
    restoreSyntheticResumeCallEvents(tstate);
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

#if PY_VERSION_HEX < 0x030C0000 && defined(CINDER_AARCH64)
// the yield-from delegation walker as a generated routine.
// PyObject* walker(PyObject* top, PyThreadState* tstate), tp_iternext
// contract.  Mirrors jit::genDelegateNext311: link each delegator (frame,
// exception stack, EXECUTING, back-pointer), enter the innermost
// generator's resume entry, and on a yield unlink the chain innermost-first.
// A finish/raise/deopt goes to JITRT_GenDelegateFinish311; anything the
// walker does not handle undoes its links and takes the generic path.
static void* generateGenDelegateWalker() {
  const char* name = "gen_delegate_walker";
  auto mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    throw std::runtime_error{
        fmt::format("CinderX not initialized, cannot generate {}", name)};
  }
  CodeHolder code;
  ICodeAllocator* code_allocator = mod_state->code_allocator.get();
  ASM_CHECK(code.init(code_allocator->asmJitEnvironment()), name);
  arch::Builder a(&code);

  constexpr int kOrigin = offsetof(PyGenObject, gi_origin_or_finalizer);
  constexpr int kFrameOff = offsetof(PyGenObject, gi_iframe);
  constexpr int kPrevOff = offsetof(_PyInterpreterFrame, previous);
  constexpr int kExcStateOff = offsetof(PyGenObject, gi_exc_state);
  constexpr int kPrevItemOff = offsetof(PyGenObject, gi_exc_state) +
      offsetof(_PyErr_StackItem, previous_item);
  constexpr int kStateOff = offsetof(PyGenObject, gi_frame_state);
  constexpr int kCframeOff = offsetof(PyThreadState, cframe);
  constexpr int kCurFrameOff = offsetof(_PyCFrame, current_frame);
  constexpr int kExcInfoOff = offsetof(PyThreadState, exc_info);
  constexpr int kTypeOff = offsetof(PyObject, ob_type);
  constexpr int kYfSub = offsetof(GenDataFooter, yf_sub);
  constexpr int kDelegator = offsetof(GenDataFooter, delegator);
  constexpr int kDirectOk = offsetof(GenDataFooter, direct_ok);
  constexpr int kTreeIter = offsetof(GenDataFooter, tree_iter_state);
  constexpr int kResumeEntry = offsetof(GenDataFooter, resumeEntry);
  constexpr int kMaxDepth = 64;

  asmjit::Label loop = a.newLabel();
  asmjit::Label innermost = a.newLabel();
  asmjit::Label unwind = a.newLabel();
  asmjit::Label done = a.newLabel();
  asmjit::Label finish = a.newLabel();
  asmjit::Label fallback = a.newLabel();
  asmjit::Label epilogue = a.newLabel();

  // Prologue: frame + callee-saved x19..x24.
  a.stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -64));
  a.mov(a64::x29, a64::sp);
  a.stp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.stp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
  a.stp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
  a.mov(a64::x19, a64::x0); // top
  a.mov(a64::x20, a64::x1); // tstate
  a.mov(a64::x21, a64::x0); // cur
  a.ldr(a64::x22, a64::ptr(a64::x21, kOrigin)); // footer(cur)
  a.mov(a64::w24, 0); // depth

  a.bind(loop);
  a.ldr(a64::x0, a64::ptr(a64::x22, kYfSub)); // sub
  a.cbz(a64::x0, fallback);
  a.ldr(a64::x2, a64::ptr(a64::x0, kTypeOff));
  a.mov(a64::x3, asmjit::Imm(reinterpret_cast<uint64_t>(&g_jitgen_type)));
  a.ldr(a64::x3, a64::ptr(a64::x3));
  a.cmp(a64::x2, a64::x3);
  a.b_ne(fallback);
  a.ldr(a64::x2, a64::ptr(a64::x22, kTreeIter));
  a.cbnz(a64::x2, fallback);
  a.cmp(a64::w24, kMaxDepth);
  a.b_ge(fallback);
  a.ldr(a64::x23, a64::ptr(a64::x0, kOrigin)); // footer(sub)
  a.ldrb(a64::w2, a64::ptr(a64::x23, kDirectOk));
  a.cbz(a64::w2, fallback);
  // link cur as the running delegator.
  a.add(a64::x4, a64::x21, kFrameOff); // frame
  a.ldr(a64::x5, a64::ptr(a64::x20, kCframeOff));
  a.ldr(a64::x6, a64::ptr(a64::x5, kCurFrameOff));
  a.str(a64::x6, a64::ptr(a64::x4, kPrevOff)); // frame->previous = current
  a.str(a64::x4, a64::ptr(a64::x5, kCurFrameOff)); // current = frame
  a.ldr(a64::x6, a64::ptr(a64::x20, kExcInfoOff));
  a.str(a64::x6, a64::ptr(a64::x21, kPrevItemOff)); // previous_item = exc_info
  a.add(a64::x7, a64::x21, kExcStateOff);
  a.str(a64::x7, a64::ptr(a64::x20, kExcInfoOff)); // exc_info = &gi_exc_state
  a.strb(a64::wzr, a64::ptr(a64::x21, kStateOff)); // FRAME_EXECUTING
  a.strb(a64::wzr, a64::ptr(a64::x22, kDirectOk)); // cur: not resumable
  a.str(
      a64::x21, a64::ptr(a64::x23, kDelegator)); // footer(sub)->delegator = cur
  a.add(a64::w24, a64::w24, 1);
  a.cmp(a64::w2, 2);
  a.b_ne(innermost);
  a.mov(a64::x21, a64::x0); // cur = sub
  a.mov(a64::x22, a64::x23);
  a.b(loop);

  a.bind(innermost);
  a.mov(a64::x21, a64::x0); // keep sub / footer(sub) across the call
  a.mov(a64::x22, a64::x23);
  a.ldr(a64::x8, a64::ptr(a64::x23, kResumeEntry));
  a.mov(a64::x1, asmjit::Imm(reinterpret_cast<uint64_t>(Py_None)));
  a.mov(a64::x2, 0);
  a.mov(a64::x3, a64::x20);
  a.blr(a64::x8); // value = resumeEntry(sub, None, 0, tstate)
  a.cbz(a64::x0, finish);
  a.ldr(a64::x2, a64::ptr(a64::x21, kTypeOff));
  a.mov(a64::x3, asmjit::Imm(reinterpret_cast<uint64_t>(&g_jitgen_type)));
  a.ldr(a64::x3, a64::ptr(a64::x3));
  a.cmp(a64::x2, a64::x3);
  a.b_ne(finish);
  a.ldrb(a64::w2, a64::ptr(a64::x22, kDirectOk));
  a.cbz(a64::w2, finish);
  // Yielded: unlink the delegators innermost-first.  x4 = footer(level).
  a.mov(a64::x4, a64::x22);
  a.bind(unwind);
  a.ldr(a64::x5, a64::ptr(a64::x4, kDelegator));
  a.cbz(a64::x5, done);
  a.str(a64::xzr, a64::ptr(a64::x4, kDelegator));
  a.mov(a64::w6, 0xff); // FRAME_SUSPENDED
  a.strb(a64::w6, a64::ptr(a64::x5, kStateOff));
  a.ldr(a64::x6, a64::ptr(a64::x5, kPrevItemOff));
  a.str(a64::x6, a64::ptr(a64::x20, kExcInfoOff));
  a.str(a64::xzr, a64::ptr(a64::x5, kPrevItemOff));
  a.add(a64::x6, a64::x5, kFrameOff);
  a.ldr(a64::x7, a64::ptr(a64::x6, kPrevOff));
  a.ldr(a64::x8, a64::ptr(a64::x20, kCframeOff));
  a.str(a64::x7, a64::ptr(a64::x8, kCurFrameOff));
  a.str(a64::xzr, a64::ptr(a64::x6, kPrevOff));
  a.ldr(a64::x4, a64::ptr(a64::x5, kOrigin)); // footer(d)
  a.mov(a64::w6, 2); // suspended at its yield-from again
  a.strb(a64::w6, a64::ptr(a64::x4, kDirectOk));
  a.b(unwind);

  a.bind(finish); // x0 = value or NULL, x21 = sub
  a.mov(a64::x1, a64::x0);
  a.mov(a64::x0, a64::x21);
  a.mov(a64::x2, a64::x20);
  a.mov(
      a64::x9,
      asmjit::Imm(reinterpret_cast<uint64_t>(JITRT_GenDelegateFinish311)));
  a.blr(a64::x9);
  a.b(epilogue);

  a.bind(fallback); // undo links from cur outward, generic path for top
  a.mov(a64::x0, a64::x21);
  a.mov(a64::x1, a64::x20);
  a.mov(
      a64::x9,
      asmjit::Imm(reinterpret_cast<uint64_t>(JITRT_GenUnwindDelegators311)));
  a.blr(a64::x9);
  a.mov(a64::x0, a64::x19);
  a.mov(a64::x9, asmjit::Imm(reinterpret_cast<uint64_t>(JITRT_GenIterNext311)));
  a.blr(a64::x9);
  a.b(epilogue);

  a.bind(done);
  a.bind(epilogue);
  a.ldp(a64::x23, a64::x24, a64::ptr(a64::sp, 48));
  a.ldp(a64::x21, a64::x22, a64::ptr(a64::sp, 32));
  a.ldp(a64::x19, a64::x20, a64::ptr(a64::sp, 16));
  a.ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 64));
  a.ret(a64::x30);

  void* result = finalizeCode(a, name);
  auto code_size = code.codeSize();
  register_raw_debug_symbol(name, __FILE__, __LINE__, result, code_size, 0);
  std::vector<std::pair<void*, std::size_t>> code_sections;
  populateCodeSections(code_sections, code, result);
  code_sections.emplace_back(result, code_size);
  perf::registerFunction(code_sections, name);
  return result;
}
#endif

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

void* NativeGeneratorFactory::genDelegateWalker() {
#if PY_VERSION_HEX < 0x030C0000 && defined(CINDER_AARCH64)
  if (gen_delegate_walker_ == nullptr) {
    gen_delegate_walker_ = generateGenDelegateWalker();
  }
  return gen_delegate_walker_;
#else
  return nullptr;
#endif
}

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
#if PY_VERSION_HEX < 0x030C0000
  bool runtime_ready = false;
  SCOPE_EXIT({
    if (!shadow && !runtime_ready && env_.code_rt != nullptr) {
      if (getThreadedCompileContext().compileRunning()) {
        env_.ctx->deferCodeRuntimeRelease(env_.code_rt);
      } else {
        env_.code_rt->releaseReferences();
        env_.ctx->recycleCodeRuntime(env_.code_rt);
      }
      env_.code_rt = nullptr;
    }
  });
#endif
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

  // Shadow compiles never run, so they take the C++ walker and allocate no
  // executable memory for the generated one.
  env_.gen_delegate_walker =
      is_shadow_compile_ ? nullptr : factory_.genDelegateWalker();

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
  try {
    checkLirSize(*lir_func);
  } catch (const CompileBudgetExceeded&) {
    // The CodeRuntime was allocated before the LIR existed, and no artifact
    // will ever adopt it, so nothing else would release the references it
    // holds.  Give it back the way an artifact does when it dies.  A shadow
    // compile owns its CodeRuntime and is cleaned up by the scope exit above.
    if (!shadow && env_.code_rt != nullptr) {
      if (getThreadedCompileContext().compileRunning()) {
        // A compile worker must not drop the references itself: the
        // decrements touch objects shared with the other workers.  Hand the
        // runtime to the context, which releases it on the main thread once
        // the workers have joined.
        env_.ctx->deferCodeRuntimeRelease(env_.code_rt);
      } else {
        env_.code_rt->releaseReferences();
#if PY_VERSION_HEX < 0x030C0000
        env_.ctx->recycleCodeRuntime(env_.code_rt);
#endif
      }
      env_.code_rt = nullptr;
    }
    throw;
  }

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
#if PY_VERSION_HEX < 0x030C0000
  runtime_ready = true;
#endif
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
#if defined(CINDER_AARCH64) && PY_VERSION_HEX < 0x030C0000
  if (env_.code_rt->entryGuardInlined()) {
    // An OSR entry bypasses the frame-build recursion debit,
    // while the exit goes through the epilogue repayment (or the deopt
    // repayment) -- debit here to balance.  The interpreter-side Enter for
    // this call is repaid by its own Leave; the two ledgers do not
    // interfere.
    as_->mov(
        a64::x9,
        reinterpret_cast<uint64_t>(&ThreadedCompileContext::interpreter()
                                        ->runtime->gilstate.tstate_current));
    as_->ldr(a64::x9, a64::ptr(a64::x9));
    as_->ldr(
        a64::w10,
        arch::ptr_offset(
            a64::x9,
            static_cast<int32_t>(offsetof(PyThreadState, recursion_remaining)),
            arch::AccessSize::k32));
    as_->sub(a64::w10, a64::w10, 1);
    as_->str(
        a64::w10,
        arch::ptr_offset(
            a64::x9,
            static_cast<int32_t>(offsetof(PyThreadState, recursion_remaining)),
            arch::AccessSize::k32));
  }
  if (env_.code_rt->activeFrameCounted()) {
    // An OSR entry bypasses the inline frame link's count-in while its exit
    // goes through the counted hard-exit epilogue -- count in here.
    as_->mov(
        a64::x9, reinterpret_cast<uint64_t>(triggerStatsActiveFramesAddress()));
    as_->ldr(a64::x10, a64::ptr(a64::x9));
    as_->add(a64::x10, a64::x10, 1);
    as_->str(a64::x10, a64::ptr(a64::x9));
  }
#endif
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

// 3.11 aarch64 inline hit path for LoadAttrCache.
// Mirrors LoadAttrCache::doInvoke + SplitMutator::getAttr exactly: tagged
// entry type compare, kind must be kSplit (the only kind 3.11 set_split
// produces), pull-validated tp_version_tag, live inline values (-4
// preheader slot), bounded val_offset, non-null slot value.  Everything
// else tail-calls LoadAttrCache::invoke, which also owns stats and
// entry reset semantics.  ABI: x0=cache, x1=obj, x2=name; scratch
// x9-x15 are free at call-like sites (postalloc contract).
void NativeGenerator::emitAarch64LoadAttrInvokeStub311(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && !defined(Py_GIL_DISABLED) && \
    PY_VERSION_HEX < 0x030C0000
  if (!env_.load_attr_invoke_stub.isValid()) {
    return;
  }

  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};

  Label descr_call = as_->newLabel();

  Label slow_path = as_->newLabel();
  Label hit = as_->newLabel();

  constexpr int kEntriesOff =
      static_cast<int>(jit::AttributeCache::entriesOffset());
  constexpr int kEntrySize = sizeof(jit::AttributeMutator);
  constexpr int kTypeOff =
      static_cast<int>(jit::AttributeMutator::typeOffset());
  constexpr int kTypeVerOff =
      static_cast<int>(jit::AttributeMutator::typeVersionOffset());
  constexpr int kValOffsetOff =
      static_cast<int>(jit::AttributeMutator::splitValOffsetOffset());
  constexpr uint64_t kKindMask = jit::AttributeMutator::kindMask();
  constexpr int kObTypeOffset = offsetof(PyObject, ob_type);
  constexpr int kRefcountOffset = offsetof(PyObject, ob_refcnt);
  constexpr int kTpVersionTagOffset = offsetof(PyTypeObject, tp_version_tag);
  constexpr int kValuesPreheaderOffset =
      -4 * static_cast<int>(sizeof(PyObject*));
  constexpr int kValuesValuesOffset = offsetof(PyDictValues, values);
  constexpr uint64_t kMemberDescrKind =
      jit::AttributeMutator::memberDescrKind();
  constexpr int kMemberdefOff =
      static_cast<int>(jit::AttributeMutator::memberDescrMemberdefOffset());
  constexpr int kMemberTypeOff = offsetof(PyMemberDef, type);
  constexpr int kMemberOffsetOff = offsetof(PyMemberDef, offset);
  constexpr uint64_t kDictKind = jit::AttributeMutator::dictKind();
  constexpr int kDictHintOff =
      static_cast<int>(jit::AttributeMutator::dictHintOffset());
  constexpr int kSplitKeysOff =
      static_cast<int>(jit::AttributeMutator::splitKeysOffset());
  constexpr int kDictPreheaderOffset = -3 * static_cast<int>(sizeof(PyObject*));
  constexpr int kMaKeysOffset = offsetof(PyDictObject, ma_keys);
  constexpr int kMaValuesOffset = offsetof(PyDictObject, ma_values);
  constexpr int kDkKindOffset = offsetof(PyDictKeysObject, dk_kind);
  constexpr int kDkLog2IndexBytesOffset =
      offsetof(PyDictKeysObject, dk_log2_index_bytes);
  constexpr int kDkNentriesOffset = offsetof(PyDictKeysObject, dk_nentries);
  constexpr int kDkIndicesOffset = offsetof(PyDictKeysObject, dk_indices);
  // constants.
  constexpr uint64_t kCvarKind = jit::AttributeMutator::descrOrClassVarKind();
  constexpr int kCvarDescrOff =
      static_cast<int>(jit::AttributeMutator::descrOrClassVarDescrOffset());
  constexpr int kCvarKeysVerOff = static_cast<int>(
      jit::AttributeMutator::descrOrClassVarKeysVersionOffset());
  constexpr int kTpDescrGetOffset = offsetof(PyTypeObject, tp_descr_get);
  constexpr int kTpGetattroOffset = offsetof(PyTypeObject, tp_getattro);
  constexpr int kTpDescrSetOffset = offsetof(PyTypeObject, tp_descr_set);
  constexpr int kTpDictoffsetOffset = offsetof(PyTypeObject, tp_dictoffset);
  constexpr int kTpFlagsOffset = offsetof(PyTypeObject, tp_flags);
  constexpr int kHtCachedKeysOffset =
      offsetof(PyHeapTypeObject, ht_cached_keys);
  constexpr int kDkVersionOffset = offsetof(PyDictKeysObject, dk_version);
  constexpr int kDkLog2SizeOffset = offsetof(PyDictKeysObject, dk_log2_size);
  constexpr int kUnicodeHashOffset = offsetof(PyASCIIObject, hash);
  // Direct descriptor calls from the stub.  A kDataDescr
  // entry (property, getset) and a kDescrOrClassVar entry whose descriptor
  // has __get__ (a function or method descriptor accessed as an attribute,
  // a classmethod) call tp_descr_get(descr, obj, type) in place instead of
  // taking the helper; the descriptor is held across the call exactly as
  // DataDescrMutator::getAttr holds it.
  // A kDescrOrClassVar entry whose name also lives in
  // the receivers' shared keys (a class default that some instances
  // override: the entry carries keys_version 0, or the keys grew since
  // the fill) is served in place: the shared keys are probed for the name
  // and the instance's values slot decides -- a live slot is the answer,
  // an empty one proves the class side wins.  A dict-form receiver that
  // holds the name reads the entry value the same way.  Mirrors
  // peekInstanceAttr311; the helper keeps the general-key and twin cases.
  constexpr uint64_t kDataDescrKind = jit::AttributeMutator::dataDescrKind();
  constexpr int kDdDescrOff =
      static_cast<int>(jit::AttributeMutator::dataDescrDescrOffset());
  constexpr int kDdTypeOff =
      static_cast<int>(jit::AttributeMutator::dataDescrTypeOffset());
  constexpr int kDdTypeVerOff =
      static_cast<int>(jit::AttributeMutator::dataDescrTypeVersionOffset());
  // Entry walk: the receiver's tp_version_tag is loaded once (w17) instead
  // of once per entry, the kind mask is a logical immediate, no empty-entry
  // cbz (a zero type word never equals a live type pointer), and the kind
  // dispatch sits outside the kSplit hit path: an entry that matches type
  // and version falls straight into the split-values read, other kinds
  // branch out once.

  if (is_shadow_compile_) {
    ASM_CHECK_THROW(as_->align(AlignMode::kCode, 8));
  } else {
    ASM_CHECK(as_->align(AlignMode::kCode, 8), GetFunction()->fullname);
  }
  as_->bind(env_.load_attr_invoke_stub);

  as_->ldr(a64::x11, arch::ptr_offset(a64::x1, kObTypeOffset));
  as_->ldr(
      a64::w17,
      arch::ptr_offset(a64::x11, kTpVersionTagOffset, arch::AccessSize::k32));

  auto emit_entry = [&](uint32_t entry_index, Label next_entry) {
    const int off = kEntriesOff + static_cast<int>(entry_index) * kEntrySize;
    // Kind dispatch: kSplit (0) inline below; kMemberDescr (slots) and
    // kDict (late attrs, hint-indexed) in their own arms; anything else
    // takes the helper.
    Label member_arm = as_->newLabel();
    Label kdict_arm = as_->newLabel();
    Label cvar_arm = as_->newLabel();
    Label ddescr_arm = as_->newLabel();
    Label kind_dispatch = as_->newLabel();
    auto emit_kind_dispatch = [&](bool kind_known_nonzero) {
      arch::cmp_immediate(as_, a64::x14, kMemberDescrKind);
      as_->b_eq(member_arm);
      arch::cmp_immediate(as_, a64::x14, kDictKind);
      as_->b_eq(kdict_arm);
      arch::cmp_immediate(as_, a64::x14, kCvarKind);
      as_->b_eq(cvar_arm);
      arch::cmp_immediate(as_, a64::x14, kDataDescrKind);
      as_->b_eq(ddescr_arm);
      if (kind_known_nonzero) {
        as_->b(slow_path);
      } else {
        as_->cbnz(a64::x14, slow_path);
      }
    };
    as_->ldr(a64::x12, arch::ptr_offset(a64::x0, off + kTypeOff));
    as_->and_(a64::x13, a64::x12, ~kKindMask);
    as_->cmp(a64::x13, a64::x11);
    as_->b_ne(next_entry);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x0, off + kTypeVerOff, arch::AccessSize::k32));
    as_->cmp(a64::w17, a64::w15);
    as_->b_ne(slow_path);
    as_->and_(a64::x14, a64::x12, kKindMask);
    as_->cbnz(a64::x14, kind_dispatch);
    // kSplit entries exist only for managed-dict types, so the
    // preheader is dereferenceable.  Live inline values read below; a
    // materialized receiver that still shares the captured keys reads
    // ma_values at the same offset (split_mat).
    Label split_mat = as_->newLabel();
    as_->ldr(a64::x15, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->cbz(a64::x15, split_mat);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kValOffsetOff));
    as_->tbnz(a64::x14, 63, slow_path);
    // Capacity guard: shared keys can grow after this instance was
    // created; values prefix byte [-1] bounds the array as
    // val_offset + 2 < prefix_size (pycore_dict.h invariant).
    as_->ldrb(a64::w9, arch::ptr_offset(a64::x15, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x10, a64::x14, 2);
    as_->cmp(a64::x10, a64::x9);
    as_->b_hs(slow_path);
    arch::add_immediate(as_, a64::x15, a64::x15, kValuesValuesOffset);
    as_->add(a64::x15, a64::x15, a64::x14, a64::lsl(3));
    as_->ldr(a64::x9, a64::ptr(a64::x15));
    as_->cbz(a64::x9, slow_path);
    as_->b(hit);
    as_->bind(kind_dispatch);
    emit_kind_dispatch(true);

    // kMemberDescr: mirror PyMember_GetOne for T_OBJECT_EX only (the
    // only member type CPython 3.11 specializes for __slots__).  fill()
    // already fail-closed audited members, so a cached entry is
    // audit-free.  An empty slot raises through the helper.
    as_->bind(member_arm);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kMemberdefOff));
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x14, kMemberTypeOff, arch::AccessSize::k32));
    arch::cmp_immediate(as_, a64::x15, T_OBJECT_EX);
    as_->b_ne(slow_path);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x14, kMemberOffsetOff));
    as_->ldr(a64::x9, a64::ptr(a64::x1, a64::x15));
    as_->cbz(a64::x9, slow_path);
    as_->b(hit);

    // split_mat: materialized receiver still sharing the captured keys.
    as_->bind(split_mat);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
    as_->cbz(a64::x15, slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kSplitKeysOff));
    as_->ldr(a64::x9, arch::ptr_offset(a64::x15, kMaKeysOffset));
    as_->cmp(a64::x9, a64::x14);
    as_->b_ne(slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x15, kMaValuesOffset));
    as_->cbz(a64::x14, slow_path);
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, off + kValOffsetOff));
    as_->tbnz(a64::x9, 63, slow_path);
    as_->ldrb(a64::w10, arch::ptr_offset(a64::x14, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x15, a64::x9, 2);
    as_->cmp(a64::x15, a64::x10);
    as_->b_hs(slow_path);
    arch::add_immediate(as_, a64::x14, a64::x14, kValuesValuesOffset);
    as_->add(a64::x14, a64::x14, a64::x9, a64::lsl(3));
    as_->ldr(a64::x9, a64::ptr(a64::x14));
    as_->cbz(a64::x9, slow_path);
    as_->b(hit);

    // kdict_arm: hint-indexed read of the receiver dict, key-identity
    // validated; the helper owns misses and hint refresh.
    as_->bind(kdict_arm);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
    as_->cbz(a64::x14, slow_path);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x14, kMaKeysOffset));
    as_->ldrb(
        a64::w9,
        arch::ptr_offset(a64::x13, kDkKindOffset, arch::AccessSize::k8));
    as_->cbz(a64::w9, slow_path);
    as_->ldr(a64::x12, arch::ptr_offset(a64::x0, off + kDictHintOff));
    as_->ldr(a64::x10, arch::ptr_offset(a64::x13, kDkNentriesOffset));
    as_->cmp(a64::x12, a64::x10);
    as_->b_hs(slow_path);
    as_->ldrb(
        a64::w9,
        arch::ptr_offset(
            a64::x13, kDkLog2IndexBytesOffset, arch::AccessSize::k8));
    as_->mov(a64::x10, 1);
    as_->lsl(a64::x10, a64::x10, a64::x9);
    as_->add(a64::x10, a64::x13, a64::x10);
    arch::add_immediate(as_, a64::x10, a64::x10, kDkIndicesOffset);
    as_->add(a64::x10, a64::x10, a64::x12, a64::lsl(4));
    as_->ldr(a64::x9, a64::ptr(a64::x10)); // me_key
    as_->cmp(a64::x9, a64::x2);
    as_->b_ne(slow_path);
    Label kdict_combined = as_->newLabel();
    as_->ldr(a64::x15, arch::ptr_offset(a64::x14, kMaValuesOffset));
    as_->cbz(a64::x15, kdict_combined);
    as_->ldrb(a64::w9, arch::ptr_offset(a64::x15, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x14, a64::x12, 2);
    as_->cmp(a64::x14, a64::x9);
    as_->b_hs(slow_path);
    arch::add_immediate(as_, a64::x15, a64::x15, kValuesValuesOffset);
    as_->add(a64::x15, a64::x15, a64::x12, a64::lsl(3));
    as_->ldr(a64::x9, a64::ptr(a64::x15));
    as_->cbz(a64::x9, slow_path);
    as_->b(hit);
    as_->bind(kdict_combined);
    as_->ldr(a64::x9, arch::ptr_offset(a64::x10, 8)); // me_value
    as_->cbz(a64::x9, slow_path);
    as_->b(hit);

    // cvar_arm: kDescrOrClassVar entry holding a PLAIN
    // class variable (descriptor protocol absent on its type).  Mirrors
    // DescrOrClassVarMutator::getAttr: the class side is pinned by the
    // type version above; the instance side must not shadow the name --
    // values-form receivers prove absence by the shared keys version the
    // entry captured, dict-form receivers by the inline negative probe
    // (as in the method stub), receivers without instance storage trivially.
    as_->bind(cvar_arm);
    Label cvar_dict = as_->newLabel();
    Label cvar_probe2 = as_->newLabel();
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, off + kCvarDescrOff));
    as_->cbz(a64::x9, slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x9, kObTypeOffset));
    // x16 carries the descriptor's tp_descr_get through the shadow proof
    // (the probe below never touches x16): zero means a plain class
    // variable (x9 is the answer), non-zero a non-data descriptor whose
    // __get__ runs once absence is proved.  A data descriptor (both
    // slots) takes precedence over the instance and needs no proof,
    // mirroring DescrOrClassVarMutator::getAttr.
    Label cvar_ok = as_->newLabel();
    as_->ldr(a64::x16, arch::ptr_offset(a64::x14, kTpDescrGetOffset));
    as_->ldr(a64::x15, arch::ptr_offset(a64::x14, kTpDescrSetOffset));
    Label cvar_no_setter = as_->newLabel();
    as_->cbz(a64::x15, cvar_no_setter);
    as_->cbz(a64::x16, slow_path); // setter without getter: helper
    as_->b(descr_call);
    as_->bind(cvar_no_setter);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x11, kTpFlagsOffset));
    Label cvar_managed = as_->newLabel();
    as_->tbnz(a64::x14, 4, cvar_managed); // Py_TPFLAGS_MANAGED_DICT
    as_->ldr(a64::x14, arch::ptr_offset(a64::x11, kTpDictoffsetOffset));
    as_->cbnz(a64::x14, slow_path); // legacy __dict__ layout: helper
    as_->b(cvar_ok); // no instance storage at all
    as_->bind(cvar_managed);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->cbz(a64::x15, cvar_dict);
    as_->ldr(
        a64::w14,
        arch::ptr_offset(
            a64::x0, off + kCvarKeysVerOff, arch::AccessSize::k32));
    Label cvar_values_probe = as_->newLabel();
    as_->cbz(a64::w14, cvar_values_probe);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x11, kHtCachedKeysOffset));
    as_->cbz(a64::x13, slow_path);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x13, kDkVersionOffset, arch::AccessSize::k32));
    as_->cmp(a64::w14, a64::w15);
    as_->b_ne(cvar_values_probe);
    as_->b(cvar_ok);
    // Values-form receiver whose shared keys may hold
    // the name: probe ht_cached_keys (unicode keys, int8 indices, two
    // probe steps like the dict form), then read the values slot.
    // x17 keeps the entry index across the entry-address arithmetic.
    as_->bind(cvar_values_probe);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x11, kHtCachedKeysOffset));
    as_->cbz(a64::x13, slow_path);
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(a64::x13, kDkKindOffset, arch::AccessSize::k8));
    as_->cbz(a64::w14, slow_path); // DICT_KEYS_GENERAL
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(
            a64::x13, kDkLog2IndexBytesOffset, arch::AccessSize::k8));
    as_->ldrb(
        a64::w15,
        arch::ptr_offset(a64::x13, kDkLog2SizeOffset, arch::AccessSize::k8));
    as_->cmp(a64::w14, a64::w15);
    as_->b_ne(slow_path); // wider than int8 indices
    as_->mov(a64::x14, 1);
    as_->lsl(a64::x15, a64::x14, a64::x15);
    as_->sub(a64::x15, a64::x15, 1); // mask
    as_->ldr(a64::x10, arch::ptr_offset(a64::x2, kUnicodeHashOffset));
    as_->cmn(a64::x10, 1);
    as_->b_eq(slow_path);
    as_->and_(a64::x12, a64::x10, a64::x15);
    Label vp_found = as_->newLabel();
    as_->add(a64::x14, a64::x13, a64::x12);
    as_->ldrsb(
        a64::w14,
        arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
    as_->cmn(a64::w14, 1);
    as_->b_eq(cvar_ok); // EMPTY: not shadowed
    as_->tbnz(a64::w14, 31, slow_path); // DUMMY
    as_->sxtw(a64::x17, a64::w14);
    as_->add(a64::x14, a64::x14, a64::x14);
    as_->lsl(a64::x14, a64::x14, 3);
    as_->add(a64::x14, a64::x14, a64::x13);
    as_->add(a64::x14, a64::x14, a64::x15);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kDkIndicesOffset + 1));
    as_->cmp(a64::x14, a64::x2);
    as_->b_eq(vp_found);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kUnicodeHashOffset));
    as_->cmp(a64::x14, a64::x10);
    as_->b_eq(slow_path); // same hash, non-identical key: helper
    as_->lsr(a64::x10, a64::x10, 5);
    as_->add(a64::x14, a64::x12, a64::x12, a64::lsl(2));
    as_->add(a64::x14, a64::x14, a64::x10);
    as_->add(a64::x14, a64::x14, 1);
    as_->and_(a64::x12, a64::x14, a64::x15);
    as_->add(a64::x14, a64::x13, a64::x12);
    as_->ldrsb(
        a64::w14,
        arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
    as_->cmn(a64::w14, 1);
    as_->b_eq(cvar_ok); // EMPTY after one perturbation: absent
    as_->tbnz(a64::w14, 31, slow_path);
    as_->sxtw(a64::x17, a64::w14);
    as_->add(a64::x14, a64::x14, a64::x14);
    as_->lsl(a64::x14, a64::x14, 3);
    as_->add(a64::x14, a64::x14, a64::x13);
    as_->add(a64::x14, a64::x14, a64::x15);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kDkIndicesOffset + 1));
    as_->cmp(a64::x14, a64::x2);
    as_->b_ne(slow_path); // deeper chain or a twin: helper
    // vp_found: x17 = entry index; the instance's values array bounds
    // it (prefix byte [-1], as in the kSplit arm); a live slot is the
    // instance's own value, an empty one leaves the class side.
    // x9 stays the class-side answer for cvar_ok; the slot value moves
    // into x9 only on the way to hit.
    as_->bind(vp_found);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->ldrb(a64::w10, arch::ptr_offset(a64::x15, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x12, a64::x17, 2);
    as_->cmp(a64::x12, a64::x10);
    as_->b_hs(cvar_ok); // slot beyond this instance's values: absent
    arch::add_immediate(as_, a64::x15, a64::x15, kValuesValuesOffset);
    as_->add(a64::x15, a64::x15, a64::x17, a64::lsl(3));
    as_->ldr(a64::x12, a64::ptr(a64::x15));
    as_->cbz(a64::x12, cvar_ok);
    as_->mov(a64::x9, a64::x12);
    as_->b(hit);
    as_->bind(cvar_dict);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
    as_->cbz(a64::x13, cvar_ok); // neither values nor dict
    as_->ldr(a64::x13, arch::ptr_offset(a64::x13, kMaKeysOffset));
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(a64::x13, kDkKindOffset, arch::AccessSize::k8));
    as_->cbz(a64::w14, slow_path); // DICT_KEYS_GENERAL
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(
            a64::x13, kDkLog2IndexBytesOffset, arch::AccessSize::k8));
    as_->ldrb(
        a64::w15,
        arch::ptr_offset(a64::x13, kDkLog2SizeOffset, arch::AccessSize::k8));
    as_->cmp(a64::w14, a64::w15);
    as_->b_ne(slow_path); // wider than int8 indices
    as_->mov(a64::x14, 1);
    as_->lsl(a64::x15, a64::x14, a64::x15);
    as_->sub(a64::x15, a64::x15, 1); // mask
    as_->ldr(a64::x10, arch::ptr_offset(a64::x2, kUnicodeHashOffset));
    as_->cmn(a64::x10, 1);
    as_->b_eq(slow_path);
    as_->and_(a64::x12, a64::x10, a64::x15);
    as_->add(a64::x14, a64::x13, a64::x12);
    as_->ldrsb(
        a64::w14,
        arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
    as_->cmn(a64::w14, 1);
    as_->b_eq(cvar_ok); // EMPTY: not shadowed
    as_->tbnz(a64::w14, 31, slow_path); // DUMMY
    as_->add(a64::x14, a64::x14, a64::x14);
    as_->lsl(a64::x14, a64::x14, 3);
    as_->add(a64::x14, a64::x14, a64::x13);
    as_->add(a64::x14, a64::x14, a64::x15);
    as_->mov(a64::x17, a64::x14); // entry address survives the key load
    as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kDkIndicesOffset + 1));
    as_->cmp(a64::x14, a64::x2);
    // Dict-form receiver holding the name: the entry
    // value (me_value follows me_key) is the instance's own attribute;
    // an empty entry (deleted key) leaves the class side.
    Label dp_found = as_->newLabel();
    Label dp_probe = as_->newLabel();
    as_->b_eq(dp_found);
    as_->ldr(a64::x17, arch::ptr_offset(a64::x14, kUnicodeHashOffset));
    as_->cmp(a64::x17, a64::x10);
    as_->b_eq(slow_path); // same hash, non-identical key
    as_->b(dp_probe);
    as_->bind(dp_found);
    // A split table (the materialised __dict__ of an instance, sharing
    // the class keys) keeps its values in ma_values, indexed by the
    // entry number; only a combined table stores them in the entry.
    Label dp_combined = as_->newLabel();
    Label dp_value = as_->newLabel();
    as_->ldr(a64::x14, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
    as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kMaValuesOffset));
    as_->cbz(a64::x14, dp_combined);
    as_->sub(a64::x12, a64::x17, a64::x13);
    as_->sub(a64::x12, a64::x12, a64::x15);
    as_->lsr(a64::x12, a64::x12, 4); // entry number
    as_->ldrb(a64::w10, arch::ptr_offset(a64::x14, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x15, a64::x12, 2);
    as_->cmp(a64::x15, a64::x10);
    as_->b_hs(slow_path); // slot beyond the values array: helper
    arch::add_immediate(as_, a64::x14, a64::x14, kValuesValuesOffset);
    as_->ldr(a64::x12, a64::ptr(a64::x14, a64::x12, a64::lsl(3)));
    as_->b(dp_value);
    as_->bind(dp_combined);
    as_->ldr(a64::x12, arch::ptr_offset(a64::x17, kDkIndicesOffset + 1 + 8));
    as_->bind(dp_value);
    as_->cbz(a64::x12, cvar_ok);
    as_->mov(a64::x9, a64::x12);
    as_->b(hit);
    as_->bind(dp_probe);
    // A different key object with the same hash may still be EQUAL to
    // the name (sqlalchemy builds its options keys as "_" + argname, so
    // the instance dict holds a non-interned twin of the interned name):
    // identity alone proved nothing, and reading past it declared the
    // attribute absent while the instance shadowed it.  Equality needs
    // __eq__: the helper's.  Only a hash mismatch may probe on (the
    // dict-form path above already settled that).
    as_->bind(cvar_probe2);
    as_->lsr(a64::x10, a64::x10, 5);
    as_->add(a64::x14, a64::x12, a64::x12, a64::lsl(2));
    as_->add(a64::x14, a64::x14, a64::x10);
    as_->add(a64::x14, a64::x14, 1);
    as_->and_(a64::x12, a64::x14, a64::x15);
    as_->add(a64::x14, a64::x13, a64::x12);
    as_->ldrsb(
        a64::w14,
        arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
    as_->cmn(a64::w14, 1);
    as_->b_eq(cvar_ok);
    as_->b(slow_path);
    // Absence proved (or no instance storage): a plain class variable is
    // the answer itself, a non-data descriptor is called.
    as_->bind(cvar_ok);
    as_->cbz(a64::x16, hit);
    as_->b(descr_call);
    // ddescr_arm: kDataDescr entry (property, getset
    // descriptor).  Mirrors doInvoke's descrVersionMatches plus
    // DataDescrMutator::getAttr: the descriptor's live type must still be
    // the captured one at the captured version (a descriptor demoted to
    // non-data would have to yield to an instance shadow), then its
    // tp_descr_get runs with the descriptor held.
    as_->bind(ddescr_arm);
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, off + kDdDescrOff));
    as_->cbz(a64::x9, slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kDdTypeOff));
    as_->ldr(a64::x15, arch::ptr_offset(a64::x9, kObTypeOffset));
    as_->cmp(a64::x14, a64::x15);
    as_->b_ne(slow_path);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x14, kTpVersionTagOffset, arch::AccessSize::k32));
    as_->ldr(
        a64::w12,
        arch::ptr_offset(a64::x0, off + kDdTypeVerOff, arch::AccessSize::k32));
    as_->cmp(a64::w15, a64::w12);
    as_->b_ne(slow_path);
    as_->ldr(a64::x16, arch::ptr_offset(a64::x14, kTpDescrGetOffset));
    as_->cbz(a64::x16, slow_path);
    as_->b(descr_call);
  };

  const uint32_t stub_entries = jit::getConfig().attr_cache_size;
  if (stub_entries == 0) {
    as_->b(slow_path);
  }
  for (uint32_t entry_index = 0; entry_index < stub_entries; ++entry_index) {
    const bool has_next_entry = entry_index + 1 != stub_entries;
    Label next_entry = has_next_entry ? as_->newLabel() : slow_path;
    emit_entry(entry_index, next_entry);
    if (has_next_entry) {
      as_->bind(next_entry);
    }
  }

  as_->bind(hit);
  // 3.11: plain 64-bit refcount, no immortality.
  as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  arch::add_immediate(as_, a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->mov(a64::x0, a64::x9);
  as_->ret(arch::lr);

  // descr_call: x9 = descriptor (borrowed from the type
  // dict, pinned by the version checks above), x16 = its tp_descr_get,
  // x1 = receiver, x11 = receiver type.  The descriptor is held across the
  // call -- the getter may run Python that deletes it from the class --
  // and released afterwards, the rare last reference through _Py_Dealloc.
  // The getter's result (a new reference, or NULL with an exception set)
  // is the stub's answer, exactly what the helper returns for this entry.
  Label descr_done = as_->newLabel();
  as_->bind(descr_call);
  // A receiver type with a __getattr__ hook (tp_getattro ==
  // slot_tp_getattr_hook, admitted by the fill when __getattribute__ is
  // generic) routes a getter's AttributeError to the hook that
  // slot_tp_getattr_hook pinned before the generic lookup; the stub has no
  // such pin, so those receivers take the helper, which does
  // (GetAttrHookSnapshot311).  The getter must not run twice, hence the
  // check precedes the call rather than following a failure.
  as_->ldr(a64::x12, arch::ptr_offset(a64::x11, kTpGetattroOffset));
  as_->mov(a64::x14, reinterpret_cast<uint64_t>(PyObject_GenericGetAttr));
  as_->cmp(a64::x12, a64::x14);
  as_->b_ne(slow_path);
  // Frame: [x29, x30] at sp, the descriptor at sp+16, the receiver and the
  // name at sp+24 / sp+32 (both are clobbered by the getter and needed by
  // the error completion below).
  as_->stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -48));
  as_->mov(a64::x29, a64::sp);
  as_->str(a64::x9, arch::ptr_offset(a64::sp, 16));
  as_->str(a64::x1, arch::ptr_offset(a64::sp, 24));
  as_->str(a64::x2, arch::ptr_offset(a64::sp, 32));
  as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  arch::add_immediate(as_, a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->mov(a64::x0, a64::x9);
  as_->mov(a64::x2, a64::x11);
  as_->blr(a64::x16);
  as_->ldr(a64::x9, arch::ptr_offset(a64::sp, 16));
  as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->sub(a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->cbnz(a64::x12, descr_done);
  as_->str(a64::x0, arch::ptr_offset(a64::sp, 16));
  as_->mov(a64::x0, a64::x9);
  as_->mov(arch::reg_scratch_br, reinterpret_cast<uint64_t>(_Py_Dealloc));
  as_->blr(arch::reg_scratch_br);
  as_->ldr(a64::x0, arch::ptr_offset(a64::sp, 16));
  as_->bind(descr_done);
  // A raising getter: the exception is completed with the name and the
  // receiver (PyObject_GetAttr's AttributeError context) by the helper the
  // stub leaves through; a result returns as it is.
  Label descr_ok = as_->newLabel();
  as_->cbnz(a64::x0, descr_ok);
  as_->ldr(a64::x0, arch::ptr_offset(a64::sp, 24));
  as_->ldr(a64::x1, arch::ptr_offset(a64::sp, 32));
  as_->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 48));
  as_->mov(
      arch::reg_scratch_br, reinterpret_cast<uint64_t>(JITRT_LoadAttrError311));
  as_->br(arch::reg_scratch_br);
  as_->bind(descr_ok);
  as_->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 48));
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

// 3.11 aarch64 inline hit path for StoreAttrCache.
// Mirrors doInvoke + the 3.11 SplitMutator::setAttr overwrite path only:
// kSplit entry, pull-validated type version, live inline values, bounded
// val_offset, existing old value with refcount > 1 (a dealloc or an
// insertion needs the helper's bookkeeping).  Store then decref-old, in
// stock SETLOCAL order.  ABI: x0=cache, x1=obj, x2=name, x3=value;
// returns 0 in w0 on the inline path.
void NativeGenerator::emitAarch64StoreAttrInvokeStub311(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && !defined(Py_GIL_DISABLED) && \
    PY_VERSION_HEX < 0x030C0000
  if (!env_.store_attr_invoke_stub.isValid()) {
    return;
  }

  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};

  Label slow_path = as_->newLabel();
  Label do_store = as_->newLabel();

  constexpr int kEntriesOff =
      static_cast<int>(jit::AttributeCache::entriesOffset());
  constexpr int kEntrySize = sizeof(jit::AttributeMutator);
  constexpr int kTypeOff =
      static_cast<int>(jit::AttributeMutator::typeOffset());
  constexpr int kTypeVerOff =
      static_cast<int>(jit::AttributeMutator::typeVersionOffset());
  constexpr int kValOffsetOff =
      static_cast<int>(jit::AttributeMutator::splitValOffsetOffset());
  constexpr uint64_t kKindMask = jit::AttributeMutator::kindMask();
  constexpr int kObTypeOffset = offsetof(PyObject, ob_type);
  constexpr int kRefcountOffset = offsetof(PyObject, ob_refcnt);
  constexpr int kTpVersionTagOffset = offsetof(PyTypeObject, tp_version_tag);
  constexpr int kValuesPreheaderOffset =
      -4 * static_cast<int>(sizeof(PyObject*));
  constexpr int kValuesValuesOffset = offsetof(PyDictValues, values);
  constexpr uint64_t kMemberDescrKind =
      jit::AttributeMutator::memberDescrKind();
  constexpr int kMemberdefOff =
      static_cast<int>(jit::AttributeMutator::memberDescrMemberdefOffset());
  constexpr int kMemberTypeOff = offsetof(PyMemberDef, type);
  constexpr int kMemberOffsetOff = offsetof(PyMemberDef, offset);
  constexpr int kMemberFlagsOff = offsetof(PyMemberDef, flags);
  Label member_store_fresh = as_->newLabel();

  if (is_shadow_compile_) {
    ASM_CHECK_THROW(as_->align(AlignMode::kCode, 8));
  } else {
    ASM_CHECK(as_->align(AlignMode::kCode, 8), GetFunction()->fullname);
  }
  as_->bind(env_.store_attr_invoke_stub);

  as_->ldr(a64::x11, arch::ptr_offset(a64::x1, kObTypeOffset));

  auto emit_entry = [&](uint32_t entry_index, Label next_entry) {
    const int off = kEntriesOff + static_cast<int>(entry_index) * kEntrySize;
    as_->ldr(a64::x12, arch::ptr_offset(a64::x0, off + kTypeOff));
    as_->cbz(a64::x12, next_entry);
    as_->mov(a64::x13, ~kKindMask);
    as_->and_(a64::x13, a64::x12, a64::x13);
    as_->cmp(a64::x13, a64::x11);
    as_->b_ne(next_entry);
    as_->ldr(
        a64::w14,
        arch::ptr_offset(a64::x13, kTpVersionTagOffset, arch::AccessSize::k32));
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x0, off + kTypeVerOff, arch::AccessSize::k32));
    as_->cmp(a64::w14, a64::w15);
    as_->b_ne(slow_path);
    Label member_arm = as_->newLabel();
    as_->and_(a64::x14, a64::x12, kKindMask);
    arch::cmp_immediate(as_, a64::x14, kMemberDescrKind);
    as_->b_eq(member_arm);
    as_->cbnz(a64::x14, slow_path);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->cbz(a64::x15, slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kValOffsetOff));
    as_->tbnz(a64::x14, 63, slow_path);
    as_->ldrb(a64::w9, arch::ptr_offset(a64::x15, -1, arch::AccessSize::k8));
    arch::add_immediate(as_, a64::x10, a64::x14, 2);
    as_->cmp(a64::x10, a64::x9);
    as_->b_hs(slow_path);
    arch::add_immediate(as_, a64::x15, a64::x15, kValuesValuesOffset);
    as_->add(a64::x10, a64::x15, a64::x14, a64::lsl(3));
    as_->ldr(a64::x9, a64::ptr(a64::x10)); // old value
    as_->cbz(a64::x9, slow_path);
    as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
    arch::cmp_immediate(as_, a64::x12, 1);
    as_->b_eq(slow_path);
    as_->b(do_store);

    // kMemberDescr (slots): mirror PyMember_SetOne for a writable
    // T_OBJECT_EX member.  A fresh slot (old == NULL) is a plain
    // store+incref; an occupied slot shares do_store, whose reload
    // keeps the aliased-store accounting right.
    as_->bind(member_arm);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kMemberdefOff));
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x14, kMemberTypeOff, arch::AccessSize::k32));
    arch::cmp_immediate(as_, a64::x15, T_OBJECT_EX);
    as_->b_ne(slow_path);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x14, kMemberFlagsOff, arch::AccessSize::k32));
    as_->tst(a64::x15, READONLY);
    as_->b_ne(slow_path);
    as_->ldr(a64::x15, arch::ptr_offset(a64::x14, kMemberOffsetOff));
    as_->add(a64::x10, a64::x1, a64::x15);
    as_->ldr(a64::x9, a64::ptr(a64::x10));
    as_->cbz(a64::x9, member_store_fresh);
    as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
    arch::cmp_immediate(as_, a64::x12, 1);
    as_->b_eq(slow_path);
    as_->b(do_store);
  };

  const uint32_t stub_entries = jit::getConfig().attr_cache_size;
  if (stub_entries == 0) {
    as_->b(slow_path);
  }
  for (uint32_t entry_index = 0; entry_index < stub_entries; ++entry_index) {
    const bool has_next_entry = entry_index + 1 != stub_entries;
    Label next_entry = has_next_entry ? as_->newLabel() : slow_path;
    emit_entry(entry_index, next_entry);
    if (has_next_entry) {
      as_->bind(next_entry);
    }
  }

  // x10 = slot address, x9 = old value (pre-checked refcount > 1).
  // The new value can BE the old value (repeated stores of the same
  // singleton are richards' bread and butter), so the old refcount must
  // be re-read after the incref of the new value -- decrementing a
  // pre-incref copy nets the pair to -1 and frees a live object.
  as_->bind(do_store);
  as_->ldr(a64::x14, arch::ptr_offset(a64::x3, kRefcountOffset));
  arch::add_immediate(as_, a64::x14, a64::x14, 1);
  as_->str(a64::x14, arch::ptr_offset(a64::x3, kRefcountOffset));
  as_->str(a64::x3, a64::ptr(a64::x10));
  as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  arch::sub_immediate(as_, a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->mov(a64::w0, 0);
  as_->ret(arch::lr);

  // Fresh slot member store: no old value to release.
  as_->bind(member_store_fresh);
  as_->ldr(a64::x14, arch::ptr_offset(a64::x3, kRefcountOffset));
  arch::add_immediate(as_, a64::x14, a64::x14, 1);
  as_->str(a64::x14, arch::ptr_offset(a64::x3, kRefcountOffset));
  as_->str(a64::x3, a64::ptr(a64::x10));
  as_->mov(a64::w0, 0);
  as_->ret(arch::lr);

  as_->bind(slow_path);
  as_->mov(
      arch::reg_scratch_br,
      reinterpret_cast<uint64_t>(jit::StoreAttrCache::invoke));
  as_->br(arch::reg_scratch_br);
#else
  (void)code;
#endif
}

// Per-function dispatch stub for non-function callables at call sites (the
// generic arm of translateCallSiteVectorCall).  Entered by blr with
// x0=callable / x1=args / x2=nargsf / x3=kwnames (NULL at these sites) and
// x15 = 1 for the method-call form (JITRT_Call's slot protocol applies), 0
// for the plain form, or the site's entry cache address for the plain form
// with a constructor arm.  Dispatches the way _PyObject_VectorcallTstate
// does: tp_flags HAVE_VECTORCALL and the slot at tp_vectorcall_offset are
// read and the slot is called directly. Result/exception consistency is
// checked inline; malformed returns go through _Py_CheckFunctionResult so
// its SystemError, exception chaining and debug-build diagnostics match
// the interpreter. Ordinary returns need no additional C helper call.
// After a successful call the eval breaker is tested and pending work runs
// through JITRT_HandlePendingAfterCall311.  A heap class takes the
// constructor arm (JITRT_ConstructSite311) when a site cache was supplied;
// anything without a vectorcall slot tail-calls the slow slot.  Out of line
// so call sites stay short.
void NativeGenerator::emitAarch64VcDirectStub311(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && !defined(Py_GIL_DISABLED) && \
    PY_VERSION_HEX < 0x030C0000
  if (!env_.vcdirect_stub.isValid()) {
    return;
  }
  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};

  Label slow = as_->newLabel();
  Label slow_call_slot = as_->newLabel();
  Label have_callable = as_->newLabel();
  Label check_receiver = as_->newLabel();
  Label dispatch = as_->newLabel();
  Label not_type = as_->newLabel();
  Label out = as_->newLabel();
  Label null_result = as_->newLabel();
  Label invalid_result = as_->newLabel();

  as_->bind(env_.vcdirect_stub);
  // --- method form: JITRT_Call's slot protocol (consuming a slot forfeits
  // PY_VECTORCALL_ARGUMENTS_OFFSET) ---
  as_->cmp(a64::x15, 1);
  as_->b_ne(dispatch);
  as_->cbnz(a64::x0, have_callable);
  as_->ldr(a64::x0, a64::ptr(a64::x1));
  as_->add(a64::x1, a64::x1, 8);
  as_->sub(a64::x2, a64::x2, 1);
  as_->and_(a64::x2, a64::x2, 0x7fffffffffffffffULL);
  as_->b(dispatch);

  as_->bind(have_callable);
  as_->mov(a64::x11, reinterpret_cast<uint64_t>(Py_None));
  as_->cmp(a64::x0, a64::x11);
  as_->b_ne(check_receiver);
  as_->ldr(a64::x14, a64::ptr(a64::x1));
  as_->cbz(a64::x14, slow_call_slot); // None with an empty receiver slot
  as_->mov(a64::x0, a64::x14);
  as_->add(a64::x1, a64::x1, 8);
  as_->sub(a64::x2, a64::x2, 1);
  as_->and_(a64::x2, a64::x2, 0x7fffffffffffffffULL);
  as_->b(dispatch);

  as_->bind(check_receiver);
  as_->ldr(a64::x14, a64::ptr(a64::x1));
  as_->cbnz(a64::x14, dispatch);
  as_->add(a64::x1, a64::x1, 8);
  as_->sub(a64::x2, a64::x2, 1);
  as_->and_(a64::x2, a64::x2, 0x7fffffffffffffffULL);

  // --- dispatch ---
  as_->bind(dispatch);
  as_->ldr(
      a64::x10,
      arch::ptr_offset(
          a64::x0, static_cast<int32_t>(offsetof(PyObject, ob_type))));
  as_->mov(a64::x11, reinterpret_cast<uint64_t>(&PyType_Type));
  as_->cmp(a64::x10, a64::x11);
  as_->b_ne(not_type);
  static_assert(Py_TPFLAGS_HEAPTYPE == (1UL << 9), "HEAPTYPE bit");
  as_->ldr(
      a64::x11,
      arch::ptr_offset(
          a64::x0, static_cast<int32_t>(offsetof(PyTypeObject, tp_flags))));
  as_->tbz(a64::x11, 9, not_type);
  // A heap class.  With a site cache in x15 the constructor arm validates
  // the class version and the __init__ entry and runs object_new plus a
  // direct __init__ entry (or falls to the slow slot itself); without one,
  // the slow slot.  x3 (kwnames) is NULL at every site that reaches this
  // stub, so it carries the cache.
  as_->cmp(a64::x15, 1);
  as_->b_ls(slow);
  as_->mov(a64::x3, a64::x15);
  as_->mov(a64::x16, reinterpret_cast<uint64_t>(JITRT_ConstructSite311));
  as_->br(a64::x16);

  as_->bind(not_type);
  static_assert(Py_TPFLAGS_HAVE_VECTORCALL == (1UL << 11), "VECTORCALL bit");
  as_->ldr(
      a64::x11,
      arch::ptr_offset(
          a64::x10, static_cast<int32_t>(offsetof(PyTypeObject, tp_flags))));
  as_->tbz(a64::x11, 11, slow);
  as_->ldr(
      a64::x12,
      arch::ptr_offset(
          a64::x10,
          static_cast<int32_t>(offsetof(PyTypeObject, tp_vectorcall_offset))));
  as_->ldr(a64::x14, a64::ptr(a64::x0, a64::x12));
  as_->cbz(a64::x14, slow);

  // Keep the callable for the error diagnostic across the vectorcall;
  // x0 becomes the result and all argument/scratch registers may be lost.
  as_->stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -32));
  as_->mov(a64::x29, a64::sp);
  as_->str(a64::x0, a64::ptr(a64::sp, 16));
  as_->blr(a64::x14);
  as_->mov(
      a64::x9,
      reinterpret_cast<uint64_t>(&ThreadedCompileContext::interpreter()
                                      ->runtime->gilstate.tstate_current));
  as_->ldr(a64::x9, a64::ptr(a64::x9));
  as_->ldr(
      a64::x10,
      arch::ptr_offset(
          a64::x9, static_cast<int32_t>(offsetof(PyThreadState, curexc_type))));
  as_->cbz(a64::x0, null_result);
  as_->cbnz(a64::x10, invalid_result);
  as_->mov(
      a64::x9,
      reinterpret_cast<uint64_t>(
          &ThreadedCompileContext::interpreter()->ceval.eval_breaker));
  as_->ldr(a64::w9, a64::ptr(a64::x9));
  as_->cbz(a64::w9, out);
  as_->mov(
      a64::x9, reinterpret_cast<uint64_t>(JITRT_HandlePendingAfterCall311));
  as_->blr(a64::x9);
  as_->bind(out);
  as_->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 32));
  as_->ret(a64::x30);

  as_->bind(null_result);
  // NULL with an exception is a legitimate error return. NULL without
  // one, or a non-NULL result with an exception, must be audited before
  // the caller's exception guard or any pending work can run.
  as_->cbnz(a64::x10, out);
  as_->bind(invalid_result);
  as_->mov(a64::x2, a64::x0);
  as_->ldr(a64::x1, a64::ptr(a64::sp, 16));
  as_->mov(a64::x0, a64::x9);
  as_->mov(a64::x3, 0);
  as_->mov(a64::x16, reinterpret_cast<uint64_t>(_Py_CheckFunctionResult));
  as_->blr(a64::x16);
  as_->b(out);

  // --- slow: tail-call the plain vectorcall helper.  The slot protocol has
  // already been applied above, so JITRT_Call must not see the shifted
  // arguments again: with zero remaining arguments its empty-receiver-slot
  // test would read past the argument vector.  Only the unresolved
  // None-with-empty-receiver shape keeps the Call slot.
  as_->bind(slow);
  as_->mov(a64::x16, reinterpret_cast<uint64_t>(&g_JITRT_Vectorcall311_slot));
  as_->ldr(a64::x16, a64::ptr(a64::x16));
  as_->br(a64::x16);
  as_->bind(slow_call_slot);
  as_->mov(a64::x16, reinterpret_cast<uint64_t>(&g_JITRT_Call311_slot));
  as_->ldr(a64::x16, a64::ptr(a64::x16));
  as_->br(a64::x16);
#else
  (void)code;
#endif
}

void NativeGenerator::emitAarch64LoadMethodInvokeStub311(
    const asmjit::CodeHolder& code) {
#if defined(CINDER_AARCH64) && !defined(Py_GIL_DISABLED) && \
    PY_VERSION_HEX < 0x030C0000
  if (!env_.load_method_invoke_stub.isValid()) {
    return;
  }

  CodeSectionOverride hot_override{as_, &code, &metadata_, CodeSection::kHot};

  Label slow_path = as_->newLabel();
  Label hit = as_->newLabel();

  constexpr int kEntriesOffset =
      static_cast<int>(jit::LoadMethodCache::entriesOffset());
  constexpr int kEntrySize =
      static_cast<int>(jit::LoadMethodCache::entrySize());
  constexpr int kTypeOff =
      static_cast<int>(jit::LoadMethodCache::entryTypeOffset());
  constexpr int kValueOff =
      static_cast<int>(jit::LoadMethodCache::entryValueOffset());
  constexpr int kKeysVerOff =
      static_cast<int>(jit::LoadMethodCache::entryKeysVersionOffset());
  constexpr int kTypeVerOff =
      static_cast<int>(jit::LoadMethodCache::entryTypeVersionOffset());
  constexpr int kPeekOff =
      static_cast<int>(jit::LoadMethodCache::entryPeekShadowOffset());
  constexpr int kPeekKeysOff =
      static_cast<int>(jit::LoadMethodCache::entryPeekKeysOffset());
  constexpr int kPeekNentriesOff =
      static_cast<int>(jit::LoadMethodCache::entryPeekNentriesOffset());
  constexpr int kPeekKeysVersionOff =
      static_cast<int>(jit::LoadMethodCache::entryPeekKeysVersionOffset());
  constexpr int kPeekHintOff =
      static_cast<int>(jit::LoadMethodCache::entryPeekHintOffset());
  constexpr int kInstAttrOff =
      static_cast<int>(jit::LoadMethodCache::entryInstAttrOffset());
  constexpr int kTpFlagsOffset = offsetof(PyTypeObject, tp_flags);
  constexpr int kDkKindOffset = offsetof(PyDictKeysObject, dk_kind);
  constexpr int kDkNentriesOffset = offsetof(PyDictKeysObject, dk_nentries);
  constexpr int kDkLog2SizeOffset = offsetof(PyDictKeysObject, dk_log2_size);
  constexpr int kDkLog2IndexBytesOffset =
      offsetof(PyDictKeysObject, dk_log2_index_bytes);
  constexpr int kDkIndicesOffset = offsetof(PyDictKeysObject, dk_indices);
  constexpr int kUnicodeHashOffset = offsetof(PyASCIIObject, hash);
  static_assert(sizeof(PyDictUnicodeEntry) == 16, "entry size");
  static_assert(DKIX_EMPTY == -1 && DKIX_DUMMY == -2, "dkix sentinels");
  static_assert(Py_TPFLAGS_MANAGED_DICT == (1UL << 4), "flag bit moved");
  // inline arm for peek entries whose memo pins the
  // name's ABSENCE (peek_hint == kPeekAbsent) -- the LoadMethodCache::lookup
  // fast answers for values-form (type cached keys anchor) and
  // materialized-dict receivers (instance dict keys anchor).  Everything
  // else about a peek entry (hint >= 0, inst_attr, split-materialized dict)
  // keeps going to the helper.
  // The receiver's tp_version_tag is hoisted into w17 (see the LoadAttr
  // stub); the entry walk otherwise keeps its shape.
  constexpr int kObTypeOffset = offsetof(PyObject, ob_type);
  constexpr int kRefcountOffset = offsetof(PyObject, ob_refcnt);
  constexpr int kTpVersionTagOffset = offsetof(PyTypeObject, tp_version_tag);
  constexpr int kTpDictoffsetOffset = offsetof(PyTypeObject, tp_dictoffset);
  constexpr int kHtCachedKeysOffset =
      offsetof(PyHeapTypeObject, ht_cached_keys);
  constexpr int kDkVersionOffset = offsetof(PyDictKeysObject, dk_version);
  constexpr int kValuesPreheaderOffset =
      -4 * static_cast<int>(sizeof(PyObject*));
  constexpr int kDictPreheaderOffset = -3 * static_cast<int>(sizeof(PyObject*));
  constexpr int kMaKeysOffset = offsetof(PyDictObject, ma_keys);

  if (is_shadow_compile_) {
    ASM_CHECK_THROW(as_->align(AlignMode::kCode, 8));
  } else {
    ASM_CHECK(as_->align(AlignMode::kCode, 8), GetFunction()->fullname);
  }
  as_->bind(env_.load_method_invoke_stub);

  // x0=cache, x1=obj, x2=name
  as_->ldr(a64::x11, arch::ptr_offset(a64::x1, kObTypeOffset));
  as_->ldr(
      a64::w17,
      arch::ptr_offset(a64::x11, kTpVersionTagOffset, arch::AccessSize::k32));

  Label mat_check = as_->newLabel();

  auto emit_entry = [&](uint32_t entry_index, Label next_entry) {
    const int off = kEntriesOffset + static_cast<int>(entry_index) * kEntrySize;
    as_->ldr(a64::x12, arch::ptr_offset(a64::x0, off + kTypeOff));
    as_->cmp(a64::x12, a64::x11);
    as_->b_ne(next_entry);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x0, off + kTypeVerOff, arch::AccessSize::k32));
    as_->cmp(a64::w17, a64::w15);
    as_->b_ne(slow_path);
    // peek entries prove no-shadow per hit in the helper.
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(a64::x0, off + kPeekOff, arch::AccessSize::k8));
    Label not_peek = as_->newLabel();
    Label peek_mat = as_->newLabel();
    Label peek_cmp = as_->newLabel();
    as_->cbz(a64::w14, not_peek);
    // peek entry: only the pinned-absent, class-side form is inlined.
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(a64::x0, off + kInstAttrOff, arch::AccessSize::k8));
    {
      // the instance-attribute callable form (a bound
      // method stored by __init__: pickle's _Unpickler.read / append,
      // 17M lookups per unpickle_pure_python run in the helper).  The
      // memo pins the slot index into the values-form storage: live
      // cached keys identical to the anchor, same entry count, the entry
      // at the hint still keyed by this name, the slot within the
      // instance's used range and non-NULL.  Result is {None, slot},
      // both references taken here as the helper does.
      Label class_side = as_->newLabel();
      as_->cbz(a64::w14, class_side);
      as_->ldr(a64::x14, arch::ptr_offset(a64::x11, kTpFlagsOffset));
      as_->tbz(a64::x14, 4, slow_path); // Py_TPFLAGS_MANAGED_DICT
      as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
      as_->cbz(a64::x13, slow_path); // materialized: helper
      as_->ldr(a64::x9, arch::ptr_offset(a64::x11, kHtCachedKeysOffset));
      as_->cbz(a64::x9, slow_path);
      as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kPeekKeysOff));
      as_->cmp(a64::x9, a64::x14);
      as_->b_ne(slow_path);
      as_->ldr(a64::x14, arch::ptr_offset(a64::x9, kDkNentriesOffset));
      as_->ldr(a64::x15, arch::ptr_offset(a64::x0, off + kPeekNentriesOff));
      as_->cmp(a64::x14, a64::x15);
      as_->b_ne(slow_path);
      as_->ldr(a64::x15, arch::ptr_offset(a64::x0, off + kPeekHintOff));
      as_->tbnz(a64::x15, 63, slow_path); // hint >= 0
      // entries = keys + indices + (1 << dk_log2_index_bytes); me_key at
      // entries[hint].
      as_->ldrb(
          a64::w14,
          arch::ptr_offset(
              a64::x9, kDkLog2IndexBytesOffset, arch::AccessSize::k8));
      as_->mov(a64::x12, 1);
      as_->lsl(a64::x12, a64::x12, a64::x14);
      as_->add(a64::x12, a64::x12, a64::x9);
      as_->add(a64::x12, a64::x12, a64::x15, a64::lsl(4));
      as_->ldr(a64::x14, arch::ptr_offset(a64::x12, kDkIndicesOffset));
      as_->cmp(a64::x14, a64::x2);
      as_->b_ne(slow_path);
      // hint + 2 < values[-1] (the instance's used-slot count byte)
      as_->ldrb(a64::w14, arch::ptr_offset(a64::x13, -1, arch::AccessSize::k8));
      as_->add(a64::x12, a64::x15, 2);
      as_->cmp(a64::x12, a64::x14);
      as_->b_hs(slow_path);
      as_->ldr(a64::x9, a64::ptr(a64::x13, a64::x15, a64::lsl(3)));
      as_->cbz(a64::x9, slow_path);
      as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
      arch::add_immediate(as_, a64::x12, a64::x12, 1);
      as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
      as_->mov(a64::x0, reinterpret_cast<uint64_t>(Py_None));
      as_->ldr(a64::x12, arch::ptr_offset(a64::x0, kRefcountOffset));
      arch::add_immediate(as_, a64::x12, a64::x12, 1);
      as_->str(a64::x12, arch::ptr_offset(a64::x0, kRefcountOffset));
      as_->mov(a64::x1, a64::x9);
      as_->ret(arch::lr);
      as_->bind(class_side);
    }
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, off + kValueOff));
    as_->cbz(a64::x9, slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x11, kTpFlagsOffset));
    Label non_managed = as_->newLabel();
    as_->tbz(a64::x14, 4, non_managed); // Py_TPFLAGS_MANAGED_DICT
    as_->ldr(a64::x15, arch::ptr_offset(a64::x0, off + kPeekHintOff));
    as_->cmn(a64::x15, 2); // peek_hint == kPeekAbsent (-2)
    as_->b_ne(slow_path);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->cbz(a64::x13, peek_mat);
    // values form: anchor is the live type's cached keys.
    as_->ldr(a64::x13, arch::ptr_offset(a64::x11, kHtCachedKeysOffset));
    as_->cbz(a64::x13, slow_path);
    as_->b(peek_cmp);
    as_->bind(peek_mat);
    // materialized form: anchor is the instance dict's own keys table;
    // split-materialized dicts (ma_values set) and non-unicode keys keep
    // the helper's probing path.
    // (split-materialized dicts -- ma_values set -- share the anchor for
    // the absent-name form inlined here.)
    as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
    as_->cbz(a64::x13, slow_path);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x13, kMaKeysOffset));
    as_->ldrb(
        a64::w14,
        arch::ptr_offset(a64::x13, kDkKindOffset, arch::AccessSize::k8));
    as_->cbz(a64::w14, slow_path); // DICT_KEYS_GENERAL
    {
      // Anchor mismatch (many instances, each with its own combined
      // keys table) falls into an inline negative probe instead of the
      // helper: up to two steps of the unicode-key probe sequence on an
      // int8 index table; an EMPTY slot proves absence (the class-side
      // method stands), a matching key is a shadow, anything else --
      // DUMMY, collision, wider tables, uncached hash -- is the helper's.
      Label probe = as_->newLabel();
      Label probe2 = as_->newLabel();
      Label probe_check = as_->newLabel();
      as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kPeekKeysOff));
      as_->cmp(a64::x13, a64::x14);
      as_->b_ne(probe);
      as_->ldr(a64::x14, arch::ptr_offset(a64::x13, kDkNentriesOffset));
      as_->ldr(a64::x15, arch::ptr_offset(a64::x0, off + kPeekNentriesOff));
      as_->cmp(a64::x14, a64::x15);
      as_->b_ne(probe);
      // A combined-keys table freed by dict.clear() and reallocated at
      // the same address with the same dk_nentries must not satisfy the
      // anchor: the anchor also records the table's keys version, which
      // a fresh table lacks (0) and any key-set change resets.
      as_->ldr(
          a64::w14,
          arch::ptr_offset(a64::x13, kDkVersionOffset, arch::AccessSize::k32));
      as_->cbz(a64::w14, probe);
      as_->ldr(
          a64::w15,
          arch::ptr_offset(
              a64::x0, off + kPeekKeysVersionOff, arch::AccessSize::k32));
      as_->cmp(a64::w14, a64::w15);
      as_->b_eq(hit);
      as_->bind(probe);
      as_->ldrb(
          a64::w14,
          arch::ptr_offset(
              a64::x13, kDkLog2IndexBytesOffset, arch::AccessSize::k8));
      as_->ldrb(
          a64::w15,
          arch::ptr_offset(a64::x13, kDkLog2SizeOffset, arch::AccessSize::k8));
      as_->cmp(a64::w14, a64::w15); // int8 indices <=> index_bytes == size
      as_->b_ne(slow_path);
      as_->mov(a64::x14, 1);
      as_->lsl(a64::x15, a64::x14, a64::x15); // size
      as_->sub(a64::x15, a64::x15, 1); // mask
      as_->ldr(a64::x10, arch::ptr_offset(a64::x2, kUnicodeHashOffset));
      as_->cmn(a64::x10, 1);
      as_->b_eq(slow_path); // hash not cached
      as_->and_(a64::x12, a64::x10, a64::x15); // i = hash & mask
      as_->bind(probe_check);
      as_->add(a64::x14, a64::x13, a64::x12);
      as_->ldrsb(
          a64::w14,
          arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
      as_->cmn(a64::w14, 1);
      as_->b_eq(hit); // DKIX_EMPTY: absent
      as_->tbnz(a64::w14, 31, slow_path); // DKIX_DUMMY: keep probing in C++
      // occupied: entries start after the index table (size bytes).
      as_->add(a64::x14, a64::x14, a64::x14); // ix*2
      as_->lsl(a64::x14, a64::x14, 3); // ix*16
      as_->add(a64::x14, a64::x14, a64::x13);
      as_->add(a64::x14, a64::x14, a64::x15); // + mask (size - 1)
      as_->ldr(
          a64::x14, arch::ptr_offset(a64::x14, kDkIndicesOffset + 1)); // me_key
      as_->cmp(a64::x14, a64::x2);
      as_->b_eq(slow_path); // shadowed by the instance
      // Non-identical key: equal iff __eq__ says so, which only the
      // helper can ask; probe on only when the cached hashes differ.
      as_->ldr(a64::x14, arch::ptr_offset(a64::x14, kUnicodeHashOffset));
      as_->cmp(a64::x14, a64::x10);
      as_->b_eq(slow_path); // same hash, non-identical key
      // second step: perturb >>= 5; i = (i*5 + perturb + 1) & mask
      as_->bind(probe2);
      as_->lsr(a64::x10, a64::x10, 5);
      as_->add(a64::x14, a64::x12, a64::x12, a64::lsl(2)); // i*5
      as_->add(a64::x14, a64::x14, a64::x10);
      as_->add(a64::x14, a64::x14, 1);
      as_->and_(a64::x12, a64::x14, a64::x15);
      as_->add(a64::x14, a64::x13, a64::x12);
      as_->ldrsb(
          a64::w14,
          arch::ptr_offset(a64::x14, kDkIndicesOffset, arch::AccessSize::k8));
      as_->cmn(a64::w14, 1);
      as_->b_eq(hit);
      as_->b(slow_path);
    }
    as_->bind(peek_cmp);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x0, off + kPeekKeysOff));
    as_->cmp(a64::x13, a64::x14);
    as_->b_ne(slow_path);
    as_->ldr(a64::x14, arch::ptr_offset(a64::x13, kDkNentriesOffset));
    as_->ldr(a64::x15, arch::ptr_offset(a64::x0, off + kPeekNentriesOff));
    as_->cmp(a64::x14, a64::x15);
    as_->b_ne(slow_path);
    as_->b(hit);
    // a peek entry on a receiver type without a
    // managed dict -- the non-heap types that still carry an instance
    // __dict__ slot (_io.BytesIO / StringIO, the unpickle and logging
    // method receivers), which the fill could not prove shadow-free.
    // The instance dict lives at tp_dictoffset; an instance that never
    // grew one cannot shadow the class-side method, so the entry value
    // stands.  A present dict (or an object-relative offset) keeps the
    // helper's probe.
    as_->bind(non_managed);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x11, kTpDictoffsetOffset));
    as_->cmp(a64::x13, 0);
    as_->b_le(slow_path);
    as_->ldr(a64::x13, a64::ptr(a64::x1, a64::x13));
    as_->cbz(a64::x13, hit);
    as_->b(slow_path);
    as_->bind(not_peek);
    as_->ldr(a64::x9, arch::ptr_offset(a64::x0, off + kValueOff));
    as_->cbz(a64::x9, next_entry);
    as_->ldr(
        a64::w15,
        arch::ptr_offset(a64::x0, off + kKeysVerOff, arch::AccessSize::k32));
    as_->cbz(a64::w15, hit);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kValuesPreheaderOffset));
    as_->cbz(a64::x13, mat_check);
    as_->ldr(a64::x13, arch::ptr_offset(a64::x11, kHtCachedKeysOffset));
    as_->cbz(a64::x13, slow_path);
    as_->ldr(
        a64::w14,
        arch::ptr_offset(a64::x13, kDkVersionOffset, arch::AccessSize::k32));
    as_->cmp(a64::w14, a64::w15);
    as_->b_ne(slow_path);
    as_->b(hit);
  };

  const uint32_t stub_entries =
      static_cast<uint32_t>(jit::LoadMethodCache::numEntries());
  for (uint32_t entry_index = 0; entry_index < stub_entries; ++entry_index) {
    const bool has_next_entry = entry_index + 1 != stub_entries;
    Label next_entry = has_next_entry ? as_->newLabel() : slow_path;
    emit_entry(entry_index, next_entry);
    if (has_next_entry) {
      as_->bind(next_entry);
    }
  }

  // Materialized receiver: no dict cannot shadow; a dict whose keys
  // version equals the entry version proves absence the same way
  // isValidKeysVersion does.  Anything else goes to the helper.
  // Entry state on arrival: x9=entry value, w15=entry keys_version.
  as_->bind(mat_check);
  as_->ldr(a64::x13, arch::ptr_offset(a64::x1, kDictPreheaderOffset));
  as_->cbz(a64::x13, hit);
  as_->ldr(a64::x14, arch::ptr_offset(a64::x13, kMaKeysOffset));
  as_->ldr(
      a64::w14,
      arch::ptr_offset(a64::x14, kDkVersionOffset, arch::AccessSize::k32));
  as_->cmp(a64::w14, a64::w15);
  as_->b_ne(slow_path);

  as_->bind(hit);
  as_->ldr(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  arch::add_immediate(as_, a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x9, kRefcountOffset));
  as_->ldr(a64::x12, arch::ptr_offset(a64::x1, kRefcountOffset));
  arch::add_immediate(as_, a64::x12, a64::x12, 1);
  as_->str(a64::x12, arch::ptr_offset(a64::x1, kRefcountOffset));
  as_->mov(a64::x0, a64::x9);
  // x1 is already obj (self), the second LoadMethodResult member.
  as_->ret(arch::lr);

  as_->bind(slow_path);
  as_->mov(
      arch::reg_scratch_br,
      reinterpret_cast<uint64_t>(jit::LoadMethodCache::lookupHelper));
  as_->br(arch::reg_scratch_br);
#else
  (void)code;
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
#if defined(CINDER_AARCH64) && PY_VERSION_HEX < 0x030C0000
  Label artifact_guarded_entry_label = as_->newLabel();
  auto artifact_entry_cursor = as_->cursor();
  as_->bind(artifact_guarded_entry_label);
  // vectorcall occupies x0-x3. Carry the compilation's stable runtime in x4
  // and tail-branch into static C text. The C entry acquires the owning
  // CompiledFunction pin before calling vectorcall_entry_, so unpublishing
  // from inside the body cannot free the stub or body while either executes.
  as_->mov(a64::x4, reinterpret_cast<uint64_t>(env_.code_rt));
  as_->mov(a64::x16, reinterpret_cast<uint64_t>(&Ci_JitShell311_ArtifactEntry));
  as_->br(a64::x16);
  env_.addAnnotation("Artifact-specific guarded entry", artifact_entry_cursor);
#endif
  bool has_static_entry = hasStaticEntry();
  if (has_static_entry) {
    generateStaticEntryPoint(env_.finish_frame_setup, static_jmp_location);
  }

#if defined(CINDER_AARCH64) && PY_VERSION_HEX < 0x030C0000
  // The generated vectorcall entry is installed directly (no guarded C
  // shell), so it is the only place left to re-prove the per-call
  // invariants the shell owned.  The entry guard: a divert stub (tail jump
  // into the stock interpreter entry with x0-x3 still in raw vectorcall
  // form), a mandatory __code__ identity check, and a tracing + recursion
  // pre-check.  The stub sits before the reentry stub: the reentry-stub to
  // vectorcall-entry distance is the JITRT_CALL_REENTRY_OFFSET hard
  // invariant and nothing may be inserted between them.
  Label entry_guard_divert = as_->newLabel();
  Label entry_guard_cstack_overflow = as_->newLabel();
  const int32_t cstack_tls_offset = Ci_JitShell311_CStackSoftLimitTlsOffset();
  {
    auto divert_cursor = as_->cursor();
    as_->bind(entry_guard_divert);
    as_->mov(a64::x16, reinterpret_cast<uint64_t>(&_PyFunction_Vectorcall));
    as_->br(a64::x16);
    env_.addAnnotation("Entry guard divert", divert_cursor);
    // C-stack overflow stub: raises RecursionError like the shell.
    auto cstack_cursor = as_->cursor();
    as_->bind(entry_guard_cstack_overflow);
    as_->mov(
        a64::x16, reinterpret_cast<uint64_t>(&Ci_JitShell311_CStackOverflow));
    as_->br(a64::x16);
    env_.addAnnotation("Entry guard C-stack stubs", cstack_cursor);
  }
  // Tracing + recursion pre-check, shared by the vectorcall entry and the
  // direct call entry.  Read-only, before any frame is built; only x9/x10
  // clobbered, x0-x3 untouched.  Tracing active or recursion budget about
  // to overflow diverts the whole call: the interpreter carries its own
  // recursion accounting and CheckRecursiveCall semantics, so the failure
  // path needs no bookkeeping on this side.
  auto emit_entry_guard = [&]() {
    as_->mov(
        a64::x9,
        reinterpret_cast<uint64_t>(&ThreadedCompileContext::interpreter()
                                        ->runtime->gilstate.tstate_current));
    as_->ldr(a64::x9, a64::ptr(a64::x9));
    // Hand the thread state to the LIR entry bind (INITIAL_TSTATE_REG) so
    // the frame link does not load it again.
    as_->mov(a64::x11, a64::x9);
    as_->ldr(
        a64::x10,
        arch::ptr_offset(
            a64::x9, static_cast<int32_t>(offsetof(PyThreadState, cframe))));
    // use_tracing is a uint8_t on 3.11 -- must be read as a byte; a
    // 32-bit load drags in neighboring padding and makes the divert
    // unconditionally true.
    static_assert(
        sizeof(reinterpret_cast<_PyCFrame*>(0)->use_tracing) == 1,
        "use_tracing width changed");
    as_->ldrb(
        a64::w10,
        arch::ptr_offset(
            a64::x10,
            static_cast<int32_t>(offsetof(_PyCFrame, use_tracing)),
            arch::AccessSize::k8));
    as_->cbnz(a64::w10, entry_guard_divert);
    // Fail closed when the JIT is paused or disabled (the usable byte
    // follows every JIT state transition) or when the frame evaluator is
    // not ours: another PEP 523 client can take the interpreter's slot
    // without going through our install/remove, so the slot itself is read
    // on every entry, exactly as the shell re-proved it
    // (Ci_EvalHook311_IsInstalled).  One materialized address serves both
    // reads.  x12 is free at the vectorcall entry; x11 carries the thread
    // state.
    {
      // Gate address from the constant pool: one PC-relative load.
      const uint64_t gate =
          reinterpret_cast<uint64_t>(&Ci_JitDirectEntryGate311);
      auto it = env_.const_pool_labels.find(gate);
      if (it == env_.const_pool_labels.end()) {
        it = env_.const_pool_labels.emplace(gate, as_->newLabel()).first;
      }
      as_->ldr(a64::x12, a64::ptr(it->second));
    }
    as_->ldrb(
        a64::w10,
        arch::ptr_offset(
            a64::x12,
            static_cast<int32_t>(
                offsetof(Ci_JitDirectEntryGate311Layout, usable)),
            arch::AccessSize::k8));
    as_->cbz(a64::w10, entry_guard_divert);
    as_->ldr(
        a64::x12,
        arch::ptr_offset(
            a64::x12,
            static_cast<int32_t>(
                offsetof(Ci_JitDirectEntryGate311Layout, eval_frame))));
    as_->ldr(
        a64::x10,
        arch::ptr_offset(
            a64::x9, static_cast<int32_t>(offsetof(PyThreadState, interp))));
    as_->ldr(
        a64::x10,
        arch::ptr_offset(
            a64::x10,
            static_cast<int32_t>(offsetof(PyInterpreterState, eval_frame))));
    as_->cmp(a64::x10, a64::x12);
    as_->b_ne(entry_guard_divert);
    as_->ldr(
        a64::w10,
        arch::ptr_offset(
            a64::x9,
            static_cast<int32_t>(offsetof(PyThreadState, recursion_remaining)),
            arch::AccessSize::k32));
    as_->cmp(a64::w10, 0);
    as_->b_le(entry_guard_divert);
    // C-stack soft limit (the shell's cStackSoftLimitReached311): compare
    // the stack pointer with this thread's limit.  Zero means the thread has
    // not established its limits yet.
    if (cstack_tls_offset >= 0) {
      Label have_limit = as_->newLabel();
      as_->mrs(a64::x10, a64::Predicate::SysReg::kTPIDR_EL0);
      as_->mov(a64::x16, static_cast<uint64_t>(cstack_tls_offset));
      as_->ldr(a64::x10, a64::ptr(a64::x10, a64::x16));
      as_->cbnz(a64::x10, have_limit);
      // First compiled call on this thread: establish its limits (the
      // vectorcall registers are preserved around the C call), then
      // re-read and continue into the function.
      as_->stp(a64::x0, a64::x1, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x2, a64::x3, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x9, a64::x11, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -16));
      as_->mov(a64::x29, a64::sp);
      as_->mov(
          a64::x16,
          reinterpret_cast<uint64_t>(&Ci_JitShell311_InitCStackLimits));
      as_->blr(a64::x16);
      as_->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x9, a64::x11, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x2, a64::x3, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x0, a64::x1, a64::ptr_post(a64::sp, 16));
      as_->mrs(a64::x10, a64::Predicate::SysReg::kTPIDR_EL0);
      as_->mov(a64::x16, static_cast<uint64_t>(cstack_tls_offset));
      as_->ldr(a64::x10, a64::ptr(a64::x10, a64::x16));
      as_->bind(have_limit);
      as_->mov(a64::x16, a64::sp);
      as_->cmp(a64::x16, a64::x10);
      as_->b_lo(entry_guard_cstack_overflow);
    } else {
      as_->stp(a64::x0, a64::x1, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x2, a64::x3, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x9, a64::x11, a64::ptr_pre(a64::sp, -16));
      as_->stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -16));
      as_->mov(a64::x29, a64::sp);
      as_->mov(
          a64::x16,
          reinterpret_cast<uint64_t>(&Ci_JitShell311_CStackSoftLimitReached));
      as_->blr(a64::x16);
      as_->mov(a64::w10, a64::w0);
      as_->ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x9, a64::x11, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x2, a64::x3, a64::ptr_post(a64::sp, 16));
      as_->ldp(a64::x0, a64::x1, a64::ptr_post(a64::sp, 16));
      as_->cbnz(a64::w10, entry_guard_cstack_overflow);
    }
  };
  // Direct call entry: the jump target of a call-site entry-cache hit.  The
  // __code__ identity check is carried by the caller's cached func_code
  // guard, and the argument-count / defaults / kwnames chain is proven once
  // by the cache fill conditions (no varargs or keyword-only parameters,
  // site argument count equal to co_argcount, kwnames NULL).  Tracing,
  // evaluator and recursion semantics are re-proved by the shared entry
  // guard; the frame record is built here and the body joined directly, so
  // a hit pays neither the prologue nor the reentry trampoline.  Sits in
  // the safe zone before the reentry stub, leaving the
  // JITRT_CALL_REENTRY_OFFSET span untouched.
  Label direct_call_entry_label = as_->newLabel();
  {
    auto direct_cursor = as_->cursor();
    as_->bind(direct_call_entry_label);
    emit_entry_guard();
    generateFunctionEntry();
    as_->b(env_.correct_arg_count);
    env_.addAnnotation("Direct-call entry", direct_cursor);
  }
  // Helper reentry (JITRT_CallWithIncorrectArgcount and
  // the keyword path jump to the stub after binding arguments) arrives
  // without the entry guard, so the thread state is loaded on the way in.
  // The stub keeps its fixed JITRT_CALL_REENTRY_OFFSET shape; only its
  // branch target moves to this trampoline in the safe zone.
  Label reentry_tstate_trampoline = as_->newLabel();
  {
    auto trampoline_cursor = as_->cursor();
    as_->bind(reentry_tstate_trampoline);
    as_->mov(
        a64::x11,
        reinterpret_cast<uint64_t>(&ThreadedCompileContext::interpreter()
                                        ->runtime->gilstate.tstate_current));
    as_->ldr(a64::x11, a64::ptr(a64::x11));
    as_->b(env_.correct_arg_count);
    env_.addAnnotation("Reentry tstate trampoline", trampoline_cursor);
  }
#endif

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
#if PY_VERSION_HEX < 0x030C0000
  as_->b(reentry_tstate_trampoline);
#else
  as_->b(env_.correct_arg_count);
#endif
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

#if defined(CINDER_AARCH64) && PY_VERSION_HEX < 0x030C0000
  {
    // __code__ identity check (mandatory under direct entry): 3.11 has no
    // function watcher and a function may replace its __code__ at runtime
    // (networkx argmap lazy compilation is exactly the swap-after-first-call
    // pattern), while the compiled artifact is bound to the compile-time
    // code object.  Compare func->func_code against the baked code pointer
    // and divert the whole call to the stock interpreter entry on mismatch
    // (which then executes the new code correctly).  The bound-args reentry
    // path enters after this check; the outer call has already been checked.
    {
      auto code_check_cursor = as_->cursor();
      as_->ldr(
          a64::x9,
          arch::ptr_offset(
              a64::x0,
              static_cast<int32_t>(offsetof(PyFunctionObject, func_code))));
      as_->mov(a64::x10, reinterpret_cast<uint64_t>(GetFunction()->code.get()));
      as_->cmp(a64::x9, a64::x10);
      as_->b_ne(entry_guard_divert);
      env_.addAnnotation("Code identity check", code_check_cursor);
    }
    // Tracing + recursion pre-check, read-only and before any frame is
    // built (only x9/x10 clobbered, x0-x3 untouched).  Tracing active or
    // recursion budget about to overflow diverts the whole call: the
    // interpreter carries its own recursion accounting and
    // CheckRecursiveCall semantics, so the failure path needs no
    // bookkeeping on this side.
    {
      auto guard_cursor = as_->cursor();
      emit_entry_guard();
      env_.addAnnotation(
          "Entry guard (tracing + recursion precheck)", guard_cursor);
    }
  }
#endif

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
  emitAarch64LoadAttrInvokeStub311(codeholder);
  emitAarch64LoadMethodInvokeStub311(codeholder);
  emitAarch64StoreAttrInvokeStub311(codeholder);
  emitAarch64VcDirectStub311(codeholder);
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
#if defined(CINDER_AARCH64) && PY_VERSION_HEX < 0x030C0000
  artifact_guarded_entry_311_ = static_cast<char*>(code_start_) +
      codeholder.labelOffsetFromBase(artifact_guarded_entry_label);
  direct_call_entry_311_ = static_cast<char*>(code_start_) +
      codeholder.labelOffsetFromBase(direct_call_entry_label);
#endif

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
