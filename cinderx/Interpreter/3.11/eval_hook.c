// Copyright (c) Meta Platforms, Inc. and affiliates.

// PEP 523 entry-point ownership for CPython 3.11.
//
// Installing an evaluator changes how every Python frame in the process runs,
// so CinderX takes the entry point only when it is still the stock one, keeps
// the pointer it displaced, and gives it back on the way out.  A third-party
// profiler, debugger or JIT that got there first is left alone, and one that
// takes over after us keeps the entry point: we never write back a stale
// pointer, and never write NULL in place of the evaluator we replaced.

#include "cinderx/Interpreter/3.11/eval_hook.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Interpreter/3.11/interpreter_contract.h"
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Interpreter/interpreter.h"

typedef struct {
  // The evaluator that was installed before ours, to be restored on removal.
  _PyFrameEvalFunction original;
  // The interpreter whose entry point we hold.
  PyInterpreterState* owner_interp;
  int installed;
  // Counts completed installations, so diagnostics can tell repeated
  // install/remove cycles apart.
  uint64_t generation;
} Ci_EvalHookState311;

static Ci_EvalHookState311 ci_eval_hook_state = {NULL, NULL, 0, 0};

static int check_main_interpreter(PyInterpreterState* interp) {
  if (interp != PyInterpreterState_Main()) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "CinderX supports the main interpreter only on CPython 3.11");
    return -1;
  }
  return 0;
}

extern void Ci_JitShell311_RefreshDirectEntryGate(void);

int Ci_EvalHook311_Install(void) {
  PyThreadState* tstate = PyThreadState_Get();
  if (tstate == NULL) {
    PyErr_SetString(
        PyExc_RuntimeError, "installing a frame evaluator requires the GIL");
    return -1;
  }

  PyInterpreterState* interp = tstate->interp;
  if (check_main_interpreter(interp) < 0) {
    return -1;
  }

  _PyFrameEvalFunction current = _PyInterpreterState_GetEvalFrameFunc(interp);
  if (current == Ci_EvalFrame) {
    // Already ours: installation is idempotent.
    return 0;
  }
  if (current != _PyEval_EvalFrameDefault) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "another component already installed a frame evaluator; CinderX will "
        "not replace it");
    return -1;
  }

  // The borrowed evaluator state (the captured empty-keys singleton) must be
  // ready before any frame can reach Ci_EvalFrame; failing here leaves the
  // stock entry point untouched.
  if (Ci_InitInterpreter_311() < 0) {
    return -1;
  }
  if (Ci_InitQuickening_311() < 0) {
    return -1;
  }

  // Runtime-mode configuration shares the takeover choke point: an
  // unaccepted CINDERX_JIT_MODE refuses the installation outright, and the
  // observe counters exist before the first frame can enter Ci_EvalFrame.
  if (Ci_Observe311_Configure() < 0) {
    return -1;
  }

  _PyInterpreterState_SetEvalFrameFunc(interp, Ci_EvalFrame);

  // Read back rather than trusting the write: ownership is only real if the
  // entry point actually holds our function.
  if (_PyInterpreterState_GetEvalFrameFunc(interp) != Ci_EvalFrame) {
    PyErr_SetString(
        PyExc_RuntimeError, "failed to install the CinderX frame evaluator");
    return -1;
  }

  ci_eval_hook_state.original = current;
  ci_eval_hook_state.owner_interp = interp;
  ci_eval_hook_state.installed = 1;
  Ci_JitShell311_RefreshDirectEntryGate();
  ci_eval_hook_state.generation++;
  return 0;
}

Ci_EvalHook311_RemoveResult Ci_EvalHook311_Remove(void) {
  if (!ci_eval_hook_state.installed) {
    // Never installed: removal is idempotent.
    return CI_EVAL_HOOK_REMOVED;
  }

  PyThreadState* tstate = PyThreadState_Get();
  if (tstate == NULL) {
    PyErr_SetString(
        PyExc_RuntimeError, "removing a frame evaluator requires the GIL");
    return CI_EVAL_HOOK_ERROR;
  }

  PyInterpreterState* interp = tstate->interp;
  if (interp != ci_eval_hook_state.owner_interp) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "the CinderX frame evaluator belongs to a different interpreter");
    return CI_EVAL_HOOK_ERROR;
  }

  if (_PyInterpreterState_GetEvalFrameFunc(interp) != Ci_EvalFrame) {
    // Someone replaced us while we were installed.  Restoring the pointer we
    // saved would silently undo their installation, so leave it in place and
    // report that our ownership is gone.
    ci_eval_hook_state.installed = 0;
    ci_eval_hook_state.owner_interp = NULL;
    ci_eval_hook_state.original = NULL;
    Ci_JitShell311_RefreshDirectEntryGate();
    return CI_EVAL_HOOK_OWNERSHIP_LOST;
  }

  _PyInterpreterState_SetEvalFrameFunc(interp, ci_eval_hook_state.original);
  ci_eval_hook_state.installed = 0;
  ci_eval_hook_state.owner_interp = NULL;
  ci_eval_hook_state.original = NULL;
  Ci_JitShell311_RefreshDirectEntryGate();
  return CI_EVAL_HOOK_REMOVED;
}

int Ci_EvalHook311_IsInstalled(void) {
  PyInterpreterState* interp = PyInterpreterState_Get();
  return ci_eval_hook_state.installed &&
      _PyInterpreterState_GetEvalFrameFunc(interp) == Ci_EvalFrame;
}

uint64_t Ci_EvalHook311_Generation(void) {
  return ci_eval_hook_state.generation;
}
