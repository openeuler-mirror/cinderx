// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "internal/pycore_interp.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#ifndef WIN32
#include <dlfcn.h>
#include <sys/mman.h>
#endif

#include <utility>
#include <vector>

#if PY_VERSION_HEX < 0x030C0000
// The MR-04 execute surface, defined in Jit/pyjit_311_gate.cpp.
#include "cinderx/Interpreter/3.11/observe.h"
#endif

namespace jit {

namespace {

// Defined further down, next to the publisher it mirrors; declared here so
// the Context destructor can drop the cache before anything is torn down.
void clearCachedCompiledIfMatches(
    BorrowedRef<PyCodeObject> code,
    CompiledFunction* compiled);

PyModuleDef* findBuiltinsModule() {
  // We want to check the exact function address, rather than relying on modules
  // which can be mutated.  First find builtins, which we have to do a search
  // for because PyEval_GetBuiltins() returns the module dict.
  BorrowedRef<> mods =
      CI_INTERP_IMPORT_FIELD(_PyInterpreterState_GET(), modules_by_index);
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(mods); i++) {
    BorrowedRef<> cur = PyList_GET_ITEM(mods.get(), i);
    if (Py_IsNone(cur)) {
      continue;
    }
    PyModuleDef* def = PyModule_GetDef(cur);
    if (def == nullptr) {
      PyErr_Clear();
      continue;
    }
    if (std::strcmp(def->m_name, "builtins") == 0) {
      return def;
    }
  }
  return nullptr;
}

} // namespace

AotContext g_aot_ctx;

std::recursive_mutex& freeThreadedJITEntrypointMutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point) {
  return yield_point->isYieldFrom()
      ? reinterpret_cast<PyObject*>(
            *(reinterpret_cast<uint64_t*>(gen_footer) +
              yield_point->yieldFromOffset()))
      : nullptr;
}

void Builtins::init() {
  auto guard = std::lock_guard{mtx_};
  if (is_initialized_) {
    return;
  }
  PyModuleDef* builtins = findBuiltinsModule();
  JIT_CHECK(builtins != nullptr, "Could not find builtins module");

  auto add = [this](const std::string& name, PyMethodDef* meth) {
    cfunc_to_name_[meth] = name;
    name_to_cfunc_[name] = meth;
  };
  // Find all free functions.
  for (PyMethodDef* fdef = builtins->m_methods; fdef->ml_name != nullptr;
       fdef++) {
    add(fdef->ml_name, fdef);
  }
  // Find all methods on types.
  PyTypeObject* types[] = {
      &PyDict_Type,
      &PyList_Type,
      &PyTuple_Type,
      &PyUnicode_Type,
  };
  for (auto type : types) {
    for (PyMethodDef* fdef = type->tp_methods; fdef->ml_name != nullptr;
         fdef++) {
      add(fmt::format("{}.{}", type->tp_name, fdef->ml_name), fdef);
    }
  }
  // Only mark as initialized after everything is done to avoid concurrent
  // reads of an unfinished map.
  is_initialized_ = true;
}

bool Builtins::isInitialized() const {
  return is_initialized_;
}

std::optional<std::string> Builtins::find(PyMethodDef* meth) const {
  auto result = cfunc_to_name_.find(meth);
  if (result == cfunc_to_name_.end()) {
    return std::nullopt;
  }
  return result->second;
}

std::optional<PyMethodDef*> Builtins::find(const std::string& name) const {
  auto result = name_to_cfunc_.find(name);
  if (result == name_to_cfunc_.end()) {
    return std::nullopt;
  }
  return result->second;
}

Context::Context()
    : zero_(Ref<>::steal(PyLong_FromLong(0))),
      str_build_class_(Ref<>::create(&_Py_ID(__build_class__))) {
#if PY_VERSION_HEX >= 0x030E0000
  for (int i = 0; i < NUM_COMMON_CONSTANTS; i++) {
    JIT_CHECK(Ci_common_consts[i] != nullptr, "common_consts[{}] is null", i);
    common_constant_types_.emplace_back(
        hir::Type::fromObject(Ci_common_consts[i]));
  }
#endif
}

Context::~Context() {
#if PY_VERSION_HEX < 0x030C0000
  // Deferred displaced anchors hold owning references whose release runs
  // Python and reaches back into the registries below (an artifact's
  // destructor erases itself from the maps); drain them first, while every
  // map is still intact.
  drainDeferredAnchorReleases();
#endif
  // The CodeExtra fast path caches a borrowed pointer to the artifact on the
  // code object, and the code object outlives this context.  clear(true)
  // below deliberately skips forgetCompiledFunction() -- the map is going
  // away anyway -- and that is the only path that drops the cache, so every
  // compiled code object would be left naming an artifact that dies with the
  // last pin.  Drop the cache here, before anything is cleared.
  for (auto& code : compiled_codes_) {
    clearCachedCompiledIfMatches(
        reinterpret_cast<PyCodeObject*>(code.first.code), code.second.get());
  }

  // Clear all of the CompiledFunction's before we clear out the memory used for
  // the CodeRuntime allocated in the slab.
#if PY_VERSION_HEX < 0x030C0000
  // compiled_codes_ holds borrowed pointers, and on 3.11 an artifact owns the
  // functions attached to it while a function's dictionary owns the artifact.
  // Clearing one entry therefore releases references that can free a
  // *different* entry, whose destructor calls forgetCompiledFunction() and
  // erases from the map being iterated right here -- and can even free the
  // entry currently being cleared, so that clear() writes its own members
  // after they are gone.  Pin every artifact for the length of the loop.  The
  // pins are dropped afterwards, by which point each artifact has lost its
  // owner and will not reach back into this context.
  std::vector<Ref<CompiledFunction>> pinned;
  pinned.reserve(compiled_codes_.size());
  for (auto& code : compiled_codes_) {
    pinned.emplace_back(Ref<CompiledFunction>::create(code.second));
  }
  for (auto& compiled : pinned) {
    compiled->clear(true /* context_finalizing */);
  }
#else
  for (auto& code : compiled_codes_) {
    code.second->clear(true /* context_finalizing */);
  }
#endif
}

void Context::mlockProfilerDependencies() {
#ifndef WIN32
  for (auto& codert : code_runtimes_) {
    if (codert.isCleared()) {
      continue;
    }
    PyCodeObject* code = codert.code().get();
    if (code == nullptr) {
      continue;
    }
    ::mlock(code, sizeof(PyCodeObject));
    ::mlock(code->co_qualname, Py_SIZE(code->co_qualname));
  }
  code_runtimes_.mlock();
#endif
}

Ref<> Context::pageInProfilerDependencies() {
  ThreadedCompileSerialize guard;
  Ref<> qualnames = Ref<>::steal(PyList_New(0));
  if (qualnames == nullptr) {
    return nullptr;
  }
  // We want to force the OS to page in the memory on the
  // code_rt->code->qualname path and keep the compiler from optimizing away
  // the code to do so. There are probably more efficient ways of doing this
  // but perf isn't a major concern.
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    BorrowedRef<> qualname = code_rt.code()->co_qualname;
    if (qualname == nullptr) {
      continue;
    }
    if (PyList_Append(qualnames, qualname) < 0) {
      return nullptr;
    }
  }
  return qualnames;
}

void** Context::findFunctionEntryCache(PyFunctionObject* function) {
  auto result = function_entry_caches_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(function),
      std::forward_as_tuple());
  if (result.second) {
    result.first->second.ptr = pointer_caches_.allocate();
    // _PyClassLoader_HasPrimitiveArgs doesn't work well in multi-threaded
    // compile in 3.12+ due to access of a dictionary with non-key strings.
    // We fix this up post-compile in the multi-threaded case.
    if (!getThreadedCompileContext().compileRunning() &&
        _PyClassLoader_HasPrimitiveArgs((PyCodeObject*)function->func_code)) {
      result.first->second.arg_info =
          Ref<_PyTypedArgsInfo>::steal(_PyClassLoader_GetTypedArgsInfo(
              (PyCodeObject*)function->func_code, 1));
    }
  }
  return result.first->second.ptr;
}

void Context::clearFunctionEntryCache(BorrowedRef<PyFunctionObject> function) {
  function_entry_caches_.erase(function);
}

// See comments in findFunctionEntryCache.
void Context::fixupFunctionEntryCachePostMultiThreadedCompile() {
  for (auto& entry : function_entry_caches_) {
    BorrowedRef<PyCodeObject> code{entry.first->func_code};
    if (entry.second.arg_info.get() == nullptr &&
        _PyClassLoader_HasPrimitiveArgs(code)) {
      entry.second.arg_info = Ref<_PyTypedArgsInfo>::steal(
          _PyClassLoader_GetTypedArgsInfo(code, 1));
    }
  }
}

bool Context::hasFunctionEntryCache(PyFunctionObject* function) const {
  return function_entry_caches_.find(function) != function_entry_caches_.end();
}

_PyTypedArgsInfo* Context::findFunctionPrimitiveArgInfo(
    PyFunctionObject* function) {
  auto cache = function_entry_caches_.find(function);
  if (cache == function_entry_caches_.end()) {
    return nullptr;
  }
  return cache->second.arg_info.get();
}

void Context::recordDeopt(
    CodeRuntime* code_runtime,
    std::size_t idx,
    BorrowedRef<> guilty_value) {
  withDeoptStatsLock([&]() {
    DeoptStat& stat = deopt_stats_[code_runtime][idx];
    stat.count++;
    if (guilty_value != nullptr) {
      stat.types.recordType(Py_TYPE(guilty_value));
    }
  });
}

const DeoptStat* Context::deoptStat(
    const CodeRuntime* code_runtime,
    std::size_t deopt_idx) const {
  auto map_it = deopt_stats_.find(code_runtime);
  if (map_it == deopt_stats_.end()) {
    return nullptr;
  }
  auto stat_it = map_it->second.find(deopt_idx);
  if (stat_it == map_it->second.end()) {
    return nullptr;
  }
  return &stat_it->second;
}

void Context::clearDeoptStats() {
  withDeoptStatsLock([&]() { deopt_stats_.clear(); });
}

InlineCacheStats Context::getAndClearLoadMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadMethodCached instr was
      // optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

InlineCacheStats Context::getAndClearLoadTypeMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_type_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadTypeMethod instr
      // was optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

void Context::setGuardFailureCallback(Context::GuardFailureCallback cb) {
  guard_failure_callback_ = cb;
}

void Context::guardFailed(const DeoptMetadata& deopt_meta) {
  if (guard_failure_callback_) {
    guard_failure_callback_(deopt_meta);
  }
}

void Context::clearGuardFailureCallback() {
  guard_failure_callback_ = nullptr;
}

void Context::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

void Context::releaseReferences() {
  for (auto& code_rt : code_runtimes_) {
    if (code_rt.isCleared()) {
      continue;
    }
    code_rt.releaseReferences();
  }
  references_.clear();
  type_deopt_patchers_.clear();
}

LoadAttrCache* Context::allocateLoadAttrCache() {
  return load_attr_caches_.allocate();
}

LoadTypeAttrCache* Context::allocateLoadTypeAttrCache() {
  return load_type_attr_caches_.allocate();
}

LoadMethodCache* Context::allocateLoadMethodCache() {
  return load_method_caches_.allocate();
}

LoadModuleAttrCache* Context::allocateLoadModuleAttrCache() {
  return load_module_attr_caches_.allocate();
}

LoadModuleMethodCache* Context::allocateLoadModuleMethodCache() {
  return load_module_method_caches_.allocate();
}

LoadTypeMethodCache* Context::allocateLoadTypeMethodCache() {
  return load_type_method_caches_.allocate();
}

StoreAttrCache* Context::allocateStoreAttrCache() {
  return store_attr_caches_.allocate();
}

const Builtins& Context::builtins() {
  // Lock-free fast path followed by single-lock slow path during
  // initialization.
  if (!builtins_.isInitialized()) {
    builtins_.init();
  }
  return builtins_;
}

void Context::unwatch(TypeDeoptPatcher* patcher) {
  type_deopt_patchers_[patcher->type()].erase(patcher);
}

void Context::watchType(
    BorrowedRef<PyTypeObject> type,
    TypeDeoptPatcher* patcher) {
  ThreadedCompileSerialize guard;
  type_deopt_patchers_[type].emplace(patcher);
  // We require the interpreter state in order to watch types
  if (getThreadedCompileContext().compileRunning()) {
    pending_watches_.emplace(type);
    return;
  }

  JIT_CHECK(
      cinderx::getModuleState()->watcher_state.watchType(type) == 0,
      "Failed to watch type {}",
      type->tp_name);
}

BorrowedRef<> Context::zero() {
  return zero_.get();
}

BorrowedRef<> Context::strBuildClass() {
  return str_build_class_.get();
}

void Context::watchPendingTypes() {
  for (auto& type : pending_watches_) {
    JIT_CHECK(
        cinderx::getModuleState()->watcher_state.watchType(type) == 0,
        "Failed to watch pending type {}",
        type->tp_name);
  }
  pending_watches_.clear();
}

void Context::notifyTypeModified(
    BorrowedRef<PyTypeObject> lookup_type,
    BorrowedRef<PyTypeObject> new_type) {
  notifyICsTypeChanged(lookup_type);

  ThreadedCompileSerialize guard;
  auto it = type_deopt_patchers_.find(lookup_type);
  if (it == type_deopt_patchers_.end()) {
    return;
  }

  std::unordered_set<TypeDeoptPatcher*> remaining_patchers;
  for (TypeDeoptPatcher* patcher : it->second) {
    if (!patcher->maybePatch(new_type)) {
      remaining_patchers.emplace(patcher);
    }
  }

  if (remaining_patchers.empty()) {
    type_deopt_patchers_.erase(it);
    // don't unwatch type; other watchers may still be watching it
  } else {
    it->second = std::move(remaining_patchers);
  }
}

bool Context::hasCompletedCompile(CompilationKey& key) {
  return completed_compiles_.contains(key);
}

void Context::addDeferredFinalization(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  ThreadedCompileSerialize guard;
  deferred_finalizations_.emplace_back(
      ThreadedRef<PyFunctionObject>::create(func), compiled);
}

void Context::finalizeMultiThreadedCompile() {
  fixupFunctionEntryCachePostMultiThreadedCompile();
  watchPendingTypes();

  for (auto& codes : completed_compiles_) {
    makeCompiledFunction(
        codes.second.second, codes.first, std::move(codes.second.first));
  }
  completed_compiles_.clear();

  for (auto& [func, compiled] : deferred_finalizations_) {
    finalizeFunc(func, compiled);
  }
  deferred_finalizations_.clear();
}

bool Context::finalizeFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
#if PY_VERSION_HEX < 0x030C0000
  // Every path that gives a function a machine-code entry point arrives
  // here: the direct compile, the batch and lazy paths, re-optimization of
  // a previously deopted function, and any re-attachment of an existing
  // artifact.  The MR-04 execute surface is therefore enforced here rather
  // than at the compile entry alone, where the batch and reopt paths
  // walked around it.  A refusal is not an error; the function simply
  // stays interpreted.
  if (Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
    return true;
  }
#endif
  compiled->setOwner(this);

#if PY_VERSION_HEX < 0x030C0000
  // A function whose __code__ was replaced still carries the association
  // to the artifact of its previous code.  Left in place forever,
  // addCompiledFunc() would report "already compiled" and the freshly
  // built artifact would never be attached, while force_compile() reported
  // success -- and an unsevered claim outlives this call: the old
  // artifact's teardown walks its member list and would dismantle whatever
  // is installed for the function by then.  The claim is found through the
  // association map -- the installed registry cannot answer "who claims
  // this function": a parked function is not in it, and after a __code__
  // swap the old artifact is not reachable through the function's current
  // code either.
  //
  // The prior claim is only identified here, not severed: publication
  // takes over from it atomically.  Severing first was irrecoverable -- a
  // publication that failed afterwards left the function refused by the
  // ownership oracle for as long as the prior artifact stayed resident,
  // with nothing able to re-associate it.
  BorrowedRef<CompiledFunction> prior;
  bool prior_was_installed = false;
  // The same-artifact case -- a parked function re-attaching to its own
  // artifact -- has pre-existing state too: its association and membership
  // predate this attempt, and a failed attempt must not tear down what it
  // did not create.
  bool had_own_association = false;
  {
    auto assoc = associated_funcs_.find(func);
    if (assoc != associated_funcs_.end()) {
      if (assoc->second.get() != compiled.get()) {
        prior = assoc->second;
        auto installed_it = compiled_funcs_.find(func);
        prior_was_installed = installed_it != compiled_funcs_.end() &&
            installed_it->second.get() == prior.get();
      } else {
        had_own_association = true;
      }
    }
  }
  const bool was_member = compiled->functions().contains(func.get());
#endif
#if PY_VERSION_HEX < 0x030C0000
  // Publish transactionally: association first, everything else after,
  // the entry point last.  The association allocates the function's
  // dictionary and stores into it, and it is what makes the artifact
  // own the function; the container writes that follow it can fail
  // too -- std::bad_alloc from the association map or the installed
  // registry -- and each failure restores what the takeover displaced.
  // In the old order a failure here left the registry entry and the
  // machine-code entry already published with no ownership behind them:
  // the guarded entry would refuse (safe), but the registry claimed a
  // compiled function that could never run, and the borrowed entry had
  // nothing keeping it from dangling.
  // Idempotence before the transaction: an installed entry naming this
  // same artifact means recompiling a compiled function, which must change
  // nothing -- not re-run the association and then dismantle a live
  // installation on the duplicate-registration path.  An entry naming a
  // different artifact is the takeover case handled below.
  {
    auto installed_it = compiled_funcs_.find(func);
    if (installed_it != compiled_funcs_.end() &&
        installed_it->second.get() == compiled.get()) {
      // Installed implies not parked.
      removeDeoptedFunc(func);
      return true;
    }
  }
  // The displaced dictionary anchor -- the prior artifact's owning
  // reference -- is detained here for the length of the transaction: it is
  // the restore token if the takeover rolls back, and on success it moves
  // to the deferred-release queue, because releasing it anywhere inside
  // the publication call stack runs arbitrary Python too early (see the
  // settlement comment below).
  Ref<> displaced_anchor;
  if (!associateFunctionWithCompiled(
          func, compiled, false /* is_nested */, &displaced_anchor)) {
    if (displaced_anchor != nullptr && func->func_dict != nullptr) {
      // The association unwound its own write; put the prior anchor back.
      // Failing that is survivable -- the oracle answers by membership,
      // which was never touched -- but never leave the failed artifact
      // anchored.
      if (PyDict_SetItem(
              func->func_dict, kCompiledFunctionKey, displaced_anchor) < 0) {
        PyErr_Clear();
      }
    }
    return false;
  }
  // The association succeeded; the container writes that follow can
  // still throw std::bad_alloc.  An exception here must not escape with
  // the dictionary anchor already written: the artifact would be alive,
  // owned by the function, connected to this context through its owner
  // pointer -- and in no registry, so nothing (context teardown
  // included) would ever find it again.  Failure restores everything the
  // takeover displaced -- map writes to existing keys and erases cannot
  // throw, so the restore itself cannot fail -- and reports through the
  // same channel as an association failure.
  bool installed = false;
  bool failed_allocation = false;
  try {
    // Record the claim in the association map, in lockstep with the
    // artifact's member set the association just updated, then the
    // installed registry.  Both writes overwrite the prior claim in
    // place when there is one.
    throwIfJitPublishStepArmedForTest(2);
    associated_funcs_[func] = compiled;
    throwIfJitPublishStepArmedForTest(3);
    if (prior_was_installed) {
      // The key exists; assignment replaces the prior artifact without
      // allocating, and the unwind below can put it back the same way.
      compiled_funcs_[func] = compiled;
      installed = true;
    } else {
      installed = addCompiledFunc(func, compiled);
    }
  } catch (const std::bad_alloc&) {
    failed_allocation = true;
  }
  if (failed_allocation || !installed) {
    // Either an allocation failed mid-transaction, or -- unreachable
    // while installs are serialized by the entry guard, kept as a
    // defensive unwind -- another artifact already claims the function.
    // Both put back exactly what predated this attempt and erase only what
    // this attempt created: a different-artifact prior gets its
    // association and registry slots written back in place (existing keys,
    // so the writes cannot throw) with its membership never touched; a
    // same-artifact claim -- a parked re-attach -- keeps the association
    // and membership it already had; and the dictionary anchor is restored
    // from the detained reference whenever one was displaced.
    if (!was_member) {
      compiled->removeFunction(func);
    }
    if (prior != nullptr) {
      associated_funcs_[func] = prior;
      if (prior_was_installed) {
        compiled_funcs_[func] = prior;
      }
    } else if (!had_own_association) {
      auto assoc = associated_funcs_.find(func);
      if (assoc != associated_funcs_.end() &&
          assoc->second.get() == compiled.get()) {
        associated_funcs_.erase(assoc);
      }
    }
    if (func->func_dict != nullptr) {
      if (displaced_anchor != nullptr) {
        if (PyDict_SetItem(
                func->func_dict, kCompiledFunctionKey, displaced_anchor) < 0) {
          // Survivable: the oracle answers by membership, which is intact.
          // But never leave the failed artifact anchored.
          PyErr_Clear();
          if (PyDict_DelItem(func->func_dict, kCompiledFunctionKey) < 0) {
            PyErr_Clear();
          }
        }
      } else {
        if (PyDict_DelItem(func->func_dict, kCompiledFunctionKey) < 0) {
          PyErr_Clear();
        }
      }
    }
    if (failed_allocation) {
      PyErr_NoMemory();
      return false;
    }
    return true;
  }

  // The takeover settles only now: the prior claim ends with the new one
  // fully published, so a failure above never leaves the function
  // claimless.  The detained anchor reference is not released here at all:
  // even after settlement, releasing it inside this call stack runs
  // arbitrary Python between the verdict a caller is about to compute and
  // the moment that verdict is reported -- a __del__ calling disable()
  // would unpublish what the caller then reports as installed -- and a
  // release inside the enable() reattach walk would mutate the parked set
  // being iterated.  It is queued instead, and drained at control-plane
  // boundaries that re-verify what they report.
  if (prior != nullptr) {
    prior->removeFunction(func);
  }
  if (displaced_anchor != nullptr) {
    deferred_anchor_releases_.emplace_back(std::move(displaced_anchor));
  }

  // In case the function had previously been deopted.
  removeDeoptedFunc(func);

  // Route 3.11 calls through the guarded entry, which re-checks the code
  // identity and the call form that compilation assumed before entering
  // machine code (see Jit/pyjit_311_gate.cpp).
  setVectorcall(func, Ci_JitShell311_GuardedEntry);
  if (hasFunctionEntryCache(func)) {
    void** indirect = findFunctionEntryCache(func);
    *indirect = compiled->staticEntry();
  }
  return true;
#else
  if (!addCompiledFunc(func, compiled)) {
    // Someone else compiled the function between when our caller checked and
    // called us.
    return true;
  }

  // In case the function had previously been deopted.
  removeDeoptedFunc(func);

  setVectorcall(func, compiled->vectorcallEntry());
  if (hasFunctionEntryCache(func)) {
    void** indirect = findFunctionEntryCache(func);
    *indirect = compiled->staticEntry();
  }

  // Associate the function with the CompiledFunction for GC tracking.
  // This is ultimately what will keep the CompiledFunction alive and
  // keep the PyFunctionObject JITed.
  return associateFunctionWithCompiled(func, compiled, false /* is_nested */);
#endif
}

namespace {

constexpr size_t kCodeDedupMaxEntries = 2048;

// A code object is eligible for content-keyed compile reuse when it is
// provably namespace-free: its bytecode never reads or writes globals,
// builtins or module-level names, so the compiled artifact cannot reference
// any globals-coupled cache and is valid under any (globals, builtins) pair.
// Suspendable and Static Python code is excluded to keep per-code runtime
// structures out of scope. These are semantic properties of the bytecode;
// which code objects actually recur is decided by behavioral evidence (see
// noteCompiledFuncDestroyed), not by name or origin heuristics.
bool isDedupEligibleCode(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags &
      (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR |
       CI_CO_STATICALLY_COMPILED)) {
    return false;
  }
  BytecodeInstructionBlock block{code};
  for (auto it = block.begin(); it != block.end(); ++it) {
    switch ((*it).opcode()) {
      case LOAD_GLOBAL:
      case STORE_GLOBAL:
      case DELETE_GLOBAL:
      case LOAD_NAME:
      case STORE_NAME:
      case DELETE_NAME:
      case LOAD_FROM_DICT_OR_GLOBALS:
      case LOAD_BUILD_CLASS:
      case IMPORT_NAME:
      case IMPORT_FROM:
        return false;
      default:
        break;
    }
  }
  // Exclude code that creates nested functions: co_consts code objects would
  // be shared through the canonical identity, and nested-function creation
  // semantics under adopted identities are not worth reasoning about for
  // this cohort (the exec-generated helpers this targets have flat bodies).
  BorrowedRef<> consts{code->co_consts};
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(consts.get()); ++i) {
    if (PyCode_Check(PyTuple_GET_ITEM(consts.get(), i))) {
      return false;
    }
  }
  return true;
}

// Cheap structural fingerprint over immutable code-object fields, used to
// prefilter registry probes so the common case (no registered donor with a
// matching shape) costs a hash lookup instead of a marshal. Deliberately
// excludes the bytecode buffer, which is mutated in place by the adaptive
// interpreter. Collisions are resolved by full marshal comparison.
uint64_t codeFingerprint(BorrowedRef<PyCodeObject> code) {
  return combineHash(
      static_cast<size_t>(Py_SIZE(code.get())),
      static_cast<size_t>(code->co_argcount),
      static_cast<size_t>(code->co_nlocalsplus),
      static_cast<size_t>(code->co_stacksize),
      static_cast<size_t>(code->co_flags),
      static_cast<size_t>(PyTuple_GET_SIZE(code->co_consts)),
      static_cast<size_t>(PyTuple_GET_SIZE(code->co_names)));
}

} // namespace

namespace {

// Same-origin gate for content reuse: CPython's code equality deliberately
// ignores filename and line numbers, but sharing artifacts across source
// locations would make tracebacks point at a different origin. Require the
// same origin so a donor is only ever the same logical function.
bool sameCodeOrigin(BorrowedRef<PyCodeObject> a, BorrowedRef<PyCodeObject> b) {
  if (a->co_firstlineno != b->co_firstlineno) {
    return false;
  }
  int eq = PyObject_RichCompareBool(a->co_filename, b->co_filename, Py_EQ);
  if (eq < 0) {
    PyErr_Clear();
    return false;
  }
  return eq == 1;
}

// Value equality of code objects, as maintained by CPython itself: compares
// bytecode (de-instrumented), constants, names and arities by value, and is
// insensitive to string interning state, which makes it stable across
// generations of recreated code (marshal output is not).
bool sameCodeContent(BorrowedRef<PyCodeObject> a, BorrowedRef<PyCodeObject> b) {
  int eq = PyObject_RichCompareBool(a.getObj(), b.getObj(), Py_EQ);
  if (eq < 0) {
    PyErr_Clear();
    return false;
  }
  return eq == 1;
}

} // namespace

bool Context::reuseDedupedCompiled(BorrowedRef<PyFunctionObject> func) {
  BorrowedRef<PyCodeObject> code{func->func_code};
  // Fingerprint prefilter: only code whose structural shape matches a
  // recorded entry pays the eligibility scan and value comparison.
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return false;
  }
  if (!isDedupEligibleCode(code)) {
    return false;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.compiled != nullptr && entry.compiled->runtime() == nullptr) {
      // Defense in depth: the artifact was gutted by clear() without going
      // through dropDedupArtifact(). Demote instead of re-installing a dead
      // entrypoint; recompilation can re-promote the entry.
      entry.compiled.reset();
    }
    if (entry.compiled == nullptr || entry.code.get() == code.get() ||
        !sameCodeOrigin(code, entry.code) ||
        !sameCodeContent(code, entry.code)) {
      continue;
    }
    // Canonicalize onto the donor code object so all per-code machinery
    // (compiled_codes_, the CodeExtra fast-attach, call counters) converges
    // on a single identity. Assigning through the attribute keeps
    // function-version and watcher bookkeeping correct.
    if (PyObject_SetAttrString(func, "__code__", entry.code.getObj()) < 0) {
      PyErr_Clear();
      return false;
    }
    JIT_DLOG("dedup: attaching twin {}", funcFullname(func));
    return finalizeFunc(func, entry.compiled);
  }
  return false;
}

void Context::noteCodeCompiled(
    const CompilationKey& key,
    BorrowedRef<CompiledFunction> compiled) {
  if (!getConfig().auto_code_twin_dedup) {
    return;
  }
  BorrowedRef<PyCodeObject> code{key.code};
  if (!isDedupEligibleCode(code)) {
    return;
  }
  uint64_t fingerprint = codeFingerprint(code);
  auto bucket_it = code_dedup_cache_.find(fingerprint);
  if (bucket_it != code_dedup_cache_.end()) {
    for (CodeDedupEntry& entry : bucket_it->second) {
      if (entry.code.get() == code.get()) {
        return;
      }
      if (!sameCodeOrigin(code, entry.code) ||
          !sameCodeContent(code, entry.code)) {
        continue;
      }
      // Second compilation of identical content: that is the recurrence
      // evidence this machinery exists for. This compilation becomes the
      // canonical donor; content-identical code compiled later attaches to
      // its pinned artifact instead of recompiling. Namespace-free code has
      // no globals-coupled caches, so the artifact is valid under any
      // (globals, builtins) pair.
      if (entry.compiled == nullptr) {
        JIT_DLOG("dedup: donor promoted for {}", codeQualname(code));
        entry.code = Ref<PyCodeObject>::create(code);
        entry.compiled = Ref<CompiledFunction>::create(compiled);
      }
      return;
    }
  }
  // First sighting: record the code object so a later compilation of
  // identical content can prove recurrence.
  if (code_dedup_size_ < kCodeDedupMaxEntries) {
    code_dedup_cache_[fingerprint].push_back(
        CodeDedupEntry{Ref<PyCodeObject>::create(code), nullptr});
    code_dedup_size_++;
  }
}

void Context::dropDedupArtifact(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<CompiledFunction> compiled) {
  if (code_dedup_cache_.empty()) {
    return;
  }
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.compiled.get() == compiled.get()) {
      JIT_DLOG("dedup: donor demoted for {}", codeQualname(entry.code));
      entry.compiled.reset();
    }
  }
}

void Context::noteCompiledFuncDestroyed(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  if (!getConfig().auto_code_twin_dedup || Py_IsFinalizing()) {
    return;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  auto bucket_it = code_dedup_cache_.find(codeFingerprint(code));
  if (bucket_it == code_dedup_cache_.end()) {
    return;
  }
  for (CodeDedupEntry& entry : bucket_it->second) {
    if (entry.code.get() == code.get()) {
      // The dying function's code is itself the recorded entry; pin its
      // artifact so a later recurrence can attach instead of recompiling.
      if (entry.compiled == nullptr && isDedupEligibleCode(code)) {
        JIT_DLOG("dedup: donor rescued from dying {}", funcFullname(func));
        entry.compiled = Ref<CompiledFunction>::create(compiled);
      }
      return;
    }
  }
}

bool Context::codeCompiled(
    BorrowedRef<PyFunctionObject> func,
    CompilationKey& key,
    CompiledFunctionData&& compiled_func) {
  addCompileTime(compiled_func.compile_time);

  if (getThreadedCompileContext().compileRunning()) {
    completed_compiles_.emplace(
        key,
        std::pair(
            std::move(compiled_func),
            ThreadedRef<PyFunctionObject>::create(func)));
    return true;
  }

  return makeCompiledFunction(func, key, std::move(compiled_func)) != nullptr;
}

const hir::Type& Context::typeForCommonConstant([[maybe_unused]] int i) const {
#if PY_VERSION_HEX >= 0x030E0000
  return common_constant_types_.at(i);
#endif
  JIT_ABORT("Common constants are a feature of 3.14+");
}

namespace {
// Publish the compiled entry on the code object's CodeExtra so a newly created
// function with the same code+globals+builtins can skip the compiled_codes_
// hashmap lookup. Stores a *borrowed* pointer -- the CompiledFunction is kept
// alive by the usual anchors (the compiling function's __dict__ / the outer
// function's nested list) and is cleared here before it is freed. The pointer
// is published with release ordering after its globals/builtins so a concurrent
// reader (under the same JIT entrypoint guard) sees a consistent triple.
void cacheCompiledOnCode(const CompilationKey& key, CompiledFunction* compiled) {
  CodeExtra* extra = codeExtra(reinterpret_cast<PyCodeObject*>(key.code));
  if (extra == nullptr) {
    return;
  }
  extra->jit_globals = key.globals;
  extra->jit_builtins = key.builtins;
  _Py_atomic_store_ptr_release(&extra->jit_compiled, compiled);
}

// Clear the cache only if it still points at `compiled`. This must run before
// `compiled` is freed and under JIT entrypoint serialization; the check guards
// against clearing an entry republished earlier in the same serialized flow, not
// against lock-free concurrent publishers.
void clearCachedCompiledIfMatches(
    BorrowedRef<PyCodeObject> code,
    CompiledFunction* compiled) {
  CodeExtra* extra = codeExtra(code);
  if (extra != nullptr &&
      _Py_atomic_load_ptr_relaxed(&extra->jit_compiled) == compiled) {
    _Py_atomic_store_ptr_release(&extra->jit_compiled, nullptr);
    extra->jit_globals = nullptr;
    extra->jit_builtins = nullptr;
  }
}
} // namespace

void Context::forgetCode(BorrowedRef<PyFunctionObject> func) {
  auto it = compiled_codes_.find(CompilationKey{func});
  if (it == compiled_codes_.end()) {
    return;
  }

  // Remove the CF from any outer function's nested compiled functions list.
  // When a nested function is compiled, its CF is stored both in the
  // function's own __dict__ and in the outer function's
  // __cinderx_nested_compiled_funcs__ list. We need to clean up the latter
  // when forgetting the code.
  BorrowedRef<CompiledFunction> cf = it->second;
  BorrowedRef<PyCodeObject> code{it->first.code};
  auto outer_it = code_outer_funcs_.find(code);
  if (outer_it != code_outer_funcs_.end() && outer_it->second != func) {
    BorrowedRef<PyFunctionObject> outer = outer_it->second;
    PyObject* outer_dict = outer->func_dict;
    if (outer_dict != nullptr) {
      Ref<> nested_list = getDictRef(outer_dict, kNestedCompiledFunctionsKey);
      if (nested_list != nullptr && PyList_CheckExact(nested_list.get())) {
        for (Py_ssize_t i = PyList_GET_SIZE(nested_list.get()) - 1; i >= 0;
             i--) {
          if (PyList_GET_ITEM(nested_list.get(), i) ==
              reinterpret_cast<PyObject*>(cf.get())) {
            if (PyList_SetSlice(nested_list.get(), i, i + 1, nullptr) < 0) {
              PyErr_Clear();
            }
            break;
          }
        }
      }
    }
  }

  clearCachedCompiledIfMatches(code, cf.get());
  dropDedupArtifact(code, cf);
  it->second->clear();
  compiled_codes_.erase(CompilationKey{func});
}

void Context::forgetCompiledFunction(CompiledFunction& function) {
  // tp_clear() can reach here from GC without going through a guarded
  // top-level JIT entrypoint, so this path has to take the FT guard itself.
  FreeThreadedJITEntrypointGuard guard;
  if (function.runtime() != nullptr) {
    for (auto pyfunc : function.functions()) {
#if PY_VERSION_HEX < 0x030C0000
      // Membership means "claimed", not "currently installed by me": a
      // member may be parked, or -- when the severing in finalizeFunc()
      // was bypassed by an even later teardown ordering -- installed by a
      // successor artifact.  A dying generation dismantles only what it
      // still owns: the registry entry and the entry point are unwound
      // solely when the registry names this artifact, and the claim map
      // entry solely when it does.  An unconditional erase here was how a
      // stale artifact's delayed destruction knocked out its successor's
      // live installation.
      auto installed = compiled_funcs_.find(pyfunc);
      if (installed != compiled_funcs_.end() &&
          installed->second.get() == &function) {
        pyfunc->vectorcall = getInterpretedVectorcall(pyfunc);
        compiled_funcs_.erase(installed);
      }
      auto assoc = associated_funcs_.find(pyfunc);
      if (assoc != associated_funcs_.end() &&
          assoc->second.get() == &function) {
        associated_funcs_.erase(assoc);
      }
#else
      compiled_funcs_.erase(pyfunc);
#endif
    }
    CompilationKey key{function};
    // Drop the CodeExtra fast-path cache before this CompiledFunction is freed,
    // otherwise the cached (borrowed) pointer would dangle.
    clearCachedCompiledIfMatches(
        reinterpret_cast<PyCodeObject*>(key.code), &function);
    compiled_codes_.erase(key);
  }
}

bool Context::didCompile(BorrowedRef<PyFunctionObject> func) {
  ThreadedCompileSerialize guard;
  return compiled_funcs_.contains(func);
}

BorrowedRef<CompiledFunction> Context::lookupFunc(
    BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

CodeRuntime* Context::lookupCodeRuntime(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* compiled = lookupFunc(func);
  if (compiled == nullptr) {
    if (func->func_dict != nullptr) {
      // For multi-threaded compile tests we clear the compiled codes. This is a
      // super funky thing to do because the functions may actually still be
      // running and we may try and get the code runtime. So here we make a
      // last-ditch effort to try and recover the runtime from the function.
      Ref<> compiled_val = getDictRef(func->func_dict, kCompiledFunctionKey);
      if (compiled_val != nullptr &&
          Py_TYPE(compiled_val) == getCompiledFunctionType()) {
        auto compiled_func =
            reinterpret_cast<CompiledFunction*>(compiled_val.get());
        if (compiled_func->functions().contains(func)) {
          return compiled_func->runtime();
        }
      }
    }
    return nullptr;
  }
  return compiled->runtime();
}

const UnorderedMap<CompilationKey, BorrowedRef<CompiledFunction>>&
Context::compiledCodes() const {
  return compiled_codes_;
}

const UnorderedMap<
    BorrowedRef<PyFunctionObject>,
    BorrowedRef<CompiledFunction>>&
Context::compiledFuncs() {
  return compiled_funcs_;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::deoptedFuncs() {
  return deopted_funcs_;
}

void Context::addCompileTime(std::chrono::nanoseconds time) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time);
  total_compile_time_ms_.fetch_add(ms.count(), std::memory_order_relaxed);
}

std::chrono::milliseconds Context::totalCompileTime() const {
  return std::chrono::milliseconds{
      total_compile_time_ms_.load(std::memory_order_relaxed)};
}

void Context::setCinderJitModule(Ref<> mod) {
  cinderjit_module_ = std::move(mod);
}

void Context::clearForMultithreadedCompileTest() {
  // Nothing here may release a reference until every artifact is pinned and
  // both borrowed registries are empty.  On 3.11 an artifact owns its
  // functions and a function's dictionary owns the artifact, so a release
  // can destroy the artifact the loop is holding by borrow -- and its
  // destructor erases from the very maps being walked.  Pin first, detach
  // second, release last.
  std::vector<Ref<CompiledFunction>> pinned;
  pinned.reserve(compiled_funcs_.size());
  UnorderedSet<CompiledFunction*> seen;
  for (auto& func_entry : compiled_funcs_) {
    BorrowedRef<CompiledFunction> compiled = func_entry.second;
    // One artifact can serve several functions; orphan each one once.
    if (seen.emplace(compiled.get()).second) {
      pinned.emplace_back(Ref<CompiledFunction>::create(compiled));
    }
  }

  // The CodeExtra cache names artifacts by borrow, and the key it is looked
  // up under comes from the runtime, so this has to run before anything is
  // detached or released.
  for (auto& compiled : pinned) {
    if (compiled->runtime() != nullptr) {
      CompilationKey key{*compiled.get()};
      clearCachedCompiledIfMatches(
          reinterpret_cast<PyCodeObject*>(key.code), compiled.get());
    }
  }

  // Empty the borrowed registries before any release can run Python: an
  // artifact destroyed below would otherwise call back in to erase from
  // them.
  compiled_codes_.clear();
  compiled_funcs_.clear();
#if PY_VERSION_HEX < 0x030C0000
  associated_funcs_.clear();
#endif

  for (auto& compiled : pinned) {
#if PY_VERSION_HEX < 0x030C0000
    // On 3.11 this set owns its functions, and clear() only reaches the
    // branch that releases them while the artifact still has an owner.
    // Detaching below would therefore take those references to the grave.
    // Release them here, under the pin above -- releasing a function can
    // drop the dictionary holding this artifact's last reference.  The
    // functions keep the guarded entry, which refuses once the association
    // is gone and sends them back to the interpreter.
    compiled->releaseOwnedFunctions();
#endif
    // Disconnect from Context so clear() on eventual destruction won't call
    // back into us (e.g., forgetCompiledFunction, unwatch).
    compiled->setOwner(nullptr);
    // Keep the old CompiledFunction alive via a strong reference.
    orphaned_compiled_codes_.emplace_back(std::move(compiled));
  }
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  auto it = compiled_funcs_.find(func);
  if (it != compiled_funcs_.end()) {
    noteCompiledFuncDestroyed(func, it->second);
    it->second->removeFunction(func);
    compiled_funcs_.erase(func);
  }
#if PY_VERSION_HEX < 0x030C0000
  // Membership survives deopt on this branch, so a function that dies while
  // parked is still named by its claiming artifact's owned-functions set;
  // the installed-registry path above does not cover it.  Left in place the
  // entry would poison the ownership oracle for whichever function the
  // allocator hands this address to next -- and, once the claiming
  // artifact's own teardown runs, its member walk would write the entry
  // point of a function that no longer exists.  The claim is resolved
  // through the association map: after a __code__ swap the artifact is not
  // reachable through the function's current code object, so the code-extra
  // ledger cannot answer for it.  Reachable only once a death notification
  // exists -- while the set owns its entries nothing in it can die.
  {
    auto assoc = associated_funcs_.find(func);
    if (assoc != associated_funcs_.end()) {
      assoc->second->removeFunction(func);
      associated_funcs_.erase(assoc);
    }
  }
#endif
  removeDeoptedFunc(func);
  // This doesn't modify compiled_codes_, so if this is a nested function it can
  // easily be reopted later.
}

BorrowedRef<CompiledFunction> Context::lookupCode(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  ThreadedCompileSerialize guard;
  auto it = compiled_codes_.find(CompilationKey{code, builtins, globals});
  return it == compiled_codes_.end() ? nullptr : it->second.get();
}

void Context::addDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  // Own the reference on 3.11.  This set is walked again when the JIT is
  // re-enabled, and nothing tells the runtime that a function died in the
  // meantime -- there are no function watchers -- so a borrowed pointer
  // here is a function that can be freed while paused and dereferenced on
  // the way back.  The reference is released when the function leaves the
  // set, which re-enabling and finalization both do.
  if (deopted_funcs_.emplace(func).second) {
    Py_INCREF(func.get());
  }
#else
  deopted_funcs_.emplace(func);
#endif
}

void Context::removeDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030C0000
  if (deopted_funcs_.erase(func) > 0) {
    Py_DECREF(func.get());
  }
#else
  deopted_funcs_.erase(func);
#endif
}

void Context::clearDeoptedFuncs() {
  // Detach before releasing.  A release here can be the last reference to a
  // function, and destroying it runs arbitrary Python -- __del__ on anything
  // its dictionary holds -- which can re-enter the JIT.  enable() walks this
  // very set, so a re-entrant call during the loop below would mutate the
  // container being iterated and release entries a second time.  Emptying it
  // first leaves re-entry nothing to walk.
  UnorderedSet<BorrowedRef<PyFunctionObject>> parked;
  parked.swap(deopted_funcs_);
#if PY_VERSION_HEX < 0x030C0000
  for (BorrowedRef<PyFunctionObject> func : parked) {
    Py_DECREF(func.get());
  }
#endif
}

#if PY_VERSION_HEX < 0x030C0000
void Context::drainDeferredAnchorReleases() {
  // One reference at a time, re-reading the queue between releases: a
  // release runs arbitrary Python, which can publish again and defer more
  // anchors into the same queue.
  while (!deferred_anchor_releases_.empty()) {
    Ref<> anchor = std::move(deferred_anchor_releases_.back());
    deferred_anchor_releases_.pop_back();
    anchor.reset();
  }
}
#endif

bool Context::addCompiledFunc(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled) {
  return compiled_funcs_.emplace(func, compiled).second;
}

bool Context::removeCompiledFunc(BorrowedRef<PyFunctionObject> func) {
  auto in_compiled_funcs = compiled_funcs_.find(func);
  if (in_compiled_funcs != compiled_funcs_.end()) {
#if PY_VERSION_HEX >= 0x030C0000
    in_compiled_funcs->second->removeFunction(func);
#else
    // Membership survives a deopt on 3.11: the artifact's owned-functions
    // set is the ownership oracle -- the only record of "whose artifact is
    // this" that Python code cannot write to -- and the one-artifact-per-
    // code refusal consults it to let a parked function back onto its own
    // artifact.  What ends here is the installation, recorded by this
    // registry and the entry point; the association ends with the
    // artifact, with an explicit re-association, or with the function's
    // own death.
#endif
    compiled_funcs_.erase(in_compiled_funcs);
    return true;
  }
  return false;
}

bool Context::addActiveCompile(CompilationKey& key) {
  return active_compiles_.insert(key).second;
}

void Context::removeActiveCompile(CompilationKey& key) {
  active_compiles_.erase(key);
}

Ref<CompiledFunction> Context::makeCompiledFunction(
    BorrowedRef<PyFunctionObject> func,
    const CompilationKey& key,
    CompiledFunctionData&& compiled_func) {
  BorrowedRef<PyFunctionObject> outer = nullptr;
  auto outer_it = code_outer_funcs_.find(key.code);
  if (outer_it != code_outer_funcs_.end() && outer_it->second != func) {
    outer = outer_it->second;
  }
  bool immortal = getConfig().immortalize_compiled_functions ||
      (func != nullptr && _Py_IsImmortal(func)) ||
      (outer != nullptr && _Py_IsImmortal(outer));
  auto compiled = CompiledFunction::create(std::move(compiled_func), immortal);
  if (compiled == nullptr) {
    return nullptr;
  }

  // If the registered outer func for the code is different than the func we
  // will register the CompiledCode on the outer most function.
  if (outer != nullptr && outer->func_globals == key.globals &&
      outer->func_builtins == key.builtins &&
      !associateFunctionWithCompiled(outer, compiled, true)) {
    return nullptr;
  }

#if PY_VERSION_HEX < 0x030C0000
  // finalizeFunc() reports a refusal as "nothing to do", which is right
  // for the re-attachment paths but wrong here: publishing below would
  // leave compiled_codes_ and the code-extra cache pointing at an artifact
  // whose only strong reference is the local one about to expire.  Refuse
  // before anything is published.
  if (func != nullptr && Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
    return nullptr;
  }
  // Reserve the execute ledger before anything publishes.  Everything after
  // finalizeFunc() succeeds treats cacheCompiledOnCode() as infallible, but
  // that call quietly does nothing when the code object's extra block
  // cannot be allocated -- and the result would be a function the control
  // plane calls compiled whose every call runs interpreted, because the
  // ledger the entry reads was never written.  An empty pre-reserved block
  // is harmless if compilation fails later.
  if (codeExtraOrError(reinterpret_cast<PyCodeObject*>(key.code)) == nullptr) {
    return nullptr;
  }
#endif
#if PY_VERSION_HEX < 0x030C0000
  // Publication order on 3.11: the compiled-codes entry is the last
  // fallible container insert, so it goes in BEFORE finalizeFunc() --
  // whose own final step, the entry point, must be the last act of the
  // whole transaction.  A failure on either side unwinds by key or by
  // artifact identity; nothing observable survives a failed publish.
  try {
    throwIfJitPublishStepArmedForTest(4);
    auto pair = compiled_codes_.emplace(key, compiled);
    JIT_CHECK(
        pair.second,
        "CompilationKey already present {}",
        PyUnicode_AsUTF8(
            reinterpret_cast<PyCodeObject*>(key.code)->co_qualname));
  } catch (const std::bad_alloc&) {
    PyErr_NoMemory();
    return nullptr;
  }
  if (func != nullptr && !finalizeFunc(func, compiled)) {
    compiled_codes_.erase(key);
    return nullptr;
  }
  cacheCompiledOnCode(key, compiled);
  try {
    noteCodeCompiled(key, compiled);
  } catch (const std::bad_alloc&) {
    // Dedup bookkeeping only; the publication is complete without it.
  }
  return compiled;
#else
  if (func != nullptr && !finalizeFunc(func, compiled)) {
    return nullptr;
  }

  // We are storing a borrowed reference to the CompiledFunction. For functions,
  // finalizeFunc has put the CompiledFunction in the function's dictionary to
  // keep it alive. Code objects will be deleted when we receive a notification
  // from Python that they are being destroyed.
  auto pair = compiled_codes_.emplace(key, compiled);
  JIT_CHECK(
      pair.second,
      "CompilationKey already present {}",
      PyUnicode_AsUTF8(reinterpret_cast<PyCodeObject*>(key.code)->co_qualname));
  cacheCompiledOnCode(key, compiled);
  noteCodeCompiled(key, compiled);
  return compiled;
#endif
}

#ifndef WIN32
void AotContext::init(void* bundle_handle) {
  JIT_CHECK(
      bundle_handle_ == nullptr,
      "Trying to register AOT bundle at {} but already have one at {}",
      bundle_handle,
      bundle_handle_);
  bundle_handle_ = bundle_handle;
}

void AotContext::destroy() {
  if (bundle_handle_ == nullptr) {
    return;
  }

  // TASK(T183003853): Unmap compiled functions and empty out private data
  // structures.

  dlclose(bundle_handle_);
  bundle_handle_ = nullptr;
}

void AotContext::registerFunc(const elf::Note& note) {
  elf::CodeNoteData note_data = elf::parseCodeNote(note);
  JIT_LOG("  Function {}", note.name);
  JIT_LOG("    File: {}", note_data.file_name);
  JIT_LOG("    Line: {}", note_data.lineno);
  JIT_LOG("    Hash: {:#x}", note_data.hash);
  JIT_LOG("    Size: {}", note_data.size);
  JIT_LOG("    Normal Entry: +{:#x}", note_data.normal_entry_offset);
  JIT_LOG(
      "    Static Entry: {}",
      note_data.static_entry_offset
          ? fmt::format("+{:#x}", *note_data.static_entry_offset)
          : "");

  // This could use std::piecewise_construct for better efficiency.
  auto [it, inserted] = funcs_.emplace(note.name, FuncState{});
  JIT_CHECK(inserted, "Duplicate ELF note for function '{}'", note.name);
  it->second.note = std::move(note_data);

  // Compute the compiled function's address after dynamic linking.
  void* address = dlsym(bundle_handle_, note.name.c_str());
  JIT_CHECK(
      address != nullptr,
      "Cannot find AOT-compiled function with name '{}' despite successfully "
      "loading the AOT bundle",
      note.name);
  it->second.compiled_code = {
      reinterpret_cast<const std::byte*>(address), it->second.note.size};
  JIT_LOG("    Address: {}", address);
}

const AotContext::FuncState* AotContext::lookupFuncState(
    BorrowedRef<PyFunctionObject> func) {
  std::string name = funcFullname(func);
  auto it = funcs_.find(name);
  return it != funcs_.end() ? &it->second : nullptr;
}
#endif

Context* getContext() {
  auto state = cinderx::getModuleState();
  if (state == nullptr) {
    return nullptr;
  }
  return static_cast<Context*>(state->jit_context.get());
}

} // namespace jit
