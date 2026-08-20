// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/code_extra.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_code.h"
#endif

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Gets the qualified name of the code object or "<null>" if it's not set.
const char* codeName(PyCodeObject* code);

// Get the internal _Py_CODEUNIT buffer from a code object.
_Py_CODEUNIT* codeUnit(PyCodeObject* code);

// Count the number of "indices" in a code object.  This used to make more sense
// when each instruction occupied a fixed number of bytes in the bytecode.  In
// some cases it's still helpful to consider sizeof(_Py_CODEUNIT) sized chunks.
size_t countIndices(PyCodeObject* code);

// Convert a specialized opcode back to its base form.
int unspecialize(int opcode);

// Convert an instrumented opcode back to its base form.
int uninstrument(PyCodeObject* code, int index);

// Get the name of a Python opcode.
const char* opcodeName(int opcode);

// Get the number of inline cache slots used by an opcode.
//
// This needs to take a code object and an opcode index to process instrumented
// opcodes.
Py_ssize_t inlineCacheSize(PyCodeObject* code, int index);

// Get the name index from a LOAD_ATTR's oparg.
int loadAttrIndex(int oparg);

// Get the name index from a LOAD_GLOBAL's oparg.
int loadGlobalIndex(int oparg);

// Initialize and finalize the index of the extra data Cinder attaches onto code
// objects.
void initCodeExtraIndex();
void finiCodeExtraIndex();

// Get the extra data object associated with a code object. Lazily allocates
// this data if this is the first access. Returns nullptr on failure with no
// Python error set.
CodeExtra* codeExtra(PyCodeObject* code);

#ifdef __cplusplus
// This header is included from C translation units; everything C++ stays
// behind the guard.
namespace jit {
// One-shot failpoint for the publication exception-safety RuntimeTests.
// Steps: 1 = the artifact's owned-functions insert (inside the
// association), 2 = the association map insert, 3 = the installed-registry
// insert, 4 = the compiled-codes insert, 5 = the code-extra reserve, where
// the armed step models setCodeExtraCapped() failing with its MemoryError
// set -- so the fault travels the reserve's real preserve-or-clear branch.
// The failpoint clears when it fires.
void failJitPublishStepForTest(int step);
void throwIfJitPublishStepArmedForTest(int step);
bool consumeJitPublishStepForTest(int step);
} // namespace jit
#endif // __cplusplus

// As codeExtra(), but an allocation failure leaves the MemoryError set:
// the machine-code publication path must report what actually happened,
// not a generic capability refusal.
CodeExtra* codeExtraOrError(PyCodeObject* code);
size_t codeCallCount(PyCodeObject* code);

// Get the extra data object associated with a code object if it already exists.
// Unlike codeExtra(), this never allocates a CodeExtra.
CodeExtra* codeExtraIfExists(PyCodeObject* code);

// Count the various frame variables that a code object will use.
int numLocals(PyCodeObject* code);
int numCellvars(PyCodeObject* code);
int numFreevars(PyCodeObject* code);
int numLocalsplus(PyCodeObject* code);

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_instruments.h"

uint8_t Cix_GetOriginalOpcode(
    _PyCoLineInstrumentationData* line_data,
    int index);
#elif PY_VERSION_HEX >= 0x030C0000
static inline uint8_t Cix_GetOriginalOpcode(
    _PyCoLineInstrumentationData* line_data,
    int index) {
  return line_data[index].original_opcode;
}
#else
typedef uint8_t _PyCoLineInstrumentationData;
static inline uint8_t Cix_GetOriginalOpcode(
    _PyCoLineInstrumentationData*,
    int) {
  return 0;
}
#endif

#ifdef __cplusplus
} // extern "C"

#include "cinderx/Common/ref.h"

#include <string>

namespace jit {

std::string codeFullname(
    BorrowedRef<PyObject> module,
    BorrowedRef<PyCodeObject> code);
std::string funcFullname(BorrowedRef<PyFunctionObject> func);

// Given a code object and an index into f_localsplus, compute which of
// code->co_varnames, code->cellvars, or code->freevars contains the name of the
// variable. Return a new reference to that tuple and adjust idx as needed.
PyObject* getVarnameTuple(BorrowedRef<PyCodeObject> code, int* idx);

// Similar to getVarnameTuple, but return the name itself rather than the
// containing tuple.
PyObject* getVarname(BorrowedRef<PyCodeObject> code, int idx);

uint32_t hashBytecode(BorrowedRef<PyCodeObject> code);

// Return the qualname of the given code object, falling back to its name or
// "<unknown>" if not set.
std::string codeQualname(BorrowedRef<PyCodeObject> code);

} // namespace jit

#endif
