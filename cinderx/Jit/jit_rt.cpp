// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/jit_rt.h"

#include "internal/pycore_call.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pyerrors.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/string.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/inline_cache.h"
#if PY_VERSION_HEX < 0x030C0000
#include "cinderx/Interpreter/3.11/interpreter_contract.h"
#include "cinderx/Interpreter/3.11/observe.h"
#endif
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/trigger_stats.h"
// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Immortalize/immortalize.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_stackref.h"
#include "internal/pycore_unicodeobject.h"
#endif

#include "internal/pycore_typeobject.h"

#ifdef Py_GIL_DISABLED
#include "internal/pycore_gc.h"
#include "internal/pycore_qsbr.h"
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

// Argument binding is not a pure function: keyword equality and
// kwdefaults lookup can run arbitrary Python.  The binder therefore
// distinguishes three outcomes so a pending exception is never replayed
// through CPython's vectorcall, and so a successful bind re-enters the
// snapshot captured before any of that Python ran.
enum class BindKwStatus { Error, Fallback, Bound };

template <typename T>
std::unique_ptr<T[]> allocateBindArray(size_t n) {
  try {
    return std::make_unique<T[]>(n);
  } catch (const std::bad_alloc&) {
    PyErr_NoMemory();
    return nullptr;
  }
}

// Caller-provided args stay borrowed.  Defaults / kwdefaults must stay alive
// until reentry returns: either attribute can be rebound while the compiled
// body is running, and the kwdefaults dict can also be mutated in place.
//
// A defaults tuple is immutable, so one reference to the tuple pins every
// positional default without allocating per-value ownership storage.  A
// kwdefaults dict is mutable, so each selected value still needs its own
// reference.
struct BoundDefaultRefs {
  Ref<> positional_tuple;
  // Owned keyword-only defaults are held in an inline array; the vector
  // (allocated on the first emplace) only takes the overflow.  Both release
  // after the call has returned.
  static constexpr size_t kInlineKeywordValues = 8;
  PyObject* inline_keyword_values[kInlineKeywordValues];
  size_t num_inline_keyword_values{0};
  std::vector<Ref<>> keyword_values;
  ~BoundDefaultRefs() {
    for (size_t i = 0; i < num_inline_keyword_values; i++) {
      Py_DECREF(inline_keyword_values[i]);
    }
  }
};

static void bindPositionalDefault(
    BoundDefaultRefs& owned_defaults,
    PyObject* defaults,
    PyObject** slot,
    PyObject* def) {
  if (owned_defaults.positional_tuple == nullptr) {
    owned_defaults.positional_tuple = Ref<>::create(defaults);
  }
  *slot = def;
}

static void bindKeywordDefault(
    BoundDefaultRefs& owned_defaults,
    PyObject** slot,
    PyObject* def) {
  if (owned_defaults.num_inline_keyword_values <
      BoundDefaultRefs::kInlineKeywordValues) {
    Py_INCREF(def);
    owned_defaults
        .inline_keyword_values[owned_defaults.num_inline_keyword_values++] =
        def;
    *slot = def;
    return;
  }
  owned_defaults.keyword_values.emplace_back(Ref<>::create(def));
  *slot = owned_defaults.keyword_values.back().get();
}

#if PY_VERSION_HEX < 0x030C0000
// Keyword-only defaults table.  A call that leaves a keyword-only parameter
// unbound looked its default up in func->func_kwdefaults by name on every
// call, one dict probe per parameter.  The values are memoised per
// (defaults dict, its version tag, code): the version tag is bumped by every
// mutation of the dict and is unique across dicts, so a matching entry still
// describes the live dict, whose own references keep the memoised values
// alive until they are taken as owned references by the binder.
namespace {
constexpr size_t kKwDefaultsEntries311 = 256;
constexpr Py_ssize_t kKwDefaultsMax311 = 12;
struct KwDefaultsEntry311 {
  PyObject* kwdefs{nullptr}; // identity only, never dereferenced (the
                             // version tag is process-unique, so a later
                             // dict at this address never matches)
  PyCodeObject* code{nullptr}; // held: the values are laid out by this
                               // code's parameter names, and a code object
                               // that died could be succeeded at its
                               // address by one with another signature
  uint64_t version{0};
  PyObject* values[kKwDefaultsMax311]; // borrowed; nullptr = no default
};
KwDefaultsEntry311 s_kwdefaults_311[kKwDefaultsEntries311];

// The memoised defaults of `co`'s keyword-only parameters under `kwdefs`,
// or nullptr (too many parameters; a lookup raised, with *error set).
// Nothing here runs Python: the entry's previous code object, when the
// rewrite displaces one, is handed back in *retired for the caller to
// release once it owns the values it took from the shared array -- the
// release can run a weakref callback (code_dealloc clears weak references),
// which can refill this very entry or empty the defaults dict.
const PyObject* const* kwOnlyDefaults311(
    PyCodeObject* co,
    PyObject* kwdefs,
    bool* error,
    PyObject** retired) {
  *error = false;
  *retired = nullptr;
  const Py_ssize_t nkw = co->co_kwonlyargcount;
  if (nkw > kKwDefaultsMax311) {
    return nullptr;
  }
  // The lookups below collect borrowed pointers and run no Python only on
  // a unicode-keyed table (every key an exact str, compared without a
  // call); any other table can run a key's __eq__, which can drop a default
  // collected earlier.  Such a dict takes the generic bind, which owns each
  // default as it is found.
  if (!hasOnlyUnicodeKeys(kwdefs)) {
    return nullptr;
  }
  // Slots are chosen by the code object, not the defaults dict: a function
  // re-created on every call brings a fresh dict (and address) each time,
  // and indexing by dict would spread one code's entries over the table,
  // each holding a reference to the code until some later dict happens to
  // land on that slot.  Per code, the entry is rewritten in place and the
  // table reaches a steady state after one fill, like the type cache.
  KwDefaultsEntry311& e = s_kwdefaults_311
      [(reinterpret_cast<uintptr_t>(co) >> 4) & (kKwDefaultsEntries311 - 1)];
  const uint64_t version =
      reinterpret_cast<PyDictObject*>(kwdefs)->ma_version_tag;
  if (e.kwdefs == kwdefs && e.code == co && e.version == version) {
    return e.values;
  }
  // The entry is rewritten in place: retire its old key first so that a
  // lookup that raises midway cannot leave another key over half-written
  // values.
  e.kwdefs = nullptr;
  for (Py_ssize_t i = 0; i < nkw; i++) {
    PyObject* name = jit::getVarname(co, co->co_argcount + i);
    PyObject* def = PyDict_GetItemWithError(kwdefs, name);
    if (def == nullptr && _PyErr_Occurred(_PyThreadState_GET())) {
      *error = true;
      return nullptr;
    }
    e.values[i] = def;
  }
  e.kwdefs = kwdefs;
  Py_INCREF(co);
  *retired = reinterpret_cast<PyObject*>(e.code);
  e.code = co;
  e.version = version;
  return e.values;
}
} // namespace
#endif

// Argument array for one bind: inline for the usual arities, heap beyond.
class BindArgs {
 public:
  PyObject** allocate(size_t n) {
    if (n <= kInline) {
      return inline_;
    }
    heap_ = allocateBindArray<PyObject*>(n);
    return heap_.get();
  }

 private:
  static constexpr size_t kInline = 32;
  PyObject* inline_[kInline];
  std::unique_ptr<PyObject*[]> heap_;
};

// This is mostly taken from ceval.c _PyEval_EvalCodeWithName
// We use the same logic to turn **args, nargsf, and kwnames into
// **args / nargsf.
static BindKwStatus JITRT_BindKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    PyObject** arg_space,
    Py_ssize_t total_args,
    Ref<PyObject>& kwdict,
    Ref<PyObject>& varargs,
    BoundDefaultRefs& owned_defaults,
    std::vector<Ref<>>& retired_codes) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  Py_ssize_t argcount = PyVectorcall_NARGS(nargsf);
  const Py_ssize_t named_args = co->co_argcount + co->co_kwonlyargcount;

  for (int i = 0; i < total_args; i++) {
    arg_space[i] = nullptr;
  }

  // Create a dictionary for keyword parameters (**kwags)
  if (co->co_flags & CO_VARKEYWORDS) {
    kwdict = Ref<>::steal(PyDict_New());
    if (kwdict == nullptr) {
      return BindKwStatus::Error;
    }
    arg_space[total_args - 1] = kwdict;
  }

  // Copy all positional arguments into local variables
  Py_ssize_t n = std::min<Py_ssize_t>(argcount, co->co_argcount);
  for (Py_ssize_t j = 0; j < n; j++) {
    arg_space[j] = args[j];
  }

  // Pack other positional arguments into the *args argument
  if (co->co_flags & CO_VARARGS) {
    varargs = Ref<>::steal(Cix_PyTuple_FromArray(args + n, argcount - n));
    if (varargs == nullptr) {
      return BindKwStatus::Error;
    }

    Py_ssize_t i = total_args - 1;
    if (co->co_flags & CO_VARKEYWORDS) {
      i--;
    }
    arg_space[i] = varargs;
  }

  // Handle keyword arguments passed as two strided arrays
  if (kwnames != nullptr) {
    for (Py_ssize_t i = 0; i < PyTuple_Size(kwnames); i++) {
      PyObject* keyword = PyTuple_GET_ITEM(kwnames, i);
      PyObject* value = args[argcount + i];
      Py_ssize_t j;

      // 3.11: exact str only.  A str subclass with __eq__ must not be
      // rich-compared here: a miss would Fallback and CPython would
      // run __eq__ again.  3.12+ Static Python primitive prologues
      // treat Fallback as a TypeError sentinel, so they keep
      // PyUnicode_Check and still accept str subclasses.
#if PY_VERSION_HEX < 0x030C0000
      if (keyword == nullptr || !PyUnicode_CheckExact(keyword)) {
#else
      if (keyword == nullptr || !PyUnicode_Check(keyword)) {
#endif
        return BindKwStatus::Fallback;
      }

      // Speed hack: do raw pointer compares. As names are
      //    normally interned this should almost always hit.
      for (j = co->co_posonlyargcount; j < named_args; j++) {
        PyObject* name = jit::getVarname(co, j);
        if (name == keyword) {
          goto kw_found;
        }
      }

      // Slow fallback, just in case
      for (j = co->co_posonlyargcount; j < named_args; j++) {
        PyObject* name = jit::getVarname(co, j);
        int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
        if (cmp > 0) {
          goto kw_found;
        } else if (cmp < 0) {
          return BindKwStatus::Error;
        }
      }

      if (kwdict == nullptr) {
        return BindKwStatus::Fallback;
      }
      if (PyDict_SetItem(kwdict, keyword, value) == -1) {
        return BindKwStatus::Error;
      }
      continue;

    kw_found:
      if (arg_space[j] != nullptr) {
        return BindKwStatus::Fallback;
      }
      arg_space[j] = value;
    }
  }

  // Check the number of positional arguments
  if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
    return BindKwStatus::Fallback;
  }

  // Add missing positional arguments (copy default values from defs)
  if (argcount < co->co_argcount) {
    PyObject* defaults = func->func_defaults;
    Py_ssize_t defcount = defaults == nullptr ? 0 : PyTuple_Size(defaults);
    Py_ssize_t first_default_arg = co->co_argcount - defcount;

    // Any unset slot before the defaults region means we are missing
    // a required positional argument.
    for (Py_ssize_t i = argcount; i < first_default_arg; i++) {
      if (arg_space[i] == nullptr) {
        return BindKwStatus::Fallback;
      }
    }

    if (defaults != nullptr) {
      PyObject* const* defs = &((PyTupleObject*)defaults)->ob_item[0];
      // Only slots in the defaults-covered tail can still need filling.
      Py_ssize_t arg_index = std::max(argcount, first_default_arg);
      for (; arg_index < co->co_argcount; arg_index++) {
        if (arg_space[arg_index] == nullptr) {
          Py_ssize_t def_index = arg_index - first_default_arg;
          bindPositionalDefault(
              owned_defaults, defaults, &arg_space[arg_index], defs[def_index]);
        }
      }
    }
  }

  // Add missing keyword arguments (copy default values from kwdefs)
  if (co->co_kwonlyargcount > 0) {
    Py_ssize_t missing = 0;
    PyObject* kwdefs = func->func_kwdefaults;
#if PY_VERSION_HEX < 0x030C0000
    // The memo is consulted at the first unbound keyword-only parameter
    // only; a call that named every one of them pays nothing here.
    const PyObject* const* cached = nullptr;
    bool probed = false;
#endif
    for (Py_ssize_t i = co->co_argcount; i < named_args; i++) {
      PyObject* name;
      if (arg_space[i] != nullptr) {
        continue;
      }
#if PY_VERSION_HEX < 0x030C0000
      if (kwdefs != nullptr && !probed) {
        probed = true;
        bool error = false;
        PyObject* retired = nullptr;
        cached = kwOnlyDefaults311(co, kwdefs, &error, &retired);
        if (retired != nullptr) {
          // Released by the caller after the call it is binding for: the
          // release can run Python (a weak reference callback), which could
          // re-code the function under the argument space, the binding and
          // the re-entry that were all derived from this code object.
          retired_codes.emplace_back(Ref<>::steal(retired));
        }
        if (error) {
          return BindKwStatus::Error;
        }
      }
      if (cached != nullptr) {
        PyObject* def = const_cast<PyObject*>(cached[i - co->co_argcount]);
        if (def != nullptr) {
          bindKeywordDefault(owned_defaults, &arg_space[i], def);
          continue;
        }
        missing++;
        continue;
      }
#endif
      name = jit::getVarname(co, i);
      if (kwdefs != nullptr) {
        PyObject* def = PyDict_GetItemWithError(kwdefs, name);
        if (def) {
          bindKeywordDefault(owned_defaults, &arg_space[i], def);
          continue;
        } else if (_PyErr_Occurred(_PyThreadState_GET())) {
          return BindKwStatus::Error;
        }
      }
      missing++;
    }
    if (missing) {
      return BindKwStatus::Fallback;
    }
  }

  return BindKwStatus::Bound;
}

#if PY_VERSION_HEX < 0x030C0000 && defined(CINDER_AARCH64)
// Activation bracket for the generated prologue's argument helpers under the
// CPython 3.11 direct entry.  The live-frame count that defers artifact
// reclamation (triggerStatsActiveFramesAddress) is otherwise maintained by
// the body: counted in once its frame is linked, out at its hard exit.  The
// helpers run before that count-in and after that count-out, and both halves
// can run Python -- the keyword binder's fallback evaluates the whole call in
// the interpreter, the defaults tuple released on the way out can run a
// destructor -- yet the helper returns into the artifact's prologue exit
// afterwards.  A retirement in that window must be deferred like one inside
// the body, so the helper counts itself as a live frame for its whole
// extent.  Declared first in each helper so it is destroyed last, after
// every owned reference the helper releases; nothing runs between its
// count-out and the return into machine code.
class PrologueActivation311 {
 public:
  PrologueActivation311() {
    ++*jit::triggerStatsActiveFramesAddress();
  }
  ~PrologueActivation311() {
    --*jit::triggerStatsActiveFramesAddress();
  }
  PrologueActivation311(const PrologueActivation311&) = delete;
  PrologueActivation311& operator=(const PrologueActivation311&) = delete;
};
#else
class PrologueActivation311 {};
#endif

#if PY_VERSION_HEX < 0x030C0000
// The generated vectorcall prologue dispatches here, then re-enters the
// compiled body at JITRT_CALL_REENTRY_OFFSET before the vectorcall entry.
// On 3.11 func->vectorcall is Ci_JitShell311_GuardedEntry, not that
// generated entry, so subtracting the offset from it would jump into
// unrelated bytes.  Use the invocation snapshot first: GuardedEntry pins
// the CompiledFunction for this call, and binding must not re-query
// function state after keyword equality has run.  InstalledArtifact is
// only the fallback when this helper is entered without a snapshot.
// The invocation artifact published by a C entry (the by-code dispatch of
// an unattached function object, the RuntimeTests invoke) describes the
// function it was published for.  While that scope is open, a nested call
// of another function through its generated entry reaches the binding
// helpers too; its prologue must not take the outer artifact, so the
// snapshot only answers for the function's own code.
static jit::CompiledFunction* invocationArtifactFor311(PyFunctionObject* func) {
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InvocationArtifact());
  if (compiled == nullptr || compiled->runtime() == nullptr) {
    return nullptr;
  }
  // The artifact is compiled for one namespace: its global loads read the
  // globals fixed at compile time.  The same code under other globals or
  // builtins (an artifact retired while still running, a twin published
  // since) is another function as far as the snapshot is concerned.
  const jit::CodeRuntime* runtime = compiled->runtime();
  if (runtime->code().get() !=
          reinterpret_cast<PyCodeObject*>(func->func_code) ||
      reinterpret_cast<PyObject*>(runtime->globals().get()) !=
          func->func_globals ||
      reinterpret_cast<PyObject*>(runtime->builtins().get()) !=
          func->func_builtins) {
    return nullptr;
  }
  return compiled;
}

static vectorcallfunc jitrtBoundArgsReentry(PyFunctionObject* func) {
  // Captured before any binding runs user code: the artifact installed for
  // the function at this moment is the one whose prologue dispatched here.
  // Direct entry installs the generated entry itself, so there is no shell
  // snapshot; the installed-artifact lookup answers instead, and a function
  // that is no longer installed re-enters the interpreter with the bound
  // arguments.
  jit::CompiledFunction* compiled = invocationArtifactFor311(func);
  if (compiled == nullptr) {
    compiled = reinterpret_cast<jit::CompiledFunction*>(
        Ci_JitShell311_InstalledArtifact(func));
  }
  if (compiled != nullptr) {
    return JITRT_GET_REENTRY(compiled->vectorcallEntry());
  }
  return getInterpretedVectorcall(func);
}
#define JITRT_CAPTURE_REENTRY(func) jitrtBoundArgsReentry(func)

static PyThreadState* allocate_and_link_interpreter_frame(
    PyFunctionObject* func,
    PyCodeObject* co);

#if !defined(CINDER_AARCH64)
thread_local _PyInterpreterFrame* t_prelinked_recursion_frame = nullptr;
#endif
thread_local int t_jit_recursion_entries = 0;

#if !defined(CINDER_AARCH64)
// CPython 3.11 binds in initialize_locals, then consumes a recursion slot only
// after the attempted frame is linked at start_frame. Binding stays in the
// generated prologue. Normal frames are prelinked here and consumed by
// LoadFrame. Lightweight frames are linked inline by LoadFrame, so they only
// consume the recursion slot here; prelinking would create a duplicate Python
// frame and leave the TLS handoff occupied across recursive calls.
class RecursiveCallAfterBind {
 public:
  explicit RecursiveCallAfterBind(PyFunctionObject* func)
      : entries_before_{t_jit_recursion_entries} {
    JIT_CHECK(
        t_prelinked_recursion_frame == nullptr,
        "nested prelinked CPython 3.11 recursion frame");
    PyThreadState* tstate = PyThreadState_GET();
    JIT_DCHECK(tstate != nullptr, "thread state cannot be null");
    if (jit::getConfig().frame_mode != jit::FrameMode::kLightweight) {
      tstate = allocate_and_link_interpreter_frame(
          func, reinterpret_cast<PyCodeObject*>(func->func_code));
      t_prelinked_recursion_frame = currentFrame(tstate);
    }
    if (Ci_JitRecursionBoundary311_IsActive()) {
      Ci_JitRecursionBoundary311_Refuse(tstate);
      unlinkPrelinkedFrame(tstate);
      return;
    }
    if (_Py_EnterRecursiveCallTstate(tstate, "") == 0) {
      entered_ = true;
      t_jit_recursion_entries++;
      if (tstate->recursion_remaining == 0) {
        Ci_JitRecursionBoundary311_Enter();
        boundary_active_ = true;
      }
      return;
    }

    // _Py_CheckRecursiveCall restored recursion_remaining after setting the
    // exception. Stock temporarily consumes one slot while clearing the
    // failed entry frame (_PyEvalFrameClearAndPop); mirror that headroom.
    unlinkPrelinkedFrame(tstate);
  }
  ~RecursiveCallAfterBind() {
    if (t_prelinked_recursion_frame != nullptr) {
      // Reentry failed before LoadFrame consumed the prelinked frame (for
      // example, a static type check). Keep the frame chain balanced.
      PyThreadState* tstate = PyThreadState_GET();
      t_prelinked_recursion_frame = nullptr;
      JITRT_UnlinkFrame(tstate);
    }
    if (boundary_active_ && Ci_JitRecursionBoundary311_IsActive()) {
      Ci_JitRecursionBoundary311_Leave();
    }
    if (entered_ && t_jit_recursion_entries == entries_before_ + 1) {
      t_jit_recursion_entries--;
      _Py_LeaveRecursiveCallTstate(PyThreadState_GET());
    } else if (entered_) {
      JIT_CHECK(
          t_jit_recursion_entries == entries_before_,
          "unbalanced transferred 3.11 recursion slot: before={} after={}",
          entries_before_,
          t_jit_recursion_entries);
    }
  }
  RecursiveCallAfterBind(const RecursiveCallAfterBind&) = delete;
  RecursiveCallAfterBind& operator=(const RecursiveCallAfterBind&) = delete;
  bool entered() const {
    return entered_;
  }

 private:
  static void unlinkPrelinkedFrame(PyThreadState* tstate) {
    if (t_prelinked_recursion_frame == nullptr) {
      return;
    }
    t_prelinked_recursion_frame = nullptr;
    tstate->recursion_remaining--;
    JITRT_UnlinkFrame(tstate);
    tstate->recursion_remaining++;
  }

  bool entered_{false};
  bool boundary_active_{false};
  int entries_before_;
};
#else
// AArch64: the body links its own frame inline and carries the recursion
// accounting in its generated entry guard, so the bound-args reentry paths
// below hand the bound arguments straight back to the body.
#endif

void JITRT_TransferRecursionToInterpreter311() {
  if (t_jit_recursion_entries == 0) {
    return;
  }
  if (Ci_JitRecursionBoundary311_IsActive()) {
    Ci_JitRecursionBoundary311_Leave();
  }
  t_jit_recursion_entries--;
  _Py_LeaveRecursiveCallTstate(_PyThreadState_GET());
}

// Near-verbatim port of import_from() from the 3.11 ceval.c (the function
// is static there).  Ownership follows stock: the looked-up attribute or
// sys.modules entry is returned as a new reference; every failure path
// leaves an ImportError set and returns NULL.
PyObject*
JITRT_ImportFrom311(PyThreadState* tstate, PyObject* v, PyObject* name) {
  DEFINE_STATIC_STRING(__name__);
  DEFINE_STATIC_STRING(__spec__);
  PyObject* x = nullptr;
  if (_PyObject_LookupAttr(v, name, &x) != 0) {
    return x;
  }
  PyObject* pkgpath = nullptr;
  PyObject* pkgname_or_unknown = nullptr;
  PyObject* errmsg = nullptr;
  // Issue #17636: a circular relative import may have failed the attribute
  // lookup although the submodule is already in sys.modules.
  PyObject* pkgname = PyObject_GetAttr(v, s___name__);
  if (pkgname == nullptr) {
    goto error;
  }
  if (!PyUnicode_Check(pkgname)) {
    Py_CLEAR(pkgname);
    goto error;
  }
  {
    PyObject* fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == nullptr) {
      Py_DECREF(pkgname);
      return nullptr;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == nullptr && !_PyErr_Occurred(tstate)) {
      goto error;
    }
    Py_DECREF(pkgname);
    return x;
  }
error:
  pkgpath = PyModule_GetFilenameObject(v);
  if (pkgname == nullptr) {
    pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
    if (pkgname_or_unknown == nullptr) {
      Py_XDECREF(pkgpath);
      return nullptr;
    }
  } else {
    pkgname_or_unknown = pkgname;
  }
  if (pkgpath == nullptr || !PyUnicode_Check(pkgpath)) {
    _PyErr_Clear(tstate);
    errmsg = PyUnicode_FromFormat(
        "cannot import name %R from %R (unknown location)",
        name,
        pkgname_or_unknown);
    // NULL checks for errmsg and pkgname are done by PyErr_SetImportError.
    PyErr_SetImportError(errmsg, pkgname, nullptr);
  } else {
    PyObject* spec = PyObject_GetAttr(v, s___spec__);
    const char* fmt = _PyModuleSpec_IsInitializing(spec)
        ? "cannot import name %R from partially initialized module %R "
          "(most likely due to a circular import) (%S)"
        : "cannot import name %R from %R (%S)";
    Py_XDECREF(spec);
    errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
    PyErr_SetImportError(errmsg, pkgname, pkgpath);
  }
  Py_XDECREF(errmsg);
  Py_XDECREF(pkgname_or_unknown);
  Py_XDECREF(pkgpath);
  return nullptr;
}

// See jit_rt.h.
PyObject* JITRT_EnterExceptHandler311(uint64_t published_idx) {
  PyThreadState* tstate = _PyThreadState_GET();
  if (tstate->c_tracefunc != nullptr) {
    // Tracing: the evaluator owns the 'exception' event; the caller deopts
    // with the exception untouched.
    return nullptr;
  }
  _PyInterpreterFrame* frame = currentFrame(tstate);
  // The evaluator's error path publishes the faulting position before the
  // traceback entry; the compiled code's own cursor stores may lag it.
  frame->prev_instr = _PyCode_CODE(frame->f_code) + published_idx;
  if (!_PyErr_Occurred(tstate)) {
    _PyErr_SetString(
        tstate, PyExc_SystemError, "error return without exception set");
  }
  if (!_PyFrame_IsIncomplete(frame)) {
    PyFrameObject* f = _PyFrame_GetFrameObject(frame);
    if (f != nullptr) {
      PyTraceBack_Here(f);
    }
  }
  PyObject* exc;
  PyObject* val;
  PyObject* tb;
  _PyErr_Fetch(tstate, &exc, &val, &tb);
  _PyErr_NormalizeException(tstate, &exc, &val, &tb);
  if (tb != nullptr) {
    PyException_SetTraceback(val, tb);
  } else {
    PyException_SetTraceback(val, Py_None);
  }
  Py_XDECREF(tb);
  Py_XDECREF(exc);
  return val;
}

PyObject* JITRT_PushExcInfo311(PyObject* exc) {
  _PyErr_StackItem* exc_info = _PyThreadState_GET()->exc_info;
  PyObject* prev = exc_info->exc_value;
  if (prev == nullptr) {
    prev = Py_NewRef(Py_None);
  }
  exc_info->exc_value = Py_NewRef(exc);
  return prev;
}

uint64_t JITRT_PopExcInfo311(PyObject* prev) {
  _PyErr_StackItem* exc_info = _PyThreadState_GET()->exc_info;
  PyObject* old = exc_info->exc_value;
  exc_info->exc_value = Py_NewRef(prev);
  Py_XDECREF(old);
  return 0;
}

int64_t JITRT_ExcMatch311(PyObject* exc, PyObject* type) {
  static const char* kCannotCatchMsg =
      "catching classes that do not inherit from BaseException is not allowed";
  PyThreadState* tstate = _PyThreadState_GET();
  if (PyTuple_Check(type)) {
    Py_ssize_t length = PyTuple_GET_SIZE(type);
    for (Py_ssize_t i = 0; i < length; i++) {
      if (!PyExceptionClass_Check(PyTuple_GET_ITEM(type, i))) {
        _PyErr_SetString(tstate, PyExc_TypeError, kCannotCatchMsg);
        return -1;
      }
    }
  } else if (!PyExceptionClass_Check(type)) {
    _PyErr_SetString(tstate, PyExc_TypeError, kCannotCatchMsg);
    return -1;
  }
  return PyErr_GivenExceptionMatches(exc, type) ? 1 : 0;
}

void JITRT_GetRecursionState311(
    int* remaining,
    int* headroom,
    int* boundary_active,
    int* jit_entries) {
  PyThreadState* tstate = _PyThreadState_GET();
  *remaining = tstate->recursion_remaining;
  *headroom = tstate->recursion_headroom;
  *boundary_active = Ci_JitRecursionBoundary311_IsActive();
  *jit_entries = t_jit_recursion_entries;
}

static PyObject* jitrtReenterAfterBind(
    vectorcallfunc reentry,
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames) {
#if !defined(CINDER_AARCH64)
  RecursiveCallAfterBind rec{reinterpret_cast<PyFunctionObject*>(func)};
  if (!rec.entered()) {
    return nullptr;
  }
#endif
  return reentry(func, args, nargsf, kwnames);
}

// Equal-argcount positional path: the prologue has already bound by
// matching nargs.  kwnames is unused; reentry always sees nullptr.
PyObject* JITRT_ReenterAfterBind(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* /*kwnames*/) {
  return jitrtReenterAfterBind(
      JITRT_CAPTURE_REENTRY(func),
      reinterpret_cast<PyObject*>(func),
      args,
      nargsf,
      nullptr);
}

#else
#define JITRT_CAPTURE_REENTRY(func) JITRT_GET_REENTRY((func)->vectorcall)

// The compiled-handler helpers are CPython 3.11 only; the CallCFunc table
// references them on every version.
PyObject* JITRT_EnterExceptHandler311(uint64_t) {
  JIT_ABORT("JITRT_EnterExceptHandler311 is CPython 3.11 only");
}
PyObject* JITRT_PushExcInfo311(PyObject*) {
  JIT_ABORT("JITRT_PushExcInfo311 is CPython 3.11 only");
}
uint64_t JITRT_PopExcInfo311(PyObject*) {
  JIT_ABORT("JITRT_PopExcInfo311 is CPython 3.11 only");
}
int64_t JITRT_ExcMatch311(PyObject*, PyObject*) {
  JIT_ABORT("JITRT_ExcMatch311 is CPython 3.11 only");
}
#endif

// This uses JITRT_BindKeywordArgs to get the newly bound keyword
// arguments.   We then turn around and dispatch to the
// JITed function with the newly packed args.
// Capture the reentry target before binding: the binder can run
// arbitrary Python.  Only CPython TypeError construction (missing /
// duplicate / unexpected keyword) falls back to the interpreter with
// the original vectorcall layout.  A pending exception is returned
// immediately and is never replayed.
PyObject* JITRT_CallWithKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames) {
  [[maybe_unused]] PrologueActivation311 activation;
  vectorcallfunc reentry = JITRT_CAPTURE_REENTRY(func);
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount +
      ((co->co_flags & CO_VARKEYWORDS) ? 1 : 0) +
      ((co->co_flags & CO_VARARGS) ? 1 : 0);
  // Code objects the keyword-only defaults memo displaced during this call,
  // released when the call is over: everything above and below (the
  // argument space, the binding, the re-entry) is derived from `co`, and a
  // release can run Python that re-codes the function.
  std::vector<Ref<>> retired_codes;
#if PY_VERSION_HEX < 0x030C0000
  // A *args / **kwargs callee is entered here on every call (its prologue
  // has no argument-count arm), and the usual shape is the positional one:
  // no keywords, every named parameter supplied.  The general binder below
  // clears and re-derives every slot, walks the default and keyword-only
  // tables and keeps its results behind owning holders; this shape needs
  // none of it: the named slots are the arguments as given, *args is the
  // tail (the empty tuple when there is none) and **kwargs is a fresh dict,
  // as initialize_locals builds them.  Missing positionals (defaults),
  // keyword-only parameters and keyword calls keep the general path; so
  // does an oversupplied call without *args, whose TypeError the
  // interpreter fallback produces.
  if (kwnames == nullptr && co->co_kwonlyargcount == 0 && total_args <= 32) {
    const Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    const Py_ssize_t argcount = co->co_argcount;
    if (nargs >= argcount &&
        (nargs == argcount || (co->co_flags & CO_VARARGS))) {
      PyObject* stack[32];
      for (Py_ssize_t i = 0; i < argcount; i++) {
        stack[i] = args[i];
      }
      Py_ssize_t idx = argcount;
      PyObject* varargs = nullptr;
      PyObject* kwdict = nullptr;
      if (co->co_flags & CO_VARARGS) {
        varargs = Cix_PyTuple_FromArray(args + argcount, nargs - argcount);
        if (varargs == nullptr) {
          return nullptr;
        }
        stack[idx++] = varargs;
      }
      if (co->co_flags & CO_VARKEYWORDS) {
        kwdict = PyDict_New();
        if (kwdict == nullptr) {
          Py_XDECREF(varargs);
          return nullptr;
        }
        stack[idx++] = kwdict;
      }
      PyObject* result = jitrtReenterAfterBind(
          reentry,
          reinterpret_cast<PyObject*>(func),
          stack,
          static_cast<size_t>(idx),
          nullptr);
      Py_XDECREF(kwdict);
      Py_XDECREF(varargs);
      return result;
    }
  }
  // A positional call that fills every positional parameter of a function
  // whose only other parameters are keyword-only ones with defaults.  The
  // bound vector is the arguments followed by the memoised defaults, owned
  // for the duration of the call; nothing else of the generic bind applies.
  if (kwnames == nullptr && co->co_kwonlyargcount > 0 &&
      !(co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) && total_args <= 32 &&
      static_cast<Py_ssize_t>(PyVectorcall_NARGS(nargsf)) == co->co_argcount &&
      func->func_kwdefaults != nullptr) {
    bool error = false;
    PyObject* retired = nullptr;
    const PyObject* const* defs =
        kwOnlyDefaults311(co, func->func_kwdefaults, &error, &retired);
    if (retired != nullptr) {
      retired_codes.emplace_back(Ref<>::steal(retired));
    }
    if (error) {
      return nullptr;
    }
    if (defs != nullptr) {
      PyObject* stack[32];
      const Py_ssize_t argcount = co->co_argcount;
      const Py_ssize_t nkw = co->co_kwonlyargcount;
      bool complete = true;
      for (Py_ssize_t k = 0; k < nkw; k++) {
        if (defs[k] == nullptr) {
          // A required keyword-only parameter: the generic bind raises the
          // proper TypeError.
          complete = false;
          break;
        }
      }
      if (complete) {
        for (Py_ssize_t i = 0; i < argcount; i++) {
          stack[i] = args[i];
        }
        for (Py_ssize_t k = 0; k < nkw; k++) {
          PyObject* d = const_cast<PyObject*>(defs[k]);
          Py_INCREF(d);
          stack[argcount + k] = d;
        }
        PyObject* result = jitrtReenterAfterBind(
            reentry,
            reinterpret_cast<PyObject*>(func),
            stack,
            static_cast<size_t>(total_args),
            nullptr);
        for (Py_ssize_t k = 0; k < nkw; k++) {
          Py_DECREF(stack[argcount + k]);
        }
        return result;
      }
    }
  }
#endif
  BindArgs bind_args;
  PyObject** arg_space = bind_args.allocate(static_cast<size_t>(total_args));
  if (arg_space == nullptr) {
    return nullptr;
  }
  Ref<PyObject> kwdict, varargs;
  BoundDefaultRefs owned_defaults;

  switch (JITRT_BindKeywordArgs(
      func,
      args,
      nargsf,
      kwnames,
      arg_space,
      total_args,
      kwdict,
      varargs,
      owned_defaults,
      retired_codes)) {
    case BindKwStatus::Bound: {
      size_t new_nargsf = total_args;
#if PY_VERSION_HEX < 0x030C0000
      return jitrtReenterAfterBind(
          reentry,
          reinterpret_cast<PyObject*>(func),
          arg_space,
          new_nargsf,
          nullptr);
#else
      return reentry((PyObject*)func, arg_space, new_nargsf, nullptr);
#endif
    }
    case BindKwStatus::Error:
      return nullptr;
    case BindKwStatus::Fallback:
      break;
  }

  return Ci_PyFunction_Vectorcall((PyObject*)func, args, nargsf, kwnames);
}

using staticvectorcallfunc = JITRT_StaticCallReturn (*)(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

using staticvectorcallfuncfp = JITRT_StaticCallFPReturn (*)(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

JITRT_StaticCallFPReturn JITRT_CallWithIncorrectArgcountFPReturn(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount) {
  [[maybe_unused]] PrologueActivation311 activation;
  vectorcallfunc reentry = JITRT_CAPTURE_REENTRY(func);
  PyObject* defaults = func->func_defaults;
  if (defaults == nullptr) {
    // Function has no defaults; there's nothing we can do.
    // Fallback to the default _PyFunction_Vectorcall implementation
    // to produce an appropriate exception.
    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, nullptr);
    return {0.0, 0.0};
  }
  Py_ssize_t defcount = PyTuple_GET_SIZE(defaults);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  BindArgs bind_args;
  PyObject** arg_space = bind_args.allocate(static_cast<size_t>(argcount));
  if (arg_space == nullptr) {
    return {0.0, 0.0};
  }
  Py_ssize_t defaulted_args = argcount - nargs;

  if (nargs + defcount < argcount || nargs > argcount) {
    // Not enough args with defaults, or too many args without defaults.
    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, nullptr);
    return {0.0, 0.0};
  }

  Py_ssize_t i;
  for (i = 0; i < nargs; i++) {
    arg_space[i] = *args++;
  }

  Ref<> owned_defaults = Ref<>::create(defaults);
  PyObject** def_items = &((PyTupleObject*)owned_defaults.get())
                              ->ob_item[defcount - defaulted_args];
  for (; i < argcount; i++) {
    arg_space[i] = *def_items++;
  }

  size_t new_nargsf = argcount;

#if PY_VERSION_HEX < 0x030C0000 && !defined(CINDER_AARCH64)
  RecursiveCallAfterBind rec{func};
  if (!rec.entered()) {
    return {0.0, 0.0};
  }
#endif
  return reinterpret_cast<staticvectorcallfuncfp>(reentry)(
      (PyObject*)func,
      arg_space,
      new_nargsf,
      // We lie to C++ here, and smuggle in the number of defaulted args filled
      // in.
      (PyObject*)defaulted_args);
}

#ifdef _WIN32
PyObject*
#else
JITRT_StaticCallReturn
#endif
JITRT_CallWithIncorrectArgcount(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount) {
  [[maybe_unused]] PrologueActivation311 activation;
  vectorcallfunc reentry = JITRT_CAPTURE_REENTRY(func);
  PyObject* defaults = func->func_defaults;
  if (defaults == nullptr) {
    // Function has no defaults; there's nothing we can do.
    // Fallback to the default _PyFunction_Vectorcall implementation
    // to produce an appropriate exception.
    auto interpVectorcall = getInterpretedVectorcall(func);
#ifdef _WIN32
    return interpVectorcall((PyObject*)func, args, nargsf, nullptr);
#else
    return {interpVectorcall((PyObject*)func, args, nargsf, nullptr), nullptr};
#endif
  }
  Py_ssize_t defcount = PyTuple_GET_SIZE(defaults);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  BindArgs bind_args;
  PyObject** arg_space = bind_args.allocate(static_cast<size_t>(argcount));
  if (arg_space == nullptr) {
#ifdef _WIN32
    return nullptr;
#else
    return {nullptr, nullptr};
#endif
  }
  Py_ssize_t defaulted_args = argcount - nargs;

  if (nargs + defcount < argcount || nargs > argcount) {
    // Not enough args with defaults, or too many args without defaults.
    auto interpVectorcall = getInterpretedVectorcall(func);
#ifdef _WIN32
    return interpVectorcall((PyObject*)func, args, nargsf, nullptr);
#else
    return {interpVectorcall((PyObject*)func, args, nargsf, nullptr), nullptr};
#endif
  }

  Py_ssize_t i;
  for (i = 0; i < nargs; i++) {
    arg_space[i] = *args++;
  }

  Ref<> owned_defaults = Ref<>::create(defaults);
  PyObject** def_items = &((PyTupleObject*)owned_defaults.get())
                              ->ob_item[defcount - defaulted_args];
  for (; i < argcount; i++) {
    arg_space[i] = *def_items++;
  }

  size_t new_nargsf = argcount;

#if PY_VERSION_HEX < 0x030C0000 && !defined(CINDER_AARCH64)
  RecursiveCallAfterBind rec{func};
  if (!rec.entered()) {
#ifdef _WIN32
    return nullptr;
#else
    return {nullptr, nullptr};
#endif
  }
#endif
#ifdef _WIN32
  return reentry(
      (PyObject*)func, arg_space, new_nargsf, (PyObject*)defaulted_args);
#else
  return reinterpret_cast<staticvectorcallfunc>(reentry)(
      (PyObject*)func,
      arg_space,
      new_nargsf,
      // We lie to C++ here, and smuggle in the number of defaulted args filled
      // in.
      (PyObject*)defaulted_args);
#endif
}

bool JITRT_PackStaticArgs(
    PyObject** args,
    _PyTypedArgsInfo* arg_info,
    void** arg_space,
    Py_ssize_t nargs) {
  Py_ssize_t arg_index = 0;

  for (Py_ssize_t i = 0; i < nargs; i++) {
    if (arg_index < Py_SIZE(arg_info) &&
        arg_info->tai_args[arg_index].tai_argnum == i) {
      _PyTypedArgInfo* cur_arg = &arg_info->tai_args[arg_index];
      PyObject* arg = args[i];
      if (cur_arg->tai_primitive_type == -1) {
        if (!_PyObject_TypeCheckOptional(
                arg,
                cur_arg->tai_type,
                cur_arg->tai_optional,
                cur_arg->tai_exact)) {
          return true;
        }
        arg_space[i] = arg;
      } else if (cur_arg->tai_primitive_type == TYPED_BOOL) {
        if (Py_TYPE(arg) != &PyBool_Type) {
          return true;
        }
        arg_space[i] = (void*)(arg == Py_True);
      } else if (cur_arg->tai_primitive_type == TYPED_DOUBLE) {
        if (!PyFloat_Check(arg)) {
          return true;
        }
        arg_space[i] = bit_cast<void*>(PyFloat_AsDouble(arg));
      } else if (cur_arg->tai_primitive_type <= TYPED_INT64) {
        // Primitive arg check
        if (!PyLong_Check(arg) ||
            !_PyClassLoader_OverflowCheck(
                arg, cur_arg->tai_primitive_type, (size_t*)&arg_space[i])) {
          return true;
        }
      } else {
        JIT_ABORT("Unsupported primitive type {}", cur_arg->tai_primitive_type);
      }
      arg_index++;
      continue;
    }
    arg_space[i] = args[i];
  }
  return false;
}

template <typename TRetType, typename TVectorcall>
TRetType JITRT_CallStaticallyWithPrimitiveSignatureWorker(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    _PyTypedArgsInfo* arg_info) {
  vectorcallfunc reentry = JITRT_CAPTURE_REENTRY(func);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  auto arg_space = allocateBindArray<void*>(static_cast<size_t>(nargs));
  if (arg_space == nullptr) {
    return TRetType();
  }
  if (JITRT_PackStaticArgs(args, arg_info, arg_space.get(), nargs)) {
    goto fail;
  }

#if PY_VERSION_HEX < 0x030C0000 && !defined(CINDER_AARCH64)
  {
    RecursiveCallAfterBind rec{func};
    if (!rec.entered()) {
      return TRetType();
    }
    return reinterpret_cast<TVectorcall>(reentry)(
        (PyObject*)func, (PyObject**)arg_space.get(), nargsf, nullptr);
  }
#else
  return reinterpret_cast<TVectorcall>(reentry)(
      (PyObject*)func, (PyObject**)arg_space.get(), nargsf, nullptr);
#endif

fail:
  auto interpVectorcall = getInterpretedVectorcall(func);
  PyObject* res = interpVectorcall((PyObject*)func, args, nargsf, nullptr);
  JIT_DCHECK(res == nullptr, "should alway be reporting an error");
  return TRetType();
}

static inline Py_ssize_t vectorcall_flags(size_t n) {
  return n & PY_VECTORCALL_ARGUMENTS_OFFSET;
}

// This can either be a static method returning a primitive or a Python object,
// so we use JITRT_StaticCallReturn.  If it's returning a primitive we'll return
// rdx from the function, or return nullptr for rdx when we dispatch to
// _PyFunction_Vectorcall for error generation.  If it returns a Python object
// we'll return an additional garbage rdx from our caller, but our caller won't
// care about it either.
template <typename TRetType, typename TVectorcall>
TRetType JITRT_CallStaticallyWithPrimitiveSignatureTemplate(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  [[maybe_unused]] PrologueActivation311 activation;
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  PyCodeObject* co = (PyCodeObject*)func->func_code;

  if ((kwnames || nargs != co->co_argcount ||
       co->co_flags & (CO_VARARGS | CO_VARKEYWORDS))) {
    // we need to fixup kwnames, defaults, etc...
    const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount +
        ((co->co_flags & CO_VARKEYWORDS) ? 1 : 0) +
        ((co->co_flags & CO_VARARGS) ? 1 : 0);
    BindArgs bind_args;
    PyObject** arg_space = bind_args.allocate(static_cast<size_t>(total_args));
    if (arg_space == nullptr) {
      return TRetType();
    }
    Ref<PyObject> kwdict, varargs;
    BoundDefaultRefs owned_defaults;
    std::vector<Ref<>> retired_codes; // held through the call below

    switch (JITRT_BindKeywordArgs(
        func,
        args,
        nargsf,
        kwnames,
        arg_space,
        total_args,
        kwdict,
        varargs,
        owned_defaults,
        retired_codes)) {
      case BindKwStatus::Bound:
        return JITRT_CallStaticallyWithPrimitiveSignatureWorker<
            TRetType,
            TVectorcall>(
            func, arg_space, total_args | vectorcall_flags(nargsf), arg_info);
      case BindKwStatus::Error:
        return TRetType();
      case BindKwStatus::Fallback:
        break;
    }

    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, kwnames);
    return TRetType();
  }

  return JITRT_CallStaticallyWithPrimitiveSignatureWorker<
      TRetType,
      TVectorcall>(func, args, nargsf, arg_info);
}

JITRT_StaticCallReturn JITRT_CallStaticallyWithPrimitiveSignature(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  return JITRT_CallStaticallyWithPrimitiveSignatureTemplate<
      JITRT_StaticCallReturn,
      staticvectorcallfunc>(func, args, nargsf, kwnames, arg_info);
}

JITRT_StaticCallFPReturn JITRT_CallStaticallyWithPrimitiveSignatureFP(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  return JITRT_CallStaticallyWithPrimitiveSignatureTemplate<
      JITRT_StaticCallFPReturn,
      staticvectorcallfuncfp>(func, args, nargsf, kwnames, arg_info);
}

JITRT_StaticCallFPReturn JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  PyObject* res =
      JITRT_ReportStaticArgTypecheckErrors(func, args, nargsf, nullptr);
  JIT_CHECK(res == nullptr, "should always return an error");
  return {0, 0};
}

JITRT_StaticCallReturn JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  PyObject* res =
      JITRT_ReportStaticArgTypecheckErrors(func, args, nargsf, nullptr);
  JIT_CHECK(res == nullptr, "should always return an error");
  return {nullptr, nullptr};
}

PyObject* JITRT_ReportStaticArgTypecheckErrors(
    PyObject* func_obj,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};
  auto interpVectorcall = getInterpretedVectorcall(func);
  int nkwonly = code->co_kwonlyargcount;
  if (code == nullptr || nkwonly == 0) {
    // We explicitly pass in nullptr for kwnames as the default arg count can
    // be smuggled in to this function in place of kwnames.
    return interpVectorcall(func, args, nargsf, nullptr);
  }
  // This function is called after we've successfully bound all
  // arguments. However, we want to use the interpreter to construct the
  // typecheck error. If the function takes any keyword-only arguments we must
  // reconstruct kwnames so the the interpreted "prologue" in
  // _PyEval_EvalCodeWithName can validate that the keyword-only arguments were
  // passed as keywords.
  Ref<> new_kwnames = Ref<>::steal(PyTuple_New(nkwonly));
  if (new_kwnames == nullptr) {
    return nullptr;
  }
  for (Py_ssize_t i = code->co_argcount; i < code->co_argcount + nkwonly; i++) {
    auto name = Ref<>::create(jit::getVarname(code, i));
    PyTuple_SetItem(new_kwnames, i - code->co_argcount, std::move(name));
  }
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf) - nkwonly;
  if (code->co_flags & CO_VARKEYWORDS) {
    nargs -= 1;
  }
  Py_ssize_t flags = vectorcall_flags(nargsf);
  return interpVectorcall(func, args, nargs | flags, new_kwnames);
}

/*
 * The reference for these two functions is _PyEvalFramePushAndInit in ceval.c.
 */

static void init_and_link_interpreter_frame(
    PyFunctionObject* func,
    PyCodeObject* co,
    PyThreadState* tstate,
    _frameowner owner,
    _PyInterpreterFrame* frame,
    jit::CodeRuntime* code_rt = nullptr) {
  jit::jitFrameInit(
      tstate,
      frame,
      func,
      co,
      // Zero all of localsplus. This allows _PyFrame_ClearExceptCode to
      // safely clear the locals.
      0,
      owner,
      currentFrame(tstate),
      code_rt != nullptr ? code_rt->reifier() : nullptr);

  // Re-use the existing cframe to avoid having to manage a new one. There
  // should always be one due to the existence of a the per-thread root
  // cframe. The cframe idea seems to have only transiently been needed
  // in 3.11 and is now a loose end removed in 3.13.
  setCurrentFrame(tstate, frame);
}

static PyThreadState* allocate_and_link_interpreter_frame(
    PyFunctionObject* func,
    PyCodeObject* co) {
  PyThreadState* tstate = _PyThreadState_GET();
  JIT_DCHECK(tstate != nullptr, "thread state cannot be null");

#if PY_VERSION_HEX < 0x030C0000 && !defined(CINDER_AARCH64)
  if (t_prelinked_recursion_frame != nullptr) {
    _PyInterpreterFrame* frame = t_prelinked_recursion_frame;
    JIT_CHECK(
        currentFrame(tstate) == frame && frame->f_func == func &&
            frame->f_code == co,
        "prelinked recursion frame does not match compiled body");
    t_prelinked_recursion_frame = nullptr;
    return tstate;
  }
#endif

#if PY_VERSION_HEX >= 0x030C0000
  _PyInterpreterFrame* frame =
      Cix_PyThreadState_PushFrame(tstate, co->co_framesize);
#else
  // 3.11 has no co_framesize; the frame size is localsplus + stack depth plus
  // the frame-header specials.
  _PyInterpreterFrame* frame = Cix_PyThreadState_PushFrame(
      tstate, co->co_nlocalsplus + co->co_stacksize + FRAME_SPECIALS_SIZE);
  // This helper is the 3.11 entry glue: every machine-code invocation of a
  // compiled function links its frame here, so this is the counting point
  // the trigger-stats contract promises (trigger_stats.h).
  jit::triggerStatsOnMachineCodeEntry(co);
#endif
  JIT_CHECK(frame != nullptr, "Failed to allocate _PyInterpreterFrame");

  init_and_link_interpreter_frame(
      func, co, tstate, FRAME_OWNED_BY_THREAD, frame);

  return tstate;
}

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Debug(
    PyFunctionObject* func,
    PyCodeObject* jit_code_object) {
  PyCodeObject* co = reinterpret_cast<PyCodeObject*>(func->func_code);
  JIT_DCHECK(co == jit_code_object, "Code object mismatch");
  return allocate_and_link_interpreter_frame(func, co);
}

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Release(
    PyFunctionObject* func) {
  PyCodeObject* co = reinterpret_cast<PyCodeObject*>(func->func_code);
  return allocate_and_link_interpreter_frame(func, co);
}

#if PY_VERSION_HEX < 0x030C0000
// Datastack-overflow arm of the inline link.  The C path
// takes the stock pair of references; drop them again so the frame matches
// the inline contract (no owned f_func / f_code).  Both stay alive through
// the caller's reference and the compiled artifact.
PyThreadState* JITRT_AllocateAndLinkInterpreterFrameNoRef311(
    PyFunctionObject* func) {
  PyCodeObject* co = reinterpret_cast<PyCodeObject*>(func->func_code);
  PyThreadState* tstate = allocate_and_link_interpreter_frame(func, co);
  Py_DECREF(func);
  Py_DECREF(co);
  return tstate;
}

// Slow unlink for a frame without owned references:
// restore stock ownership first, the generic clear / take-over path then
// releases (or transfers) them as usual.
void JITRT_UnlinkFrameNoRef311(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  Py_INCREF(frame->f_func);
  Py_INCREF(frame->f_code);
  JITRT_UnlinkFrame(tstate);
}
#endif

void JITRT_InitFrameCellVars(
    PyFunctionObject* func,
    int nvars,
    PyThreadState* tstate) {
  PyObject* closure = func->func_closure;
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  int offset = co->co_nlocalsplus - nvars;
  _PyInterpreterFrame* frame = interpFrameFromThreadState(tstate);
  for (int i = 0; i < offset; i++) {
    frame->localsplus[i] = Ci_STACK_NULL;
  }
  for (int i = 0; i < nvars; i++) {
    frame->localsplus[offset + i] =
        Ci_STACK_NEWREF(PyTuple_GET_ITEM(closure, i));
  }
#if PY_VERSION_HEX < 0x030E0000
  frame->stacktop = nvars + offset;
#else
  frame->stackpointer = &frame->localsplus[nvars + offset];
#endif
}

std::pair<PyThreadState*, jit::GenDataFooter*>
JITRT_AllocateAndLinkGenAndInterpreterFrame(
    PyFunctionObject* func,
    jit::CodeRuntime* code_rt,
    GenResumeFunc resume_func,
    uint64_t original_frame_pointer) {
  JIT_DCHECK(
      PyCode_Check(func->func_code),
      "Non-code object for JIT function: {}",
      jit::repr(reinterpret_cast<PyObject*>(func)));
  BorrowedRef<PyCodeObject> co{func->func_code};
  JIT_DCHECK(co == code_rt->code(), "Code object mismatch");

  uint64_t spill_words = code_rt->spillWords();
  PyThreadState* tstate = _PyThreadState_GET();
  JIT_DCHECK(tstate != nullptr, "thread state cannot be null");
#if PY_VERSION_HEX < 0x030C0000
  // The runtime knows its owning artifact; the guarded shell's invocation
  // artifact and the installed-artifact lookup name the same object for the
  // code running here.  Direct entry bypasses the shell, so the invocation
  // artifact alone is not enough.  Resolved and pinned before the generator
  // is allocated: the allocation can run a collection whose callbacks retire
  // this function, and nothing counts this activation as a live frame yet.
  auto* compiled = code_rt->compiledFunction();
  if (compiled == nullptr) {
    compiled = invocationArtifactFor311(func);
  }
  if (compiled == nullptr) {
    compiled = reinterpret_cast<jit::CompiledFunction*>(
        Ci_JitShell311_InstalledArtifact(func));
  }
  JIT_CHECK(
      compiled != nullptr,
      "3.11 JIT generator created without an invocation artifact");
  Ref<jit::CompiledFunction> compiled_pin =
      Ref<jit::CompiledFunction>::create(compiled);
#endif
  auto [gen, gen_size] = cinderx::getModuleState()->jit_gen_free_list->allocate(
      co, spill_words * sizeof(uint64_t) + sizeof(jit::GenDataFooter));

  gen->gi_frame_state = FRAME_CREATED;
#if PY_VERSION_HEX < 0x030C0000
  // Stock 3.11 make_gen() holds gi_code independently of the frame's
  // f_code.  The two refs are balanced on clear/deopt/dealloc; omitting
  // this one leaves g.gi_code NULL and leaks the code object the frame
  // does not own.
  gen->gi_code = reinterpret_cast<PyCodeObject*>(Py_NewRef(func->func_code));
#endif
  gen->gi_weakreflist = nullptr;
  gen->gi_exc_state.exc_value = nullptr;
  gen->gi_exc_state.previous_item = nullptr;
  JIT_DCHECK(func->func_name != nullptr, "func_name is null");
  gen->gi_name = Py_NewRef(func->func_name);
  JIT_DCHECK(func->func_qualname != nullptr, "func_qualname is null");
  gen->gi_qualname = Py_NewRef(func->func_qualname);

#ifdef ENABLE_GENERATOR_AWAITER
  gen->gi_ci_awaiter = nullptr;
#endif

  gen->gi_hooks_inited = 0;
  gen->gi_closed = 0;
  gen->gi_running_async = 0;
  if (co->co_flags & CO_COROUTINE) {
    int origin_depth = tstate->coroutine_origin_tracking_depth;
    if (origin_depth == 0) {
      gen->gi_origin_or_finalizer = nullptr;
    } else {
      _PyInterpreterFrame* current_frame = interpFrameFromThreadState(tstate);
      PyObject* cr_origin = Cix_compute_cr_origin(origin_depth, current_frame);
      gen->gi_origin_or_finalizer = cr_origin;
      if (!cr_origin) {
        JIT_LOG(
            "Failed to compute cr_origin for {}",
            jit::repr(func->func_qualname));
        PyErr_Clear();
      }
    }
  } else {
    gen->gi_origin_or_finalizer = nullptr;
  }

  _PyInterpreterFrame* frame = generatorFrame(gen);
  auto footer =
      reinterpret_cast<jit::GenDataFooter*>( // NOLINT performance-no-int-to-ptr
          reinterpret_cast<uintptr_t>(gen) + gen_size -
          sizeof(jit::GenDataFooter));
  *gen->genDataFooterPtr(co) = footer;
#if PY_VERSION_HEX < 0x030C0000
  // the 3.11 generator frame initialised in place (what
  // _PyFrame_InitializeSpecials plus jitFrameInit do, without the calls).
  frame->f_func = reinterpret_cast<PyFunctionObject*>(Py_NewRef(func));
  frame->f_code = reinterpret_cast<PyCodeObject*>(Py_NewRef(co));
  frame->f_builtins = func->func_builtins;
  frame->f_globals = func->func_globals;
  frame->f_locals = nullptr;
  frame->stacktop = co->co_nlocalsplus;
  frame->frame_obj = nullptr;
  frame->prev_instr = _PyCode_CODE(co) - 1;
  frame->is_entry = false;
  frame->owner = FRAME_OWNED_BY_GENERATOR;
  for (int i = 0; i < co->co_nlocalsplus; i++) {
    frame->localsplus[i] = nullptr;
  }
  frame->previous = currentFrame(tstate);
  setCurrentFrame(tstate, frame);
#else
  init_and_link_interpreter_frame(
      func, co, tstate, FRAME_OWNED_BY_GENERATOR, frame, code_rt);
#endif

  footer->resumeEntry = resume_func;
  footer->yieldPoint = nullptr;
  footer->gen = static_cast<PyGenObject*>(gen);
  footer->code_rt = code_rt;
#if PY_VERSION_HEX < 0x030C0000
  footer->compiled = compiled_pin.release();
#endif
  footer->tree_iter_state = nullptr;
#if PY_VERSION_HEX < 0x030C0000
  footer->resume_finish_flag = 0;
  // a freshly created generator is directly resumable; the
  // footer is reachable from the (otherwise unused) gi_origin_or_finalizer
  // slot of a plain generator.
  footer->direct_ok = 1;
  footer->yf_sub = nullptr;
  footer->delegator = nullptr;
  if (!(co->co_flags & CO_COROUTINE)) {
    gen->gi_origin_or_finalizer = reinterpret_cast<PyObject*>(footer);
  }
#endif
  footer->originalFramePointer = original_frame_pointer;
  footer->linkAddress =
      *reinterpret_cast<uint64_t*>( // NOLINT performance-no-int-to-ptr
          original_frame_pointer);
  footer->returnAddress =
      *(reinterpret_cast<uint64_t*>( // NOLINT performance-no-int-to-ptr
            original_frame_pointer) +
        1);

  // Copy any data the register allocator may have spilled to the stack frame
  // into the generator's data area. After the caller swaps the frame pointer
  // to point to the footer, spill slot reloads will read from the generator's
  // memory instead of the stack. This copy ensures those reloads find the
  // correct values.
  if (spill_words > 0) {
    size_t spill_bytes = spill_words * sizeof(uint64_t);
    auto* src = reinterpret_cast<char*>( // NOLINT performance-no-int-to-ptr
                    original_frame_pointer) -
        spill_bytes;
    auto* dst = reinterpret_cast<char*>(footer) - spill_bytes;
    memcpy(dst, src, spill_bytes);
  }

#if PY_VERSION_HEX < 0x030C0000
  _PyObject_GC_TRACK(gen);
#else
  PyObject_GC_Track(gen);
#endif

  return {tstate, footer};
}

#if PY_VERSION_HEX < 0x030C0000
// the initial yield already holds the footer in the frame
// pointer; unlink the generator frame and hand back gen/footer without the
// frame-to-generator lookups.
std::pair<jit::JitGenObject*, jit::GenDataFooter*> JITRT_UnlinkGenFrame311(
    PyThreadState* tstate,
    jit::GenDataFooter* footer) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);
  frame->previous = nullptr;
  return {reinterpret_cast<jit::JitGenObject*>(footer->gen), footer};
}
#endif

std::pair<jit::JitGenObject*, jit::GenDataFooter*>
JITRT_UnlinkGenFrameAndReturnGenDataFooter(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  frame->previous = nullptr;

  BorrowedRef<PyGenObject> base_gen = _PyGen_GetGeneratorFromFrame(frame);
  jit::JitGenObject* gen = jit::JitGenObject::cast(base_gen.get());
  return {gen, gen->genDataFooter()};
}

void JITRT_MarkGeneratorCompleted() {
  PyThreadState* tstate = _PyThreadState_GET();
  _PyInterpreterFrame* frame = currentFrame(tstate);
  if (frame->owner != FRAME_OWNED_BY_GENERATOR) {
    return;
  }
  PyGenObject* gen = _PyGen_GetGeneratorFromFrame(frame);
  if (gen->gi_frame_state == FRAME_EXECUTING) {
#if PY_VERSION_HEX >= 0x030F0000
    gen->gi_frame_state = FRAME_CLEARED;
#else
    gen->gi_frame_state = FRAME_COMPLETED;
#endif
  }
}

void JITRT_DecrefFrame(PyFrameObject* frame) {
  if (Py_REFCNT(frame) > 1) {
    // If the frame escaped it needs to be tracked
    Py_DECREF(frame);
    if (!_PyObject_GC_IS_TRACKED(frame)) {
      PyObject_GC_Track(frame);
    }
  } else {
    Py_DECREF(frame);
  }
}

static void cleanupFrameExecutable(_PyInterpreterFrame* frame) {
#if PY_VERSION_HEX >= 0x030E0000
  // Can't use a plain decref as this needs to be symmetric with
  // _PyFrame_Initialize() which uses PyStackRef_FromPyObjectNew() for
  // f_executable.
  PyStackRef_CLOSE(frame->f_executable);
#else
  Py_DECREF(frameExecutable(frame));
#endif
}

// Non-generator frames don't incref f_funcobj during setup — the caller
// keeps the function alive. Incref now to balance the decref that
// jitFrameClearExceptCode will perform.
static void increfFuncObjForNonGenerator(_PyInterpreterFrame* frame) {
  if (jit::getConfig().frame_mode != jit::FrameMode::kLightweight) {
    return;
  }
#if PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000
  // The 3.14 port eagerly materializes CPython-visible frame fields and keeps
  // the setup-time function reference, so clear paths do not need a top-up ref.
  return;
#endif
  if (frameCode(frame)->co_flags & jit::kCoFlagsAnyGenerator) {
    return;
  }
#if PY_VERSION_HEX >= 0x030E0000
  Py_INCREF(PyStackRef_AsPyObjectBorrow(frame->f_funcobj));
#elif defined(ENABLE_LIGHTWEIGHT_FRAMES)
  // On 3.12+LW, f_funcobj holds the reifier, not the function. The
  // function lives in FrameHeader.func. For inlined frames,
  // jitFrameRemoveReifier already handles the incref via Py_NewRef,
  // so only incref for non-inlined (top-level) frames here.
  if (!jit::isInlinedFrame(frame)) {
    Py_XINCREF(jit::jitFrameGetFunction(frame));
  }
#else
  Py_INCREF(frame->f_funcobj);
#endif
}

void JITRT_UnlinkFrame(PyThreadState* tstate) {
  /*
   * The reference for this is _PyEvalFrameClearAndPop in ceval.c.
   */

  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  // This is needed particularly because it handles the work of copying
  // data to a PyFrameObject if one has escaped the function.
#if !(PY_VERSION_HEX >= 0x030E0000 && PY_VERSION_HEX < 0x030F0000)
  if (jit::getConfig().frame_mode == jit::FrameMode::kLightweight) {
    increfFuncObjForNonGenerator(frame);
  }
#endif
  jit::jitFrameClearExceptCode(frame);
  cleanupFrameExecutable(frame);

  if (jit::getConfig().frame_mode != jit::FrameMode::kLightweight) {
    _PyThreadState_PopFrame(tstate, frame);
  }
}

PyObject*
JITRT_LoadGlobal(PyObject* globals, PyObject* builtins, PyObject* name) {
  PyObject* result =
      _PyDict_LoadGlobal((PyDictObject*)globals, (PyDictObject*)builtins, name);
  if ((result == nullptr) && !PyErr_Occurred()) {
    // name is converted to a `char*` by format_exc_check_arg
    _PyEval_FormatExcCheckArg(
        _PyThreadState_GET(),
        PyExc_NameError,
        "name '%.200s' is not defined",
        name);
  }
  // PyDict_LoadGlobal returns a new reference on 3.14+
#if PY_VERSION_HEX < 0x030E0000
  Py_XINCREF(result);
#endif
  return result;
}

#if PY_VERSION_HEX < 0x030C0000
namespace {
void unpinGlobalLoadCell311(jit::GlobalLoadCell311* cell) {
  cell->globals_keys_version = jit::kGlobalLoadCellUnpinned311;
  cell->builtins_keys_version = jit::kGlobalLoadCellUnpinned311;
  cell->slot = nullptr;
}

// Pins the cell to the entry slot the lookup answered from, recording the
// keys versions the fast arm compares against.  Runs on the executing
// thread with the GIL held: the version allocator and the dict tables are
// not touched from compile threads.
bool pinGlobalLoadCell311(
    jit::GlobalLoadCell311* cell,
    PyObject* globals_obj,
    PyObject* builtins_obj,
    PyObject* name,
    PyObject* value) {
  auto* globals = reinterpret_cast<PyDictObject*>(globals_obj);
  auto* builtins = reinterpret_cast<PyDictObject*>(builtins_obj);
  // Only combined, unicode-keyed tables have a stable per-entry value slot
  // and exact-str keys whose comparison runs no Python code.
  if (!PyDict_CheckExact(globals_obj) || !PyDict_CheckExact(builtins_obj) ||
      globals->ma_values != nullptr || builtins->ma_values != nullptr ||
      !hasOnlyUnicodeKeys(globals_obj) || !hasOnlyUnicodeKeys(builtins_obj)) {
    unpinGlobalLoadCell311(cell);
    return false;
  }
  // The versions are read after the lookups below cannot run anymore.  A
  // zero version (the allocator is exhausted) cannot be pinned: a live zero
  // would compare equal to it; nor can the unpinned sentinel, which the
  // vendored allocator reserves.
  PyDictObject* owner = globals;
  Py_ssize_t index = getDictKeysIndex(globals->ma_keys, name);
  if (index < 0 ||
      DK_UNICODE_ENTRIES(globals->ma_keys)[index].me_value == nullptr) {
    if (cell->globals_only) {
      // The site checks the globals version only, which cannot witness a
      // builtins entry; it stays on the slow arm until the global returns.
      unpinGlobalLoadCell311(cell);
      return false;
    }
    owner = builtins;
    index = getDictKeysIndex(builtins->ma_keys, name);
    if (index < 0) {
      unpinGlobalLoadCell311(cell);
      return false;
    }
  }
  PyObject** slot = &DK_UNICODE_ENTRIES(owner->ma_keys)[index].me_value;
  if (*slot != value) {
    // The scan and the lookup disagree (a deleted entry below dk_nentries,
    // or a value swapped in between): do not pin a slot the lookup did not
    // answer from.
    unpinGlobalLoadCell311(cell);
    return false;
  }
  uint32_t globals_version = dictGetKeysVersion(nullptr, globals->ma_keys);
  uint32_t builtins_version = dictGetKeysVersion(nullptr, builtins->ma_keys);
  if (globals_version == 0 || builtins_version == 0 ||
      globals_version == jit::kGlobalLoadCellUnpinned311 ||
      builtins_version == jit::kGlobalLoadCellUnpinned311) {
    unpinGlobalLoadCell311(cell);
    return false;
  }
  cell->globals_keys_version = globals_version;
  cell->builtins_keys_version = builtins_version;
  cell->slot = slot;
  return true;
}
} // namespace

PyObject** JITRT_LoadGlobalRepin311(
    jit::GlobalLoadCell311* cell,
    PyObject* globals,
    PyObject* builtins,
    PyObject* name) {
  PyObject* value = JITRT_LoadGlobal(globals, builtins, name);
  if (value == nullptr) {
    unpinGlobalLoadCell311(cell);
    return &jit::kGlobalLoadNoValue311;
  }
  if (pinGlobalLoadCell311(cell, globals, builtins, name, value)) {
    // The dict entry keeps the value alive; the site reads it from there.
    Py_DECREF(value);
    return cell->slot;
  }
  // Unpinnable shape: the cell keeps this answer alive until the next one.
  Py_XSETREF(cell->held, value);
  return &cell->held;
}

uint64_t JITRT_StoreFrameLocal311(uint64_t idx, PyObject* value) {
  // MR-08 frame observability: keep the materialized frame's localsplus
  // current so sys._getframe()/f_locals readers see what stock 3.11
  // shows.  The frame owns its copy (the existing clear paths --
  // JITRT_UnlinkFrame, deopt reification, _PyFrame_Clear -- release it);
  // computation keeps using registers.  The old value's release can run
  // __del__, which observes the frame already holding the new value,
  // exactly like stock SETLOCAL.
  _PyInterpreterFrame* frame = interpFrameFromThreadState(_PyThreadState_GET());
  PyObject* old = frame->localsplus[idx];
  frame->localsplus[idx] = Py_XNewRef(value);
  Py_XDECREF(old);
  return 0;
}

int64_t JITRT_ConsumeForcedDeopt(jit::CodeRuntime* code_rt, uint64_t deopt_id) {
  if (code_rt == nullptr) {
    return 0;
  }
  return code_rt->consumeForcedDeopt(static_cast<std::size_t>(deopt_id));
}

#endif

PyObject* JITRT_LoadGlobalFromThreadState(
    PyThreadState* tstate,
    PyObject* name) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  return JITRT_LoadGlobal(frame->f_globals, frame->f_builtins, name);
}

PyObject* JITRT_LoadGlobalsDict(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  return frame->f_globals;
}

static int listPrefixReverseAssignFallback(PyObject* list, PyObject* index) {
  Ref<> minus_one = Ref<>::steal(PyLong_FromLong(-1));
  if (minus_one == nullptr) {
    return -1;
  }
  Ref<> rhs_slice = Ref<>::steal(PySlice_New(index, Py_None, minus_one.get()));
  if (rhs_slice == nullptr) {
    return -1;
  }
  Ref<> rhs_value = Ref<>::steal(PyObject_GetItem(list, rhs_slice.get()));
  if (rhs_value == nullptr) {
    return -1;
  }
  Ref<> one = Ref<>::steal(PyLong_FromLong(1));
  if (one == nullptr) {
    return -1;
  }
  Ref<> lhs_stop = Ref<>::steal(PyNumber_Add(index, one.get()));
  if (lhs_stop == nullptr) {
    return -1;
  }
  Ref<> lhs_slice = Ref<>::steal(PySlice_New(Py_None, lhs_stop.get(), Py_None));
  if (lhs_slice == nullptr) {
    return -1;
  }
  return PyObject_SetItem(list, lhs_slice.get(), rhs_value.get());
}

int JITRT_ListPrefixReverseAssign(PyObject* list, PyObject* index) {
  if (!PyList_CheckExact(list) || !PyLong_CheckExact(index)) {
    return listPrefixReverseAssignFallback(list, index);
  }

  Py_ssize_t idx = PyNumber_AsSsize_t(index, nullptr);
  if (idx == -1 && PyErr_Occurred()) {
    return -1;
  }

#ifdef Py_GIL_DISABLED
  return listPrefixReverseAssignFallback(list, index);
#else
  if (idx < 0) {
    return listPrefixReverseAssignFallback(list, index);
  }

  Py_ssize_t size = PyList_GET_SIZE(list);
  if (size <= 1) {
    return 0;
  }
  if (idx >= size) {
    idx = size - 1;
  }

  PyObject** items = reinterpret_cast<PyListObject*>(list)->ob_item;
  Py_ssize_t lo = 0;
  Py_ssize_t hi = idx;
  while (lo < hi) {
    PyObject* tmp = items[lo];
    items[lo] = items[hi];
    items[hi] = tmp;
    ++lo;
    --hi;
  }
  return 0;
#endif
}

PyObject* JITRT_LoadFunctionIndirect(PyObject** func, PyObject* descr) {
  PyObject* res = *func;
  if (!res) {
    res = _PyClassLoader_ResolveFunction(descr, nullptr);
    Py_XDECREF(res);
  }

  return res;
}

static bool is_eval_breaker_set(PyThreadState* tstate) {
  auto value =
#if PY_VERSION_HEX >= 0x030D0000
      reinterpret_cast<std::atomic_int64_t*>(&tstate->eval_breaker)
#else
      reinterpret_cast<std::atomic_int32_t*>(
          &tstate->interp->ceval.eval_breaker)
#endif
      ;
  return value->load(std::memory_order_relaxed);
}

// 3.12 CALL checks the eval breaker after a non-Python callable
// returns.  3.11 CALL does the same via CHECK_EVAL_BREAKER /
// eval_frame_handle_pending (signals, pending calls, GIL drop,
// async exceptions).  Py_MakePendingCalls covers only the first two.
static bool handle_periodic_activities_on_call(
    PyThreadState* tstate,
    PyObject* res,
    PyObject* callable) {
  JITRT_AtQuiescentState(tstate);
  return res != nullptr && !PyFunction_Check(callable) &&
      is_eval_breaker_set(tstate) &&
#if PY_VERSION_HEX < 0x030C0000
      Ci_EvalFrameHandlePending_311(tstate) != 0;
#else
      _Py_HandlePending(tstate) != 0;
#endif
}

// CALL_FUNCTION_EX always CHECK_EVAL_BREAKER after do_call_core,
// including when the callee is a Python function.
static bool handle_periodic_activities_on_call_ex(
    PyThreadState* tstate,
    PyObject* res) {
  JITRT_AtQuiescentState(tstate);
  return res != nullptr && is_eval_breaker_set(tstate) &&
#if PY_VERSION_HEX < 0x030C0000
      Ci_EvalFrameHandlePending_311(tstate) != 0;
#else
      _Py_HandlePending(tstate) != 0;
#endif
}

PyObject*
JITRT_CallFunctionEx(PyObject* func, PyObject* pargs, PyObject* kwargs) {
  // Normalize p + kw args to tuple and dict types exactly.
  Ref<> new_pargs;
  // Logically, I don't think this incref of kwargs is needed but not having it
  // breaks the C-version of functools.partial. The problem is a ref-count of 1
  // on "kw" going into partial_new() triggers an optimization where the kwargs
  // are not copied. This fails test_functoools.TestPartial*.test_kwargs_copy
  // which asserts it's not possible to alter the kwargs after the call. A
  // tempting alternative to this explicit ref management is to set-up
  // the memory effects of CallEx to steal the kwargs input. Unfortunately this
  // breaks test_contextlib.ContextManagerTestCase.test_nokeepref by keeping
  // kwargs and their contents alive for longer than expected.
  auto new_kwargs = Ref<>::create(kwargs);
  if (kwargs) {
    if (!PyDict_CheckExact(kwargs)) {
      PyObject* d = PyDict_New();
      if (d == nullptr) {
        return nullptr;
      }
      if (PyDict_Update(d, kwargs) != 0) {
        Py_DECREF(d);
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
#if PY_VERSION_HEX < 0x030C0000
          // ceval format_kwargs_error clears first: FunctionStr is not
          // safe with AttributeError still set.
          PyErr_Clear();
          PyObject* funcstr = _PyObject_FunctionStr(func);
          if (funcstr != nullptr) {
            PyErr_Format(
                PyExc_TypeError,
                "%U argument after ** must be a mapping, not %.200s",
                funcstr,
                kwargs->ob_type->tp_name);
            Py_DECREF(funcstr);
          }
#else
          PyErr_Format(
              PyExc_TypeError,
              "%.200s%.200s argument after ** "
              "must be a mapping, not %.200s",
              PyEval_GetFuncName(func),
              PyEval_GetFuncDesc(func),
              kwargs->ob_type->tp_name);
#endif
        }
        return nullptr;
      }
      kwargs = d;
      new_kwargs = Ref<>::steal(kwargs);
    }
    JIT_DCHECK(PyDict_CheckExact(kwargs), "Expect kwargs to be a dict");
  }
  if (!PyTuple_CheckExact(pargs)) {
    if (pargs->ob_type->tp_iter == nullptr && !PySequence_Check(pargs)) {
#if PY_VERSION_HEX < 0x030C0000
      // Stock 3.11 uses _PyObject_FunctionStr so the message carries the
      // qualified name (module.qualname) rather than co_name + "()".
      // Clear first: ceval check_args_iterable may run with a live
      // exception, and FunctionStr is not exception-safe.
      PyErr_Clear();
      PyObject* funcstr = _PyObject_FunctionStr(func);
      if (funcstr != nullptr) {
        PyErr_Format(
            PyExc_TypeError,
            "%U argument after * must be an iterable, not %.200s",
            funcstr,
            Py_TYPE(pargs)->tp_name);
        Py_DECREF(funcstr);
      }
#else
      PyErr_Format(
          PyExc_TypeError,
          "%.200s%.200s argument after * "
          "must be an iterable, not %.200s",
          PyEval_GetFuncName(func),
          PyEval_GetFuncDesc(func),
          pargs->ob_type->tp_name);
#endif
      return nullptr;
    }
    pargs = PySequence_Tuple(pargs);
    if (pargs == nullptr) {
      return nullptr;
    }
    new_pargs = Ref<>::steal(pargs);
  }
  JIT_DCHECK(PyTuple_CheckExact(pargs), "Expected pargs to be a tuple");

  PyThreadState* tstate = _PyThreadState_GET();
  PyObject* res = _PyObject_Call(tstate, func, pargs, kwargs);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call_ex(tstate, res)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

PyObject* JITRT_Call(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET),
      "JITRT_Call must always be called as a vectorcall");

#if PY_VERSION_HEX < 0x030C0000
  // 3.11 CALL has two dummy slots that HIR CallMethod encodes as operands:
  // PUSH_NULL / LOAD_GLOBAL|1 puts nullptr in the callable slot and the
  // callable in self(); a LOAD_METHOD miss puts Py_None in the callable
  // slot (HIR uses None, not nullptr, because nullptr means deopt) and
  // the attribute in self().  Calling a real None uses LOAD_FAST /
  // LOAD_CONST then PUSH_NULL, so callable is None and self is nullptr
  // -- that is not a miss, and shifting would vectorcall a null
  // pointer.  Only treat Py_None as a dummy when self is present.
  // The offset flag is dropped because the slot below the shifted args
  // is that dummy, not callee scratch.
  constexpr size_t kVectorcallOffset =
      static_cast<size_t>(PY_VECTORCALL_ARGUMENTS_OFFSET);
  if (callable == nullptr || (callable == Py_None && args[0] != nullptr)) {
    callable = args[0];
    args += 1;
    nargsf = (nargsf - 1) & ~kVectorcallOffset;
  } else if (args[0] == nullptr) {
    args += 1;
    nargsf = (nargsf - 1) & ~kVectorcallOffset;
  }
#else
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    // Calling a bound method leaves us with an unused first arg.
    if (args[0] == nullptr) {
      args += 1;
      nargsf -= 1;
    }
  } else {
    // Trying to call a function rather than a method on an object.  Shift the
    // arguments over by one.
    //
    // In theory this is supposed to expect nullptr on the stack, but our HIR
    // implementation of LOAD_ATTR/LOAD_METHOD uses Py_None.  Check for nullptr
    // just in case.
    if (callable == nullptr || Py_IsNone(callable)) {
      callable = args[0];
      args += 1;
      nargsf -= 1;
    }
  }
#endif

  PyObject* res =
      _PyObject_VectorcallTstate(tstate, callable, args, nargsf, kwnames);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call(tstate, res, callable)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

#if PY_VERSION_HEX < 0x030C0000
// STORE_ATTR_WITH_HINT as a lean helper mirroring the
// interpreter's handler (upstream/ceval.c) for a materialized, combined
// instance dict: hint in range, unicode keys, cached key identity, existing
// value -- overwrite in place.  Returns 1 when done, 0 to take the generic
// path.  The replacement goes through the same routine as the hint-indexed
// C++ overwrite in DictMutator::setAttr, so the dict's PEP 509 version
// advances exactly as stock's handler advances it.
int JITRT_StoreAttrWithHint311(
    PyObject* owner,
    PyObject* name,
    Py_ssize_t hint,
    PyObject* value) {
  auto* dict = *reinterpret_cast<PyDictObject**>(
      reinterpret_cast<PyObject**>(owner) - 3);
  if (dict == nullptr || !PyDict_CheckExact(dict) ||
      dict->ma_values != nullptr) {
    return 0;
  }
  PyDictKeysObject* keys = dict->ma_keys;
  if (!DK_IS_UNICODE(keys) ||
      static_cast<size_t>(hint) >= static_cast<size_t>(keys->dk_nentries)) {
    return 0;
  }
  PyDictUnicodeEntry* ep = DK_UNICODE_ENTRIES(keys) + hint;
  if (ep->me_key != name) {
    return 0;
  }
  if (ep->me_value == nullptr) {
    return 0;
  }
  jit::replaceMaterializedDictValue311(dict, &ep->me_value, value);
  return 1;
}

// Insertion into a fresh split-values slot, the
// interpreter's STORE_ATTR_INSTANCE_VALUE old_value == NULL arm:
// _PyDictValues_AddToInsertionOrder is not linkable, so its two-byte
// bookkeeping ([-1] prefix size, [-2] used size, order bytes below) is
// reproduced here.  `values` arrives as the raw values pointer.
void JITRT_StoreSplitValueInsert311(
    PyObject* values_raw,
    Py_ssize_t index,
    PyObject* value) {
  auto* values = reinterpret_cast<PyDictValues*>(values_raw);
  Py_INCREF(value);
  values->values[index] = value;
  uint8_t* size_ptr = reinterpret_cast<uint8_t*>(values) - 2;
  int size = *size_ptr;
  size++;
  size_ptr[-size] = static_cast<uint8_t>(index);
  *size_ptr = static_cast<uint8_t>(size);
}
#endif

#if PY_VERSION_HEX < 0x030C0000
// Construction fast path for plain Python classes, the 3.11 counterpart of
// 3.12's CALL_ALLOC_AND_ENTER_INIT.  Stock routes `Cls(args)` through
// _PyObject_MakeTpCall (argument tuple), type_call, object_new and
// slot_tp_init, the last two each doing a _PyType_Lookup.  For a heap type
// with the default tp_new / tp_alloc whose __init__ resolves to a Python
// function, the same effect is: allocate, call the function with self
// prepended, insist on a None result.  The __init__ resolution is cached per
// type and validated against tp_version_tag.
namespace {
// Stock initialisation of an instance object_new already produced, for a
// constructor fast path whose cached resolution no longer holds after the
// allocation: type_call reads tp_init only after tp_new returns, and the
// allocation can run Python (a collection reached from the GC link runs
// callbacks and finalizers).  Returns -1 with an exception set.
int initAfterNew311(
    PyObject* self,
    PyObject* const* args,
    Py_ssize_t nargs,
    PyObject* kwnames) {
  auto tuple = Ref<>::steal(Cix_PyTuple_FromArray(args, nargs));
  if (tuple == nullptr) {
    return -1;
  }
  Ref<> kwargs;
  if (kwnames != nullptr && PyTuple_GET_SIZE(kwnames) > 0) {
    kwargs = Ref<>::steal(PyDict_New());
    if (kwargs == nullptr) {
      return -1;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
      if (PyDict_SetItem(
              kwargs, PyTuple_GET_ITEM(kwnames, i), args[nargs + i]) < 0) {
        return -1;
      }
    }
  }
  // The containers above are allocations too: the slot is read once they
  // exist, immediately before the call.
  initproc init = Py_TYPE(self)->tp_init;
  if (init == nullptr) {
    return 0;
  }
  return init(self, tuple, kwargs);
}

struct CtorCacheEntry311 {
  PyTypeObject* type{nullptr};
  uint32_t version{0};
  PyObject* init{nullptr}; // borrowed; pinned by the class dict at `version`
};
constexpr size_t kCtorCacheSize = 128;
CtorCacheEntry311 s_ctor_cache_311[kCtorCacheSize];

// Returns true when the call was handled (result in *out, possibly NULL
// with an exception set).  False means "not this fast path".
bool constructFast311(
    PyTypeObject* type,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames,
    PyObject** out) {
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  Py_ssize_t nkw = kwnames == nullptr ? 0 : PyTuple_GET_SIZE(kwnames);
  if (nargs + nkw > 7) {
    return false;
  }
  uint32_t version = type->tp_version_tag;
  if (version == 0) {
    return false;
  }
  CtorCacheEntry311& e = s_ctor_cache_311
      [(reinterpret_cast<uintptr_t>(type) >> 4) % kCtorCacheSize];
  if (e.type != type || e.version != version) {
    if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE) ||
        (type->tp_flags & Py_TPFLAGS_IS_ABSTRACT) ||
        type->tp_new != PyBaseObject_Type.tp_new ||
        type->tp_alloc != PyType_GenericAlloc ||
        type->tp_init == PyBaseObject_Type.tp_init ||
        !Ci_Type_HasValidVersionTag(type)) {
      return false;
    }
    PyObject* init = _PyType_Lookup(type, &_Py_ID(__init__));
    if (init == nullptr || !PyFunction_Check(init)) {
      return false;
    }
    e.type = type;
    e.version = type->tp_version_tag;
    e.init = init;
  }
  // object_new itself, not just tp_alloc: on 3.11 it is object_new that
  // attaches the split values array to a managed-dict instance
  // (_PyObject_InitializeDict, not linkable), and __init__ stores need it.
  // The allocation can run Python: the initialiser is held across it, and
  // the resolution is proved again afterwards -- a moved one (the class
  // re-versioned, or this shared entry refilled by a nested construction)
  // initialises the instance the stock way instead.
  PyObject* init = e.init;
  Py_INCREF(init);
  static PyObject* empty_args = PyTuple_New(0);
  PyObject* self = PyBaseObject_Type.tp_new(type, empty_args, nullptr);
  if (self == nullptr) {
    Py_DECREF(init);
    *out = nullptr;
    return true;
  }
  if (e.type != type || e.version != type->tp_version_tag ||
      !Ci_Type_HasValidVersionTag(type) || e.init != init) {
    Py_DECREF(init);
    if (initAfterNew311(self, args, nargs, kwnames) < 0) {
      Py_DECREF(self);
      *out = nullptr;
      return true;
    }
    *out = self;
    return true;
  }
  PyObject* stack[8];
  stack[0] = self;
  for (Py_ssize_t i = 0; i < nargs + nkw; i++) {
    stack[i + 1] = args[i];
  }
  // init is a Python function (checked at fill).
  PyObject* r = reinterpret_cast<PyFunctionObject*>(init)->vectorcall(
      init, stack, static_cast<size_t>(nargs + 1), kwnames);
  Py_DECREF(init);
  if (r == nullptr) {
    Py_DECREF(self);
    *out = nullptr;
    return true;
  }
  if (r != Py_None) {
    PyErr_Format(
        PyExc_TypeError,
        "__init__() should return None, not '%.200s'",
        Py_TYPE(r)->tp_name);
    Py_DECREF(r);
    Py_DECREF(self);
    *out = nullptr;
    return true;
  }
  Py_DECREF(r);
  *out = self;
  return true;
}

// Generic-call shapes that 3.11 routes through _PyObject_MakeTpCall
// (argument tuple, tp_call) although the effect has a direct C entry:
//  * str(x) / int(x) / float(x) / bool(x) / tuple(x) with one positional
//    argument: the type objects carry no vectorcall on 3.11, so each call
//    built a tuple and went through type_call -> tp_new; the *_new
//    implementations reduce to PyObject_Str / PyNumber_Long / PyNumber_Float
//    / PyObject_IsTrue / PySequence_Tuple for that shape.
//  * object.__init__(self): the base-class initialiser reached through
//    super().__init__() chains; with no extra argument object_init has
//    nothing to check and returns None.
//  * min(a, b) / max(a, b): the builtins are METH_VARARGS | METH_KEYWORDS,
//    so stock packs an argument tuple and min_max iterates it with a generic
//    compare per item; for two items the answer is one rich comparison, the
//    first argument kept on a false or NaN result exactly as min_max does.
//  * an instance of a plain Python class whose __call__ is a Python
//    function: stock builds the tuple, slot_tp_call looks __call__ up and
//    _PyObject_Call_Prepend copies the arguments once more with self in
//    front.  The __call__ resolution is cached per type and validated
//    against tp_version_tag like the constructor cache above.
struct CallCacheEntry311 {
  PyTypeObject* type{nullptr};
  uint32_t version{0};
  PyObject* call{nullptr}; // borrowed; pinned by the class dict at `version`
};
constexpr size_t kCallCacheSize = 128;
CallCacheEntry311 s_call_cache_311[kCallCacheSize];

// Returns true when the call was handled (result in *out, possibly NULL
// with an exception set).  False means "not this fast path".
bool callFast311(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames,
    PyObject** out) {
  PyTypeObject* ct = Py_TYPE(callable);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  if (ct == &PyType_Type) {
    if (kwnames != nullptr || nargs != 1) {
      return false;
    }
    PyObject* x = args[0];
    if (callable == reinterpret_cast<PyObject*>(&PyUnicode_Type)) {
      *out = PyObject_Str(x);
      return true;
    }
    if (callable == reinterpret_cast<PyObject*>(&PyLong_Type)) {
      *out = PyNumber_Long(x);
      return true;
    }
    if (callable == reinterpret_cast<PyObject*>(&PyFloat_Type)) {
      *out = PyNumber_Float(x);
      return true;
    }
    if (callable == reinterpret_cast<PyObject*>(&PyBool_Type)) {
      int truth = PyObject_IsTrue(x);
      *out = truth < 0 ? nullptr : PyBool_FromLong(truth);
      return true;
    }
    if (callable == reinterpret_cast<PyObject*>(&PyTuple_Type)) {
      *out = PySequence_Tuple(x);
      return true;
    }
    return false;
  }
  if (ct == &PyWrapperDescr_Type) {
    // Identity against object's own slot wrapper; a subclass wrapper takes
    // the stock path.
    if (kwnames != nullptr || nargs != 1) {
      return false;
    }
    static PyObject* s_object_init =
        PyDict_GetItemString(PyBaseObject_Type.tp_dict, "__init__");
    if (callable != s_object_init) {
      return false;
    }
    Py_INCREF(Py_None);
    *out = Py_None;
    return true;
  }
  if (ct == &PyCFunction_Type) {
    // The builtins module's own min and max, identified by implementation:
    // the object's method definition must be the entry of the builtins
    // module's own method table (the Builtins table of the context, which
    // records those entries by address).  A C function created by an
    // extension with its own definition -- whatever its name, module or
    // calling convention -- never matches.  The identity is proved once per
    // object and the object then held, so a later object at a reused
    // address cannot alias it.
    if (kwnames != nullptr || nargs != 2) {
      return false;
    }
    static PyObject* s_min = nullptr; // strong
    static PyObject* s_max = nullptr; // strong
    int op;
    if (callable == s_min) {
      op = Py_LT;
    } else if (callable == s_max) {
      op = Py_GT;
    } else {
      auto* cf = reinterpret_cast<PyCFunctionObject*>(callable);
      const jit::Builtins& builtins = jit::getContext()->builtins();
      std::optional<std::string> name = builtins.find(cf->m_ml);
      if (!name.has_value()) {
        return false;
      }
      if (*name == "min") {
        Py_XSETREF(s_min, Py_NewRef(callable));
        op = Py_LT;
      } else if (*name == "max") {
        Py_XSETREF(s_max, Py_NewRef(callable));
        op = Py_GT;
      } else {
        return false;
      }
    }
    PyObject* first = args[0];
    PyObject* second = args[1];
    int cmp = JITRT_RichCompareBool(second, first, op);
    if (cmp < 0) {
      *out = nullptr;
      return true;
    }
    PyObject* res = cmp > 0 ? second : first;
    Py_INCREF(res);
    *out = res;
    return true;
  }
  if (!PyType_HasFeature(ct, Py_TPFLAGS_HEAPTYPE) ||
      PyType_HasFeature(ct, Py_TPFLAGS_HAVE_VECTORCALL)) {
    return false;
  }
  Py_ssize_t nkw = kwnames == nullptr ? 0 : PyTuple_GET_SIZE(kwnames);
  if (nargs + nkw > 7) {
    return false;
  }
  uint32_t version = ct->tp_version_tag;
  if (version == 0 || !Ci_Type_HasValidVersionTag(ct)) {
    return false;
  }
  CallCacheEntry311& e =
      s_call_cache_311[(reinterpret_cast<uintptr_t>(ct) >> 4) % kCallCacheSize];
  if (e.type != ct || e.version != version) {
    PyObject* call = _PyType_Lookup(ct, &_Py_ID(__call__));
    if (call == nullptr || !PyFunction_Check(call)) {
      return false;
    }
    e.type = ct;
    e.version = ct->tp_version_tag;
    e.call = call;
  }
  PyObject* stack[8];
  stack[0] = callable;
  for (Py_ssize_t i = 0; i < nargs + nkw; i++) {
    stack[i + 1] = args[i];
  }
  // A compiled frame does not own its function: the caller keeps it alive
  // for the whole call.  The class dict may be the function's only owner
  // and the body may delete it, so the call holds the function captured
  // here (not the shared slot, which a nested call can refill).
  PyObject* call = e.call;
  Py_INCREF(call);
  // call is a Python function (checked at fill).
  *out = reinterpret_cast<PyFunctionObject*>(call)->vectorcall(
      call, stack, static_cast<size_t>(nargs + 1), kwnames);
  Py_DECREF(call);
  return true;
}
} // namespace
#endif

PyObject* JITRT_VectorcallTstate(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  PyObject* res;
#if PY_VERSION_HEX < 0x030C0000
  if (Py_IS_TYPE(callable, &PyType_Type) &&
      constructFast311(
          reinterpret_cast<PyTypeObject*>(callable),
          args,
          nargsf,
          kwnames,
          &res)) {
    // fall through to the periodic check below
  } else if (callFast311(callable, args, nargsf, kwnames, &res)) {
    // fall through to the periodic check below
  } else if (PyFunction_Check(callable)) {
    // A Python function's vectorcall is either the generated entry or the
    // interpreter's; neither needs _PyObject_VectorcallTstate's result
    // audit, which the compiled call sites already skip.
    res = reinterpret_cast<PyFunctionObject*>(callable)->vectorcall(
        callable, args, nargsf, kwnames);
  } else
#endif
    res = _PyObject_VectorcallTstate(tstate, callable, args, nargsf, kwnames);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call(tstate, res, callable)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

#if PY_VERSION_HEX < 0x030C0000
namespace {
// _PyThreadState_GET() is the core inline load; the public PyThreadState_GET
// macro is a PLT call to PyThreadState_Get() on 3.11.
PyObject* JITRT_Vectorcall311(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  return JITRT_VectorcallTstate(
      _PyThreadState_GET(), callable, args, nargsf, kwnames);
}

PyObject* JITRT_Call311(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  return JITRT_Call(_PyThreadState_GET(), callable, args, nargsf, kwnames);
}
} // namespace

extern "C" {
void* g_JITRT_Vectorcall311_slot = reinterpret_cast<void*>(JITRT_Vectorcall311);
void* g_JITRT_Call311_slot = reinterpret_cast<void*>(JITRT_Call311);
}

PyObject* JITRT_HandlePendingAfterCall311(PyObject* res) {
  PyThreadState* tstate = _PyThreadState_GET();
  if (Ci_EvalFrameHandlePending_311(tstate) != 0) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

// Call-site entry-cache miss helper.  kwnames is NULL by construction at
// probed sites (KwArgs sites do not emit the probe), and the probe reloads
// the x3 argument slot with the cache pointer.  Dispatch semantics match
// the plain arm: an exact Python function calls its own vectorcall slot
// (or the code artifact entry, see below), everything else goes through
// the JITRT_Vectorcall slot.  Filling happens before dispatch, within
// budget:
//  - a direct fill requires a bare compiled entry installed (vectorcall ==
//    compiled entry), a direct entry present, no varargs / keyword-only
//    parameters, site argc == co_argcount (the 32-bit mirror of the
//    prologue's comparison) and a non-static prototype (static prototypes
//    carry the typecheck chain and cannot be bypassed);
//  - otherwise a neutral fill records the current vectorcall as the fast
//    target, semantically the plain arm's per-call slot load.
// Promotion, reattachment and recompilation change vectorcall, the identity
// guard fails back here, and refills within budget track the lifecycle.
extern "C" PyObject* JITRT_CallSiteEntryMiss311(
    PyObject* callable,
    PyObject** args,
    size_t nargsf,
    void* cache_raw) {
  auto* cache = reinterpret_cast<jit::CallSiteEntryCache*>(cache_raw);
  bool is_function = Py_IS_TYPE(callable, &PyFunction_Type);
  if (cache->helper_budget > 0) {
    cache->helper_budget--;
    if (is_function) {
      auto* func = reinterpret_cast<PyFunctionObject*>(callable);
      auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
      void* fast = reinterpret_cast<void*>(func->vectorcall);
      auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
          Ci_JitShell311_InstalledArtifact(func));
      // A function object without an installed artifact takes the by-code
      // entry: it serves an artifact of the function's code when one exists
      // (fresh comprehension / closure objects over compiled code) and the
      // function's own entry otherwise.  The cache keys on (code,
      // vectorcall), which neither a later compile of the code nor an
      // attachment of another object changes, so a neutral fill recording
      // the interpreter entry would never see the artifact published after
      // it; the by-code entry resolves that per call.
      if (compiled == nullptr) {
        fast = reinterpret_cast<void*>(Ci_JitShell311_CodeArtifactVectorcall);
      }
      if (compiled != nullptr) {
        void* direct = compiled->directCallEntry311();
        if (direct != nullptr &&
            func->vectorcall == compiled->vectorcallEntry() &&
            !(code->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) &&
            code->co_kwonlyargcount == 0 &&
            !(code->co_flags & CI_CO_STATICALLY_COMPILED) &&
            static_cast<uint32_t>(nargsf) ==
                static_cast<uint32_t>(code->co_argcount)) {
          fast = direct;
        }
      }
      // The caller's artifact owns the code object the entry names for as
      // long as it lives: the hit compares addresses only, and a code
      // object that died could be succeeded at its address by one of
      // another signature while the artifact entry at the cached address
      // is replaced by one of that signature's artifact.
      if (cache->runtime != nullptr) {
        cache->runtime->addReference(BorrowedRef<>{func->func_code});
      }
      cache->code = func->func_code;
      cache->vectorcall = reinterpret_cast<void*>(func->vectorcall);
      cache->fast_target = fast;
    }
  }
  if (is_function) {
    return Ci_JitShell311_CodeArtifactVectorcall(
        callable, args, nargsf, nullptr);
  }
  return reinterpret_cast<vectorcallfunc>(g_JITRT_Vectorcall311_slot)(
      callable, args, nargsf, nullptr);
}

// Constructor arm of the call-site entry cache.  A class callable reached
// the site probe's non-function arm; the site keeps the class resolution
// itself: the class version tag pins the __init__ lookup, the function's
// vectorcall identity pins its entry, and a hit is object_new plus a direct
// entry into the compiled __init__ with self prepended -- the effect of
// type_call -> object_new -> slot_tp_init for a heap class with the default
// tp_new / tp_alloc, exactly the set constructFast311 admits.  Anything
// else (a non-heap class, an overridden __new__, an abstract class, an
// __init__ that is not a Python function, more than seven arguments, an
// exhausted budget) takes the slow slot unchanged.
namespace {
bool ctorSiteFill311(
    jit::CallSiteEntryCache* cache,
    PyTypeObject* type,
    Py_ssize_t nargs) {
  cache->ctor_type = nullptr;
  if (cache->ctor_budget == 0) {
    return false;
  }
  cache->ctor_budget--;
  if (nargs > 7 || !PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE) ||
      (type->tp_flags & Py_TPFLAGS_IS_ABSTRACT) ||
      type->tp_new != PyBaseObject_Type.tp_new ||
      type->tp_alloc != PyType_GenericAlloc ||
      type->tp_init == PyBaseObject_Type.tp_init ||
      !Ci_Type_HasValidVersionTag(type)) {
    return false;
  }
  PyObject* init = _PyType_Lookup(type, &_Py_ID(__init__));
  if (init == nullptr || !PyFunction_Check(init)) {
    return false;
  }
  auto* func = reinterpret_cast<PyFunctionObject*>(init);
  auto* code = reinterpret_cast<PyCodeObject*>(func->func_code);
  void* target = reinterpret_cast<void*>(func->vectorcall);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  if (compiled != nullptr) {
    // The same admission as the function arm's direct fill: a bare compiled
    // entry installed, a direct entry present, positional-only shape, and
    // self plus the site's arguments filling co_argcount exactly.
    void* direct = compiled->directCallEntry311();
    if (direct != nullptr && func->vectorcall == compiled->vectorcallEntry() &&
        !(code->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) &&
        code->co_kwonlyargcount == 0 &&
        !(code->co_flags & CI_CO_STATICALLY_COMPILED) &&
        code->co_argcount == nargs + 1) {
      target = direct;
    }
  }
  // The identity of the initialiser's code is compared by address: the
  // caller's artifact owns it (see the function arm's fill).
  if (cache->runtime != nullptr) {
    cache->runtime->addReference(BorrowedRef<>{func->func_code});
  }
  cache->ctor_type = type;
  cache->ctor_version = type->tp_version_tag;
  cache->ctor_init = init;
  cache->ctor_init_vectorcall = reinterpret_cast<void*>(func->vectorcall);
  cache->ctor_init_code = func->func_code;
  cache->ctor_target = target;
  return true;
}

// The constructor arm's resolution still holds for `type`: the class and
// its version, and the initialiser's entry and code (the direct entry
// carries no __code__ check of its own, and a __code__ assignment changes
// neither the class version nor the function's vectorcall).
bool ctorSiteHolds311(
    const jit::CallSiteEntryCache* cache,
    PyTypeObject* type) {
  if (cache->ctor_type != type || cache->ctor_version != type->tp_version_tag ||
      !(type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG)) {
    return false;
  }
  auto* init = reinterpret_cast<PyFunctionObject*>(cache->ctor_init);
  return reinterpret_cast<void*>(init->vectorcall) ==
      cache->ctor_init_vectorcall &&
      init->func_code == cache->ctor_init_code;
}
} // namespace

namespace {
// The construction helper is tail-called by the dispatch stub, so its
// result does not pass the stub's post-call eval-breaker check: a signal,
// pending call or asynchronous exception that became due during __init__
// is handled here, as the generic vectorcall path handles it after a
// non-Python callable returns.
PyObject* ctorSiteReturn311(PyObject* callable, PyObject* self) {
  if (handle_periodic_activities_on_call(
          _PyThreadState_GET(), self, callable)) {
    Py_DECREF(self);
    return nullptr;
  }
  return self;
}
} // namespace

extern "C" PyObject* JITRT_ConstructSite311(
    PyObject* callable,
    PyObject** args,
    size_t nargsf,
    void* cache_raw) {
  auto* cache = reinterpret_cast<jit::CallSiteEntryCache*>(cache_raw);
  auto* type = reinterpret_cast<PyTypeObject*>(callable);
  const Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  if (!ctorSiteHolds311(cache, type) && !ctorSiteFill311(cache, type, nargs)) {
    return reinterpret_cast<vectorcallfunc>(g_JITRT_Vectorcall311_slot)(
        callable, args, nargsf, nullptr);
  }
  // object_new itself, not just tp_alloc: on 3.11 it is object_new that
  // attaches the split values array to a managed-dict instance.  The
  // allocation can run Python: the initialiser is held across it, and the
  // resolution is proved again afterwards -- a moved one (the class
  // re-versioned, its __init__ re-coded, or this site refilled by a nested
  // construction of another class) initialises the instance the stock way
  // instead of constructing a second one.
  PyObject* init = cache->ctor_init;
  Py_INCREF(init);
  void* target = cache->ctor_target;
  static PyObject* empty_args = PyTuple_New(0);
  PyObject* self = PyBaseObject_Type.tp_new(type, empty_args, nullptr);
  if (self == nullptr) {
    Py_DECREF(init);
    return nullptr;
  }
  if (!ctorSiteHolds311(cache, type) || cache->ctor_init != init ||
      cache->ctor_target != target) {
    Py_DECREF(init);
    if (initAfterNew311(self, args, nargs, nullptr) < 0) {
      Py_DECREF(self);
      return nullptr;
    }
    return ctorSiteReturn311(callable, self);
  }
  PyObject* stack[8];
  stack[0] = self;
  for (Py_ssize_t i = 0; i < nargs; i++) {
    stack[i + 1] = args[i];
  }
  PyObject* r = reinterpret_cast<vectorcallfunc>(target)(
      init, stack, static_cast<size_t>(nargs + 1), nullptr);
  Py_DECREF(init);
  if (r == nullptr) {
    Py_DECREF(self);
    return nullptr;
  }
  if (r != Py_None) {
    PyErr_Format(
        PyExc_TypeError,
        "__init__() should return None, not '%.200s'",
        Py_TYPE(r)->tp_name);
    Py_DECREF(r);
    Py_DECREF(self);
    return nullptr;
  }
  Py_DECREF(r);
  return ctorSiteReturn311(callable, self);
}
#endif

PyObject* JITRT_VectorcallPythonFunction(
    PyThreadState*,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(PyFunction_Check(callable), "expected exact Python function");
#if PY_VERSION_HEX < 0x030C0000
  // An unattached function object over compiled code takes the artifact
  // entry; anything else its own vectorcall.
  return Ci_JitShell311_CodeArtifactVectorcall(callable, args, nargsf, kwnames);
#else
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(callable);
  return func->vectorcall(callable, args, nargsf, kwnames);
#endif
}

PyObject* JITRT_UnaryNot(PyObject* value) {
  int res = PyObject_IsTrue(value);
  if (res == 0) {
    Py_INCREF(Py_True);
    return Py_True;
  } else if (res > 0) {
    Py_INCREF(Py_False);
    return Py_False;
  }
  return nullptr;
}

LoadMethodResult JITRT_GetMethod(PyObject* obj, PyObject* name) {
  PyObject* method = nullptr;
  int found = _PyObject_GetMethod(obj, name, &method);
  if (method == nullptr) {
    return {nullptr, nullptr};
  }
  if (!found) {
    Py_INCREF(Py_None);
    return {Py_None, method};
  }
  Py_INCREF(obj);
  return {method, obj};
}

#if PY_VERSION_HEX >= 0x030E0000
LoadMethodResult JITRT_LoadAttrMethodWithValues(
    PyObject* obj,
    int64_t type_version,
    int64_t keys_version,
    PyObject* descr,
    PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
  if (tp->tp_version_tag != static_cast<uint32_t>(type_version)) {
    return JITRT_GetMethod(obj, name);
  }

  if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
      !PyType_HasFeature(tp, Py_TPFLAGS_HEAPTYPE)) {
    return JITRT_GetMethod(obj, name);
  }

  PyDictValues* values = _PyObject_InlineValues(obj);
  if (!values->valid) {
    return JITRT_GetMethod(obj, name);
  }

  auto* heap_type = reinterpret_cast<PyHeapTypeObject*>(tp);
  PyDictKeysObject* keys = heap_type->ht_cached_keys;
  if (keys == nullptr ||
      keys->dk_version != static_cast<uint32_t>(keys_version)) {
    return JITRT_GetMethod(obj, name);
  }

  if (descr == nullptr ||
      !PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
    return JITRT_GetMethod(obj, name);
  }

  Py_INCREF(descr);
  Py_INCREF(obj);
  return {descr, obj};
}
#endif

#if PY_VERSION_HEX < 0x030C0000
// The attribute load stub's getter arm returns the descriptor's own result
// to the generated code; an AttributeError it raised is completed with the
// name and the receiver here, as PyObject_GetAttr would complete it.
PyObject* JITRT_LoadAttrError311(PyObject* obj, PyObject* name) {
  if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
    Cix_set_attribute_error_context(obj, name);
  }
  return nullptr;
}
#endif

PyObject* JITRT_LoadAttrInstanceValueOrGeneric(
    PyObject* obj,
    PyObject* name,
    uint32_t type_version,
    Py_ssize_t index) {
#if PY_VERSION_HEX < 0x030C0000
  PyTypeObject* type = Py_TYPE(obj);
  if (type->tp_version_tag == type_version &&
      PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    PyDictValues* values = *_PyObject_ValuesPointer(obj);
    if (values != nullptr) {
      PyObject* value = values->values[index];
      if (value != nullptr) {
        Py_INCREF(value);
        return value;
      }
    }
  }
#else
  (void)type_version;
  (void)index;
#endif
  return PyObject_GetAttr(obj, name);
}

#if PY_VERSION_HEX < 0x030C0000
static PyTypeObject* supercheck_builtin(PyTypeObject* type, PyObject* obj) {
  if (PyType_Check(obj) &&
      PyType_IsSubtype(reinterpret_cast<PyTypeObject*>(obj), type)) {
    Py_INCREF(obj);
    return reinterpret_cast<PyTypeObject*>(obj);
  }

  if (PyType_IsSubtype(Py_TYPE(obj), type)) {
    Py_INCREF(Py_TYPE(obj));
    return Py_TYPE(obj);
  }

  PyObject* class_attr = nullptr;
  if (_PyObject_LookupAttr(obj, &_Py_ID(__class__), &class_attr) < 0) {
    return nullptr;
  }
  if (class_attr != nullptr && PyType_Check(class_attr) &&
      reinterpret_cast<PyTypeObject*>(class_attr) != Py_TYPE(obj)) {
    int ok =
        PyType_IsSubtype(reinterpret_cast<PyTypeObject*>(class_attr), type);
    if (ok) {
      return reinterpret_cast<PyTypeObject*>(class_attr);
    }
  }
  Py_XDECREF(class_attr);

  PyErr_SetString(
      PyExc_TypeError,
      "super(type, obj): obj must be an instance or subtype of type");
  return nullptr;
}

static PyObject* super_lookup_descr_builtin(
    PyTypeObject* type,
    PyTypeObject* obj_type,
    PyObject* name) {
  PyObject* mro = obj_type->tp_mro;
  if (mro == nullptr) {
    return nullptr;
  }

  JIT_DCHECK(PyTuple_Check(mro), "expected type MRO to be a tuple");
  Py_ssize_t n = PyTuple_GET_SIZE(mro);
  Py_ssize_t i = 0;
  for (; i + 1 < n; i++) {
    if (reinterpret_cast<PyObject*>(type) == PyTuple_GET_ITEM(mro, i)) {
      break;
    }
  }
  i++;
  if (i >= n) {
    return nullptr;
  }

  Py_INCREF(mro);
  for (; i < n; i++) {
    PyObject* obj = PyTuple_GET_ITEM(mro, i);
    PyObject* dict = reinterpret_cast<PyTypeObject*>(obj)->tp_dict;
    JIT_DCHECK(dict != nullptr && PyDict_Check(dict), "expected type dict");

    PyObject* res = PyDict_GetItemWithError(dict, name);
    if (res != nullptr) {
      Py_INCREF(res);
      Py_DECREF(mro);
      return res;
    }
    if (PyErr_Occurred()) {
      Py_DECREF(mro);
      return nullptr;
    }
  }
  Py_DECREF(mro);
  return nullptr;
}

static PyObject* super_lookup_builtin(
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    int* meth_found) {
  if (meth_found) {
    *meth_found = 0;
  }

  Ref<PyTypeObject> obj_type =
      Ref<PyTypeObject>::steal(supercheck_builtin(type, self));
  if (obj_type == nullptr) {
    return nullptr;
  }

  PyObject* res = super_lookup_descr_builtin(type, obj_type, name);
  if (res != nullptr) {
    if (meth_found &&
        (PyFunction_Check(res) ||
         PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR))) {
      *meth_found = 1;
      return res;
    }

    descrgetfunc f = Py_TYPE(res)->tp_descr_get;
    if (f != nullptr) {
      PyObject* descr_res = f(
          res,
          self == reinterpret_cast<PyObject*>(obj_type.get()) ? nullptr : self,
          reinterpret_cast<PyObject*>(obj_type.get()));
      Py_DECREF(res);
      res = descr_res;
    }
    return res;
  }
  if (PyErr_Occurred()) {
    return nullptr;
  }

  Ref<> super_instance = Ref<>::steal(PyObject_CallFunctionObjArgs(
      reinterpret_cast<PyObject*>(&PySuper_Type), type, self, NULL));
  if (super_instance == nullptr) {
    return nullptr;
  }
  return PyObject_GetAttr(super_instance, name);
}
#endif

static inline PyObject* super_lookup_method_or_attr(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    int call_no_args,
    int* meth_found) {
  if (global_super != (PyObject*)&PySuper_Type) {
    Ref<> super_instance;
    if (call_no_args) {
      super_instance = Ref<>::steal(PyObject_CallNoArgs(global_super));
    } else {
      super_instance = Ref<>::steal(
          PyObject_CallFunctionObjArgs(global_super, type, self, NULL));
    }
    if (super_instance == nullptr) {
      return nullptr;
    }
    if (meth_found) {
      *meth_found = 0;
    }
    return PyObject_GetAttr(super_instance, name);
  }
#if PY_VERSION_HEX < 0x030C0000
  return super_lookup_builtin(type, self, name, meth_found);
#else
  // Check Py_TYPE(self) because in a class method super call
  // self can be a type. https://github.com/python/cpython/pull/106977
  if (Py_TYPE(self)->tp_getattro != PyObject_GenericGetAttr) {
    meth_found = nullptr;
  }
  return _PySuper_Lookup(type, self, name, meth_found);
#endif
}

LoadMethodResult JITRT_GetMethodFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  int meth_found = 0;
  PyObject* result = super_lookup_method_or_attr(
      global_super, type, self, name, no_args_in_super_call, &meth_found);
  if (result == nullptr) {
    return {nullptr, nullptr};
  }
  if (meth_found) {
    if (!(PyFunction_Check(result) || Py_TYPE(result) == &PyMethodDescr_Type ||
          Py_TYPE(result) == &PyWrapperDescr_Type ||
          PyType_HasFeature(Py_TYPE(result), Py_TPFLAGS_METHOD_DESCRIPTOR))) {
      meth_found = 0;
    }
  } else {
    meth_found = 0;
  }
  if (meth_found) {
    Py_INCREF(self);
    return {result, self};
  }
  Py_INCREF(Py_None);
  return {Py_None, result};
}

PyObject* JITRT_GetAttrFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  return super_lookup_method_or_attr(
      global_super, type, self, name, no_args_in_super_call, nullptr);
}

#if PY_VERSION_HEX < 0x030C0000
namespace {
// The instance type super() resolves against on the builtin fast path: the
// receiver itself when it is a class (classmethod form), else its type.
inline PyTypeObject* superObjType311(PyObject* self) {
  return PyType_Check(self) ? reinterpret_cast<PyTypeObject*>(self)
                            : Py_TYPE(self);
}

inline bool superSiteHit311(
    const jit::SuperSiteCache311* cache,
    PyTypeObject* obj_type,
    PyTypeObject* type) {
  return obj_type == cache->obj_type && type == cache->start_type &&
      obj_type->tp_version_tag == cache->version &&
      (obj_type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG) != 0;
}

// Resolve the descriptor through the MRO on a miss and fill the entry when
// the answer is replayable (builtin super, subtype relation holds, lookup
// found the name, instance type carries a valid version tag).  Returns a new
// reference, or nullptr with no error set when the generic path must run.
PyObject* superSiteLookupFill311(
    jit::SuperSiteCache311* cache,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    PyTypeObject** obj_type_out) {
  PyTypeObject* obj_type = superObjType311(self);
  if (!PyType_IsSubtype(obj_type, type)) {
    return nullptr;
  }
  PyObject* res = super_lookup_descr_builtin(type, obj_type, name);
  if (res == nullptr) {
    return nullptr;
  }
  if ((obj_type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG) == 0 ||
      obj_type->tp_version_tag == 0) {
    // The MRO walk above used raw dict lookups; the type lookup assigns a
    // version tag when it can.
    _PyType_Lookup(obj_type, name);
  }
  if ((obj_type->tp_flags & Py_TPFLAGS_VALID_VERSION_TAG) != 0 &&
      obj_type->tp_version_tag != 0) {
    cache->obj_type = obj_type;
    cache->version = obj_type->tp_version_tag;
    cache->start_type = type;
    cache->descr = res;
  }
  *obj_type_out = obj_type;
  return res;
}

inline bool superIsMethodDescr311(PyObject* res) {
  return PyFunction_Check(res) ||
      PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR);
}

// Apply the descriptor protocol the way super_lookup_builtin() does for a
// non-method result.  Steals `res`.
PyObject* superBind311(PyObject* res, PyObject* self, PyTypeObject* obj_type) {
  descrgetfunc f = Py_TYPE(res)->tp_descr_get;
  if (f == nullptr) {
    return res;
  }
  PyObject* value =
      f(res,
        self == reinterpret_cast<PyObject*>(obj_type) ? nullptr : self,
        reinterpret_cast<PyObject*>(obj_type));
  Py_DECREF(res);
  return value;
}
} // namespace

LoadMethodResult JITRT_GetMethodFromSuperCached311(
    jit::SuperSiteCache311* cache,
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  if (global_super == reinterpret_cast<PyObject*>(&PySuper_Type)) {
    PyTypeObject* obj_type = superObjType311(self);
    PyObject* res;
    if (superSiteHit311(cache, obj_type, type)) {
      res = cache->descr;
      Py_INCREF(res);
    } else {
      res = superSiteLookupFill311(cache, type, self, name, &obj_type);
      if (res == nullptr && PyErr_Occurred()) {
        return {nullptr, nullptr};
      }
    }
    if (res != nullptr) {
      if (superIsMethodDescr311(res)) {
        Py_INCREF(self);
        return {res, self};
      }
      PyObject* value = superBind311(res, self, obj_type);
      if (value == nullptr) {
        return {nullptr, nullptr};
      }
      Py_INCREF(Py_None);
      return {Py_None, value};
    }
  }
  return JITRT_GetMethodFromSuper(
      global_super, type, self, name, no_args_in_super_call);
}

PyObject* JITRT_GetAttrFromSuperCached311(
    jit::SuperSiteCache311* cache,
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  if (global_super == reinterpret_cast<PyObject*>(&PySuper_Type)) {
    PyTypeObject* obj_type = superObjType311(self);
    PyObject* res;
    if (superSiteHit311(cache, obj_type, type)) {
      res = cache->descr;
      Py_INCREF(res);
    } else {
      res = superSiteLookupFill311(cache, type, self, name, &obj_type);
      if (res == nullptr && PyErr_Occurred()) {
        return nullptr;
      }
    }
    if (res != nullptr) {
      return superBind311(res, self, obj_type);
    }
  }
  return JITRT_GetAttrFromSuper(
      global_super, type, self, name, no_args_in_super_call);
}
#endif

PyObject* JITRT_InvokeMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject*) {
  PyTypeObject* self_type = Py_TYPE(args[0]);
  _PyType_VTable* vtable = (_PyType_VTable*)self_type->tp_cache;

  return _PyClassLoader_InvokeMethod(vtable, slot, args, nargs);
}

PyObject* JITRT_InvokeClassMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject*) {
  PyTypeObject* self_type = (PyTypeObject*)args[0];
  _PyType_VTable* vtable = (_PyType_VTable*)self_type->tp_cache;

  return _PyClassLoader_InvokeMethod(vtable, slot, args, nargs);
}

/* This function is inlined to LIR via kCHelpersManual, so changes here will
 * have no effect. */
PyObject* JITRT_Cast(PyObject* obj, PyTypeObject* type) {
  if (PyObject_TypeCheck(obj, type)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastOptional(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 1, /* exact */ 0)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastExact(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 0, /* exact */ 1)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected exactly '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastOptionalExact(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 1, /* exact */ 1)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected exactly '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

/* Needed because cast to float does extra work that would be a pain to add to
 * the manual inlined LIR for JITRT_Cast. */
PyObject* JITRT_CastToFloat(PyObject* obj) {
  if (PyObject_TypeCheck(obj, &PyFloat_Type)) {
    // cast to float is not considered pass-through by refcount insertion (since
    // it may produce a new reference), so even if in fact it is pass-through
    // (because we got a float), we need to return a new reference.
    Py_INCREF(obj);
    return obj;
  } else if (PyObject_TypeCheck(obj, &PyLong_Type)) {
    // special case because Python typing pretends int subtypes float
    return PyFloat_FromDouble(PyLong_AsDouble(obj));
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected 'float', got '%s'",
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastToFloatOptional(PyObject* obj) {
  if (_PyObject_TypeCheckOptional(
          obj, &PyFloat_Type, /* opt */ 1, /* exact */ 0)) {
    // cast to float is not considered pass-through by refcount insertion (since
    // it may produce a new reference), so even if in fact it is pass-through
    // (because we got a float), we need to return a new reference.
    Py_INCREF(obj);
    return obj;
  } else if (PyObject_TypeCheck(obj, &PyLong_Type)) {
    // special case because Python typing pretends int subtypes float
    return PyFloat_FromDouble(PyLong_AsDouble(obj));
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected 'float', got '%s'",
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

int64_t JITRT_ShiftLeft64(int64_t x, int64_t y) {
  return x << y;
}
int32_t JITRT_ShiftLeft32(int32_t x, int32_t y) {
  return x << y;
}

int64_t JITRT_ShiftRight64(int64_t x, int64_t y) {
  return x >> y;
}
int32_t JITRT_ShiftRight32(int32_t x, int32_t y) {
  return x >> y;
}

uint64_t JITRT_ShiftRightUnsigned64(uint64_t x, uint64_t y) {
  return x >> y;
}
uint32_t JITRT_ShiftRightUnsigned32(uint32_t x, uint32_t y) {
  return x >> y;
}

int64_t JITRT_Mod64(int64_t x, int64_t y) {
  return x % y;
}
int32_t JITRT_Mod32(int32_t x, int32_t y) {
  return x % y;
}

uint64_t JITRT_ModUnsigned64(uint64_t x, uint64_t y) {
  return x % y;
}
uint32_t JITRT_ModUnsigned32(uint32_t x, uint32_t y) {
  return x % y;
}

PyObject* JITRT_BoxI32(int32_t i) {
  return PyLong_FromLong(i);
}

PyObject* JITRT_BoxU32(uint32_t i) {
  return PyLong_FromUnsignedLong(i);
}

PyObject* JITRT_BoxBool(uint32_t i) {
  if (i) {
    return Py_True;
  }
  return Py_False;
}

PyObject* JITRT_BoxI64(int64_t i) {
  return PyLong_FromSsize_t(i);
}

PyObject* JITRT_BoxU64(uint64_t i) {
  return PyLong_FromSize_t(i);
}

PyObject* JITRT_BoxDouble(double_t d) {
  return PyFloat_FromDouble(d);
}

PyObject* JITRT_LongFloatTrueDivide(PyObject* left, PyObject* right) {
  JIT_DCHECK(PyLong_CheckExact(left), "left operand must be exact int");
  JIT_DCHECK(PyFloat_CheckExact(right), "right operand must be exact float");

  double lhs = PyLong_AsDouble(left);
  if (lhs == -1.0 && PyErr_Occurred()) {
    return nullptr;
  }

  double rhs = PyFloat_AS_DOUBLE(right);
  if (rhs == 0.0) {
    PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
    return nullptr;
  }

  return PyFloat_FromDouble(lhs / rhs);
}

double JITRT_PowerDouble(double x, double y) {
  return pow(x, y);
}

double JITRT_SqrtDouble(double x) {
  return sqrt(x);
}

double JITRT_Power32(int32_t x, int32_t y) {
  return pow(x, y);
}

double JITRT_PowerUnsigned32(uint32_t x, uint32_t y) {
  return pow(x, y);
}

double JITRT_Power64(int64_t x, int64_t y) {
  return pow(x, y);
}

double JITRT_PowerUnsigned64(uint64_t x, uint64_t y) {
  return pow(x, y);
}

template <typename T>
static T checkedUnboxImpl(PyObject* obj) {
  constexpr bool is_signed = std::is_signed_v<T>;
  std::conditional_t<is_signed, int64_t, uint64_t> res;
  if constexpr (is_signed) {
    res = PyLong_AsSsize_t(obj);
  } else {
    res = PyLong_AsSize_t(obj);
  }
  if (T(res) == res || (!is_signed && res == T(-1) && PyErr_Occurred())) {
    return res;
  }
  PyErr_SetString(PyExc_OverflowError, "int overflow");
  return -1;
}

uint64_t JITRT_UnboxU64(PyObject* obj) {
  return PyLong_AsSize_t(obj);
}

uint32_t JITRT_UnboxU32(PyObject* obj) {
  return checkedUnboxImpl<uint32_t>(obj);
}

uint16_t JITRT_UnboxU16(PyObject* obj) {
  return checkedUnboxImpl<uint16_t>(obj);
}

uint8_t JITRT_UnboxU8(PyObject* obj) {
  return checkedUnboxImpl<uint8_t>(obj);
}

int64_t JITRT_UnboxI64(PyObject* obj) {
  return PyLong_AsSsize_t(obj);
}

int64_t JITRT_UnboxExactIndexI64(PyObject* obj, PyObject* exc) {
  JIT_DCHECK(PyLong_CheckExact(obj), "expected exact int index");
  JIT_DCHECK(exc != nullptr, "exact index conversion requires an exception");
  Py_ssize_t index = PyLong_AsSsize_t(obj);
  if (index == -1 && PyErr_ExceptionMatches(PyExc_OverflowError)) {
    PyErr_Clear();
    PyErr_Format(
        exc,
        "cannot fit '%.200s' into an index-sized integer",
        Py_TYPE(obj)->tp_name);
  }
  return index;
}

int32_t JITRT_UnboxI32(PyObject* obj) {
  return checkedUnboxImpl<int32_t>(obj);
}

int16_t JITRT_UnboxI16(PyObject* obj) {
  return checkedUnboxImpl<int16_t>(obj);
}

int8_t JITRT_UnboxI8(PyObject* obj) {
  return checkedUnboxImpl<int8_t>(obj);
}

PyObject* JITRT_ImportName(
    PyThreadState* tstate,
    PyObject* name,
    PyObject* fromlist,
    PyObject* level) {
  DEFINE_STATIC_STRING(__import__);
  PyObject* globals = PyEval_GetGlobals();
  PyObject* builtins = tstate->interp->builtins;

  auto import_func =
      Ref<>::create(PyDict_GetItemWithError(builtins, s___import__));

  JIT_DCHECK(
      import_func || !PyErr_Occurred(),
      "_PyDict_GetItemIdWithError should only fail with invalid identifiers");
  if (import_func == nullptr) {
    PyErr_SetString(PyExc_ImportError, "__import__ not found");
    return nullptr;
  }

  /* Fast path for not overloaded __import__. */
  if (import_func == CI_INTERP_IMPORT_FIELD(tstate->interp, import_func)) {
    int ilevel = PyLong_AsInt(level);
    if (ilevel == -1 && _PyErr_Occurred(tstate)) {
      return nullptr;
    }
    return PyImport_ImportModuleLevelObject(
        name,
        globals,
        // Locals are not actually used by the builtin import.
        // This is documented behavior as of Python 3.7.
        Py_None,
        fromlist,
        ilevel);
  }

  // In this implementation we always pass None for locals as it's easier than
  // fully materializing them now. The CPython interpreter has strange (probably
  // broken) behavior - it will only pass a dictionary of locals to
  // __builtins__.__import__() if the locals have been materialized already, for
  // example by a call to locals(). Reliance on this behavior is unlikely.
  PyObject* locals = Py_None;

  return PyObject_CallFunctionObjArgs(
      import_func, name, globals, locals, fromlist, level, nullptr);
}

void JITRT_SetCurrentAwaiter(PyObject* awaitable, PyThreadState* ts) {
#ifdef ENABLE_GENERATOR_AWAITER

  _PyInterpreterFrame* frame = interpFrameFromThreadState(ts);
  // Matches SEND/SEND_GEN's check in bytecodes.c
  if (frame->owner != FRAME_OWNED_BY_GENERATOR ||
      (!(frame->f_code->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)))) {
    return;
  }
  auto awaiter =
      reinterpret_cast<PyObject*>(_PyGen_GetGeneratorFromFrame(frame));

  Ci_PyAwaitable_SetAwaiter(awaitable, awaiter);
#endif // ENABLE_GENERATOR_AWAITER
}

JITRT_GenSendRes JITRT_GenSend(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame) {
#if PY_VERSION_HEX < 0x030C0000
  if (frame != nullptr) {
    // a delegation finished by the C++ shortcut resumed this
    // generator with the outcome in the footer flag: 1 = v is the
    // sub-iterator's return value, 2 = it raised (exception already set).
    jit::GenDataFooter* footer = jit::jitGenDataFooter(frame);
    if (footer != nullptr && footer->resume_finish_flag != 0) {
      uint64_t flag = footer->resume_finish_flag;
      footer->resume_finish_flag = 0;
      if (flag == 2) {
        return {nullptr, 1};
      }
      finish_yield_from = 1;
    }
  }
#endif
  if (v == nullptr) {
    return {nullptr, 1};
  }
  if (finish_yield_from) {
    Py_INCREF(v);
    return {v, 1};
  }
  PyObject* retval;

#ifdef ENABLE_GENERATOR_AWAITER
  if (_PyFrame_GetCode(frame)->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)) {
    BorrowedRef<PyGenObject> base_gen = _PyGen_GetGeneratorFromFrame(frame);
    Ci_PyAwaitable_SetAwaiter(gen, base_gen);
  }
#endif

  PySendResult gen_status;
  PyAsyncMethods* async_methods = Py_TYPE(gen)->tp_as_async;
  if (!jit::g_jitgen_am_send_patched && jit::JitGen_CheckAny(gen)) {
    // JitGen-to-JitGen send bypasses the am_send slot.
    gen_status = jit::jitgenSendDirect(gen, v, &retval);
  } else if (async_methods != nullptr && async_methods->am_send != nullptr) {
    // Inline PyIter_Send's first branch while honoring the current slot, which
    // may be replaced by the instrumentation deopt wrapper.
    gen_status = async_methods->am_send(gen, v, &retval);
    assert(_Py_CheckSlotResult(gen, "am_send", gen_status != PYGEN_ERROR));
  } else {
    gen_status = PyIter_Send(gen, v, &retval);
  }

  if (gen_status == PYGEN_RETURN) {
    return {retval, 1};
  }
  if (gen_status == PYGEN_ERROR) {
    return {nullptr, 1};
  }
  JIT_DCHECK(
      gen_status == PYGEN_NEXT,
      "Unexpected gen_status: {}",
      fmt::underlying(gen_status));
  return {retval, 0};
}

JITRT_GenSendRes JITRT_GenSendHandleStopAsyncIteration(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame) {
  JITRT_GenSendRes res = JITRT_GenSend(gen, v, finish_yield_from, frame);
  if ((res.retval == nullptr) && (res.done == 1) &&
      PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
    PyErr_Clear();
    res.retval = &JITRT_IterDoneSentinel;
  }
  return res;
}

PyObject* JITRT_FormatValue(
    PyThreadState* tstate,
    PyObject* fmt_spec,
    PyObject* value,
    int conversion) {
  PyObject* (*conv_fn)(PyObject*);

  /* See if any conversion is specified. */
  switch (conversion) {
    case FVC_NONE:
      conv_fn = nullptr;
      break;
    case FVC_STR:
      conv_fn = PyObject_Str;
      break;
    case FVC_REPR:
      conv_fn = PyObject_Repr;
      break;
    case FVC_ASCII:
      conv_fn = PyObject_ASCII;
      break;
    default:
      _PyErr_Format(
          tstate,
          PyExc_SystemError,
          "unexpected conversion flag %d",
          conversion);
      return nullptr;
  }

  /* If there's a conversion function, call it and replace
     value with that result. Otherwise, just use value,
     without conversion. */
  Ref<> converted;
  if (conv_fn != nullptr) {
    converted = Ref<>::steal(conv_fn(value));
    if (converted == nullptr) {
      return nullptr;
    }
    value = converted.get();
  }

  /* If value is a unicode object, and there's no fmt_spec,
     then we know the result of format(value) is value
     itself. In that case, skip calling format(). I plan to
     move this optimization in to PyObject_Format()
     itself. */
  if (PyUnicode_CheckExact(value) && fmt_spec == nullptr) {
    /* Do nothing, just return. */
    Py_INCREF(value);
    return value;
  }

  /* Actually call format(). */
  return PyObject_Format(value, fmt_spec);
}

PyObject* JITRT_BuildString(
    PyThreadState* /*tstate*/,
    void* /*unused*/,
    PyObject** args,
    size_t nargsf,
    void* /*unused*/) {
  size_t nargs = PyVectorcall_NARGS(nargsf);

  Ref<> empty = Ref<>::steal(PyUnicode_New(0, 0));
  if (empty == nullptr) {
    return nullptr;
  }

  return _PyUnicode_JoinArray(empty, args, nargs);
}

JITRT_StaticCallReturn JITRT_FailedDeferredCompileShim(PyObject** args) {
  void* no_error = reinterpret_cast<void*>(1);

  // The function object is always the first argument in the static calling
  // convention.
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(args[0]);
  PyCodeObject* code = (PyCodeObject*)func->func_code;
  int total_args = code->co_argcount;
  if (code->co_flags & CO_VARARGS) {
    total_args++;
  }
  if (code->co_flags & CO_VARKEYWORDS) {
    total_args++;
  }

  // PyObject** args is:
  // arg0 - function object
  // arg1 - first real argument
  // argx - remaining register arguments
  // ...
  // previous frame pointer
  // return address to JITed code
  // memory argument 0 - first stack argument
  // memory argument 1
  // ...

  PyObject** dest_args;
  auto final_args = std::make_unique<PyObject*[]>(total_args);
  int cc_reg_args = jit::codegen::ARGUMENT_REGS.size();

  if (total_args < cc_reg_args) {
    // no gap in args to worry about
    dest_args = args + 1;
  } else {
    for (int i = 0; i < cc_reg_args - 1; i++) {
      final_args[i] = args[i + 1];
    }
    for (int i = cc_reg_args - 1; i < total_args; i++) {
      final_args[i] = args[i + 3];
    }
    dest_args = final_args.get();
  }

  _PyTypedArgsInfo* arg_info =
      jit::getContext()->findFunctionPrimitiveArgInfo(func);
  auto allocated_args = std::make_unique<PyObject*[]>(
      arg_info == nullptr ? 0 : Py_SIZE(arg_info));
  int allocated_count = 0;

  if (arg_info != nullptr) {
    // We have primitive values that need to be converted into boxed values
    // to run the interpreter loop.
    for (Py_ssize_t i = 0; i < Py_SIZE(arg_info); i++) {
      if (arg_info->tai_args[i].tai_primitive_type != -1) {
        // primitive type, box...
        int arg = arg_info->tai_args[i].tai_argnum + 1;
        uint64_t arg_val;
        if (arg >= cc_reg_args) {
          arg += 4;
        }
        arg_val = (uint64_t)args[arg];

        PyObject* new_val = _PyClassLoader_Box(
            arg_val, arg_info->tai_args[i].tai_primitive_type);

        if (new_val == nullptr) {
          for (int j = 0; j < allocated_count; j++) {
            Py_DECREF(allocated_args[j]);
          }
          return JITRT_StaticCallReturn{nullptr, nullptr};
        }

        // we can update the incoming arg array, either it's
        // the pushed values on the stack by the trampoline, or
        // it's final_args we allocated above.
        dest_args[arg - 1] = new_val;
        allocated_args[allocated_count++] = new_val;
      }
    }
  }

  PyObject* res =
      _PyObject_Vectorcall((PyObject*)func, dest_args, total_args, nullptr);

  for (int j = 0; j < allocated_count; j++) {
    Py_DECREF(allocated_args[j]);
  }

  // If there was an error, don't try to unbox null
  if (res == nullptr) {
    return JITRT_StaticCallReturn{res, nullptr};
  }

  // If we are supposed to be returning a primitive, it needs unboxing because
  // our caller expected this to be a static->static direct invoke, we just
  // failed to JIT the callee.
  int optional, exact;
  PyTypeObject* ret_type = _PyClassLoader_ResolveType(
      _PyClassLoader_GetReturnTypeDescr(func), &optional, &exact);
  int ret_code = _PyClassLoader_GetTypeCode(ret_type);
  Py_DECREF(ret_type);
  if (ret_code != TYPED_OBJECT) {
    // we can always unbox to 64-bit, the JIT will just ignore the higher bits.
    // This means that overflow here will give weird results, but overflow in
    // primitive ints in static python is undefined behavior right now anyway,
    // until we implement overflow checking. It doesn't make sense to implement
    // overflow checking just here in the "unjitable" code path, when overflow
    // won't be checked if the code is JITted.
    void* ival;
    if (ret_code == TYPED_BOOL) {
      ival = (void*)(res == Py_True);
    } else if (ret_code & TYPED_INT_SIGNED) {
      ival = (void*)JITRT_UnboxI64(res);
    } else {
      ival = (void*)JITRT_UnboxU64(res);
    }
    return JITRT_StaticCallReturn{ival, no_error};
  }

  return JITRT_StaticCallReturn{res, no_error};
}

PyObject* JITRT_UnpackExToTuple(
    PyThreadState* tstate,
    PyObject* iterable,
    int before,
    int after) {
  JIT_DCHECK(iterable != nullptr, "The iterable cannot be null.");

  Ref<> it = Ref<>::steal(PyObject_GetIter(iterable));
  if (it == nullptr) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        iterable->ob_type->tp_iter == nullptr && !PySequence_Check(iterable)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "cannot unpack non-iterable %.200s object",
          iterable->ob_type->tp_name);
    }
    return nullptr;
  }

  int totalargs = before + after + 1;
  Ref<PyTupleObject> tuple = Ref<PyTupleObject>::steal(PyTuple_New(totalargs));
  if (tuple == nullptr) {
    return nullptr;
  }
  int ti = 0;

  for (int i = 0; i < before; i++) {
    PyObject* w = PyIter_Next(it);
    if (w == nullptr) {
      /* Iterator done, via error or exhaustion. */
      if (!_PyErr_Occurred(tstate)) {
        if (after == -1) {
          _PyErr_Format(
              tstate,
              PyExc_ValueError,
              "not enough values to unpack "
              "(expected %d, got %d)",
              before,
              i);
        } else {
          _PyErr_Format(
              tstate,
              PyExc_ValueError,
              "not enough values to unpack "
              "(expected at least %d, got %d)",
              before + after,
              i);
        }
      }
      return nullptr;
    }
    tuple->ob_item[ti++] = w;
  }

  JIT_DCHECK(
      after >= 0,
      "This function should only be used for UNPACK_EX, where after >= 0.");

  PyObject* list = PySequence_List(it);
  if (list == nullptr) {
    return nullptr;
  }
  tuple->ob_item[ti++] = list;

  Py_ssize_t list_size = PyList_GET_SIZE(list);
  if (list_size < after) {
    _PyErr_Format(
        tstate,
        PyExc_ValueError,
        "not enough values to unpack (expected at least %d, got %zd)",
        before + after,
        before + list_size);
    return nullptr;
  }

  /* Pop the "after-variable" args off the list. */
  for (int j = after; j > 0; j--) {
    tuple->ob_item[ti++] = PyList_GET_ITEM(list, list_size - j);
  }
  /* Resize the list. */
  Py_SET_SIZE(list, list_size - after);

  return reinterpret_cast<PyObject*>(tuple.release());
}

int JITRT_UnpackSequence(
    PyThreadState* tstate,
    PyObject* iterable,
    PyObject** items,
    int count) {
  JIT_DCHECK(iterable != nullptr, "The iterable cannot be null.");

  Ref<> it = Ref<>::steal(PyObject_GetIter(iterable));
  if (it == nullptr) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        iterable->ob_type->tp_iter == nullptr && !PySequence_Check(iterable)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "cannot unpack non-iterable %.200s object",
          iterable->ob_type->tp_name);
    }
    return -1;
  }

  for (int i = 0; i < count; i++) {
    PyObject* w = PyIter_Next(it);
    if (w == nullptr) {
      /* Iterator done, via error or exhaustion. */
      if (!_PyErr_Occurred(tstate)) {
        _PyErr_Format(
            tstate,
            PyExc_ValueError,
            "not enough values to unpack "
            "(expected %d, got %d)",
            count,
            i);
      }
      /* Clean up any items already stored. */
      for (int j = 0; j < i; j++) {
        Py_DECREF(items[j]);
      }
      return -1;
    }
    items[i] = w;
  }

  /* We should have exhausted the iterator now. */
  PyObject* w = PyIter_Next(it);
  if (w != nullptr) {
    Py_DECREF(w);
    _PyErr_Format(
        tstate,
        PyExc_ValueError,
        "too many values to unpack (expected %d)",
        count);
    for (int j = 0; j < count; j++) {
      Py_DECREF(items[j]);
    }
    return -1;
  }
  if (_PyErr_Occurred(tstate)) {
    for (int j = 0; j < count; j++) {
      Py_DECREF(items[j]);
    }
    return -1;
  }

  return 0;
}

int JITRT_UnicodeEquals(PyObject* s1, PyObject* s2, int equals) {
  // one of these must be unicode for the quality comparison to be okay
  assert(PyUnicode_CheckExact(s1) || PyUnicode_CheckExact(s2));
  if (s1 == s2) {
    return equals == Py_EQ;
  }

  if (PyUnicode_CheckExact(s1) && PyUnicode_CheckExact(s2)) {
    if (PyUnicode_READY(s1) < 0 || PyUnicode_READY(s2) < 0) {
      return -1;
    }

    Py_ssize_t length = PyUnicode_GET_LENGTH(s1);
    if (length != PyUnicode_GET_LENGTH(s2)) {
      return equals == Py_NE;
    }

    Py_hash_t hash1 = ((PyASCIIObject*)s1)->hash;
    Py_hash_t hash2 = ((PyASCIIObject*)s2)->hash;
    if (hash1 != hash2 && hash1 != -1 && hash2 != -1) {
      return equals == Py_NE;
    }

    int kind = PyUnicode_KIND(s1);
    if (kind != PyUnicode_KIND(s2)) {
      return equals == Py_NE;
    }
    void* data1 = PyUnicode_DATA(s1);
    void* data2 = PyUnicode_DATA(s2);
    if (PyUnicode_READ(kind, data1, 0) != PyUnicode_READ(kind, data2, 0)) {
      return equals == Py_NE;
    } else if (length == 1) {
      return equals == Py_EQ;
    } else {
      int result = memcmp(data1, data2, (size_t)(length * kind));
      return (equals == Py_EQ) ? (result == 0) : (result != 0);
    }
  }
  return PyObject_RichCompareBool(s1, s2, equals);
}

PyObject* JITRT_SequenceContains(PyObject* haystack, PyObject* needle) {
  int result = PySequence_Contains(haystack, needle);
  if (result < 0) {
    return nullptr;
  }
  if (result) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* JITRT_SequenceNotContains(PyObject* haystack, PyObject* needle) {
  int result = PySequence_Contains(haystack, needle);
  if (result < 0) {
    return nullptr;
  }
  if (result) {
    Py_RETURN_FALSE;
  }
  Py_RETURN_TRUE;
}

int JITRT_NotContainsBool(PyObject* w, PyObject* v) {
  int res = PySequence_Contains(w, v);
  if (res == -1) {
    return -1;
  }
  return !res;
}

#if CINDERX_JIT_COMPACT_LONG_COMPARE_BOOL_FASTPATH
namespace {

bool JITRT_TryCompactLongCompareBool(
    PyObject* v,
    PyObject* w,
    int op,
    int* result) {
  if (!PyLong_CheckExact(v) || !PyLong_CheckExact(w)) {
    return false;
  }
  auto left_long = reinterpret_cast<PyLongObject*>(v);
  auto right_long = reinterpret_cast<PyLongObject*>(w);
  if (!PyUnstable_Long_IsCompact(left_long) ||
      !PyUnstable_Long_IsCompact(right_long)) {
    return false;
  }

  Py_ssize_t left = PyUnstable_Long_CompactValue(left_long);
  Py_ssize_t right = PyUnstable_Long_CompactValue(right_long);
  switch (op) {
    case Py_LT:
      *result = left < right;
      return true;
    case Py_LE:
      *result = left <= right;
      return true;
    case Py_EQ:
      *result = left == right;
      return true;
    case Py_NE:
      *result = left != right;
      return true;
    case Py_GT:
      *result = left > right;
      return true;
    case Py_GE:
      *result = left >= right;
      return true;
    default:
      return false;
  }
}

} // namespace

int JITRT_FastPyObjectRichCompareBool(PyObject* v, PyObject* w, int op) {
  int result = 0;
  if (JITRT_TryCompactLongCompareBool(v, w, op, &result)) {
    return result;
  }
  return PyObject_RichCompareBool(v, w, op);
}

#define DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(NAME, OP, PYOP)             \
  int JITRT_FastPyObjectRichCompareBool##NAME(PyObject* v, PyObject* w) { \
    if (PyLong_CheckExact(v) && PyLong_CheckExact(w)) {                   \
      auto left_long = reinterpret_cast<PyLongObject*>(v);                \
      auto right_long = reinterpret_cast<PyLongObject*>(w);               \
      if (PyUnstable_Long_IsCompact(left_long) &&                         \
          PyUnstable_Long_IsCompact(right_long)) {                        \
        Py_ssize_t left = PyUnstable_Long_CompactValue(left_long);        \
        Py_ssize_t right = PyUnstable_Long_CompactValue(right_long);      \
        return left OP right;                                             \
      }                                                                   \
    }                                                                     \
    return PyObject_RichCompareBool(v, w, PYOP);                          \
  }

DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(LessThan, <, Py_LT)
DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(LessThanEqual, <=, Py_LE)
DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(Equal, ==, Py_EQ)
DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(NotEqual, !=, Py_NE)
DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(GreaterThan, >, Py_GT)
DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL(GreaterThanEqual, >=, Py_GE)

#undef DEFINE_FAST_COMPACT_LONG_COMPARE_BOOL
#endif

PyObject* JITRT_RichCompare(PyObject* v, PyObject* w, int op) {
#if PY_VERSION_HEX < 0x030C0000
  // Stock 3.11's warmed exact-int COMPARE_OP does not enter Python
  // recursion. The generic JIT helper does, so at the last admitted JIT
  // frame it would raise before the recursive CALL and move the traceback
  // cursor. Borrow CPython recovery headroom only around this exact-builtin
  // operation; user-defined comparison slots never receive it.
  PyThreadState* tstate = _PyThreadState_GET();
  bool exact_long_boundary = Ci_JitRecursionBoundary311_IsActive() &&
      tstate->recursion_headroom == 0 && PyLong_CheckExact(v) &&
      PyLong_CheckExact(w);
  if (exact_long_boundary) {
    tstate->recursion_headroom++;
  }
#endif
  PyObject* result = PyObject_RichCompare(v, w, op);
#if PY_VERSION_HEX < 0x030C0000
  if (exact_long_boundary) {
    tstate->recursion_headroom--;
  }
#endif
  return result;
}

static int JITRT_RichCompareBoolGeneric(PyObject* v, PyObject* w, int op) {
  Ref<> res = Ref<>::steal(JITRT_RichCompare(v, w, op));

  if (res == nullptr) {
    return -1;
  } else if (PyBool_Check(res)) {
    return res == Py_True;
  }

  return PyObject_IsTrue(res);
}

#if CINDERX_JIT_COMPACT_LONG_COMPARE_BOOL_FASTPATH
int JITRT_RichCompareBoolLessThanEqual(PyObject* v, PyObject* w) {
  if (PyLong_CheckExact(v) && PyLong_CheckExact(w)) {
    auto left_long = reinterpret_cast<PyLongObject*>(v);
    auto right_long = reinterpret_cast<PyLongObject*>(w);
    if (PyUnstable_Long_IsCompact(left_long) &&
        PyUnstable_Long_IsCompact(right_long)) {
      Py_ssize_t left = PyUnstable_Long_CompactValue(left_long);
      Py_ssize_t right = PyUnstable_Long_CompactValue(right_long);
      return left <= right;
    }
  }
  return JITRT_RichCompareBoolGeneric(v, w, Py_LE);
}
#endif

/* Perform a rich comparison with integer result.  This wraps
   PyObject_RichCompare(), returning -1 for error, 0 for false, 1 for true. */
int JITRT_RichCompareBool(PyObject* v, PyObject* w, int op) {
#if CINDERX_JIT_COMPACT_LONG_COMPARE_BOOL_FASTPATH
  int fast_result = 0;
  if (JITRT_TryCompactLongCompareBool(v, w, op, &fast_result)) {
    return fast_result;
  }
#endif
#if PY_VERSION_HEX < 0x030C0000
  // Two shapes that dominate the generic arm.  (1) exact float vs exact
  // float: compare the doubles (C ordering matches Python for exact floats,
  // NaN included).  (2) EQ / NE between objects whose types both keep
  // object's own tp_richcompare: object_richcompare answers by identity and
  // NotImplemented resolves to identity too, so the outcome is the pointer
  // test.  A type overriding __eq__/__ne__ (slot_tp_richcompare) or
  // inheriting a C base's compare is excluded by the slot identity check.
  if (PyFloat_CheckExact(v) && PyFloat_CheckExact(w)) {
    double a = PyFloat_AS_DOUBLE(v);
    double b = PyFloat_AS_DOUBLE(w);
    switch (op) {
      case Py_LT:
        return a < b;
      case Py_LE:
        return a <= b;
      case Py_EQ:
        return a == b;
      case Py_NE:
        return a != b;
      case Py_GT:
        return a > b;
      case Py_GE:
        return a >= b;
      default:
        break;
    }
  }
  if ((op == Py_EQ || op == Py_NE) &&
      Py_TYPE(v)->tp_richcompare == PyBaseObject_Type.tp_richcompare &&
      Py_TYPE(w)->tp_richcompare == PyBaseObject_Type.tp_richcompare) {
    return (v == w) == (op == Py_EQ);
  }
#endif

  return JITRT_RichCompareBoolGeneric(v, w, op);
}

/* perform a batch decref to the objects in args */
void JITRT_BatchDecref(PyObject** args, int nargs) {
  for (int i = 0; i < nargs; i++) {
    Py_DECREF(args[i]);
  }
}

Py_ssize_t JITRT_CheckSequenceBounds(PyObject* s, Py_ssize_t i) {
  JIT_DCHECK(!PyErr_Occurred(), "called with error set");
  Py_ssize_t original_i = i;
  i = i < 0 ? i + Py_SIZE(s) : i;
  if (i < 0 || i >= Py_SIZE(s)) {
    // If the access is out of bounds then call the runtime lookup function just
    // to make sure we get a consistent exceptions between interpreter + JIT.
    Ref<> i_obj = Ref<>::steal(PyLong_FromSsize_t(original_i));
    if (i_obj == nullptr) {
      return -1;
    }
    JIT_CHECK(
        PyObject_GetItem(s, i_obj) == nullptr,
        "JIT found bound error, but runtime did not");
    return -1;
  }
  return i;
}

PyObject* JITRT_GetLength(PyObject* obj) {
  // Same as GET_LEN handler in Python/ceval.c
  Py_ssize_t len = PyObject_Length(obj);
  if (len < 0) {
    return nullptr;
  }
  return PyLong_FromSsize_t(len);
}

int JITRT_DictUpdate(PyThreadState* tstate, PyObject* dict, PyObject* update) {
  if (PyDict_Update(dict, update) < 0) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "'%.200s' object is not a mapping",
          Py_TYPE(update)->tp_name);
    }
    return -1;
  }
  return 0;
}

int JITRT_DictMerge(
    PyThreadState* tstate,
    PyObject* dict,
    PyObject* update,
    PyObject* func) {
#if PY_VERSION_HEX >= 0x030F0000
  PyObject* dupkey = NULL;
  if (_PyDict_MergeUniq(dict, update, &dupkey) < 0) {
    _PyEval_FormatKwargsError(tstate, func, update, dupkey);
    Py_XDECREF(dupkey);
    return -1;
  }
#else
  if (_PyDict_MergeEx(dict, update, 2) < 0) {
    _PyEval_FormatKwargsError(tstate, func, update);
    return -1;
  }
#endif
  return 0;
}

PyObject* JITRT_CopyDictWithoutKeys(PyObject* subject, PyObject* keys) {
  // Copied from Python/ceval.c implementation of COPY_DICT_WITHOUT_KEYS.
  Ref<> rest(Ref<>::steal(PyDict_New()));
  if (rest == nullptr || PyDict_Update(rest, subject)) {
    return nullptr;
  }
  JIT_DCHECK(PyTuple_CheckExact(keys), "Expected keys to be an exact tuple");
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
    if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
      return nullptr;
    }
  }
  return rest.release();
}

PyObject* JITRT_LoadName(PyThreadState* tstate, int name_idx) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  return PyTuple_GET_ITEM(frameCode(frame)->co_names, name_idx);
}

void JITRT_FormatAwaitableError(
    PyThreadState* tstate,
    PyTypeObject* type,
    bool is_aenter) {
  if (type->tp_as_async != nullptr && type->tp_as_async->am_await != nullptr) {
    return;
  }
  const char* msg = is_aenter
      ? "'async with' received an object from __aenter__ "
        "that does not implement __await__: %.100s"
      : "'async with' received an object from __aexit__ "
        "that does not implement __await__: %.100s";
  _PyErr_Format(tstate, PyExc_TypeError, msg, type->tp_name);
}

// See ref.cpp: the 3.12 helpers do not exist on 3.11, where the total is
// a process-wide global.
void JITRT_IncRefTotal() {
#ifdef Py_REF_DEBUG
#if PY_VERSION_HEX >= 0x030C0000
  _Py_INCREF_IncRefTotal();
#else
  _Py_RefTotal++;
#endif
#endif
}

void JITRT_DecRefTotal() {
#ifdef Py_REF_DEBUG
#if PY_VERSION_HEX >= 0x030C0000
  _Py_DECREF_DecRefTotal();
#else
  _Py_RefTotal--;
#endif
#endif
}

PyObject* JITRT_LookupAttrSpecial(
    PyObject* obj,
    PyObject* attr,
    const char* failure_fmt_str) {
  PyObject* res = _PyObject_LookupSpecial(obj, attr);
  if (res == nullptr && !_PyErr_Occurred(_PyThreadState_GET())) {
    _PyErr_Format(
        _PyThreadState_GET(),
        PyExc_TypeError,
        failure_fmt_str,
        Py_TYPE(obj)->tp_name);
  }
  return res;
}

LoadMethodResult JITRT_LoadSpecial(
    [[maybe_unused]] PyObject* self,
    [[maybe_unused]] int special_idx) {
#if PY_VERSION_HEX >= 0x030E0000
  _PyStackRef method_and_self[2] = {
      PyStackRef_NULL, PyStackRef_FromPyObjectNew(self)};
  PyObject* name = _Py_SpecialMethods[special_idx].name;
  int err = _PyObject_LookupSpecialMethod(name, method_and_self);
  if (err <= 0) {
    PyStackRef_CLOSE(method_and_self[1]);
    if (err < 0) {
      // When __get__ raises, method_and_self[0] holds a ref to the descriptor
      PyStackRef_XCLOSE(method_and_self[0]);
    } else if (err == 0) {
      PyObject* owner = PyStackRef_AsPyObjectBorrow(method_and_self[1]);
      const char* errfmt = _PyEval_SpecialMethodCanSuggest(owner, special_idx)
          ? _Py_SpecialMethods[special_idx].error_suggestion
          : _Py_SpecialMethods[special_idx].error;
      PyThreadState* tstate = _PyThreadState_GET();
      JIT_CHECK(!_PyErr_Occurred(tstate), "Unexpected existing exception");
      JIT_CHECK(errfmt, "No error message for special method");
      _PyErr_Format(tstate, PyExc_TypeError, errfmt, owner);
    }
    return {nullptr, nullptr};
  }
  LoadMethodResult result;
  result.callable = PyStackRef_AsPyObjectSteal(method_and_self[0]);
  result.self_or_null = PyStackRef_IsNull(method_and_self[1])
      ? nullptr
      : PyStackRef_AsPyObjectSteal(method_and_self[1]);
  return result;
#endif
  JIT_ABORT("JITRT_LoadSpecial not valid with this version of Python");
}

void JITRT_AtQuiescentState([[maybe_unused]] PyThreadState* tstate) {
#ifdef Py_GIL_DISABLED
  _Py_qsbr_quiescent_state(
      (reinterpret_cast<_PyThreadStateImpl*>(tstate))->qsbr);
#endif
}

// clang-format off
PyObject JITRT_IterDoneSentinel = {
    _PyObject_EXTRA_INIT
#if PY_VERSION_HEX < 0x030C0000
    1,
#elif PY_VERSION_HEX >= 0x030E0000
#ifdef Py_GIL_DISABLED
    .ob_tid = _Py_UNOWNED_TID,
    .ob_flags = _Py_STATICALLY_ALLOCATED_FLAG,
    .ob_mutex = {0},
    .ob_gc_bits = _PyGC_BITS_DEFERRED,
    .ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL,
    .ob_ref_shared = 0,
#else
    {.ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT,
     .ob_flags = _Py_STATIC_FLAG_BITS},
#endif
#else
    {.ob_refcnt = _Py_IMMORTAL_REFCNT},
#endif
    nullptr};
// clang-format on

PyObject* JITRT_InvokeIterNext(PyObject* iterator) {
  iternextfunc iternext_f = Py_TYPE(iterator)->tp_iternext;
  if (iternext_f == nullptr) {
    PyErr_Format(
        PyExc_TypeError,
        "'%.100s' object is not an iterator",
        Py_TYPE(iterator)->tp_name);
    return nullptr;
  }
  PyObject* val = iternext_f(iterator);
  if (val != nullptr) {
    return val;
  }
  if (PyErr_Occurred()) {
    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
      return nullptr;
    }
    PyErr_Clear();
  }
  // The sentinel is a borrowed address marker: consumers only compare
  // the pointer and nothing in the tree decrefs it.  On 3.11 it is
  // MORTAL, so an incref here leaked one reference per exhausted loop
  // (on 3.12+ it is immortal and an incref was merely a no-op).
  return &JITRT_IterDoneSentinel;
}

#if PY_VERSION_HEX < 0x030C0000
// Tail of an inlined tp_iternext call that returned NULL: StopIteration (or
// no exception at all) is the done sentinel, anything else propagates.
PyObject* JITRT_IterNextExhausted311() {
  if (PyErr_Occurred()) {
    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
      return nullptr;
    }
    PyErr_Clear();
  }
  return &JITRT_IterDoneSentinel;
}
#endif

// ---------------------------------------------------------------------------
// TreeIter state machine runtime helpers
// ---------------------------------------------------------------------------

int JITRT_EnsureTreeIterState(jit::GenDataFooter* footer) {
  if (footer->tree_iter_state != nullptr) {
    return 0;
  }
  jit::TreeIterState* state = jit::allocateTreeIterState();
  if (state == nullptr) {
    PyErr_NoMemory();
    return -1;
  }
  state->tree_iter_depth_budget = Py_GetRecursionLimit();
  footer->tree_iter_state = state;
  return 0;
}

void JITRT_SaveCurrentNode(jit::GenDataFooter* footer, PyObject* node) {
  jit::TreeIterState* state = footer->tree_iter_state;
  JIT_DCHECK(state != nullptr, "tree_iter_state must be allocated");
  if (node != nullptr) {
    Py_INCREF(node);
  }
  PyObject* old = state->tree_iter_current_node;
  state->tree_iter_current_node = node;
  Py_XDECREF(old);
}

PyObject* JITRT_LoadCurrentNode(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state == nullptr) {
    return nullptr;
  }
  PyObject* node = state->tree_iter_current_node;
  Py_XINCREF(node);
  return node;
}

void JITRT_SavePhase(jit::GenDataFooter* footer, int32_t phase) {
  jit::TreeIterState* state = footer->tree_iter_state;
  JIT_DCHECK(state != nullptr, "tree_iter_state must be allocated");
  state->tree_iter_current_phase = phase;
}

int32_t JITRT_LoadPhase(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state == nullptr) {
    return 0;
  }
  return state->tree_iter_current_phase;
}

int JITRT_StateStackPush(
    jit::GenDataFooter* footer,
    PyObject* node,
    int32_t phase) {
  jit::TreeIterState* state = footer->tree_iter_state;
  JIT_DCHECK(state != nullptr, "tree_iter_state must be allocated");
  int32_t top = state->tree_iter_stack_top;
  if (top >= state->tree_iter_stack_capacity) {
    if (state->tree_iter_stack_capacity >
        std::numeric_limits<int32_t>::max() / 2) {
      PyErr_NoMemory();
      return -1;
    }
    int32_t new_cap = state->tree_iter_stack_capacity * 2;
    auto* new_stack = static_cast<jit::TreeIterStackEntry*>(
        std::realloc(
            state->tree_iter_stack,
            static_cast<std::size_t>(new_cap) *
                sizeof(jit::TreeIterStackEntry)));
    if (new_stack == nullptr) {
      PyErr_NoMemory();
      return -1;
    }
    state->tree_iter_stack = new_stack;
    state->tree_iter_stack_capacity = new_cap;
  }
  if (node != nullptr) {
    Py_INCREF(node);
  }
  state->tree_iter_stack[top].node = node;
  state->tree_iter_stack[top].phase = phase;
  state->tree_iter_stack_top = top + 1;
  return 0;
}

PyObject* JITRT_StateStackPop(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  JIT_DCHECK(state != nullptr, "tree_iter_state must be allocated");
  int32_t top = state->tree_iter_stack_top - 1;
  JIT_DCHECK(top >= 0, "stack underflow in StateStackPop");
  state->tree_iter_stack_top = top;
  PyObject* node = state->tree_iter_stack[top].node;
  state->tree_iter_popped_phase = state->tree_iter_stack[top].phase;
  state->tree_iter_stack[top].node = nullptr;
  state->tree_iter_stack[top].phase = 0;
  return node;
}

int32_t JITRT_LoadPoppedPhase(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state == nullptr) {
    return 0;
  }
  return state->tree_iter_popped_phase;
}

int32_t JITRT_LoadStackTop(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state == nullptr) {
    return 0;
  }
  return state->tree_iter_stack_top;
}

int JITRT_CheckTreeIterChildEntry(
    jit::GenDataFooter* footer,
    PyObject* child) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state == nullptr) {
    PyErr_SetString(PyExc_SystemError, "TreeIter state is not allocated");
    return -1;
  }
  if (child == nullptr || child == Py_None) {
    return 0;
  }
  PyObject* current = state->tree_iter_current_node;
  if (current == nullptr || !Py_IS_TYPE(child, Py_TYPE(current))) {
    PyErr_SetString(
        PyExc_TypeError,
        "TreeIter child type does not match the current node type");
    return -1;
  }

  int32_t next_depth = state->tree_iter_stack_top + 1;
  if (state->tree_iter_depth_budget > 0 &&
      next_depth >= state->tree_iter_depth_budget) {
    PyErr_SetString(
        PyExc_RecursionError,
        "maximum recursion depth exceeded while traversing tree iterator");
    return -1;
  }

  if (state->tree_iter_active_path == nullptr &&
      next_depth <= jit::kTreeIterActivePathHashThreshold) {
    if (child == current) {
      PyErr_SetString(
          PyExc_RecursionError,
          "cycle detected while traversing tree iterator");
      return -1;
    }
    for (int32_t i = 0; i < state->tree_iter_stack_top; i++) {
      if (child == state->tree_iter_stack[i].node) {
        PyErr_SetString(
            PyExc_RecursionError,
            "cycle detected while traversing tree iterator");
        return -1;
      }
    }
    return 0;
  }

  if (state->tree_iter_active_path == nullptr &&
      jit::treeIterMaterializeActivePath(state) < 0) {
    PyErr_NoMemory();
    return -1;
  }

  if (jit::treeIterActivePathInsert(state, current) < 0) {
    PyErr_NoMemory();
    return -1;
  }

  if (jit::treeIterActivePathContains(state, child)) {
    PyErr_SetString(
        PyExc_RecursionError,
        "cycle detected while traversing tree iterator");
    return -1;
  }

  if (jit::treeIterActivePathInsert(state, child) < 0) {
    PyErr_NoMemory();
    return -1;
  }
  return 0;
}

void JITRT_TreeIterEnterChild(
    jit::GenDataFooter* footer,
    PyObject* /* child */) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state != nullptr) {
    state->tree_iter_depth++;
  }
}

void JITRT_TreeIterLeaveCurrentNode(jit::GenDataFooter* footer) {
  jit::TreeIterState* state = footer->tree_iter_state;
  if (state != nullptr && state->tree_iter_depth > 0) {
    jit::treeIterActivePathErase(state, state->tree_iter_current_node);
    state->tree_iter_depth--;
  }
}

void JITRT_ClearTreeIterState(jit::GenDataFooter* footer) {
  jit::clearTreeIterState(footer);
}

static BindKwStatus JITRT_BindKeywordArgsSimple(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    PyObject** arg_space,
    Py_ssize_t total_args,
    BoundDefaultRefs& owned_defaults) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  Py_ssize_t argcount = PyVectorcall_NARGS(nargsf);
  if (argcount > co->co_argcount) {
    return BindKwStatus::Fallback;
  }

  for (int i = 0; i < total_args; i++) {
    arg_space[i] = nullptr;
  }

  // Copy all positional arguments into local variables
  Py_ssize_t n = std::min<Py_ssize_t>(argcount, co->co_argcount);
  for (Py_ssize_t j = 0; j < n; j++) {
    arg_space[j] = args[j];
  }

  // Handle keyword arguments
  if (kwnames != nullptr) {
    for (Py_ssize_t i = 0; i < PyTuple_Size(kwnames); i++) {
      PyObject* keyword = PyTuple_GET_ITEM(kwnames, i);
      PyObject* value = args[argcount + i];
      Py_ssize_t j;

      // Exact str only; subclass __eq__ is CPython's once, not twice.
      if (keyword == nullptr || !PyUnicode_CheckExact(keyword)) {
        return BindKwStatus::Fallback;
      }

      // Speed hack: do raw pointer compares.
      for (j = co->co_posonlyargcount; j < total_args; j++) {
        PyObject* name = jit::getVarname(co, j);
        if (name == keyword) {
          goto kw_found;
        }
      }

      // Slow fallback
      for (j = co->co_posonlyargcount; j < total_args; j++) {
        PyObject* name = jit::getVarname(co, j);
        int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
        if (cmp > 0) {
          goto kw_found;
        } else if (cmp < 0) {
          return BindKwStatus::Error;
        }
      }

      return BindKwStatus::Fallback;

    kw_found:
      if (arg_space[j] != nullptr) {
        return BindKwStatus::Fallback;
      }
      arg_space[j] = value;
    }
  }

  // Check number of positional arguments
  if ((argcount > co->co_argcount) && kwnames == nullptr) {
    return BindKwStatus::Fallback;
  }

  // Add missing positional arguments (copy default values from defs)
  if (argcount < co->co_argcount) {
    PyObject* defaults = func->func_defaults;
    Py_ssize_t defcount = defaults == nullptr ? 0 : PyTuple_Size(defaults);
    Py_ssize_t first_default_arg = co->co_argcount - defcount;

    for (Py_ssize_t i = argcount; i < first_default_arg; i++) {
      if (arg_space[i] == nullptr) {
        return BindKwStatus::Fallback;
      }
    }

    if (defaults != nullptr) {
      PyObject* const* defs = &((PyTupleObject*)defaults)->ob_item[0];
      Py_ssize_t arg_index = std::max(argcount, first_default_arg);
      for (; arg_index < co->co_argcount; arg_index++) {
        if (arg_space[arg_index] == nullptr) {
          Py_ssize_t def_index = arg_index - first_default_arg;
          bindPositionalDefault(
              owned_defaults, defaults, &arg_space[arg_index], defs[def_index]);
        }
      }
    }
  }

  return BindKwStatus::Bound;
}

PyObject* JITRT_CallWithKeywordArgsSimple(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames) {
  [[maybe_unused]] PrologueActivation311 activation;
  vectorcallfunc reentry = JITRT_CAPTURE_REENTRY(func);
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  JIT_DCHECK(
      !(co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)),
      "JITRT_CallWithKeywordArgsSimple doesn't support varargs");
  JIT_DCHECK(
      !co->co_kwonlyargcount,
      "JITRT_CallWithKeywordArgsSimple doesn't support kw only args");
  const Py_ssize_t total_args = co->co_argcount;

  // stack allocate
  auto arg_space = (PyObject**)alloca(total_args * sizeof(PyObject*));
  BoundDefaultRefs owned_defaults;

  switch (JITRT_BindKeywordArgsSimple(
      func, args, nargsf, kwnames, arg_space, total_args, owned_defaults)) {
    case BindKwStatus::Bound: {
      size_t new_nargsf = total_args;
#if PY_VERSION_HEX < 0x030C0000
      return jitrtReenterAfterBind(
          reentry,
          reinterpret_cast<PyObject*>(func),
          arg_space,
          new_nargsf,
          nullptr);
#else
      return reentry((PyObject*)func, arg_space, new_nargsf, nullptr);
#endif
    }
    case BindKwStatus::Error:
      return nullptr;
    case BindKwStatus::Fallback:
      break;
  }

  return Ci_PyFunction_Vectorcall((PyObject*)func, args, nargsf, kwnames);
}
