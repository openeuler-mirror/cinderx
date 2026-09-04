// Copyright (c) Meta Platforms, Inc. and affiliates.

// Formal CPython 3.11.6 evaluator wrapper. Every source deviation is expressed
// as a macro here; upstream/ceval.c is hash-locked and never patched.

#define Py_BUILD_CORE

// Route frame operations to the separately wrapped, pristine frame.c.
// These mappings must precede pycore_frame.h so its inline helpers bind to the
// vendored symbols rather than libpython's private, unexported definitions.
#define _PyFrame_MakeAndSetFrameObject Ci_PyFrame_MakeAndSetFrameObject_311
#define _PyFrame_Copy Ci_PyFrame_Copy_311
#define _PyFrame_Clear Ci_PyFrame_Clear_311
#define _PyFrame_Push Ci_PyFrame_Push_311
#define _PyInterpreterFrame_GetLine Ci_PyInterpreterFrame_GetLine_311

#include "Python.h"
#include "internal/pycore_code.h"
#include "internal/pycore_dict.h"
#include "internal/pycore_opcode.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Interpreter/3.11/interpreter_dependencies.h"
#include "cinderx/Interpreter/3.11/interpreter_internal.h"

// Stable, versioned entry point. Recursive evaluator calls remain in the
// vendored loop and cannot bind to libpython's stock entry point.
#define _PyEval_EvalFrameDefault Ci_EvalFrameDefault_311

// Reuse exact common Borrow implementations instead of duplicating them in
// the interpreter-local cache.
#define _PyAsyncGenValueWrapperNew Cix_PyAsyncGenValueWrapperNew
#define _PyThreadState_BumpFramePointerSlow Cix_PyThreadState_PushFrame
#define _PyDict_SetItem_Take2 Cix_PyDict_SetItem_Take2_311

// Allocate PEP 509 versions from libpython's real stream via the private clock
// dict owned by common Borrow. No independent shadow counter is permitted.
#undef DICT_NEXT_VERSION
#define DICT_NEXT_VERSION() Cix_PyDict_NextVersion()

// The Release 34 devel package defines WITH_DTRACE but does not ship the
// generated pydtrace_probes.h. The vendored loop therefore keeps DTrace probes
// disabled; CinderX's independent ENABLE_USDT instrumentation remains intact.
#undef WITH_DTRACE

// With a PEP 523 evaluator installed, CPython's CALL handler cannot use its
// inlined-Python-function path. The generic vectorcall path invokes the callee
// before advancing over CALL's inline caches, so an introspecting Python
// callee would otherwise observe a caller position different from Stock.
// Route only evaluator-local vectorcalls through this cursor shim: ordinary
// raising opcodes and calls to C objects keep their original opcode position.
static PyObject* Ci_PyObject_Vectorcall_311(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  if (PyFunction_Check(callable)) {
    PyThreadState* tstate = _PyThreadState_GET();
    _PyInterpreterFrame* frame = tstate->cframe->current_frame;
    if (frame != NULL && frame->prev_instr >= _PyCode_CODE(frame->f_code) &&
        frame->prev_instr <
            _PyCode_CODE(frame->f_code) + Py_SIZE(frame->f_code)) {
      int opcode = _Py_OPCODE(*frame->prev_instr);
      if (_PyOpcode_Deopt[opcode] == CALL) {
        frame->prev_instr += INLINE_CACHE_ENTRIES_CALL;
      }
    }
  }
  return PyObject_Vectorcall(callable, args, nargsf, kwnames);
}

extern int Ci_QuickenWarmupStep_311;
static inline void Ci_CodeWarmup_311(PyCodeObject* code) {
  if (code->co_warmup != 0) {
    code->co_warmup += Ci_QuickenWarmupStep_311;
    if (code->co_warmup >= 0) {
      code->co_warmup = 0;
      _PyCode_Quicken(code);
    }
  }
}
#define _PyCode_Warmup Ci_CodeWarmup_311

#define PyObject_Vectorcall Ci_PyObject_Vectorcall_311
#include "upstream/ceval.c"
#undef PyObject_Vectorcall
#undef _PyCode_Warmup

// Defined after the include so the static eval_frame_handle_pending() above
// is in scope.  The wrapper adds nothing: the anchored source stays the
// oracle for what a back edge must service.
int Ci_EvalFrameHandlePending_311(PyThreadState* tstate) {
  return eval_frame_handle_pending(tstate);
}
