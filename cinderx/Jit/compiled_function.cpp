// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/disassembler.h"
#if PY_VERSION_HEX < 0x030C0000
// The MR-04 execute surface, defined in Jit/pyjit_311_gate.cpp.
#include "cinderx/Interpreter/3.11/observe.h"
#endif
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <iostream>

extern "C" {

bool isJitCompiled(const PyFunctionObject* func) {
  // Possible that this is called during finalization after the module state is
  // destroyed.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return false;
  }
#if PY_VERSION_HEX < 0x030C0000
  // On 3.11 the installed entry is the guarded one, which is ordinary
  // extension code rather than generated code, so the allocator test below
  // cannot see it.  The question this answers is whether a call will
  // execute machine code, and for the guarded entry that is exactly
  // whether the function's current code object still has a published
  // artifact -- which is also what makes the answer go false again after a
  // __code__ swap.
  if (reinterpret_cast<void*>(func->vectorcall) ==
      reinterpret_cast<void*>(Ci_JitShell311_GuardedEntry)) {
    // Exactly the predicate the entry uses for function state, so this
    // cannot report a function as compiled after a __code__ swap that
    // already sent every call to the interpreter.  Live defaults are
    // rebound by the generated prologue and do not clear the artifact.
    return Ci_JitShell311_InstalledArtifact(
               const_cast<PyFunctionObject*>(func)) != nullptr;
  }
#endif
  jit::ICodeAllocator* code_allocator = mod_state->code_allocator.get();
  return code_allocator != nullptr &&
      code_allocator->contains(reinterpret_cast<const void*>(func->vectorcall));
}

} // extern "C"

namespace jit {

// The key used to store the CompiledFunction in a function's __dict__.
PyObject *kCompiledFunctionKey, *kNestedCompiledFunctionsKey = nullptr;

namespace {

void compiledfunc_dealloc(PyObject* self) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  PyObject_GC_UnTrack(self);

  // Call destructor for C++ members.
  cf->~CompiledFunction();

  Py_TYPE(self)->tp_free(self);
}

int compiledfunc_traverse(PyObject* self, visitproc visit, void* arg) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  return cf->traverse(visit, arg);
}

int compiledfunc_clear(PyObject* self) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  cf->clear();
  return 0;
}

// A CompiledFunction holds per-process JIT state (machine code, native entry
// point, CodeRuntime) stashed under a function's __dict__.  It cannot and need
// not survive pickling or copying: the underlying function serializes normally
// and is re-JIT'd in the destination process.  Reduce it to a call that yields
// a harmless None placeholder so cloudpickle/pickle/copy of any JIT-compiled
// function succeed.
PyObject* compiledfunc_reduce(PyObject* /*self*/, PyObject* /*ignored*/) {
  // The reconstructor lives in the native cinderjit module (not the cinderx
  // Python layer), keeping this a native -> native reference.  It is always
  // loaded when a CompiledFunction exists.
  auto jit_module = Ref<>::steal(PyImport_ImportModule("cinderjit"));
  if (jit_module == nullptr) {
    return nullptr;
  }
  auto reconstructor = Ref<>::steal(PyObject_GetAttrString(
      jit_module.get(), "_reconstruct_pickled_compiled_function"));
  if (reconstructor == nullptr) {
    return nullptr;
  }
  // The 2-tuple (reconstructor, ()) tells pickle/copy to rebuild the value by
  // calling reconstructor(), which returns None.
  return Py_BuildValue("(O())", reconstructor.get());
}

PyMethodDef compiledfunc_methods[] = {
    {"__reduce__",
     jit::compiledfunc_reduce,
     METH_NOARGS,
     PyDoc_STR("Reduce to a picklable None placeholder, dropping JIT machine "
               "code so a JIT-compiled function can be pickled/copied.")},
    {nullptr, nullptr, 0, nullptr},
};

static PyTypeObject _CiCompiledFunction_Type = {
    .ob_base = PyVarObject_HEAD_INIT(NULL, 0).tp_name = "CompiledFunction",
    .tp_basicsize = sizeof(jit::CompiledFunction),
    .tp_dealloc = jit::compiledfunc_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = jit::compiledfunc_traverse,
    .tp_clear = jit::compiledfunc_clear,
    .tp_methods = jit::compiledfunc_methods,
};

} // namespace

} // namespace jit

namespace jit {

int initCompiledFunctionType() {
  if (PyType_Ready(&_CiCompiledFunction_Type) < 0) {
    return -1;
  }

  // Create the key used to store CompiledFunction in function's __dict__.
  kCompiledFunctionKey =
      PyUnicode_InternFromString("__cinderx_compiled_func__");
  if (kCompiledFunctionKey == nullptr) {
    return -1;
  }

  kNestedCompiledFunctionsKey =
      PyUnicode_InternFromString("__cinderx_nested_compiled_funcs__");
  if (kNestedCompiledFunctionsKey == nullptr) {
    return -1;
  }

  return 0;
}

BorrowedRef<PyTypeObject> getCompiledFunctionType() {
  return &_CiCompiledFunction_Type;
}

Ref<CompiledFunction> CompiledFunction::create(
    CompiledFunctionData&& compiled_func,
    bool immortal) {
  // Allocate the Python object.
  CompiledFunction* cf;
  if (!immortal) {
    cf = PyObject_GC_New(CompiledFunction, &_CiCompiledFunction_Type);
    if (cf == nullptr) {
      return nullptr;
    }
    // Use placement new to construct C++ members in-place.
    new (cf) CompiledFunction(std::move(compiled_func));
    PyObject_GC_Track(reinterpret_cast<PyObject*>(cf));
  } else {
    cf = new CompiledFunction(std::move(compiled_func));
    if (cf == nullptr) {
      return nullptr;
    }
    PyObject_Init(&cf->ob_base, &_CiCompiledFunction_Type);
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(&cf->ob_base);
#else
    _Py_SetImmortal(&cf->ob_base);
#endif
  }

  // Trigger-proof accounting: every compiled-function object ever created
  // is counted, regardless of how it is later installed or released.
  jit::triggerStatsOnCompiledFunctionCreate();
  // Physical residency: the object now owns executable memory, and holds
  // it until its destructor -- clear() and registry removal do not release
  // the buffer, and an externally pinned artifact keeps its machine code.
  if (cf->codeBuffer().data() != nullptr) {
    jit::triggerStatsOnCodeBufferAcquired();
  }

  // Return a stolen reference - the caller owns it.
  return Ref<CompiledFunction>::steal(cf);
}

CompiledFunction::~CompiledFunction() {
  clear();

  if (data_.code.data() != nullptr) {
    // The buffer's lifetime ends with this object (or ended with the
    // allocator at teardown); either way it stops being resident here.
    jit::triggerStatsOnCodeBufferReleased();
    auto mod_state = cinderx::getModuleState();
    if (mod_state != nullptr) {
      auto code_allocator = mod_state->code_allocator.get();
      if (code_allocator != nullptr) {
        code_allocator->releaseCode(const_cast<std::byte*>(data_.code.data()));
      }
    }
  }
}

bool associateFunctionWithCompiled(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled,
    bool is_nested,
    Ref<>* displaced_anchor) {
  if (_Py_IsImmortal(func)) {
    // The function can never be freed, so we can keep the CompiledFunction
    // alive
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(compiled);
#else
    _Py_SetImmortal(compiled);
#endif
#if PY_VERSION_HEX < 0x030C0000
    // The 3.11 execute ledger still requires the association: the guarded
    // entry runs machine code only for a function its artifact names, so
    // skipping addFunction() here would leave a function the registry
    // calls compiled but the entry forever refuses.  The reference this
    // takes on a pseudo-immortal function is moot by definition.
    compiled->addFunction(func);
#endif
    return true;
  } else if (_Py_IsImmortal(compiled)) {
    // The CompiledFunction is already immortal, no need to associate it
    return true;
  }
  // Store a reference to the CompiledFunction in the function's __dict__.
  JIT_DCHECK(
      kCompiledFunctionKey != nullptr && kNestedCompiledFunctionsKey != nullptr,
      "failed to create compilation key");
  PyObject* func_dict = func->func_dict;
  if (func_dict == nullptr) {
    func_dict = PyDict_New();
    if (func_dict == nullptr) {
      return false;
    }
    func->func_dict = func_dict;
  }

  if (is_nested) {
    // Store CompiledFunction for nested function in outer function, we'll
    // also store it in the nested function's __dict__ when we do a
    // finalizeFunc on it.
    Ref<> nested_list = getDictRef(func_dict, kNestedCompiledFunctionsKey);
    if (nested_list == nullptr || !PyList_CheckExact(nested_list)) {
      if (PyErr_Occurred()) {
        return false;
      }
      nested_list = Ref<>::steal(PyList_New(0));
      if (nested_list == nullptr) {
        return false;
      } else if (
          PyDict_SetItem(func_dict, kNestedCompiledFunctionsKey, nested_list) <
          0) {
        return false;
      }
    }
    return PyList_Append(nested_list, compiled) == 0;
  }
  if (displaced_anchor != nullptr) {
    // Detain the value this write displaces.  PyDict_SetItem releases the
    // old value in place, and on 3.11 that value is the prior artifact's
    // owning reference: destroying it here would run arbitrary Python in
    // the middle of a publication (a __del__ can reenter force_compile()).
    // The caller holds the reference until its transaction settles, and
    // uses it as the restore token if the takeover is rolled back.
    BorrowedRef<> displaced =
        PyDict_GetItemWithError(func_dict, kCompiledFunctionKey);
    if (displaced == nullptr && PyErr_Occurred()) {
      PyErr_Clear();
    }
    if (displaced != nullptr) {
      *displaced_anchor = Ref<>::create(displaced);
    }
  }

  // Store the reference (this increfs the CompiledFunction).
  if (PyDict_SetItem(
          func_dict,
          kCompiledFunctionKey,
          reinterpret_cast<PyObject*>(compiled.get())) < 0) {
    return false;
  }

  // Add the function to the CompiledFunction's set.
#if PY_VERSION_HEX < 0x030C0000
  // The set insert can throw std::bad_alloc, and the dictionary anchor
  // is already written: letting the exception escape here would leave
  // an owned, registered-nowhere artifact behind.  Take the anchor back
  // and report through the same channel as the dictionary failure.
  try {
    throwIfJitPublishStepArmedForTest(1);
    compiled->addFunction(func);
  } catch (const std::bad_alloc&) {
    if (PyDict_DelItem(func_dict, kCompiledFunctionKey) < 0) {
      PyErr_Clear();
    }
    PyErr_NoMemory();
    return false;
  }
#else
  compiled->addFunction(func);
#endif

  return true;
}

void CompiledFunction::disassemble() const {
  auto start = reinterpret_cast<const char*>(codeBuffer().data());
  Disassembler dis{start, codeSize()};
  dis.disassembleAll(std::cout);
}

void CompiledFunction::printHIR() const {
  JIT_CHECK(
      data_.irfunc != nullptr,
      "Can only call CompiledFunction::printHIR() from a debug build");
  jit::hir::HIRPrinter printer;
  printer.Print(std::cout, *data_.irfunc);
}

std::chrono::nanoseconds CompiledFunction::compileTime() const {
  return data_.compile_time;
}

void CompiledFunction::setCompileTime(std::chrono::nanoseconds time) {
  data_.compile_time = time;
}

void CompiledFunction::setCodePatchers(
    std::vector<std::unique_ptr<CodePatcher>>&& code_patchers) {
  data_.code_patchers = std::move(code_patchers);
}

void CompiledFunction::setHirFunc(std::unique_ptr<hir::Function>&& irfunc) {
  data_.irfunc = std::move(irfunc);
}

void* CompiledFunction::staticEntry() const {
  if (data_.runtime == nullptr ||
      !(data_.runtime->code()->co_flags & CI_CO_STATICALLY_COMPILED)) {
    return nullptr;
  }

  return reinterpret_cast<void*>(
      JITRT_GET_STATIC_ENTRY(data_.vectorcall_entry));
}

void CompiledFunction::addFunction(BorrowedRef<PyFunctionObject> func) {
  // Store a borrowed reference to the function. The function is responsible
  // for removing itself via funcDestroyed() when it is deallocated.
  // We don't incref to avoid preventing garbage collection of functions
  // when multiple functions share the same CompiledFunction.
  //
  // On 3.11 that responsibility is carried by the weak-reference death watch
  // the context arms (see Context::watchFunctionDeath), which stands in for
  // the function watcher this branch does not have.
  functions_.insert(func.get());
}

void CompiledFunction::removeFunction(BorrowedRef<PyFunctionObject> func) {
  // Remove the borrowed reference. No decref needed since we don't own it.
  functions_.erase(func.get());
}

int CompiledFunction::traverse(visitproc visit, void* arg) {
  // Don't traverse functions_ - these are borrowed references that we don't
  // own. The functions are responsible for removing themselves via
  // funcDestroyed() when they are deallocated. Not traversing them allows
  // functions to be garbage collected independently of this CompiledFunction.

  // Traverse all references held by the CodeRuntime.
  if (data_.runtime != nullptr) {
    return data_.runtime->traverse(visit, arg);
  }

  return 0;
}

#if PY_VERSION_HEX < 0x030C0000
void CompiledFunction::forgetFunctions() {
  functions_.clear();
}
#endif

void CompiledFunction::clear(bool context_finalizing) {
  // Copy function pointers before clearing the set.
  if (owner_ != nullptr) {
    if (!context_finalizing) {
      // These will be cleared on their own if we're finaling the context,
      // let's not complicate the iteration.
      owner_->forgetCompiledFunction(*this);
    }
    for (auto& patcher : data_.code_patchers) {
      if (auto typed_patcher = dynamic_cast<TypeDeoptPatcher*>(patcher.get())) {
        owner_->unwatch(typed_patcher);
      }
    }

    // We only de-opt the functions if we still have our owner. This is so that
    // our multi-threaded compile tests work properly where we are clearing
    // things with functions still running.
    auto funcs_to_deopt = std::move(functions_);

    // Deopt all associated functions. No decref needed since these are borrowed
    // refs.
    for (PyFunctionObject* func : funcs_to_deopt) {
#if PY_VERSION_HEX < 0x030C0000
      // On 3.11 membership means "claimed", not "installed by me", and
      // forgetCompiledFunction() above has already detached exactly the
      // entry points this artifact still had installed.  Writing every
      // member's entry point here would hand a stale generation the power
      // to deopt its successor's fresh installation.  The one caller that
      // skips the owner's bookkeeping is context finalization, where the
      // registries are already gone and every artifact dies: the
      // interpreter entry is restored unconditionally there.
      if (context_finalizing) {
        func->vectorcall = getInterpretedVectorcall(func);
      }
#else
      func->vectorcall = getInterpretedVectorcall(func);
#endif
    }

    owner_ = nullptr;
  }

  // Clear all references held by the CodeRuntime.
  if (data_.runtime != nullptr) {
    data_.runtime->releaseReferences();
    data_.runtime = nullptr;
  }
}

} // namespace jit
