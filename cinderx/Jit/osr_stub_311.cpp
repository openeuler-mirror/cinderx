// Copyright (c) Meta Platforms, Inc. and affiliates.

// OSR is out of scope for the CPython 3.11 delivery.  Jit/osr.cpp is 3.14
// machine-code machinery -- it reads _PyStackRef and the 3.14 frame internals
// (internal/pycore_interpframe.h) that do not exist on 3.11 -- so it is
// excluded from the 3.11 build.  This translation unit supplies the OSR
// symbols the rest of the runtime links against, all inert.  A real 3.11 OSR
// implementation is explicitly out of scope.

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000

#include "cinderx/Jit/osr.h"

// The interpreter's OSR C-API macros read these; kept off so no path is ever
// eligible.  osr_capi.h itself is not included: it pulls 3.14-only atomic
// headers, and these three ints are the only symbols from it the 3.11 build
// links against.
extern "C" {
int cinderx_osr_enabled = 0;
int cinderx_osr_capable = 0;
int cinderx_osr_state = 0;
}

namespace jit {

void initOSRCodeExtraIndex() {}
void finiOSRCodeExtraIndex() {}
void syncOSRFlags() {}
void resetOSRState(PyCodeObject*) {}

bool osrCompileBudgetCheck(BorrowedRef<PyCodeObject>) {
  return false;
}

bool isOSREligible(PyThreadState*, _PyInterpreterFrame*, PyCodeObject*) {
  return false;
}

int performOSR(
    PyThreadState*,
    _PyInterpreterFrame*,
    const OSRMetadata*,
    const CompiledFunction*,
    PyObject**) {
  return 0;
}

// Called unconditionally from preload; with OSR off there are no backedge
// entry targets to collect.
std::vector<BCOffset> collectBackedgeTargetOffsets(BorrowedRef<PyCodeObject>) {
  return {};
}

} // namespace jit

#endif // PY_VERSION_HEX < 0x030C0000
