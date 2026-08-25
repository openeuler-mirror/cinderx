// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/ref.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp.h"
#endif

#include "internal/pycore_pystate.h"

#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
#include "internal/pycore_tstate.h"

#include <atomic>
#endif

void incref_total([[maybe_unused]] PyThreadState* tstate) {
#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  std::atomic_ref<Py_ssize_t>(tstate_impl->reftotal)
      .fetch_add(1, std::memory_order_relaxed);
#endif
}

void decref_total([[maybe_unused]] PyThreadState* tstate) {
#if defined(Py_REF_DEBUG) && defined(Py_GIL_DISABLED)
  _PyThreadStateImpl* tstate_impl = (_PyThreadStateImpl*)tstate;
  std::atomic_ref<Py_ssize_t>(tstate_impl->reftotal)
      .fetch_sub(1, std::memory_order_relaxed);
#endif
}

// CPython moved the reference-count total into the interpreter state in
// 3.12; before that it is a single process-wide global.  Only a
// Py_REF_DEBUG interpreter has either, and that is exactly the build a
// regrtest -R leg needs, so the older spelling has to be here for 3.11 to
// be measurable at all.
void incref_total([[maybe_unused]] PyInterpreterState* interp) {
#if defined(Py_REF_DEBUG) && !defined(Py_GIL_DISABLED)
#if PY_VERSION_HEX >= 0x030C0000
  interp->object_state.reftotal++;
#else
  _Py_RefTotal++;
#endif
#endif
}

void decref_total([[maybe_unused]] PyInterpreterState* interp) {
#if defined(Py_REF_DEBUG) && !defined(Py_GIL_DISABLED)
#if PY_VERSION_HEX >= 0x030C0000
  interp->object_state.reftotal--;
#else
  _Py_RefTotal--;
#endif
#endif
}
