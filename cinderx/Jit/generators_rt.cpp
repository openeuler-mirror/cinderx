// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/generators_rt.h"

#include "internal/pycore_frame.h"
#include "internal/pycore_gc.h" // _PyObject_GC_IS_TRACKED()
#include "internal/pycore_genobject.h"
#include "internal/pycore_object.h" // _PyObject_GC_UNTRACK()
#include "internal/pycore_pyerrors.h" // _PyErr_ClearExcState()
#include "internal/pycore_pystate.h" // _PyThreadState_GET()

#include "cinderx/Common/log.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/Jit/generators_mm.h"
#include "cinderx/Jit/tree_iter_state.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <string_view>

namespace jit {

bool g_jitgen_am_send_patched = false;
// the JIT generator and coroutine type objects, cached at
// type initialisation so type checks on the resume path do not chase the
// module-state pointer.
PyTypeObject* g_jitgen_type = nullptr;
PyTypeObject* g_jitcoro_type = nullptr;

// process gate for the direct resume call from JIT code.
uint8_t g_gen_direct_gate = 0;

void updateGenDirectGate() {
#if PY_VERSION_HEX < 0x030C0000
  // Direct resumes bypass the C++ resume path: closed while the am_send
  // slot is patched for an instrumentation deopt or an entry ledger is
  // armed (both need every resume to pass through C++).
  g_gen_direct_gate =
      (!g_jitgen_am_send_patched && !*triggerStatsEntryLedgerEnabledAddress())
      ? 1
      : 0;
#endif
}

bool deopt_jit_gen_with_footer(
    JitGenObject* jit_gen,
    GenDataFooter* gen_footer,
    bool tree_iter_cleared);

PyObject* JitGenObject::yieldFrom() {
  if (gi_frame_state == FRAME_CREATED || FRAME_STATE_FINISHED(gi_frame_state)) {
    return nullptr;
  }
  GenDataFooter* gen_footer = genDataFooter();
  const GenYieldPoint* yield_point = gen_footer->yieldPoint;
  PyObject* yield_from = nullptr;
  if (yield_point) {
    yield_from = yieldFromValue(gen_footer, yield_point);
    Py_XINCREF(yield_from);
  }
  return yield_from;
}

namespace {

const destructor original_gen_dealloc = PyGen_Type.tp_dealloc;
const destructor original_coro_dealloc = PyCoro_Type.tp_dealloc;

bool has_active_tree_iter_state(JitGenObject* gen) {
  if (gen == nullptr) {
    return false;
  }
  return gen->genDataFooter()->tree_iter_state != nullptr;
}

void raise_tree_iter_deopt_blocked() {
  PyErr_SetString(
      PyExc_RuntimeError,
      "cannot deopt a suspended TreeIter state-machine generator");
}

void clearGeneratorCompiledFunction(GenDataFooter* gen_footer) {
#if PY_VERSION_HEX < 0x030C0000
  Py_CLEAR(gen_footer->compiled);
#endif
}

// Reimplementation of CPython's gen_dealloc that uses our custom free-list
// (Ci_free_jit_list_gen) instead of PyObject_GC_Del for memory recycling.
void gen_dealloc_with_custom_free(PyObject* self) {
  // On 3.11 the fast finalizer closes a suspended JIT generator in place
  // and restores its stock type before frame cleanup below.
  JIT_DCHECK(
      PyGen_Check(self) || PyCoro_CheckExact(self) ||
          (PY_VERSION_HEX < 0x030C0000 && Py_IS_TYPE(self, g_jitgen_type)),
      "gen_dealloc_with_custom_free called on a non-generator object");

  auto* gen = reinterpret_cast<PyGenObject*>(self);

  if (_PyObject_GC_IS_TRACKED(self)) {
    _PyObject_GC_UNTRACK(self);
  }

  if (gen->gi_weakreflist != nullptr) {
    PyObject_ClearWeakRefs(self);
  }

  // Re-track so the finalizer can run; it may resurrect the object.
  PyObject_GC_Track(self);
  if (PyObject_CallFinalizerFromDealloc(self)) {
    return;
  }
  JIT_DCHECK(
      PyGen_Check(self) || PyCoro_CheckExact(self),
      "generator finalizer did not restore its stock type");
  PyObject_GC_UnTrack(self);

  JIT_DCHECK(
      !PyAsyncGen_CheckExact(gen),
      "Async generators aren't supported by the JIT");

  if (PyCoro_CheckExact(gen)) {
    Py_CLEAR(reinterpret_cast<PyCoroObject*>(gen)->cr_origin_or_finalizer);
  }

  _PyInterpreterFrame* frame = generatorFrame(gen);
  bool cleared_frame_here = false;
  if (gen->gi_frame_state < FRAME_CLEARED) {
    gen->gi_frame_state = FRAME_CLEARED;
    frame->previous = nullptr;
    _PyFrame_ClearExceptCode(frame);
    _PyErr_ClearExcState(&gen->gi_exc_state);
    cleared_frame_here = true;
  }

  // 3.11 stock gen_dealloc only Py_CLEAR(gi_code) once the frame is
  // already FRAME_CLEARED (finalize/close cleared f_code).  The extra
  // Ci_STACK_CLEAR must run only when *this* path still holds the
  // frame's f_code ref that ClearExceptCode preserved.
#if PY_VERSION_HEX < 0x030C0000
  if (cleared_frame_here) {
    Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
  }
  Py_CLEAR(gen->gi_code);
#else
  Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
#endif

  Py_CLEAR(gen->gi_name);
  Py_CLEAR(gen->gi_qualname);

#ifdef ENABLE_GENERATOR_AWAITER
  Py_CLEAR(gen->gi_ci_awaiter);
#endif

  cinderx::getModuleState()->jit_gen_free_list->free(self);
}

void gen_dealloc_finished_coro_fast(PyObject* self) {
  JIT_DCHECK(
      PyCoro_CheckExact(self),
      "gen_dealloc_finished_coro_fast called on a non-coroutine object");

  auto* gen = reinterpret_cast<PyGenObject*>(self);
  auto* coro = reinterpret_cast<PyCoroObject*>(self);
  JIT_DCHECK(gen->gi_weakreflist == nullptr, "fast path has weakrefs");
  JIT_DCHECK(
      coro->cr_origin_or_finalizer == nullptr,
      "fast path has coroutine origin/finalizer");
  JIT_DCHECK(
      gen->gi_frame_state == FRAME_CLEARED, "fast path frame is not cleared");

  if (_PyObject_GC_IS_TRACKED(self)) {
    _PyObject_GC_UNTRACK(self);
  }

  _PyInterpreterFrame* frame = generatorFrame(gen);
  JIT_DCHECK(frame->previous == nullptr, "fast path frame is linked");

  Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
  Py_CLEAR(gen->gi_name);
  Py_CLEAR(gen->gi_qualname);

#ifdef ENABLE_GENERATOR_AWAITER
  JIT_DCHECK(gen->gi_ci_awaiter == nullptr, "fast path has awaiter");
#endif

  cinderx::getModuleState()->jit_gen_free_list->free(self);
}

void deopt_finished_coro_object_only(JitGenObject* gen) {
  JIT_DCHECK(
      Py_TYPE(gen) == cinderx::getModuleState()->coro_type,
      "fast path called on a non-JIT coroutine");
  PyTypeObject* old_type = Py_TYPE(gen);
  Py_DECREF(old_type);
  Py_SET_TYPE(reinterpret_cast<PyObject*>(gen), &PyCoro_Type);
}

#if PY_VERSION_HEX < 0x030C0000
bool jitgen_fast_close_eligible(JitGenObject* gen);
#endif

void jitgen_dealloc(PyObject* self) {
  // Clear TreeIter state before deopt so the footer pointer is still valid.
  JitGenObject* jit_gen = JitGenObject::cast(self);
  GenDataFooter* gen_footer = nullptr;
  bool fast_finished_coro = false;
  bool had_tree_iter = false;
  if (jit_gen != nullptr) {
    gen_footer = jit_gen->genDataFooter();
    had_tree_iter = gen_footer->tree_iter_state != nullptr;
#if PY_VERSION_HEX >= 0x030E0000
    // Older frame layouts need the generic deopt path's reified-frame cleanup.
    fast_finished_coro =
        Py_IS_TYPE(self, cinderx::getModuleState()->coro_type) &&
        gen_footer->yieldPoint == nullptr &&
        gen_footer->tree_iter_state == nullptr &&
        jit_gen->gi_frame_state == FRAME_CLEARED &&
        jit_gen->gi_weakreflist == nullptr &&
        reinterpret_cast<PyCoroObject*>(jit_gen)->cr_origin_or_finalizer ==
            nullptr &&
        generatorFrame(jit_gen)->previous == nullptr;
#endif
    if (!fast_finished_coro) {
      clearTreeIterState(gen_footer);
    }
  }

  if (fast_finished_coro) {
    deopt_finished_coro_object_only(jit_gen);
    gen_dealloc_finished_coro_fast(self);
    return;
  }

#if PY_VERSION_HEX < 0x030C0000
  // a plain generator dropped while suspended at a
  // handler-free plain yield (or never started) runs no user code on close:
  // release the suspend point's references and free it natively, skipping
  // the finalizer protocol, the type switch and the generic dealloc path.
  // (A yield-from suspend point closes its sub-iterator, which may run
  // Python code, so it keeps the finalizer protocol.)
  if (jit_gen != nullptr && !had_tree_iter && !_PyGC_FINALIZED(self) &&
      jitgen_fast_close_eligible(jit_gen) &&
      !gen_footer->yieldPoint->isYieldFrom()) {
    if (_PyObject_GC_IS_TRACKED(self)) {
      _PyObject_GC_UNTRACK(self);
    }
    if (jit_gen->gi_weakreflist != nullptr) {
      PyObject_ClearWeakRefs(self);
    }
    const GenYieldPoint* yield_point = gen_footer->yieldPoint;
    const DeoptMetadata& deopt_meta =
        gen_footer->code_rt->getDeoptMetadata(yield_point->deoptIdx());
    // Releasing the live values runs their __del__ methods: read as running
    // and unresumable first (see jitgen_try_fast_close).
    jit_gen->gi_frame_state = FRAME_EXECUTING;
    gen_footer->direct_ok = 0;
    gen_footer->yieldPoint = nullptr;
    releaseRefs(deopt_meta, gen_footer);
    _PyInterpreterFrame* frame = generatorFrame(jit_gen);
    frame->previous = nullptr;
    jit_gen->gi_frame_state = FRAME_CLEARED;
    jitFrameClearExceptCode(frame);
    _PyErr_ClearExcState(&jit_gen->gi_exc_state);
    Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
    clearGeneratorCompiledFunction(gen_footer);
    Py_CLEAR(jit_gen->gi_code);
    Py_CLEAR(jit_gen->gi_name);
    Py_CLEAR(jit_gen->gi_qualname);
    PyTypeObject* type = Py_TYPE(self);
    cinderx::getModuleState()->jit_gen_free_list->free(self);
    Py_DECREF(type);
    return;
  }
#endif
  bool deopted = true;
  if (jit_gen == nullptr) {
    deopted = deopt_jit_gen(self);
  } else if (
      gen_footer->yieldPoint == nullptr &&
      FRAME_STATE_FINISHED(jit_gen->gi_frame_state)) {
#if PY_VERSION_HEX < 0x030C0000
    // a completed plain generator whose frame was cleared at
    // completion holds only gi_code/gi_name/gi_qualname and its heap type:
    // release them and hand the storage back without the stock finalizer
    // protocol (a finished generator's finalizer is a no-op).
    if (jit_gen->gi_frame_state == FRAME_CLEARED &&
        Py_IS_TYPE(self, g_jitgen_type)) {
      if (_PyObject_GC_IS_TRACKED(self)) {
        _PyObject_GC_UNTRACK(self);
      }
      if (jit_gen->gi_weakreflist != nullptr) {
        PyObject_ClearWeakRefs(self);
      }
      clearGeneratorCompiledFunction(gen_footer);
      Py_CLEAR(jit_gen->gi_code);
      Py_CLEAR(jit_gen->gi_name);
      Py_CLEAR(jit_gen->gi_qualname);
      _PyErr_ClearExcState(&jit_gen->gi_exc_state);
      PyTypeObject* type = Py_TYPE(self);
      cinderx::getModuleState()->jit_gen_free_list->free(self);
      Py_DECREF(type);
      return;
    }
#endif
    deopt_jit_gen_object_only(jit_gen);
    clearGeneratorCompiledFunction(gen_footer);
  } else {
#if PY_VERSION_HEX < 0x030C0000
    // a generator dropped while suspended outside every
    // handler needs no interpreter unwinding.  Leave it a JitGen so the
    // finalizer (run by gen_dealloc_with_custom_free under the resurrection
    // protocol, since closing a sub-iterator may run Python code) performs
    // the in-place fast close; anything else takes the deopt path.
    if (!had_tree_iter && !_PyGC_FINALIZED(self) &&
        jitgen_fast_close_eligible(jit_gen)) {
      deopted = true;
    } else {
      deopted = deopt_jit_gen_with_footer(
          jit_gen, gen_footer, true /* tree_iter_cleared */);
    }
#else
    deopted = deopt_jit_gen_with_footer(
        jit_gen, gen_footer, true /* tree_iter_cleared */);
#endif
  }
  if (!deopted) {
    JIT_ABORT("Tried to dealloc a running JIT generator");
  }

  gen_dealloc_with_custom_free(self);
}

int jitgen_traverse(PyObject* obj, visitproc visit, void* arg) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen != nullptr) {
    const GenDataFooter* gen_footer = jit_gen->genDataFooter();
#if PY_VERSION_HEX < 0x030C0000
    Py_VISIT(reinterpret_cast<PyObject*>(gen_footer->compiled));
#endif
    // Only visit JIT-specific live values if we have a valid yield point.
    // If yieldPoint is null, the generator hasn't yielded yet or has completed,
    // but we still need to call PyGen_Type.tp_traverse below to visit standard
    // generator references (gi_code, gi_frame, etc.).
    if (gen_footer->yieldPoint != nullptr) {
      size_t deopt_idx = gen_footer->yieldPoint->deoptIdx();
      const DeoptMetadata& meta =
          gen_footer->code_rt->getDeoptMetadata(deopt_idx);
      for (const LiveValue& value : meta.live_values) {
        if (value.ref_kind != hir::RefKind::kOwned) {
          continue;
        }
        codegen::PhyLocation loc = value.location;
        JIT_CHECK(
            !loc.is_register(),
            "DeoptMetadata for Yields should not reference registers");
        PyObject* v = *reinterpret_cast<PyObject**>(
            reinterpret_cast<uintptr_t>(gen_footer) + loc.loc);
        Py_VISIT(v);
      }
      JIT_CHECK(JitGen_CheckAny(obj), "Deopted during GC traversal");
    }

    // Visit TreeIter state machine owned references if present.
    if (int r = visitTreeIterState(gen_footer->tree_iter_state, visit, arg)) {
      return r;
    }

#if PY_VERSION_HEX < 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    // In lightweight frame mode, frame->f_funcobj is set to a reifier singleton
    // rather than the actual function. The real function is stored in the
    // FrameHeader and contains func_closure with closure cells that may
    // participate in reference cycles. We must visit it explicitly since
    // _PyFrame_Traverse won't see it.
    if (getConfig().frame_mode == FrameMode::kLightweight &&
        jit_gen->gi_frame_state < FRAME_CLEARED) {
      _PyInterpreterFrame* frame = generatorFrame(jit_gen);
      BorrowedRef<PyFunctionObject> func = jitFrameGetFunction(frame);
      Py_VISIT(func.get());
    }
#endif
  }
  // Try to use CPython traverse as much as we can as it has internals which
  // are hard to borrow in 3.14 (compares 'visit' to a specific internal
  // function).
  return PyGen_Type.tp_traverse(obj, visit, arg);
}

void raise_already_running_exception(JitGenObject* jit_gen) {
  // If the executor is running we cannot deopt so have to replicate the
  // errors from CPython here.
  const char* msg = "generator already executing";
  if (Py_TYPE(jit_gen) == cinderx::getModuleState()->coro_type) {
    msg = "coroutine already executing";
  }
  PyErr_SetString(PyExc_ValueError, msg);
}

// Resumes a JIT generator. Calling this performs the same work as invoking the
// interpreter on a generator with a freshly created/suspended frame. As much
// as possible is broken out into C++ before control is passed to JIT code.
Ref<> send_core(
    JitGenObject* jit_gen,
    PyObject* arg,
    PyThreadState* tstate,
    GenDataFooter* gen_footer,
    uint64_t finish_yield_from = 0) {
  PyObject* gen_obj = reinterpret_cast<PyObject*>(jit_gen);

  _PyInterpreterFrame* frame = generatorFrame(jit_gen);
  // See comment about reusing the cframe in jit_rt.cpp,
  // allocate_and_link_interpreter_frame().

  constexpr bool kGenProtoPossible = PY_VERSION_HEX < 0x030C0000;
  constexpr bool genproto = kGenProtoPossible;
  if (!genproto) {
    frame->previous = currentFrame(tstate);
    setCurrentFrame(tstate, frame);
  }

  // Enter generated code.
  JIT_DCHECK(
      gen_footer->yieldPoint != nullptr,
      "Attempting to resume a generator with no yield point");
#if PY_VERSION_HEX < 0x030C0000
  // Generator resume enters machine code without passing through
  // JITRT_AllocateAndLinkInterpreterFrame(), the ordinary 3.11 counting
  // point.  Count at this unique resume dispatch so canary evidence covers
  // both the initial next() and later send()/throw()/close() resumes.
  if (genproto) {
    // the machine code counts the entry; only the ledger
    // is fed from here.
    if (*triggerStatsEntryLedgerEnabledAddress()) {
      triggerStatsOnMachineCodeEntryLedger(_PyFrame_GetCode(frame));
    }
  } else {
    jit::triggerStatsOnMachineCodeEntry(_PyFrame_GetCode(frame));
  }
#endif
  Ref<> result = Ref<>::steal(gen_footer->resumeEntry(
      gen_obj,
      arg,
      finish_yield_from /* consumed by 3.11 JITRT_GenSend */,
      tstate));

  // If we deopted then the interpreter will handle setting frame state and
  // there will no longer be any JIT state. We can check if this happened by
  // seeing if the type of the generator object is no longer a JitGenObject.
  if (JitGen_CheckAny(gen_obj)) {
#if PY_VERSION_HEX >= 0x030C0000
    tstate->exc_info = jit_gen->gi_exc_state.previous_item;
    jit_gen->gi_exc_state.previous_item = nullptr;
#endif
    // (3.11 restores the exception-stack link in the resume wrapper, on every
    // exit path.)
    if (genproto && jit_gen->gi_frame_state == FRAME_SUSPENDED) {
      // a yield exit already unlinked and marked suspended.
      return result;
    }
    setCurrentFrame(tstate, frame->previous);

    frame->previous = nullptr;
    if (FRAME_STATE_FINISHED(jit_gen->gi_frame_state)) {
      jit_gen->gi_frame_state = FRAME_CLEARED;
      jitFrameClearExceptCode(frame);
    } else {
#if PY_VERSION_HEX >= 0x030E0000
      jit_gen->gi_frame_state = gen_footer->yieldPoint->isYieldFrom()
          ? FRAME_SUSPENDED_YIELD_FROM
          : FRAME_SUSPENDED;
#else
      jit_gen->gi_frame_state = FRAME_SUSPENDED;
#endif
    }
  }
#if PY_VERSION_HEX < 0x030C0000
  else {
    // An in-stack deopt converted this generator to stock PyGen/PyCoro while
    // its machine code was running; the interpreter finished the frame and
    // left its back link in place, as it does for gen_send_ex2, whose caller
    // then clears it.  This resume is that caller.
    setCurrentFrame(tstate, frame->previous);
    frame->previous = nullptr;
    // The deopt trampoline still consumes this
    // artifact's metadata and epilogue after prepareForDeopt(), so the handler
    // cannot release the MakeGen pin. The resume call has now fully left
    // machine code, and stock traversal/deallocation will never visit the
    // footer, making this the final safe point to balance that pin.
    clearGeneratorCompiledFunction(gen_footer);
  }
#endif

  return result;
}

// This is a cut down version of gen_send_ex2() from genobject.c which only
// handles sending in values, and calls send_core() above to dispatch to a
// JIT function rather than executing with the interpreter. If any of the
// inputs would lead to an exception, try to deopt and hand back to the CPython
// version.
PySendResult jitgen_resume_machine_code(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate,
    GenDataFooter* footer,
    uint64_t finish_yield_from);
PySendResult jitgen_am_send_impl(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate);
#if PY_VERSION_HEX < 0x030C0000
PySendResult jitgen_resume_fast(
    JitGenObject* gen,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate,
    bool plain_gen);
#endif
PySendResult jitgen_resume_post(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject* result,
    PyObject** presult,
    PyThreadState* tstate,
    _PyErr_StackItem* prev_exc_info);

#if PY_VERSION_HEX < 0x030C0000
// send into the sub-iterator of a delegating generator directly.
// The delegating generator is marked running and its frame and exception
// state are linked exactly as a machine-code resume would do, so re-entrancy
// raises "generator already executing" and frame/exception chains stay intact.
// A yielded value is handed straight to the caller; a return value or an
// exception resumes the delegating generator at its Send with the finish flag.
PySendResult jitgen_delegate_send(
    JitGenObject* gen,
    PyObject* obj,
    GenDataFooter* footer,
    JitGenObject* sub,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate) {
  _PyInterpreterFrame* frame = generatorFrame(gen);
  frame->previous = currentFrame(tstate);
  setCurrentFrame(tstate, frame);
  _PyErr_StackItem* prev_exc_info = tstate->exc_info;
  gen->gi_exc_state.previous_item = prev_exc_info;
  tstate->exc_info = &gen->gi_exc_state;
  gen->gi_frame_state = FRAME_EXECUTING;
  // Not directly resumable while it delegates (see linkDelegator).
  footer->direct_ok = 0;

  PyObject* sub_result = nullptr;
  // the sub-iterator is a suspended plain JitGen by
  // construction, so it qualifies for the straight-line lane.
  PySendResult status = jitgen_resume_fast(sub, arg, &sub_result, tstate, true);

  gen->gi_frame_state = FRAME_SUSPENDED;
  footer->direct_ok = 2;
  tstate->exc_info = prev_exc_info;
  gen->gi_exc_state.previous_item = nullptr;
  setCurrentFrame(tstate, frame->previous);
  frame->previous = nullptr;

  if (status == PYGEN_NEXT) {
    *presult = sub_result;
    return PYGEN_NEXT;
  }
  // Finished (sub_result owned) or raised (exception set): resume the
  // delegating generator at its Send with the outcome.
  PySendResult res = jitgen_resume_machine_code(
      gen,
      obj,
      status == PYGEN_RETURN ? sub_result : Py_None,
      presult,
      tstate,
      footer,
      status == PYGEN_RETURN ? 1 : 2);
  Py_XDECREF(sub_result);
  return res;
}
#endif

#if PY_VERSION_HEX < 0x030C0000
// the common resume in one straight sequence.  Callers
// guarantee an unpatched JitGen/JitCoro in state SUSPENDED, or CREATED with
// a None (or absent) argument; plain_gen says it is the plain generator
// type.  Yield-from delegation and the rare completion/raise outcomes fall
// through to the shared routines.
PySendResult jitgen_resume_fast(
    JitGenObject* gen,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate,
    bool plain_gen) {
  PyObject* obj = reinterpret_cast<PyObject*>(gen);
  if (arg == nullptr) {
    arg = Py_None;
  }
  GenDataFooter* footer = gen->genDataFooter();
  const GenYieldPoint* yield_point = footer->yieldPoint;
  if (plain_gen && gen->gi_frame_state == FRAME_SUSPENDED &&
      yield_point != nullptr && yield_point->isYieldFrom() &&
      footer->tree_iter_state == nullptr) {
    PyObject* yf = yieldFromValue(footer, yield_point);
    if (yf != nullptr && Py_IS_TYPE(yf, g_jitgen_type) &&
        reinterpret_cast<JitGenObject*>(yf)->gi_frame_state ==
            FRAME_SUSPENDED) {
      return jitgen_delegate_send(
          gen,
          obj,
          footer,
          reinterpret_cast<JitGenObject*>(yf),
          arg,
          presult,
          tstate);
    }
  }
  _PyErr_StackItem* prev_exc_info = tstate->exc_info;
  _PyInterpreterFrame* frame = generatorFrame(gen);
  // the resume entry links the exception state and the
  // frame and marks the generator running; a plain or yield-from exit
  // unlinks and marks it suspended.  Only the entry ledger is fed here.
  if (*triggerStatsEntryLedgerEnabledAddress()) {
    triggerStatsOnMachineCodeEntryLedger(_PyFrame_GetCode(frame));
  }
  PyObject* result = footer->resumeEntry(obj, arg, 0, tstate);
  if (__builtin_expect(
          result != nullptr && JitGen_CheckAny(obj) &&
              gen->gi_frame_state == FRAME_SUSPENDED,
          1)) {
    *presult = result;
    return PYGEN_NEXT;
  }
  if (JitGen_CheckAny(obj)) {
    setCurrentFrame(tstate, frame->previous);
    frame->previous = nullptr;
    if (FRAME_STATE_FINISHED(gen->gi_frame_state)) {
      gen->gi_frame_state = FRAME_CLEARED;
      jitFrameClearExceptCode(frame);
    } else {
      gen->gi_frame_state = FRAME_SUSPENDED;
    }
  } else {
    // Deopted in place: the interpreter ran the frame and left the back
    // link for its caller to clear (gen_send_ex2 semantics).
    setCurrentFrame(tstate, frame->previous);
    frame->previous = nullptr;
    clearGeneratorCompiledFunction(footer);
  }
  return jitgen_resume_post(
      gen, obj, arg, result, presult, tstate, prev_exc_info);
}
#endif

PySendResult jitgen_am_send_impl(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate) {
  // A finalizer can re-enter a generator while its return-path refcount
  // cleanup is still running.  At that point the generator is already
  // logically complete, but deopting it would race the cleanup that is
  // releasing JIT-owned live values.
  if (FRAME_STATE_FINISHED(gen->gi_frame_state) &&
      Py_TYPE(obj) == g_jitgen_type) {
    *presult = Py_NewRef(Py_None);
    return PYGEN_RETURN;
  }

  // Check for user programming errors.
  if (gen->gi_frame_state >= FRAME_EXECUTING ||
      (gen->gi_frame_state == FRAME_CREATED && arg && !Py_IsNone(arg))) {
    // Try to deopt to easily reproduce CPython errors.
    if (!deopt_jit_gen(obj)) {
      raise_already_running_exception(gen);
      *presult = nullptr;
      return PYGEN_ERROR;
    }
    return Py_TYPE(obj)->tp_as_async->am_send(obj, arg, presult);
  }

  if (arg == nullptr) {
    arg = Py_None;
  }

  GenDataFooter* footer = gen->genDataFooter();
#if PY_VERSION_HEX < 0x030C0000
  // a generator suspended at a yield-from whose sub-iterator is
  // a suspended JIT generator forwards the send without re-entering its own
  // machine code; only the sub-iterator's completion resumes it.
  if (Py_IS_TYPE(obj, g_jitgen_type) &&
      gen->gi_frame_state == FRAME_SUSPENDED) {
    const GenYieldPoint* yield_point = footer->yieldPoint;
    if (yield_point != nullptr && yield_point->isYieldFrom() &&
        footer->tree_iter_state == nullptr) {
      PyObject* yf = yieldFromValue(footer, yield_point);
      if (yf != nullptr && Py_IS_TYPE(yf, g_jitgen_type) &&
          reinterpret_cast<JitGenObject*>(yf)->gi_frame_state ==
              FRAME_SUSPENDED) {
        return jitgen_delegate_send(
            gen,
            obj,
            footer,
            reinterpret_cast<JitGenObject*>(yf),
            arg,
            presult,
            tstate);
      }
    }
  }
#endif

  return jitgen_resume_machine_code(gen, obj, arg, presult, tstate, footer, 0);
}

// Resume the generator's machine code with a (borrowed) sent value.
// finish_yield_from is stored by the resume entry and consumed by the first
// JITRT_GenSend (see GenDataFooter::resume_finish_flag).
// Everything after the machine code returned: exception-stack unlink, the
// completed/raised bookkeeping and the result contract of am_send.  Shared by
// the general wrapper and the fast lane.
PySendResult jitgen_resume_post(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject* result,
    PyObject** presult,
    PyThreadState* tstate,
    _PyErr_StackItem* prev_exc_info);

PySendResult jitgen_resume_machine_code(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject** presult,
    PyThreadState* tstate,
    GenDataFooter* footer,
    uint64_t finish_yield_from) {
  _PyErr_StackItem* prev_exc_info = tstate->exc_info;
  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    gen->gi_exc_state.previous_item = prev_exc_info;
    tstate->exc_info = &gen->gi_exc_state;
    gen->gi_frame_state = FRAME_EXECUTING;
  }
  EVAL_CALL_STAT_INC(EVAL_CALL_GENERATOR);

  // Execution happens here
  PyObject* result =
      send_core(gen, arg, tstate, footer, finish_yield_from).release();
  return jitgen_resume_post(
      gen, obj, arg, result, presult, tstate, prev_exc_info);
}

PySendResult jitgen_resume_post(
    JitGenObject* gen,
    PyObject* obj,
    PyObject* arg,
    PyObject* result,
    PyObject** presult,
    PyThreadState* tstate,
    _PyErr_StackItem* prev_exc_info) {
#if PY_VERSION_HEX < 0x030C0000
  // The vendored 3.11 evaluator returns from an entry generator frame
  // without restoring the exception-stack link that gen_send_ex2 owns.
  // send_core restores it while the object remains a JitGen, but an
  // exception deopt changes the object to stock PyGen_Type first.  Restore
  // unconditionally at this owner boundary or the next generator chains its
  // exception state through the freed predecessor object.
  tstate->exc_info = prev_exc_info;
  gen->gi_exc_state.previous_item = nullptr;

  if (gen->gi_frame_state == FRAME_EXECUTING) {
    // Exception exits skip the Return epilogue that stores COMPLETED.
    // send_core only updates state while the object is still a JitGen;
    // a deopt mid-raise would otherwise leave FRAME_EXECUTING.
#if PY_VERSION_HEX >= 0x030F0000
    gen->gi_frame_state = FRAME_CLEARED;
#else
    gen->gi_frame_state = FRAME_COMPLETED;
#endif
  }
#endif

  JIT_DCHECK(tstate->exc_info == prev_exc_info, "Invalid exc_info");
  JIT_DCHECK(gen->gi_exc_state.previous_item == nullptr, "Invalid exc_state");
  JIT_DCHECK(gen->gi_frame_state != FRAME_EXECUTING, "Invalid frame state");
  _PyInterpreterFrame* frame = generatorFrame(gen);
  JIT_DCHECK(
      JitGen_CheckAny(obj) || frame->previous == nullptr,
      "Previous frame still linked");

  /* If the generator just returned (as opposed to yielding), signal
   * that the generator is exhausted. */
  if (result) {
    if (!FRAME_STATE_FINISHED(gen->gi_frame_state)) {
      *presult = result;
      return PYGEN_NEXT;
    }

    JIT_DCHECK(
        Py_IsNone(result) || !PyAsyncGen_CheckExact(gen), "Invalid result");
    if (Py_IsNone(result) && !PyAsyncGen_CheckExact(gen) && !arg) {
      /* Return NULL if called by gen_iternext() */
      Py_CLEAR(result);
    }

  } else {
#if PY_VERSION_HEX < 0x030C0000
    // PEP 479: a StopIteration that bubbles out of the generator body is
    // converted at this boundary.  CPython does the same in gen_send_ex2;
    // JIT-compiled RAISE never goes through the eval loop that used to
    // be the other conversion site.
    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
      const char* msg = "generator raised StopIteration";
      if (PyCoro_CheckExact(obj) ||
          Py_TYPE(obj) == cinderx::getModuleState()->coro_type) {
        msg = "coroutine raised StopIteration";
      } else if (PyAsyncGen_CheckExact(obj)) {
        msg = "async generator raised StopIteration";
      }
      _PyErr_FormatFromCause(PyExc_RuntimeError, "%s", msg);
    } else if (
        PyAsyncGen_CheckExact(obj) &&
        PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
      _PyErr_FormatFromCause(
          PyExc_RuntimeError, "async generator raised StopAsyncIteration");
    }
#else
    JIT_DCHECK(
        !PyErr_ExceptionMatches(PyExc_StopIteration),
        "Generator should not raise StopIteration");
    JIT_DCHECK(
        !PyAsyncGen_CheckExact(gen) ||
            !PyErr_ExceptionMatches(PyExc_StopAsyncIteration),
        "Async gen should not raise StopAsyncIteration");
#endif
  }

#ifdef ENABLE_GENERATOR_AWAITER
  Py_CLEAR(gen->gi_ci_awaiter);
#endif

#if PY_VERSION_HEX < 0x030C0000
  _PyErr_ClearExcState(&gen->gi_exc_state);
  if (!JitGen_CheckAny(obj)) {
    // Match CPython 3.11 gen_send_ex2's completed-generator ownership
    // boundary after a deopt changed the object to stock PyGen_Type.  A
    // still-JIT generator has a separate generated live-value cleanup and
    // must not be cleared a second time here.
    gen->gi_frame_state = FRAME_CLEARED;
    frame->previous = nullptr;
    _PyFrame_Clear(frame);
  }
#elif PY_VERSION_HEX < 0x030E0000
  _PyErr_ClearExcState(&gen->gi_exc_state);
#else
  JIT_DCHECK(
      gen->gi_exc_state.exc_value == nullptr,
      "Should not have an exception by now");
#endif
  JIT_DCHECK(gen->gi_frame_state == FRAME_CLEARED, "Frame not cleared");

#if PY_VERSION_HEX < 0x030C0000
  if (JitGen_CheckAny(obj)) {
    // All JIT state has now been consumed.  Convert before closing the code
    // owner so later stock deallocation never has to locate the footer through
    // FRAME_EXECUTABLE.  jitFrameClearExceptCode() already cleared the rest of
    // the frame, so only the preserved executable remains to be released.
    GenDataFooter* gen_footer = gen->genDataFooter();
    // The footer is keyed on gi_code, so a finished generator stays a
    // JitGen and takes the native dealloc path.
    clearGeneratorCompiledFunction(gen_footer);
    Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
  }
#endif

  *presult = result;
  return result ? PYGEN_RETURN : PYGEN_ERROR;
}

PySendResult jitgen_am_send(PyObject* obj, PyObject* arg, PyObject** presult) {
  JitGenObject* gen = JitGenObject::cast(obj);
  if (gen == nullptr) {
    return Py_TYPE(obj)->tp_as_async->am_send(obj, arg, presult);
  }
  return jitgen_am_send_impl(gen, obj, arg, presult, PyThreadState_Get());
}

// Wrapper installed as am_send when the JIT is paused (e.g. instrumentation
// active). Deopts the generator so it resumes in interpreter mode with proper
// monitoring support, then delegates to the (now CPython) type's am_send.
PySendResult
jitgen_am_send_with_deopt(PyObject* obj, PyObject* arg, PyObject** presult) {
  if (deopt_jit_gen(obj)) {
    return Py_TYPE(obj)->tp_as_async->am_send(obj, arg, presult);
  }
  // Generator is FRAME_EXECUTING and can't be deopted right now.
  return jitgen_am_send(obj, arg, presult);
}

PyObject* jitgen_send(PyObject* obj, PyObject* arg) {
  PyObject* result = nullptr;
  if (Py_TYPE(obj)->tp_as_async->am_send(obj, arg, &result) == PYGEN_RETURN) {
    if (result == Py_None) {
      PyErr_SetNone(PyExc_StopIteration);
    } else {
      _PyGen_SetStopIterationValue(result);
    }
    Py_CLEAR(result);
  }
  return result;
}

PyObject* jitgen_iternext(PyObject* obj) {
  PyObject* result = nullptr;
  PySendResult status;
  // an unpatched JitGen resumes through the direct lane,
  // skipping the am_send slot dispatch and the thread-state call.
  PyTypeObject* type = Py_TYPE(obj);
  bool plain_gen = type == g_jitgen_type;
  if (!g_jitgen_am_send_patched && (plain_gen || type == g_jitcoro_type)) {
    JitGenObject* gen = reinterpret_cast<JitGenObject*>(obj);
#if PY_VERSION_HEX < 0x030C0000
    int8_t state = gen->gi_frame_state;
    if (state == FRAME_SUSPENDED || state == FRAME_CREATED) {
      status = jitgen_resume_fast(
          gen, nullptr, &result, _PyThreadState_GET(), plain_gen);
    } else
#endif
    {
      status =
          jitgen_am_send_impl(gen, obj, nullptr, &result, _PyThreadState_GET());
    }
  } else {
    status = type->tp_as_async->am_send(obj, nullptr, &result);
  }
  if (status == PYGEN_RETURN) {
    if (result != Py_None) {
      _PyGen_SetStopIterationValue(result);
    }
    Py_CLEAR(result);
  }
  return result;
}

#if PY_VERSION_HEX < 0x030C0000
// finishes a direct resume from JIT code whose outcome was
// not a plain yield: a yield-from suspension (already unlinked by the machine
// code), a return, a raise, or a resume that deopted in place.  Same contract
// as tp_iternext: the value, or NULL with StopIteration/error set.
// Shared tail of a direct machine-code resume whose outcome was not a plain
// yield: returns the send status and the (owned) value/return value.  `arg`
// selects the post convention: nullptr (tp_iternext: a None return becomes
// NULL) or Py_None (send: the return value stays an object).
PySendResult finishDirectResume(
    JitGenObject* gen,
    PyObject* value,
    PyObject** presult,
    PyThreadState* tstate,
    PyObject* arg) {
  PyObject* obj = reinterpret_cast<PyObject*>(gen);
  _PyInterpreterFrame* frame = generatorFrame(gen);
  _PyErr_StackItem* prev_exc_info = gen->gi_exc_state.previous_item;
  GenDataFooter* footer = gen->genDataFooter();
  if (JitGen_CheckAny(obj)) {
    if (gen->gi_frame_state == FRAME_SUSPENDED) {
      *presult = value;
      return PYGEN_NEXT;
    }
    setCurrentFrame(tstate, frame->previous);
    frame->previous = nullptr;
    if (FRAME_STATE_FINISHED(gen->gi_frame_state)) {
      gen->gi_frame_state = FRAME_CLEARED;
      jitFrameClearExceptCode(frame);
    } else {
      gen->gi_frame_state = FRAME_SUSPENDED;
    }
  } else {
    // Deopted in place: the interpreter ran the frame and left the back
    // link for its caller to clear (gen_send_ex2 semantics).
    setCurrentFrame(tstate, frame->previous);
    frame->previous = nullptr;
    clearGeneratorCompiledFunction(footer);
  }
  return jitgen_resume_post(
      gen, obj, arg, value, presult, tstate, prev_exc_info);
}

PyObject* genNextFinish311(PyObject* obj, PyObject* value) {
  JitGenObject* gen = reinterpret_cast<JitGenObject*>(obj);
  PyThreadState* tstate = _PyThreadState_GET();
  PyObject* result = nullptr;
  PySendResult status =
      finishDirectResume(gen, value, &result, tstate, nullptr);
  if (status == PYGEN_RETURN) {
    if (result != Py_None) {
      _PyGen_SetStopIterationValue(result);
    }
    Py_CLEAR(result);
  }
  return result;
}

// link `gen` as the running delegator of its sub-iterator
// without entering its machine code (the entry half of the resume protocol).
inline void linkDelegator(JitGenObject* gen, PyThreadState* tstate) {
  _PyInterpreterFrame* frame = generatorFrame(gen);
  frame->previous = currentFrame(tstate);
  setCurrentFrame(tstate, frame);
  gen->gi_exc_state.previous_item = tstate->exc_info;
  tstate->exc_info = &gen->gi_exc_state;
  gen->gi_frame_state = FRAME_EXECUTING;
  // A linked delegator is running: the direct paths (JIT loops, walkers)
  // must not take it again while the chain is live, so re-entry goes
  // through am_send and raises "generator already executing".
  gen->genDataFooter()->direct_ok = 0;
}

inline void unlinkDelegator(JitGenObject* gen, PyThreadState* tstate) {
  _PyInterpreterFrame* frame = generatorFrame(gen);
  gen->gi_frame_state = FRAME_SUSPENDED;
  // Every linked delegator was suspended at a yield-from (direct_ok == 2).
  gen->genDataFooter()->direct_ok = 2;
  tstate->exc_info = gen->gi_exc_state.previous_item;
  gen->gi_exc_state.previous_item = nullptr;
  setCurrentFrame(tstate, frame->previous);
  frame->previous = nullptr;
}

// Unlink every delegator recorded from `from` outward (innermost first).
inline void unwindDelegators(JitGenObject* from, PyThreadState* tstate) {
  JitGenObject* level = from;
  while (true) {
    GenDataFooter* footer = level->genDataFooter();
    JitGenObject* d = footer->delegator;
    if (d == nullptr) {
      break;
    }
    footer->delegator = nullptr;
    unlinkDelegator(d, tstate);
    level = d;
  }
}

// The innermost generator finished, raised or deopted: complete each
// delegator's Send with the outcome, outermost last.  Returns the
// top-level outcome with tp_iternext semantics.
PyObject* delegateFinish(
    JitGenObject* innermost,
    PyObject* value,
    PyThreadState* tstate) {
  PyObject* val = nullptr;
  // Send convention: a None return must reach the delegator's Send as an
  // object (the finish flag hands it over as the sub-iterator's value).
  PySendResult st = finishDirectResume(innermost, value, &val, tstate, Py_None);
  JitGenObject* level = innermost;
  while (true) {
    GenDataFooter* footer = level->genDataFooter();
    JitGenObject* d = footer->delegator;
    if (d == nullptr) {
      break;
    }
    footer->delegator = nullptr;
    unlinkDelegator(d, tstate);
    if (st == PYGEN_NEXT) {
      // A level yielded again after completing its Send: the value passes
      // straight through the remaining (now unlinked) delegators.
      level = d;
      continue;
    }
    PyObject* next_val = nullptr;
    PySendResult r = jitgen_resume_machine_code(
        d,
        reinterpret_cast<PyObject*>(d),
        st == PYGEN_RETURN ? val : Py_None,
        &next_val,
        tstate,
        d->genDataFooter(),
        st == PYGEN_RETURN ? 1 : 2);
    Py_XDECREF(val);
    val = next_val;
    st = r;
    level = d;
  }
  if (st == PYGEN_NEXT) {
    return val;
  }
  if (st == PYGEN_RETURN) {
    // tp_iternext contract: a non-None return travels as StopIteration's
    // value (the loop's exhausted path clears it).
    if (val != nullptr && val != Py_None) {
      _PyGen_SetStopIterationValue(val);
    }
  }
  Py_XDECREF(val);
  return nullptr;
}

// Resume a plain JIT generator suspended at a yield-from from a JIT'd loop:
// walk the delegation chain, enter the innermost generator's machine code
// directly, unwind on a yield.  Anything the walker cannot handle falls back
// to the generic iternext after undoing its links.
PyObject* genDelegateNext311(PyObject* top_obj, PyThreadState* tstate) {
  constexpr int kMaxWalkDepth = 64;
  JitGenObject* top = reinterpret_cast<JitGenObject*>(top_obj);
  JitGenObject* cur = top;
  GenDataFooter* fcur = top->genDataFooter();
  int depth = 0;
  while (true) {
    PyObject* sub_obj = fcur->yf_sub;
    JitGenObject* sub = reinterpret_cast<JitGenObject*>(sub_obj);
    GenDataFooter* fsub = nullptr;
    uint8_t ok = 0;
    if (sub_obj != nullptr && Py_IS_TYPE(sub_obj, g_jitgen_type) &&
        fcur->tree_iter_state == nullptr && depth < kMaxWalkDepth) {
      fsub = sub->genDataFooter();
      ok = fsub->direct_ok;
    }
    if (ok == 0) {
      // Not a chain the walker handles: undo the links made so far and let
      // the generic path (yield-from delegation in C++) take it.
      unwindDelegators(cur, tstate);
      return jitgen_iternext(top_obj);
    }
    linkDelegator(cur, tstate);
    fsub->delegator = cur;
    ++depth;
    if (ok == 2) {
      cur = sub;
      fcur = fsub;
      continue;
    }
    // ok == 1: the innermost generator is directly resumable.
    if (*triggerStatsEntryLedgerEnabledAddress()) {
      triggerStatsOnMachineCodeEntryLedger(
          _PyFrame_GetCode(generatorFrame(sub)));
    }
    PyObject* value = fsub->resumeEntry(sub_obj, Py_None, 0, tstate);
    if (value != nullptr && JitGen_CheckAny(sub_obj) && fsub->direct_ok != 0) {
      // Yielded (plain or at a new yield-from): pass it out, unlinking the
      // delegators innermost-first.
      unwindDelegators(sub, tstate);
      return value;
    }
    return delegateFinish(sub, value, tstate);
  }
}
#endif

// Cached methods from base generator filled in by init_jit_genobject_type().
// These are unlikely to be performance sensitive and don't need to run
// particularly fast so we could do dynamic lookups. However, the obvious way
// of doing this is to first deopt and then do a method call. Deopting on throw
// or close isn't too bad, but doing so on __sizeof__() is a bit dubious as the
// generator may end up in the interpreter unnecessarily. So, I made the
// machinery to cache methods anyway and we may as well use it. This does all
// make the assumption that the methods on PyGen_Type don't change.
using GenThrowMeth =
    PyObject* (*)(PyObject* obj, PyObject* const* args, Py_ssize_t nargs);
GenThrowMeth gen_throw_meth;
PyCFunction gen_close_meth;
PyCFunction gen___sizeof___meth;

PyObject* jitgen_throw(PyObject* obj, PyObject* const* args, Py_ssize_t nargs) {
  // Always deopt as an exception being raised internally would cause a JIT
  // generator to deopt anyway.
  // The sub-iterator chain is captured before the deopt, which reifies the
  // frame; stock _PyGen_yf answers equally well afterwards.
  PyObject* sub = JitGen_yf(reinterpret_cast<PyGenObject*>(obj));
  if (!deopt_jit_gen(obj)) {
    Py_XDECREF(sub);
    JitGenObject* gen = reinterpret_cast<JitGenObject*>(obj);
    if (has_active_tree_iter_state(gen)) {
      raise_tree_iter_deopt_blocked();
    } else {
      raise_already_running_exception(gen);
    }
    return nullptr;
  }
  // Stock _gen_throw links a delegator's frame only when the sub-iterator is
  // an exact generator or coroutine; a JIT sub-iterator would be thrown into
  // with its delegator missing from the frame chain (traceback and
  // sys._getframe() would not show it).  Deopt the suspended yield-from
  // chain first so the stock path sees exact types all the way down.
  while (sub != nullptr && JitGen_CheckAny(sub)) {
    PyObject* next = JitGen_yf(reinterpret_cast<PyGenObject*>(sub));
    if (!deopt_jit_gen(sub)) {
      Py_XDECREF(next);
      break;
    }
    Py_DECREF(sub);
    sub = next;
  }
  Py_XDECREF(sub);
  return gen_throw_meth(obj, args, nargs);
}

#if PY_VERSION_HEX < 0x030C0000
// close a JIT generator without deopting when GeneratorExit
// could not run any code in its frame: the suspend point lies outside every
// exception-table range (no try/with/finally around it), so the whole effect
// of close() is releasing the live values and clearing the frame.  A
// delegating generator first closes its sub-iterator, as gen_close does.
// Returns 1 when closed, 0 when not eligible, -1 with an exception set.
// Side-effect-free part of the fast close: true when close() could not run
// any code in this generator's frame.
bool jitgen_fast_close_eligible(JitGenObject* gen) {
  PyObject* obj = reinterpret_cast<PyObject*>(gen);
  if (g_jitgen_am_send_patched || !Py_IS_TYPE(obj, g_jitgen_type) ||
      (gen->gi_frame_state != FRAME_SUSPENDED &&
       gen->gi_frame_state != FRAME_CREATED)) {
    return false;
  }
  GenDataFooter* footer = gen->genDataFooter();
  const GenYieldPoint* yield_point = footer->yieldPoint;
  return yield_point != nullptr && yield_point->handlerFree() &&
      footer->tree_iter_state == nullptr;
}

int jitgen_try_fast_close(JitGenObject* gen) {
  if (!jitgen_fast_close_eligible(gen)) {
    return 0;
  }
  GenDataFooter* footer = gen->genDataFooter();
  const GenYieldPoint* yield_point = footer->yieldPoint;
  // Close the resume entry before anything that can run Python code: the
  // sub-iterator's close() and the release of the live values can reach an
  // object's __del__ that calls next() or close() on this very generator.
  // From here on the generator reads as running, so a re-entry raises
  // "generator already executing" exactly as the interpreter does while
  // gen_close runs, and no direct path resumes it.
  gen->gi_frame_state = FRAME_EXECUTING;
  footer->direct_ok = 0;
  footer->yieldPoint = nullptr;
  int rc = 1;
  if (yield_point->isYieldFrom()) {
    PyObject* yf = yieldFromValue(footer, yield_point);
    if (yf != nullptr) {
      Py_INCREF(yf);
      PyObject* res = nullptr;
      static PyObject* close_str = PyUnicode_InternFromString("close");
      if (JitGen_CheckAny(yf) || PyGen_CheckExact(yf) ||
          PyCoro_CheckExact(yf)) {
        res = PyObject_CallMethodNoArgs(yf, close_str);
      } else {
        PyObject* meth = nullptr;
        if (_PyObject_LookupAttr(yf, close_str, &meth) < 0) {
          PyErr_WriteUnraisable(yf);
        }
        if (meth != nullptr) {
          res = PyObject_CallNoArgs(meth);
          Py_DECREF(meth);
        } else {
          res = Py_NewRef(Py_None);
        }
      }
      Py_DECREF(yf);
      if (res == nullptr) {
        // gen_close throws the sub-iterator's error into the frame.  The
        // suspend point lies outside every handler, so it propagates out of
        // the (now finished) generator: GeneratorExit counts as a completed
        // close and a bare StopIteration becomes RuntimeError (PEP 479).
        if (PyErr_ExceptionMatches(PyExc_GeneratorExit)) {
          PyErr_Clear();
        } else {
          if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
            _PyErr_FormatFromCause(
                PyExc_RuntimeError, "generator raised StopIteration");
          }
          rc = -1;
        }
      } else {
        Py_DECREF(res);
      }
    }
  }
  // Releasing the live values may run __del__ methods; keep the exception
  // being returned (if any) out of their reach.
  PyObject *exc_type, *exc_value, *exc_tb;
  PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
  const DeoptMetadata& deopt_meta =
      footer->code_rt->getDeoptMetadata(yield_point->deoptIdx());
  releaseRefs(deopt_meta, footer);
  _PyInterpreterFrame* frame = generatorFrame(gen);
  frame->previous = nullptr;
  gen->gi_frame_state = FRAME_CLEARED;
  jitFrameClearExceptCode(frame);
  _PyErr_ClearExcState(&gen->gi_exc_state);
  deopt_jit_gen_object_only(gen);
  clearGeneratorCompiledFunction(footer);
  Ci_STACK_CLEAR(frame->FRAME_EXECUTABLE);
  PyErr_Restore(exc_type, exc_value, exc_tb);
  return rc;
}
#endif

PyObject* jitgen_close(PyObject* obj, PyObject*) {
#if PY_VERSION_HEX < 0x030C0000
  if (JitGen_CheckAny(obj)) {
    int fast = jitgen_try_fast_close(reinterpret_cast<JitGenObject*>(obj));
    if (fast > 0) {
      Py_RETURN_NONE;
    }
    if (fast < 0) {
      return nullptr;
    }
  }
#endif
  // Always deopt as closing either raises an exception in the generator which
  // would cause a deopt anyway or if the generator is already done then deopt
  // is cheap and won't rexecute in the interpreter.
  if (!deopt_jit_gen(obj)) {
    JitGenObject* gen = reinterpret_cast<JitGenObject*>(obj);
    if (has_active_tree_iter_state(gen)) {
      raise_tree_iter_deopt_blocked();
    } else {
      raise_already_running_exception(gen);
    }
    return nullptr;
  }
  return gen_close_meth(obj, nullptr);
}

PyObject* jitgen_sizeof(PyObject* obj, PyObject*) {
  Ref<> base_size = Ref<>::steal(gen___sizeof___meth(obj, nullptr));
  if (base_size == nullptr) {
    return nullptr;
  }
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return base_size.release();
  }
  Py_ssize_t base_size_int = PyLong_AsSsize_t(base_size);
  if (base_size_int == -1 && PyErr_Occurred()) {
    return nullptr;
  }
  // +1 word for storing the GenDataFooter pointer
  // +size of the GenDataFooter
  // +the size of the JIT register spill area.
  return PyLong_FromSsize_t(
      base_size_int + sizeof(GenDataFooter*) + sizeof(GenDataFooter) +
      jit_gen->genDataFooter()->spillWords * sizeof(uint64_t));
}

PyObject* jitgen_getyieldfrom(PyObject* obj, void*) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return PyObject_GetAttrString(obj, "gi_yieldfrom");
  }
  PyObject* yield_from = jit_gen->yieldFrom();
  if (yield_from == nullptr) {
    Py_RETURN_NONE;
  }
  return yield_from;
}

PyObject* jitgen_getclass(PyObject*, void*) {
  return Py_NewRef(&PyGen_Type);
}

PyObject* jitcoro_getclass(PyObject*, void*) {
  return Py_NewRef(&PyCoro_Type);
}

void jitgen_finalize(PyObject* obj) {
  PyGenObject* gen = reinterpret_cast<PyGenObject*>(obj);

  // Fast-path: generator has completed so there's nothing to do.
  if (FRAME_STATE_FINISHED(gen->gi_frame_state)) {
    return;
  }

#if PY_VERSION_HEX < 0x030C0000
  if (JitGen_CheckAny(obj)) {
    // Like _PyGen_Finalize: a finalizer runs while the caller may have an
    // exception in flight (a container being released after a failed
    // subscript), and closing a sub-iterator runs Python code.
    PyObject *exc_type, *exc_value, *exc_tb;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    int fast = jitgen_try_fast_close(reinterpret_cast<JitGenObject*>(obj));
    if (fast < 0) {
      // The sub-iterator's close raised; the generator is finished all the
      // same, so report it here as gen_close's caller would.
      PyErr_WriteUnraisable(obj);
    }
    PyErr_Restore(exc_type, exc_value, exc_tb);
    if (fast != 0) {
      return;
    }
  }
#endif
  // Slow-path: generator is still running, so we deopt and defer to runtime
  // logic for raising errors/warnings and possibly closing the generator (which
  // would require a deopt anyway).
  JIT_CHECK(deopt_jit_gen(obj), "Tried to finalize a running JIT generator");
  PyGen_Type.tp_finalize(obj);
}

int jitgen_clear(PyObject* obj) {
  // tp_clear is called by the cyclic GC to break reference cycles.
  // We need to deopt the JIT generator so that the standard generator
  // tp_clear can properly clear all references.
  //
  // If the generator is currently executing (gi_frame_state ==
  // FRAME_EXECUTING), we cannot deopt it. In that case, return 0 without
  // clearing - the GC will try again later or the generator will complete and
  // be collected normally.
  if (!deopt_jit_gen(obj)) {
    return 0;
  }
  // After deopting, the object is now a regular PyGenObject/PyCoroObject.
  // Delegate to the base type's tp_clear to break cycles.
  if (PyGen_Type.tp_clear != nullptr) {
    return PyGen_Type.tp_clear(obj);
  }
  return 0;
}

struct JitCoroWrapper {
  PyObject_HEAD
  PyObject* cw_coroutine;
};

void jitcoro_wrapper_dealloc(JitCoroWrapper* cw) {
  PyObject_GC_UnTrack(reinterpret_cast<PyObject*>(cw));
  Py_CLEAR(cw->cw_coroutine);
  PyObject_GC_Del(cw);
}

PyObject* jitcoro_wrapper_iternext(JitCoroWrapper* cw) {
  return jitgen_iternext(cw->cw_coroutine);
}

PyObject* jitcoro_wrapper_send(JitCoroWrapper* cw, PyObject* arg) {
  return jitgen_send(cw->cw_coroutine, arg);
}

PyObject* jitcoro_wrapper_throw(
    JitCoroWrapper* cw,
    PyObject* const* args,
    Py_ssize_t nargs) {
  return jitgen_throw(cw->cw_coroutine, args, nargs);
}

PyObject* jitcoro_wrapper_close(JitCoroWrapper* cw, PyObject* args) {
  return jitgen_close(cw->cw_coroutine, args);
}

int jitcoro_wrapper_traverse(JitCoroWrapper* cw, visitproc visit, void* arg) {
  Py_VISIT(cw->cw_coroutine);
  return 0;
}

static PyMethodDef jitcoro_wrapper_methods[] = {
    {"send",
     reinterpret_cast<PyCFunction>(jitcoro_wrapper_send),
     METH_O,
     nullptr},
    {"throw", _PyCFunction_CAST(jitcoro_wrapper_throw), METH_FASTCALL, nullptr},
    {"close",
     reinterpret_cast<PyCFunction>(jitcoro_wrapper_close),
     METH_NOARGS,
     nullptr},
    {} /* Sentinel */
};

#ifdef ENABLE_GENERATOR_AWAITER

static void jitocorowrapper_set_awaiter(
    JitCoroWrapper* self,
    PyObject* awaiter) {
  Ci_PyAwaitable_SetAwaiter(self->cw_coroutine, awaiter);
}

static Ci_AsyncMethodsWithExtra jitcorowrapper_as_async = {
    .ame_async_methods = {},
    // Filled in by init_jit_genobject_type()
    .ame_setawaiter =
        reinterpret_cast<setawaiterfunc>(jitocorowrapper_set_awaiter),
};

static PyAsyncMethods* jitcorowrapper_async_methods =
    &jitcorowrapper_as_async.ame_async_methods;

#else

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0

static PyAsyncMethods* jitcorowrapper_async_methods = nullptr;

#endif
} // namespace

PyTypeObject _JitCoroWrapper_Type = {
    PyVarObject_HEAD_INIT(NULL, 0) "coroutine_wrapper",
    sizeof(JitCoroWrapper), /* tp_basicsize */
    0, /* tp_itemsize */
    reinterpret_cast<destructor>(
        jitcoro_wrapper_dealloc), /* destructor tp_dealloc */
    0, /* tp_vectorcall_offset */
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    jitcorowrapper_async_methods, /* tp_as_async */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    nullptr, /* tp_as_mapping */
    nullptr, /* tp_hash */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    PyObject_GenericGetAttr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Ci_TPFLAGS_HAVE_AM_EXTRA, /* tp_flags */
    "A wrapper object implementing __await__ for coroutines.",
    reinterpret_cast<traverseproc>(jitcoro_wrapper_traverse), /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    PyObject_SelfIter, /* tp_iter */
    reinterpret_cast<iternextfunc>(jitcoro_wrapper_iternext), /* tp_iternext */
    jitcoro_wrapper_methods, /* tp_methods */
    nullptr, /* tp_members */
    nullptr, /* tp_getset */
    nullptr, /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    nullptr, /* tp_init */
    nullptr, /* tp_alloc */
    nullptr, /* tp_new */
    nullptr, /* tp_free */
    nullptr, /* tp_is_gc */
    nullptr, /* tp_bases */
    nullptr, /* tp_mro */
    nullptr, /* tp_cache */
    nullptr, /* tp_subclasses */
    nullptr, /* tp_weaklist */
    nullptr, /* tp_del */
    0, /* tp_version_tag */
    nullptr, /* tp_finalize */
};

namespace {
PyObject* jitcoro_await(PyCoroObject* coro) {
  JitCoroWrapper* cw = PyObject_GC_New(JitCoroWrapper, &_JitCoroWrapper_Type);
  if (cw == nullptr) {
    return nullptr;
  }
  cw->cw_coroutine = Py_NewRef(coro);
  PyObject_GC_Track(cw);
  return reinterpret_cast<PyObject*>(cw);
}

// Unfortunately we need to fork this because it uses _PyCoroObject_CAST which
// includes an assert on the exact type of coroutine.
PyObject* jitcoro_repr(PyObject* self) {
  PyCoroObject* coro = reinterpret_cast<PyCoroObject*>(self);
  return PyUnicode_FromFormat(
      "<coroutine object %S at %p>", coro->cr_qualname, coro);
}

static PyMethodDef jitgen_methods[] = {
    {"send", reinterpret_cast<PyCFunction>(jitgen_send), METH_O, nullptr},
    {"throw",
     reinterpret_cast<PyCFunction>(jitgen_throw),
     METH_FASTCALL,
     nullptr},
    {"close",
     reinterpret_cast<PyCFunction>(jitgen_close),
     METH_NOARGS,
     nullptr},
    {"__sizeof__",
     reinterpret_cast<PyCFunction>(jitgen_sizeof),
     METH_NOARGS,
     nullptr},
#if PY_VERSION_HEX >= 0x030E0000
    {"__class_getitem__", nullptr, METH_O | METH_CLASS, nullptr},
#endif
    {} /* Sentinel */
};

// Null fields are copied from the base generator type by
// init_jit_genobject_type(). The order must match the one in genobject.c
static PyGetSetDef jitgen_getsetlist[] = {
    {"__name__", nullptr, nullptr, nullptr},
    {"__qualname__", nullptr, nullptr, nullptr},
    {"gi_yieldfrom", (getter)jitgen_getyieldfrom, nullptr, nullptr},
    {"gi_running", nullptr, nullptr, nullptr},
    {"gi_frame", nullptr, nullptr, nullptr},
    {"gi_suspended", nullptr, nullptr, nullptr},
#if PY_VERSION_HEX >= 0x030C0000
    {"gi_code", nullptr, nullptr, nullptr},
#endif
#if PY_VERSION_HEX >= 0x030F0000
    {"gi_state", nullptr, nullptr, nullptr},
#endif
    {"__class__", jitgen_getclass, nullptr, nullptr},
    {} /* Sentinel */
};

// The fully null entries are filled in by init_jit_genobject_type()
static PyGetSetDef jitcoro_getsetlist[] = {
    {"__name__", nullptr, nullptr, nullptr},
    {"__qualname__", nullptr, nullptr, nullptr},
    {"cr_await", (getter)jitgen_getyieldfrom, nullptr, nullptr},
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
    {"cr_running", (getter)Cix_cr_getrunning, nullptr, nullptr},
#else
    {"cr_running", nullptr, nullptr, nullptr},
#endif
    {"cr_frame", nullptr, nullptr, nullptr},
#if PY_VERSION_HEX >= 0x030C0000
    {"cr_code", nullptr, nullptr, nullptr},
#endif
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
    {"cr_suspended", (getter)Cix_cr_getsuspended, nullptr, nullptr},
#else
    {"cr_suspended", nullptr, nullptr, nullptr},
#endif
#if PY_VERSION_HEX >= 0x030F0000
    {"cr_state", nullptr, nullptr, nullptr},
#endif
#ifdef ENABLE_GENERATOR_AWAITER
    {"cr_ci_awaiter", nullptr, nullptr, nullptr},
    {"cr_awaiter", nullptr, nullptr, nullptr},
#endif
    {"__class__", jitcoro_getclass, nullptr, nullptr},
    {} /* Sentinel */
};

#if PY_VERSION_HEX < 0x030C0000
// Stock 3.11 exposes gi_code as a member, not a getset.  The 3.12+
// getset over the frame executable is gated out of jitgen_getsetlist
// on this version, so the member has to live here or g.gi_code raises.
static PyMemberDef jitgen_memberlist[] = {
    {"gi_code", T_OBJECT, offsetof(PyGenObject, gi_code), READONLY},
    {} /* Sentinel */
};
#endif

static PyMemberDef jitcoro_memberlist[] = {
    {"cr_origin",
     T_OBJECT,
     offsetof(PyCoroObject, cr_origin_or_finalizer),
     READONLY},
    {} /* Sentinel */
};

static PyMethodDef jitcoro_methods[] = {
    {"send", (PyCFunction)jitgen_send, METH_O, nullptr},
    {"throw", _PyCFunction_CAST(jitgen_throw), METH_FASTCALL, nullptr},
    {"close", (PyCFunction)jitgen_close, METH_NOARGS, nullptr},
    {"__sizeof__", (PyCFunction)jitgen_sizeof, METH_NOARGS, nullptr},
#if PY_VERSION_HEX >= 0x030E0000
    {"__class_getitem__", nullptr, METH_O | METH_CLASS, nullptr},
#endif
#ifdef ENABLE_GENERATOR_AWAITER
    {"__set_awaiter__", nullptr, METH_O, nullptr},
#endif
    {} /* Sentinel */
};

#ifdef ENABLE_GENERATOR_AWAITER

static Ci_AsyncMethodsWithExtra jitcoro_as_async = {
    .ame_async_methods =
        {
            .am_await = reinterpret_cast<unaryfunc>(jitcoro_await),
            .am_aiter = nullptr,
            .am_anext = nullptr,
            .am_send = (sendfunc)jitgen_am_send,
        },
    // Filled in by init_jit_genobject_type()
    .ame_setawaiter = nullptr,
};

#else

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0

static PyAsyncMethods jitcoro_as_async = {
    .am_await = reinterpret_cast<unaryfunc>(jitcoro_await),
    .am_aiter = NULL,
    .am_anext = NULL,
    .am_send = (sendfunc)jitgen_am_send,
};

#endif

} // namespace

PySendResult
jitgenSendDirect(PyObject* gen, PyObject* arg, PyObject** presult) {
  JitGenObject* jit_gen = reinterpret_cast<JitGenObject*>(gen);
#if PY_VERSION_HEX < 0x030C0000
  int8_t state = jit_gen->gi_frame_state;
  if (state == FRAME_SUSPENDED ||
      (state == FRAME_CREATED && (arg == nullptr || Py_IsNone(arg)))) {
    return jitgen_resume_fast(
        jit_gen,
        arg,
        presult,
        _PyThreadState_GET(),
        Py_IS_TYPE(gen, g_jitgen_type));
  }
#endif
  return jitgen_am_send_impl(jit_gen, gen, arg, presult, _PyThreadState_GET());
}

void patchJitGenAmSendForDeopt() {
  g_jitgen_am_send_patched = true;
  updateGenDirectGate();
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->gen_type;
  BorrowedRef<PyTypeObject> coro_type = cinderx::getModuleState()->coro_type;
  gen_type->tp_as_async->am_send =
      reinterpret_cast<sendfunc>(jitgen_am_send_with_deopt);
  coro_type->tp_as_async->am_send =
      reinterpret_cast<sendfunc>(jitgen_am_send_with_deopt);
}

void unpatchJitGenAmSendForDeopt() {
  g_jitgen_am_send_patched = false;
  updateGenDirectGate();
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->gen_type;
  BorrowedRef<PyTypeObject> coro_type = cinderx::getModuleState()->coro_type;
  gen_type->tp_as_async->am_send = reinterpret_cast<sendfunc>(jitgen_am_send);
  coro_type->tp_as_async->am_send = reinterpret_cast<sendfunc>(jitgen_am_send);
}

PyType_Slot gen_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(jitgen_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(jitgen_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(jitgen_clear)},
    {Py_tp_finalize, reinterpret_cast<void*>(jitgen_finalize)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void*>(jitgen_iternext)},
    {Py_tp_methods, jitgen_methods},
    {Py_tp_getset, jitgen_getsetlist},
#if PY_VERSION_HEX < 0x030C0000
    {Py_tp_members, jitgen_memberlist},
#endif
    {Py_am_send, reinterpret_cast<void*>(jitgen_am_send)},
    // gi_weakreflist
    {0, nullptr},
};

// kGenObjectSize lives in gen_data_footer.h so the footer address can be
// computed without consulting the type object.

PyType_Spec JitGen_Spec = {
    .name = "builtins.generator",
// Footer pointer is the extra variable slot after locals+stack
// (computeSlots +1).  On 3.11, putting it in basicsize overlays
// localsplus[0]: InitializeSpecials sets stacktop=co_nlocalsplus, so
// gen_traverse / _PyFrame_Clear visit that slot as a PyObject.
#if PY_VERSION_HEX < 0x030C0000
    .basicsize = kGenObjectSize,
#else
    .basicsize = kGenObjectSize + sizeof(GenDataFooter*),
#endif
    .itemsize = sizeof(PyObject*),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = gen_slots,
};

static_assert(sizeof(PyGenObject) == sizeof(PyCoroObject));

PyType_Slot coro_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(jitgen_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(jitgen_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(jitgen_clear)},
    {Py_tp_finalize, reinterpret_cast<void*>(jitgen_finalize)},
    {Py_tp_methods, jitcoro_methods},
    {Py_tp_members, jitcoro_memberlist},
    {Py_tp_getset, jitcoro_getsetlist},
    {Py_am_await, reinterpret_cast<void*>(jitcoro_await)},
    {Py_am_send, reinterpret_cast<void*>(jitgen_am_send)},
    {Py_tp_repr, reinterpret_cast<void*>(jitcoro_repr)},
    {0, nullptr},
};

PyType_Spec JitCoro_Spec = {
    .name = "builtins.coroutine",
#if PY_VERSION_HEX < 0x030C0000
    .basicsize = kGenObjectSize,
#else
    .basicsize = kGenObjectSize + sizeof(GenDataFooter*),
#endif
    .itemsize = sizeof(PyObject*),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Ci_TPFLAGS_HAVE_AM_EXTRA | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = coro_slots,
};

void deopt_jit_gen_object_only(JitGenObject* gen) {
  PyTypeObject* old_type = Py_TYPE(gen);

  PyTypeObject* type =
      Py_TYPE(gen) == g_jitgen_type ? &PyGen_Type : &PyCoro_Type;
  Py_DECREF(old_type);
  Py_SET_TYPE(reinterpret_cast<PyObject*>(gen), type);
#if PY_VERSION_HEX < 0x030C0000
  if (type == &PyGen_Type) {
    // the footer pointer parked in gi_origin_or_finalizer
    // belongs to the JIT type only.
    gen->gi_origin_or_finalizer = nullptr;
  }
#endif
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  if (getConfig().frame_mode == FrameMode::kLightweight) {
    auto frame = generatorFrame(gen);
    if (gen->gi_frame_state != FRAME_CLEARED) {
      jitFrameRemoveReifier(frame);
    } else if constexpr (PY_VERSION_HEX < 0x030E0000) {
      // Normally we'll clear the function via jitFrameClearExceptCode. But
      // a user can call clear on a reified frame object which transfers
      // ownership of the _PyInterpreterFrame to the PyFrameObject and marks
      // the generator frame as cleared. In that case we still need to decref
      // the function which is stored before the _PyInterpreterFrame in 3.12.
      Py_XDECREF(jitFrameGetFunction(frame));
    }
  }
#endif
}

bool deopt_jit_gen_with_footer(
    JitGenObject* jit_gen,
    GenDataFooter* gen_footer,
    bool tree_iter_cleared) {
  if (jit_gen->gi_frame_state == FRAME_EXECUTING) {
    return false;
  }
  if (gen_footer->tree_iter_state != nullptr) {
    return false;
  }

  if (gen_footer->yieldPoint) {
    // TODO: This "deopting" mechanism should be better shared with the
    // similar machinery for general JIT deopting. Among other things we're
    // missing deopt logging here. Although if we used the existing stuff
    // for this it might be misleading as the "cause" will not be an
    // executed instruction.
    const DeoptMetadata& deopt_meta = gen_footer->code_rt->getDeoptMetadata(
        gen_footer->yieldPoint->deoptIdx());
    JIT_CHECK(
        deopt_meta.inline_depth() == 0,
        "inline functions not supported for generators");
    auto frame = generatorFrame(jit_gen);
    if (getConfig().frame_mode == FrameMode::kLightweight) {
      jitFramePopulateFrame(frame);
    }
    reifyGeneratorFrame(
        frame, deopt_meta, deopt_meta.innermostFrame(), gen_footer);
#if PY_VERSION_HEX < 0x030C0000
    BorrowedRef<PyCodeObject> ledger_code = deopt_meta.innermostFrame().code;
    int resume_offset = static_cast<int>(
        (frame->prev_instr + 1 - _PyCode_CODE(ledger_code)) *
        sizeof(_Py_CODEUNIT));
    runtimeTransitionLedgerRecord(
        ledger_code,
        "generator-deopt",
        deoptReasonName(deopt_meta.reason),
        deopt_meta.innermostFrame().cause_instr_idx.value(),
        resume_offset,
        false,
        isJitPaused());
#endif
    // Ownership of references has been transferred from JIT to interpreter.
    releaseRefs(deopt_meta, gen_footer);
  } else {
    JIT_CHECK(
        FRAME_STATE_FINISHED(jit_gen->gi_frame_state),
        "JIT generator has no yield point and is not running or completed");
  }

  if (!tree_iter_cleared) {
    clearTreeIterState(gen_footer);
  }
  deopt_jit_gen_object_only(jit_gen);
  clearGeneratorCompiledFunction(gen_footer);

  return true;
}

bool deopt_jit_gen(PyObject* obj) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return true;
  }
  return deopt_jit_gen_with_footer(
      jit_gen, jit_gen->genDataFooter(), false /* tree_iter_cleared */);
}

// Cache/copy features of PyGen_Type so we don't need to reimplement them.
void init_jit_genobject_type() {
  using namespace std::literals;
  // Copy base type functions
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->gen_type;
  BorrowedRef<PyTypeObject> coro_type = cinderx::getModuleState()->coro_type;
  g_jitgen_type = gen_type;
  g_jitcoro_type = coro_type;
  updateGenDirectGate();

  gen_type->tp_repr = PyGen_Type.tp_repr;

  // Fields that can't be set via PyType_Spec
  gen_type->tp_dictoffset = 0;
  gen_type->tp_weaklistoffset = offsetof(PyGenObject, gi_weakreflist);

  coro_type->tp_dictoffset = 0;
  coro_type->tp_weaklistoffset = offsetof(PyGenObject, gi_weakreflist);
  coro_type->tp_as_async = reinterpret_cast<PyAsyncMethods*>(&jitcoro_as_async);

  // Copy globally cached methods
  for (PyMethodDef* gen_methods = PyGen_Type.tp_methods;
       gen_methods->ml_name != nullptr;
       ++gen_methods) {
    if (gen_methods->ml_name == "throw"sv) {
      gen_throw_meth = reinterpret_cast<GenThrowMeth>(gen_methods->ml_meth);
    } else if (gen_methods->ml_name == "close"sv) {
      gen_close_meth = gen_methods->ml_meth;
    } else if (gen_methods->ml_name == "__sizeof__"sv) {
      gen___sizeof___meth = gen_methods->ml_meth;
    }
  }
  JIT_CHECK(
      gen_throw_meth != nullptr && gen_close_meth != nullptr &&
          gen___sizeof___meth != nullptr,
      "Could not find all needed methods in PyGen_Type");

  // Copy get/setters
  auto copy_getset = [](PyGetSetDef* src, PyGetSetDef* target) {
    int i;
    for (i = 0; src[i].name != nullptr; ++i) {
      JIT_CHECK(
          target[i].name != nullptr,
          "Missing getter/setter on JIT generator: {}",
          src[i].name);
      JIT_CHECK(
          std::string_view(target[i].name) == src[i].name,
          "Name mismatch: {} != {}",
          target[i].name,
          src[i].name);
      if (target[i].get == nullptr) {
        target[i].get = src[i].get;
      }
      if (target[i].set == nullptr) {
        target[i].set = src[i].set;
      }
      if (target[i].doc == nullptr) {
        target[i].doc = src[i].doc;
      }
    }
    if (target[i].name != nullptr && strcmp(target[i].name, "__class__") == 0) {
      // Allow extra class which overrides the type to be the CPython type
      // for the purpose of isinstance checks.
      i++;
    }
    JIT_CHECK(target[i].name == nullptr, "Extra name: {}", target[i].name);
  };
  copy_getset(PyGen_Type.tp_getset, gen_type->tp_getset);
  copy_getset(PyCoro_Type.tp_getset, coro_type->tp_getset);

  // Copy methods
  auto copy_methods = [](PyMethodDef* src, PyMethodDef* target) {
    int i;
    for (i = 0; src[i].ml_name != nullptr; ++i) {
      JIT_CHECK(
          target[i].ml_name != nullptr,
          "Missing method on JIT generator: {}",
          src[i].ml_name);
      JIT_CHECK(
          std::string_view(target[i].ml_name) == src[i].ml_name,
          "Name mismatch: {} != {}",
          target[i].ml_name,
          src[i].ml_name);
      if (target[i].ml_meth == nullptr) {
        target[i].ml_meth = src[i].ml_meth;
      }
      JIT_CHECK(
          target[i].ml_flags == src[i].ml_flags,
          "Flags mismatch for {}",
          target[i].ml_name);
      if (target[i].ml_doc == nullptr) {
        target[i].ml_doc = src[i].ml_doc;
      }
    }
    JIT_CHECK(
        target[i].ml_name == nullptr, "Extra name: {}", target[i].ml_name);
  };
  copy_methods(PyGen_Type.tp_methods, gen_type->tp_methods);
  copy_methods(PyCoro_Type.tp_methods, coro_type->tp_methods);

  if constexpr (kFreeThreadedBuild) {
    cinderx::getModuleState()->jit_gen_free_list.reset(
        new JITGenFreeThreadedFreeList());
  } else {
    cinderx::getModuleState()->jit_gen_free_list.reset(new JitGenFreeList());
  }

#if PY_VERSION_HEX >= 0x030C0000
  // Override dealloc so we can use a "free-list" for our objects.
  JIT_CHECK(
      PyGen_Type.tp_dealloc == original_gen_dealloc &&
          PyCoro_Type.tp_dealloc == original_coro_dealloc,
      "PyGen/Coro_Type already overridden");
  PyGen_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);
  PyCoro_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);
#else
  // the same override on 3.11 lets deopted generators die
  // through the arena-aware path, which in turn allows the free list.
  JIT_CHECK(
      PyGen_Type.tp_dealloc == original_gen_dealloc &&
          PyCoro_Type.tp_dealloc == original_coro_dealloc,
      "PyGen/Coro_Type already overridden");
  PyGen_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);
  PyCoro_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);
#endif

#ifdef ENABLE_GENERATOR_AWAITER
  JIT_CHECK(
      PyCoro_Type.tp_flags & Ci_TPFLAGS_HAVE_AM_EXTRA,
      "No AM_EXTRA on coro type");
  auto coro_ame =
      reinterpret_cast<Ci_AsyncMethodsWithExtra*>(PyCoro_Type.tp_as_async);
  jitcoro_as_async.ame_setawaiter = coro_ame->ame_setawaiter;
#endif
}

void shutdown_jit_genobject_type() {
  g_jitgen_type = nullptr;
  g_jitcoro_type = nullptr;
  g_gen_direct_gate = 0;
  PyGen_Type.tp_dealloc = original_gen_dealloc;
  PyCoro_Type.tp_dealloc = original_coro_dealloc;
}

} // namespace jit

PyObject* JitGen_yf(PyGenObject* gen) {
  jit::JitGenObject* jit_gen = jit::JitGenObject::cast(gen);
  return jit_gen == nullptr ? _PyGen_yf(gen) : jit_gen->yieldFrom();
}

#if PY_VERSION_HEX < 0x030C0000
PyObject* JITRT_GenNextFinish311(PyObject* gen, PyObject* value) {
  return jit::genNextFinish311(gen, value);
}

PyObject* JITRT_GenDelegateNext311(PyObject* gen, PyThreadState* tstate) {
  return jit::genDelegateNext311(gen, tstate);
}

PyObject* JITRT_GenDelegateFinish311(
    PyObject* innermost,
    PyObject* value,
    PyThreadState* tstate) {
  return jit::delegateFinish(
      reinterpret_cast<jit::JitGenObject*>(innermost), value, tstate);
}

void JITRT_GenUnwindDelegators311(PyObject* from, PyThreadState* tstate) {
  jit::unwindDelegators(reinterpret_cast<jit::JitGenObject*>(from), tstate);
}

PyObject* JITRT_GenIterNext311(PyObject* gen) {
  return jit::jitgen_iternext(gen);
}
#endif
