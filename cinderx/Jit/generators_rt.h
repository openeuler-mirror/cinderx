// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#include "internal/pycore_frame.h"

#include "cpython/genobject.h"

#ifdef __cplusplus

#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/gen_data_footer.h"
#include "cinderx/module_state.h" // @donotremove

namespace jit {

struct GenDataFooter;
extern PyType_Spec JitGen_Spec;
extern PyType_Spec JitCoro_Spec;
extern PyTypeObject _JitCoroWrapper_Type;

// cached copies of ModuleState::gen_type / coro_type, valid
// between init_jit_genobject_type() and shutdown_jit_genobject_type().  Null
// outside that window, when no JIT generator can exist either.
extern PyTypeObject* g_jitgen_type;
extern PyTypeObject* g_jitcoro_type;

template <typename PyObjectT>
int JitGen_CheckAny(PyObjectT* op) {
  PyTypeObject* type = Py_TYPE(reinterpret_cast<PyObject*>(op));
  return type == g_jitgen_type || type == g_jitcoro_type;
}

struct JitGenObject : PyGenObject {
  JitGenObject() = delete;

  template <typename PyGenObjectT>
  static JitGenObject* cast(PyGenObjectT* gen) {
    return JitGen_CheckAny(gen) ? reinterpret_cast<JitGenObject*>(gen)
                                : nullptr;
  }

  GenDataFooter** genDataFooterPtr(PyCodeObject* gen_code) {
#if PY_VERSION_HEX < 0x030C0000
    // JitGen_Spec.basicsize is kGenObjectSize and the item
    // size is one pointer, so the footer pointer slot follows the frame at a
    // position computable without the type object.
    return reinterpret_cast<GenDataFooter**>(
        reinterpret_cast<uintptr_t>(this) + kGenObjectSize +
        _PyFrame_NumSlotsForCodeObject(gen_code) * sizeof(PyObject*));
#else
    PyTypeObject* gen_type = cinderx::getModuleState()->gen_type;
    size_t python_frame_data_bytes =
        _PyFrame_NumSlotsForCodeObject(gen_code) * gen_type->tp_itemsize;
    return reinterpret_cast<GenDataFooter**>(
        reinterpret_cast<uintptr_t>(this) + gen_type->tp_basicsize +
        python_frame_data_bytes);
#endif
  }

  GenDataFooter** genDataFooterPtr() {
#if PY_VERSION_HEX < 0x030C0000
    // gi_code holds the code object until deallocation, so the
    // footer stays reachable after the frame's executable has been cleared at
    // completion (the frame's f_code is the same object while it is set).
    return genDataFooterPtr(gi_code);
#else
    _PyInterpreterFrame* gen_frame = generatorFrame(this);
    return genDataFooterPtr(_PyFrame_GetCode(gen_frame));
#endif
  }

  GenDataFooter* genDataFooter() {
    return *genDataFooterPtr();
  }

  PyObject* yieldFrom();
};

// Converts a JitGenObject into a regular PyGenObject. This assumes deopting
// the associated frame will be done elsewhere.
void deopt_jit_gen_object_only(JitGenObject* gen);

// Fully deopt a generator so it'll be ready for use in the interpreter. Note
// this cannot be done on a currently executing JIT generator and will return
// false in this case. The caller should issue an appropriate error.
bool deopt_jit_gen(PyObject* op);
inline bool deopt_jit_gen(PyGenObject* gen) {
  return deopt_jit_gen(reinterpret_cast<PyObject*>(gen));
}

// true while am_send is swapped for the instrumentation deopt
// wrapper; the direct resume lanes must go through the slot then.
extern bool g_jitgen_am_send_patched;
// Process gate for the direct resume call from JIT code (CPython 3.11): 1
// while plain JIT generators may be resumed by a direct call, 0 while the
// am_send slot is patched for an instrumentation deopt or an entry ledger is
// armed.  Recomputed by updateGenDirectGate() at every transition.
extern uint8_t g_gen_direct_gate;
void updateGenDirectGate();
// resume a generator known to be an unpatched JitGen/JitCoro
// without the am_send slot dispatch.  arg may be nullptr (next()).
PySendResult jitgenSendDirect(PyObject* gen, PyObject* arg, PyObject** presult);
void init_jit_genobject_type();
void shutdown_jit_genobject_type();

// Swap am_send on JIT gen/coro types to a wrapper that deopts generators
// before resuming. Called when the JIT is paused for instrumentation.
void patchJitGenAmSendForDeopt();
// Restore the normal JIT am_send. Called when the JIT is re-enabled.
void unpatchJitGenAmSendForDeopt();

} // namespace jit
#endif // __cplusplus

#ifdef __cplusplus
extern "C" {
#endif

int JitGen_CheckExact(PyObject* o);
int JitCoro_CheckExact(PyObject* o);
PyObject* JitCoro_GetAwaitableIter(PyObject* o);
PyObject* JitGen_yf(PyGenObject* gen);

#ifdef __cplusplus
}
#endif
