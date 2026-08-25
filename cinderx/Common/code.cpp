// Copyright (c) Meta Platforms, Inc. and affiliates.

// For opcodeName().
#define NEED_OPCODE_NAMES

#include "cinderx/Common/code.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/observe.h"
#endif
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
#include "cinderx/module_state.h"

#include <cstddef>
#include <new>
#include <utility>

#ifdef ENABLE_ZLIB
#include <zlib.h>
#endif

#include "cpython/code.h"

namespace {

CodeExtra* codeExtraIfPresent(BorrowedRef<PyCodeObject> code) {
  return codeExtraIfExists(code);
}

#if PY_VERSION_HEX < 0x030C0000
// Layout of PyCodeObject::co_extra, private to Objects/codeobject.c but
// stable across 3.11: a size-prefixed inline pointer array.
struct CodeObjectExtra311 {
  Py_ssize_t ce_size;
  void* ce_extras[1];
};
#endif

CodeDestroyedHook g_code_destroyed_hook = nullptr;

// Live owned code-extra blocks: incremented where one is allocated,
// decremented where it is freed.  A GAUGE.  It is the only way to see
// this allocation from a test: the block comes from the raw allocator, so
// it is invisible to gc.get_objects(), and a leak of one per code object
// would look exactly like a clean census.
size_t g_live_code_extra_blocks = 0;

#if PY_VERSION_HEX < 0x030C0000
// 3.11: the free call is a dead code object's only signal, so the block
// names its owner in a version-local header (shared struct untouched).
struct OwnedCodeExtra311 {
  PyCodeObject* owner;
  CodeExtra extra;
};

OwnedCodeExtra311* ownedBlockOf(void* extra) {
  auto* bytes = reinterpret_cast<char*>(extra);
  return reinterpret_cast<OwnedCodeExtra311*>(
      bytes - offsetof(OwnedCodeExtra311, extra));
}
#endif

// Code-extra free function; CPython also calls it for never-written
// indices, so a null argument is expected.
void codeExtraFree(void* extra) {
  if (extra == nullptr) {
    return;
  }
#if PY_VERSION_HEX < 0x030C0000
  OwnedCodeExtra311* block = ownedBlockOf(extra);
  PyCodeObject* owner = block->owner;
  if (owner != nullptr) {
    // The observer's table watches code objects the JIT may know nothing
    // about -- observe mode runs with no JIT at all -- so its notice is
    // not routed through the hook below, which the JIT owns.  Key only,
    // and it neither allocates nor raises.
    Ci_Observe311_OnCodeDeath(owner);
  }
  if (owner != nullptr && g_code_destroyed_hook != nullptr) {
    // Key only.  code_dealloc is already tearing the object down, so no
    // C++ exception may cross this boundary and the block must be freed
    // on every path.  The hook contains its own failures; this arm is the
    // last line of defense for a hook that does not.
    try {
      g_code_destroyed_hook(owner);
    } catch (...) {
    }
  }
  if (g_live_code_extra_blocks > 0) {
    g_live_code_extra_blocks--;
  }
  PyMem_Free(block);
#else
  PyMem_Free(extra);
#endif
}

// Store a code-extra value, keeping the co_extra allocation as small as the
// target index allows.
//
// _PyCode_SetExtra sizes a fresh co_extra to the full number of registered
// indices, and code_dealloc later invokes EVERY registered freefunc below
// that size -- whether or not this code object ever stored a value in the
// slot.  Third-party code can register a mortal freefunc (test.test_code
// registers a ctypes closure at import time) that is already dead by the
// time long-lived code objects reach dealloc during shutdown.  Since the
// runtime attaches its data to broad swaths of code, cap an allocation we
// create at exactly our own slot.
//
// What that buys is precise: every foreign index ABOVE ours stops being
// walked.  A foreign index below ours still is -- the layout is a dense
// prefix array, so storing at index N requires spanning [0, N] -- and there
// it behaves exactly as stock CPython already does, because the capped
// allocation is never larger than the one the stock setter would make.
// Claiming our index during module initialization is what usually keeps
// foreign indices above ours; it is a property of load order, not something
// enforced here.
int setCodeExtraCapped(PyObject* code_obj, Py_ssize_t index, void* extra) {
#if PY_VERSION_HEX < 0x030C0000
  auto code = reinterpret_cast<PyCodeObject*>(code_obj);
  auto ce = reinterpret_cast<CodeObjectExtra311*>(code->co_extra);
  // Read back the size through our own view of the layout before trusting
  // it for arithmetic: CPython caps registered indices at MAX_CO_EXTRA_USERS
  // (255), so anything outside that range means this declaration no longer
  // matches the interpreter and the write must not proceed.
  JIT_CHECK(
      ce == nullptr || (ce->ce_size >= 0 && ce->ce_size <= 255),
      "co_extra size {} is outside the range CPython can produce; the "
      "private layout this build assumes no longer matches the runtime",
      ce == nullptr ? 0 : ce->ce_size);
  if (ce != nullptr && ce->ce_size > index) {
    // Fits in the existing allocation; the stock path will not grow it.
    return PyUnstable_Code_SetExtra(code_obj, index, extra);
  }
  Py_ssize_t old_size = ce != nullptr ? ce->ce_size : 0;
  auto grown = reinterpret_cast<CodeObjectExtra311*>(
      PyMem_Realloc(ce, sizeof(CodeObjectExtra311) + index * sizeof(void*)));
  if (grown == nullptr) {
    // This helper speaks the int-returning Python C API convention, so a
    // failed allocation must leave the exception the convention promises;
    // without it the caller's error path reports a capability refusal
    // where a MemoryError happened.
    PyErr_NoMemory();
    return -1;
  }
  for (Py_ssize_t i = old_size; i <= index; i++) {
    grown->ce_extras[i] = nullptr;
  }
  grown->ce_size = index + 1;
  code->co_extra = grown;
  grown->ce_extras[index] = extra;
  return 0;
#else
  return PyUnstable_Code_SetExtra(code_obj, index, extra);
#endif
}

std::string fullnameImpl(PyObject* module, PyObject* qualname) {
  auto safe_str = [](BorrowedRef<> str) {
    if (str == nullptr || !PyUnicode_Check(str)) {
      return "<invalid>";
    }
    return PyUnicode_AsUTF8(str);
  };
  return fmt::format("{}:{}", safe_str(module), safe_str(qualname));
}

} // namespace

namespace jit {

std::string codeFullname(
    BorrowedRef<PyObject> module,
    BorrowedRef<PyCodeObject> code) {
  return fullnameImpl(module, code->co_qualname);
}

std::string funcFullname(BorrowedRef<PyFunctionObject> func) {
  return fullnameImpl(func->func_module, func->func_qualname);
}

PyObject* getVarnameTuple(BorrowedRef<PyCodeObject> code, int* idx) {
  if (*idx < code->co_nlocals) {
    return PyCode_GetVarnames(code);
  }

  *idx -= code->co_nlocals;
  auto cellvars = Ref<>::steal(PyCode_GetCellvars(code));
  auto ncellvars = PyTuple_GET_SIZE(cellvars.get());
  if (*idx < ncellvars) {
    return cellvars.release();
  }

  *idx -= ncellvars;
  return PyCode_GetFreevars(code);
}

PyObject* getVarname(BorrowedRef<PyCodeObject> code, int idx) {
  return PyTuple_GET_ITEM(code->co_localsplusnames, idx);
}

uint32_t hashBytecode(BorrowedRef<PyCodeObject> code) {
  auto bc = Ref<>::steal(PyCode_GetCode(code));
#ifdef ENABLE_ZLIB
  uint32_t crc = crc32(0, nullptr, 0);
  if (!PyBytes_Check(bc)) {
    return crc;
  }

  char* buffer;
  Py_ssize_t len;
  if (PyBytes_AsStringAndSize(bc, &buffer, &len) < 0) {
    return crc;
  }

  return crc32(crc, reinterpret_cast<unsigned char*>(buffer), len);
#else
  return PyObject_Hash(bc);
#endif
}

std::string codeQualname(BorrowedRef<PyCodeObject> code) {
  if (code->co_qualname != nullptr) {
    return unicodeAsString(code->co_qualname);
  }
  if (code->co_name != nullptr) {
    return unicodeAsString(code->co_name);
  }
  return "<unknown>";
}

} // namespace jit

extern "C" {

const char* codeName(PyCodeObject* code) {
  if (code->co_qualname == nullptr) {
    return "<null>";
  }
  return PyUnicode_AsUTF8(code->co_qualname);
}

_Py_CODEUNIT* codeUnit(PyCodeObject* code) {
  return _PyCode_CODE(code);
}

size_t countIndices(PyCodeObject* code) {
  // PyCode_GetCode can allocate to create a copy of the de-opted code
  // which we don't need just to determine the number of indices.
  return _PyCode_NBYTES(code) / sizeof(_Py_CODEUNIT);
}

int unspecialize(int opcode) {
  // The deopt table has size 256, and pseudo-opcodes and stubs are by
  // definition unspecialized already.
  return (opcode >= 0 && opcode <= 255) ? _CiOpcode_Deopt[opcode] : opcode;
}

int uninstrument(PyCodeObject* code, int index) {
  int opcode = _Py_OPCODE(codeUnit(code)[index]);

  // Check if there's an equivalent opcode without instrumentation.
  uint8_t base_opcode = Cix_DEINSTRUMENT(static_cast<uint8_t>(opcode));
  if (base_opcode != 0) {
    return base_opcode;
  }

// Instrumented lines and arbitrary instrumented instructions need to check
// different tables. CPython 3.11 does not have PEP 669 monitoring opcodes.
#if PY_VERSION_HEX >= 0x030C0000
  if (opcode == INSTRUMENTED_INSTRUCTION) {
    return code->_co_monitoring->per_instruction_opcodes[index];
  }
  if (opcode == INSTRUMENTED_LINE) {
    return Cix_GetOriginalOpcode(code->_co_monitoring->lines, index);
  }
#endif

  return opcode;
}

const char* opcodeName(int opcode) {
#if PY_VERSION_HEX < 0x030C0000
  // CPython 3.11 only compiles its opcode name table under Py_DEBUG, and the
  // only consumers of this helper are JIT diagnostics, which are not built on
  // 3.11.  Report the number rather than generating a table nothing reads.
  (void)opcode;
  return "<opcode name unavailable on 3.11>";
#else
  constexpr size_t num_opcodes =
      sizeof(_CiOpcode_OpName) / sizeof(_CiOpcode_OpName[0]);
  if (opcode < 0 || opcode >= num_opcodes) {
    return "<unrecognized opcode>";
  }
  const char* name = _CiOpcode_OpName[opcode];
  return name != nullptr ? name : "<unknown opcode>";
#endif
}

Py_ssize_t inlineCacheSize(PyCodeObject* code, int index) {
  return _CiOpcode_Caches[unspecialize(uninstrument(code, index))];
}

int loadAttrIndex(int oparg) {
#if PY_VERSION_HEX < 0x030C0000
  return oparg;
#else
  return oparg >> 1;
#endif
}

int loadGlobalIndex(int oparg) {
  return oparg >> 1;
}

void initCodeExtraIndex() {
  auto state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to initialize code extra index but there's no module state");
  JIT_CHECK(
      state->code_extra_index == -1,
      "Cannot re-initialize code extra index without finalizing it first");

  state->code_extra_index =
      PyUnstable_Eval_RequestCodeExtraIndex(codeExtraFree);
}

Py_ssize_t codeExtraSlotIndex() {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->code_extra_index : -1;
}

size_t liveCodeExtraBlocks() {
  return g_live_code_extra_blocks;
}

void setCodeDestroyedHook(CodeDestroyedHook hook) {
  g_code_destroyed_hook = hook;
}

void finiCodeExtraIndex() {
  auto state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to finalize code extra index but there's no module state");
  JIT_CHECK(
      state->code_extra_index != -1,
      "Cannot finalize code extra index without initializing it first");

  state->code_extra_index = -1;
}

namespace jit {
namespace {
int s_publish_failpoint_for_test = 0;
} // namespace

void failJitPublishStepForTest(int step) {
  s_publish_failpoint_for_test = step;
}

void throwIfJitPublishStepArmedForTest(int step) {
  if (s_publish_failpoint_for_test == step) {
    s_publish_failpoint_for_test = 0;
    throw std::bad_alloc();
  }
}

bool consumeJitPublishStepForTest(int step) {
  if (s_publish_failpoint_for_test == step) {
    s_publish_failpoint_for_test = 0;
    return true;
  }
  return false;
}
} // namespace jit

namespace {

// Get-or-create the CodeExtra block.  The two callers want different
// failure contracts: counters and observation are best-effort and swallow
// an allocation failure, while machine-code publication must surface the
// MemoryError so force_compile() reports what actually happened instead of
// a generic capability refusal.
CodeExtra* codeExtraImpl(PyCodeObject* code, bool preserve_error) {
  auto* state = cinderx::getModuleState();
  // On shutdown the module state becomes inaccessible.
  if (state == nullptr) {
    return nullptr;
  }
  Py_ssize_t extra_index = state->code_extra_index;
  if (extra_index == -1) {
    return nullptr;
  }

  auto code_obj = reinterpret_cast<PyObject*>(code);

  // Lock the code object to prevent concurrent get-or-create races under
  // FT-Python. Under GIL builds this is a no-op.
  jit::CriticalSectionGuard guard(code_obj);

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(code_obj, extra_index, &data_ptr) < 0) {
    JIT_LOG("Failed to get code extra data for {}", codeName(code));
    if (!preserve_error) {
      jit::printPythonException();
      PyErr_Clear();
    }
    return nullptr;
  }
  if (data_ptr != nullptr) {
    return reinterpret_cast<CodeExtra*>(data_ptr);
  }

#if PY_VERSION_HEX < 0x030C0000
  auto block = reinterpret_cast<OwnedCodeExtra311*>(
      PyMem_Calloc(1, sizeof(OwnedCodeExtra311)));
  if (block == nullptr) {
    if (preserve_error) {
      PyErr_NoMemory();
    }
    return nullptr;
  }
  block->owner = code;
  g_live_code_extra_blocks++;
  CodeExtra* extra = &block->extra;
  void* owned = block;
#else
  auto extra = reinterpret_cast<CodeExtra*>(PyMem_Calloc(1, sizeof(CodeExtra)));
  if (extra == nullptr) {
    if (preserve_error) {
      PyErr_NoMemory();
    }
    return nullptr;
  }
  void* owned = extra;
#endif

  int set_rc;
  if (jit::consumeJitPublishStepForTest(5)) {
    // Model setCodeExtraCapped() failing out of memory, exception
    // included: what is under test from here on is the preserve-or-clear
    // branch below and the propagation above it, not the injection site.
    PyErr_NoMemory();
    set_rc = -1;
  } else {
    set_rc = setCodeExtraCapped(code_obj, extra_index, extra);
  }
  if (set_rc < 0) {
    JIT_LOG("Failed to set code extra data for {}", codeName(code));
    if (!preserve_error) {
      jit::printPythonException();
      PyErr_Clear();
    }
#if PY_VERSION_HEX < 0x030C0000
    // The gauge was taken when the block was made; this path gives the
    // block back, so it has to give the count back too.  A gauge that
    // only ever rises reports a leak on the one path that did not leak.
    if (g_live_code_extra_blocks > 0) {
      g_live_code_extra_blocks--;
    }
#endif
    PyMem_Free(owned);
    return nullptr;
  }

  return extra;
}

size_t codeCallCount(PyCodeObject* code) {
  CodeExtra* extra = codeExtraIfPresent(code);
  return extra != nullptr ? Ci_code_extra_get_calls(extra) : 0;
}

} // namespace

CodeExtra* codeExtra(PyCodeObject* code) {
  return codeExtraImpl(code, false /* preserve_error */);
}

CodeExtra* codeExtraOrError(PyCodeObject* code) {
  return codeExtraImpl(code, true /* preserve_error */);
}

CodeExtra* codeExtraIfExists(PyCodeObject* code) {
  auto* state = cinderx::getModuleState();
  // On shutdown the module state becomes inaccessible.
  if (state == nullptr) {
    return nullptr;
  }
  Py_ssize_t extra_index = state->code_extra_index;
  if (extra_index == -1) {
    return nullptr;
  }

  auto code_obj = reinterpret_cast<PyObject*>(code);

  // Match codeExtra()'s locking discipline without allocating on misses.
  jit::CriticalSectionGuard guard(code_obj);

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(code_obj, extra_index, &data_ptr) < 0) {
    JIT_LOG("Failed to get code extra data for {}", codeName(code));
    jit::printPythonException();
    PyErr_Clear();
    return nullptr;
  }
  return reinterpret_cast<CodeExtra*>(data_ptr);
}

#if PY_VERSION_HEX < 0x030C0000
bool codeAutoJitDisabled311(PyCodeObject* code) {
  CodeExtra* extra = codeExtraIfExists(code);
  return extra != nullptr && Ci_code_extra_jit311_auto_disabled(extra);
}

void disableCodeAutoJit311(PyCodeObject* code) {
  CodeExtra* extra = codeExtra(code);
  if (extra != nullptr) {
    Ci_code_extra_jit311_disable_auto(extra);
  }
}
#endif

int numLocals(PyCodeObject* code) {
  return code->co_nlocals;
}

int numCellvars(PyCodeObject* code) {
  return code->co_ncellvars;
}

int numFreevars(PyCodeObject* code) {
  return code->co_nfreevars;
}

int numLocalsplus(PyCodeObject* code) {
  return code->co_nlocalsplus;
}

} // extern "C"
