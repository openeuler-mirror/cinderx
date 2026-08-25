// Copyright (c) Meta Platforms, Inc. and affiliates.

// CPython 3.11 observe/shadow/execute compilation gate.  Observe terminates
// at a capability refusal. Shadow runs synchronously under the GIL through
// bytecode, HIR, optimization, LIR, register allocation, target codegen and
// relocation, then discards the artifact without publishing an entry point.
// Execute (the product auto-JIT, MR-11; "canary" is its test-only spelling)
// compiles, installs and runs machine code, hands fresh function objects the
// artifact their code already has, and records a failed automatic attempt
// on the code object so it is never scheduled again.

#include "cinderx/python.h"

#include "cinderx/Interpreter/3.11/eval_hook.h"

#if PY_VERSION_HEX < 0x030C0000

#include "internal/pycore_frame.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/code_extra.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/3.11/observe.h"
#include "cinderx/Jit/autojit_import.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/trigger_stats.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <string_view>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace {

const char* functionName(BorrowedRef<PyFunctionObject> func) {
  if (func == nullptr || func->func_qualname == nullptr) {
    return "<unknown>";
  }
  const char* name = PyUnicode_AsUTF8(func->func_qualname);
  return name != nullptr ? name : "<unknown>";
}

constexpr int kRequiredCodeFlags = CO_OPTIMIZED | CO_NEWLOCALS;

// Live state: answers "may machine code run right now".  The wrong
// question for choosing which PROTOCOL a request follows -- a pause makes
// it false and the execute protocol must handle a pause; use
// configuredExecuteMode() there.
bool executeMode() {
  return jit::getConfig().state == jit::State::kRunning;
}

// Which protocol this process runs under -- a configuration, not a state,
// so a pause does not change it.  disable() lowers the capability; it does
// not turn an execute-mode process into a shadow-mode one.
bool configuredExecuteMode() {
  return Ci_Observe311_Mode() == CI_JIT_MODE_311_EXECUTE;
}

bool unicodeEquals311(BorrowedRef<> value, const char* expected) {
  if (value == nullptr || !PyUnicode_Check(value)) {
    return false;
  }
  // ASCII compare: no UTF-8 conversion, so a lone surrogate cannot leave
  // a pending exception on a refusal path.
  return PyUnicode_EqualToUTF8(value, expected);
}

bool unicodeContainsAscii311(BorrowedRef<> value, const char* needle) {
  if (value == nullptr || !PyUnicode_Check(value)) {
    return false;
  }
  Ref<> needle_u = Ref<>::steal(PyUnicode_FromString(needle));
  if (needle_u == nullptr) {
    PyErr_Clear();
    return true;
  }
  int rc = PyUnicode_Contains(value, needle_u);
  if (rc < 0) {
    PyErr_Clear();
    return true;
  }
  return rc == 1;
}

// Mirrors pyjit.cpp: compiling importlib or cinderx on 3.11 is ineligible
// in getCompilationEligibility(), but the canary observe gate does not
// go through that function.  Once CALL is on the execute whitelist,
// those helpers compile and the next import dies in _ImportLockContext.
bool isCinderModule311(BorrowedRef<PyFunctionObject> func) {
  return unicodeEquals311(func->func_module, "cinderx");
}

bool isImportlibBootstrap311(BorrowedRef<PyFunctionObject> func) {
  if (unicodeEquals311(func->func_module, "_frozen_importlib") ||
      unicodeEquals311(func->func_module, "_frozen_importlib_external") ||
      unicodeEquals311(func->func_module, "importlib._bootstrap") ||
      unicodeEquals311(func->func_module, "importlib._bootstrap_external")) {
    return true;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  if (code == nullptr) {
    return false;
  }
  return unicodeContainsAscii311(code->co_filename, "importlib._bootstrap");
}

const char* eligibilityReason(BorrowedRef<PyFunctionObject> func) {
  if ((!jit::isJitShadow() && !executeMode()) ||
      cinderx::getModuleState() == nullptr ||
      cinderx::getModuleState()->jit_context == nullptr) {
    return CI_OBSERVE_311_REFUSAL;
  }
  if (func == nullptr || !PyFunction_Check(func)) {
    return "REFUSE_SHAPE_NON_FUNCTION_SCOPE";
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if ((code->co_flags & kRequiredCodeFlags) != kRequiredCodeFlags) {
    return "REFUSE_SHAPE_NON_FUNCTION_SCOPE";
  }
  if (code->co_flags & jit::kCoFlagsAsyncCode) {
    return "REFUSE_SHAPE_ASYNC_CODE";
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return "REFUSE_SHAPE_JIT_SUPPRESSED";
  }
  if (code->co_flags & CI_CO_STATICALLY_COMPILED) {
    return "REFUSE_SHAPE_STATIC_RUNTIME_CACHE";
  }
  if (!PyDict_CheckExact(func->func_globals) ||
      !PyDict_CheckExact(func->func_builtins)) {
    return "REFUSE_SHAPE_NAMESPACE_UNSUPPORTED";
  }
  if (isCinderModule311(func)) {
    return "REFUSE_SHAPE_CINDER_MODULE";
  }
  if (isImportlibBootstrap311(func)) {
    return "REFUSE_SHAPE_IMPORTLIB_BOOTSTRAP";
  }

  if (const char* reason = jit::hir::unsupportedShapeReason311(code)) {
    return reason;
  }
  return jit::hir::unsupportedOpcodeReason311(code);
}

} // namespace

namespace {

// A refusal reason produced inside the compiler, where the eligibility
// predicate cannot see it: the optimizer's own output is only knowable
// once the passes have run.  Thread-local because compilation is
// per-thread; consumed and cleared by the scheduling gate below.
thread_local const char* t_last_execute_refusal = nullptr;
thread_local int t_execute_refusal_opcode = -1;
thread_local int t_execute_refusal_offset = -1;

// Argument binding for defaults, keyword-only parameters and the
// variadic collectors is handled by the generated vectorcall prologue
// (JITRT_CallWithKeywordArgs / JITRT_CallWithIncorrectArgcount).  A
// body made of whitelisted opcodes is enough.
const char* unsupportedArgumentShape311(BorrowedRef<PyFunctionObject>) {
  return nullptr;
}

// One published artifact per code object: CodeExtra carries exactly one
// (artifact, globals, builtins) triple, so a second publication under a
// different namespace would overwrite the first owner's.  A second
// function over the SAME key is the fresh instance 3.11 creates per
// closure/lambda/comprehension; it attaches instead of compiling again,
// with membership kept honest by the function death watch.
const char* unsupportedSharedArtifact311(BorrowedRef<PyFunctionObject> func) {
  auto* ctx =
      static_cast<jit::Context*>(cinderx::getModuleState()->jit_context.get());
  if (ctx == nullptr) {
    return nullptr;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  CodeExtra* extra = codeExtraIfExists(code);
  auto* published = extra == nullptr
      ? nullptr
      : reinterpret_cast<jit::CompiledFunction*>(
            _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (published != nullptr && !published->functions().contains(func) &&
      (extra->jit_globals != func->func_globals ||
       extra->jit_builtins != func->func_builtins)) {
    // The artifact's member set is the ownership oracle, and it lives
    // entirely in C++: a deopt removes the installation but not the
    // membership, so a parked function passes this check on re-enable, and
    // nothing Python code can write -- the artifact reference in
    // func.__dict__ included -- can forge a positive.  A dictionary-based
    // check was tried here and is exactly backwards: that key is ordinary
    // writable function state, so copying it onto a second function forged
    // ownership and reopened the ledger overwrite this refusal exists to
    // prevent.
    return "REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED";
  }
  return nullptr;
}

// CPython 3.14's c_stack_soft_limit is a PyThreadStateImpl field this
// 3.11 tstate does not have.  Cache the same geometry per thread: usable
// base is the pthread stack address plus the guard region, then hard
// limit one margin above that base and soft limit two margins (stacks
// grow down).  Sanitizer builds use a 32 KiB margin, matching
// _PyOS_STACK_MARGIN_BYTES.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr std::uintptr_t kCStackMarginBytes311 = 32 * 1024;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr std::uintptr_t kCStackMarginBytes311 = 32 * 1024;
#else
constexpr std::uintptr_t kCStackMarginBytes311 = 16 * 1024;
#endif
#else
constexpr std::uintptr_t kCStackMarginBytes311 = 16 * 1024;
#endif

thread_local std::uintptr_t t_c_stack_soft_limit = 0;
thread_local std::uintptr_t t_c_stack_hard_limit = 0;
thread_local jit::CompiledFunction* t_invocation_artifact = nullptr;

class InvocationArtifactScope {
 public:
  explicit InvocationArtifactScope(jit::CompiledFunction* compiled)
      : prev_(t_invocation_artifact) {
    t_invocation_artifact = compiled;
  }
  ~InvocationArtifactScope() {
    t_invocation_artifact = prev_;
  }
  InvocationArtifactScope(const InvocationArtifactScope&) = delete;
  InvocationArtifactScope& operator=(const InvocationArtifactScope&) = delete;

 private:
  jit::CompiledFunction* prev_;
};

std::uintptr_t machineStackPointer311() {
  // pthread_getattr_np reports the OS thread stack.  A C local's address
  // is the ASAN fake stack and sits in a different mapping; using it
  // here trips the 16KiB hard margin on the first deep canary RuntimeTest
  // that actually enters machine code
  // (EnableKeepsParkedFunctionsAcrossFailures).
#if defined(__aarch64__)
  std::uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  return sp;
#elif defined(__x86_64__)
  std::uintptr_t sp;
  __asm__ volatile("movq %%rsp, %0" : "=r"(sp));
  return sp;
#else
  volatile char here;
  return reinterpret_cast<std::uintptr_t>(&here);
#endif
}

void initCStackLimits311() {
  if (t_c_stack_soft_limit != 0) {
    return;
  }
#if defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* stackaddr = nullptr;
    size_t stacksize = 0;
    size_t guard_size = 0;
    int err = pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    err |= pthread_attr_getguardsize(&attr, &guard_size);
    pthread_attr_destroy(&attr);
    if (err == 0 && stackaddr != nullptr) {
      auto raw = reinterpret_cast<std::uintptr_t>(stackaddr);
      auto base = raw + guard_size;
      auto top = raw + stacksize;
      if (top > base + 3 * kCStackMarginBytes311) {
        t_c_stack_hard_limit = base + kCStackMarginBytes311;
        t_c_stack_soft_limit = base + 2 * kCStackMarginBytes311;
        return;
      }
    }
  }
#endif
  std::uintptr_t here = machineStackPointer311();
  constexpr std::uintptr_t kFallbackBudget = 1024 * 1024;
  if (here > kFallbackBudget + 2 * kCStackMarginBytes311) {
    t_c_stack_soft_limit = here - kFallbackBudget;
    t_c_stack_hard_limit = t_c_stack_soft_limit - kCStackMarginBytes311;
  } else {
    t_c_stack_soft_limit = 1;
    t_c_stack_hard_limit = 1;
  }
}

bool cStackSoftLimitReached311() {
  initCStackLimits311();
  std::uintptr_t here = machineStackPointer311();
  if (here < t_c_stack_hard_limit) {
    Py_FatalError("Unrecoverable stack overflow");
  }
  return here < t_c_stack_soft_limit;
}

} // namespace

// The strict MR-04 execute surface: everything the shadow eligibility
// rejects, plus the argument shape, the artifact ownership rule and the
// machine-code whitelist scan.  Exported so the single compile-and-install
// choke point in compileFunction() enforces the same surface no matter
// which door a request came through.
extern "C" const char* Ci_JitShell311_ExecuteRefusal(
    PyFunctionObject* raw_func) {
  t_execute_refusal_opcode = -1;
  t_execute_refusal_offset = -1;
  BorrowedRef<PyFunctionObject> func{raw_func};
  const char* reason = eligibilityReason(func);
  if (reason != nullptr) {
    return reason;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  if ((code->co_flags & CO_GENERATOR) != 0 &&
      !jit::getConfig().sync_generator_jit) {
    return "REFUSE_SHAPE_GENERATOR_RUNTIME_UNAUDITED";
  }
  if (const char* arg_reason = unsupportedArgumentShape311(func)) {
    return arg_reason;
  }
  if (const char* share_reason = unsupportedSharedArtifact311(func)) {
    return share_reason;
  }
  jit::hir::ExecuteRefusal311 detail =
      jit::hir::unsupportedExecuteDetail311(code);
  t_execute_refusal_opcode = detail.opcode;
  t_execute_refusal_offset = detail.offset;
  return detail.reason;
}

extern "C" void Ci_JitShell311_GetExecuteRefusalDetail(
    int* opcode,
    int* offset) {
  if (opcode != nullptr) {
    *opcode = t_execute_refusal_opcode;
  }
  if (offset != nullptr) {
    *offset = t_execute_refusal_offset;
  }
}

extern "C" void Ci_JitShell311_SetExecuteRefusal(const char* reason) {
  t_last_execute_refusal = reason;
}

// Read and clear.  Every consumer takes the reason exactly once, so a
// refusal cannot be attributed to a later, unrelated compile.
extern "C" const char* Ci_JitShell311_TakeExecuteRefusal(void) {
  const char* reason = t_last_execute_refusal;
  t_last_execute_refusal = nullptr;
  return reason;
}

// The entry point installed on 3.11 canary functions.
//
// CPython 3.11 has no function watchers, so nothing tells the runtime when
// a function's code object changes after it was compiled: the compatibility
// shim's PyFunction_AddWatcher registers nothing.  Machine code that
// assumed the old code object would keep running against the new one.
// Defaults and keyword names are rebound on every call by the generated
// prologue, so they do not force a fallback here.
//
// So the entry re-checks, on every call, what compilation assumed, and
// hands anything else back to the interpreter.  The artifact is looked up
// through the current code object rather than captured, so a code swap
// routes to that code's own artifact or to the interpreter, never to a
// stale one.
// The function-state half of the entry's decision, shared with
// isJitCompiled() so "is this compiled?" and "will this call run machine
// code?" cannot answer differently.  Tracing and profiling are thread
// state, not function state: they fall back in the entry without clearing
// the artifact.
extern "C" void* Ci_JitShell311_InstalledArtifact(PyFunctionObject* func) {
  // Fail closed when the frame-evaluator entry point is no longer ours.
  // Initialization installed and verified it, but the slot can change
  // hands afterwards: remove_frame_evaluator() stays published for
  // testing, and another PEP 523 client can take the slot outright.
  // Without the evaluator, the interpreter's specialized CALL pushes
  // callee frames inline and never consults the vectorcall entry, so
  // answering "installed" here would call a function compiled for
  // exactly the calls that bypass its machine code.  Nothing is torn
  // down: reinstalling the evaluator makes the same published artifact
  // serve again.
  if (!Ci_EvalHook311_IsInstalled()) {
    return nullptr;
  }
  // Fail closed unless the JIT is usable right now: a publication in
  // flight can finish after a disable() and would install this entry
  // point behind its back.  The newer decision wins; enable() makes the
  // same published artifact serve again.
  if (!jit::isJitUsable()) {
    return nullptr;
  }
  // Fail closed under legacy tracing.  MR-04 implements no instrumentation,
  // and machine code delivers none of the call, line and return events a
  // registered trace or profile function is owed -- so while either is
  // active on this thread, nothing may run compiled.  The criterion is the
  // same one hasActiveLegacyTracing() applies on this branch; it is read
  // directly because the answer has to be per-call and per-thread.
  PyThreadState* tstate = PyThreadState_Get();
  if (tstate == nullptr || tstate->c_tracefunc != nullptr ||
      tstate->c_profilefunc != nullptr) {
    return nullptr;
  }
  // With membership persisting across a deopt, the ledger alone no longer
  // distinguishes installed from parked; the entry point does.  A parked
  // function's vectorcall is the interpreter's, so the honest answer to
  // "will this call run machine code" is null until re-enable installs the
  // guarded entry again.
  if (func->vectorcall !=
      reinterpret_cast<vectorcallfunc>(Ci_JitShell311_GuardedEntry)) {
    return nullptr;
  }
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  CodeExtra* extra = codeExtraIfExists(code);
  jit::CompiledFunction* compiled = extra == nullptr
      ? nullptr
      : reinterpret_cast<jit::CompiledFunction*>(
            _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (compiled == nullptr || extra->jit_globals != func->func_globals ||
      extra->jit_builtins != func->func_builtins ||
      // The artifact must be this function's own.
      !compiled->functions().contains(func)) {
    return nullptr;
  }
  return compiled;
}

extern "C" void* Ci_JitShell311_InvocationArtifact(void) {
  return t_invocation_artifact;
}

extern "C" PyObject* Ci_JitShell311_GuardedEntry(
    PyObject* func_obj,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  auto func = reinterpret_cast<PyFunctionObject*>(func_obj);
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      Ci_JitShell311_InstalledArtifact(func));
  PyThreadState* tstate = PyThreadState_GET();
  if (compiled == nullptr || tstate->c_tracefunc != nullptr ||
      tstate->c_profilefunc != nullptr) {
    return getInterpretedVectorcall(func)(func_obj, args, nargsf, kwnames);
  }
  // Keyword names and a mismatched positional count are the generated
  // prologue's job (JITRT_CallWithKeywordArgs /
  // JITRT_CallWithIncorrectArgcount).  Do not filter them here.
  // C-stack first so a soft-limit hit does not mutate recursion_remaining.
  // Py_EnterRecursiveCall waits until bind succeeds (body reentry), matching
  // CPython 3.11 initialize_locals then start_frame.  A bind TypeError at
  // recursion_remaining == 0 must stay TypeError, not RecursionError.
  if (cStackSoftLimitReached311()) {
    PyErr_SetString(PyExc_RecursionError, "maximum recursion depth exceeded");
    return nullptr;
  }
  // Pin the artifact for the duration of the call.  The body runs
  // arbitrary Python through operators and iteration, and that code can
  // drop the artifact's last reference -- clearing the function's __dict__
  // is a documented way to uncompile -- which would free the very code
  // buffer being executed.  The reference is held until machine code has
  // returned.  The same pin is the invocation snapshot for argument
  // binding: keyword equality can run user Python that enables tracing,
  // disables the JIT or swaps __code__, and that must not retarget this
  // call onto a different artifact or the interpreter's vectorcall layout.
  Ref<jit::CompiledFunction> pin{Ref<jit::CompiledFunction>::create(compiled)};
  InvocationArtifactScope invocation(compiled);
  return compiled->vectorcallEntry()(func_obj, args, nargsf, kwnames);
}

namespace {

// Is this refusal a property of the CODE OBJECT?  Only those are
// verdicts: a verdict is recorded permanently on the code object, so a
// refusal that actually describes the FUNCTION (a namespace twin, say)
// must not take the code down with it.  An allowlist, not a denylist --
// an unlisted reason costs a re-attempt at worst, where the opposite
// default silently disables a code object nobody meant to disable.
bool isCodeScopedRefusal311(const char* reason) {
  static constexpr const char* kCodeScoped[] = {
      // co_flags / co_filename / code shape -- true of the code wherever
      // it is used, and true again next time.
      "REFUSE_SHAPE_ASYNC_CODE",
      "REFUSE_SHAPE_JIT_SUPPRESSED",
      "REFUSE_SHAPE_STATIC_RUNTIME_CACHE",
      "REFUSE_SHAPE_IMPORTLIB_BOOTSTRAP",
      "REFUSE_SHAPE_NON_FUNCTION_SCOPE",
      "REFUSE_SHAPE_GENERATOR_AUTO_DISABLED",
      "REFUSE_SHAPE_GENERATOR_RUNTIME_UNAUDITED",
      "REFUSE_SHAPE_INVALID_UTF8_NAME",
      // The compiler could not produce an artifact for this code.
      "REFUSE_SHAPE_CODEGEN_SPAN",
      "REFUSE_SHAPE_SPECULATIVE_GUARD",
      "SUPPORTED_OPCODE_FAILURE",
  };
  for (const char* known : kCodeScoped) {
    if (std::strcmp(reason, known) == 0) {
      return true;
    }
  }
  return false;
}

// Does this code object already carry a published artifact?
//
// Attachability follows this, not the verdict text: a function refused
// because the artifact belongs to another namespace still leaves the
// artifact there for the namespace it does belong to.
bool codeHasArtifact311(BorrowedRef<PyCodeObject> code) {
  CodeExtra* extra = codeExtraIfExists(code);
  return extra != nullptr &&
      _Py_atomic_load_ptr_acquire(&extra->jit_compiled) != nullptr;
}

// The attempt was withheld because the function stopped holding the code
// it was counted for.  Distinct from every refusal, which describes a code
// object; this describes a function that changed underneath one.
constexpr const char* kCodeMoved311 = "__code_moved__";

// Defined below, next to the frame-entry attachment it also serves.
int attachFreshImpl311(PyFunctionObject* raw_func);

const char* requestExecute311(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyCodeObject> code) {
  if (codeAutoJitDisabled311(code)) {
    return "REFUSE_SHAPE_AUTO_JIT_DISABLED";
  }
  if ((code->co_flags & CO_GENERATOR) != 0) {
    // Generators compile on request only: the measured verdict is that
    // compiling them automatically loses to both compiling them all and
    // interpreting them all, so the auto default stays off (MR-11).
    return "REFUSE_SHAPE_GENERATOR_AUTO_DISABLED";
  }
  if (const char* execute_reason = Ci_JitShell311_ExecuteRefusal(func)) {
    return execute_reason;
  }
  if (isJitCompiled(func)) {
    return CI_JIT_RESULT_311_INSTALLED;
  }
  // Everything above reads Python objects and can therefore allocate,
  // collect and run a finalizer.  Compilation takes the function, not the
  // code, so a __code__ assignment landing in that stretch would compile
  // and install whatever the function holds now -- code that has not run
  // a single frame, from a threshold another code object earned.
  if (func->func_code != code) {
    return kCodeMoved311;
  }
  // An existing artifact makes this a fresh ATTACHMENT, not a compile,
  // and it must go through the same budget door the frame-entry
  // attachment uses; force_compile() keeps its own unbudgeted way in.
  int attached = attachFreshImpl311(func);
  if (attached > 0) {
    return CI_JIT_RESULT_311_INSTALLED;
  }
  if (attached < 0) {
    return "REFUSE_SHAPE_FRESH_ATTACH_BUDGET";
  }
  try {
    // The subject travels with the request: every boundary inside that
    // can run Python re-checks it, and a function that moved comes back
    // as CODE_MOVED rather than as a refusal about this code object.
    jit::Result result = jit::compileFunction(func, code);
    if (result == jit::Result::OK) {
      return CI_JIT_RESULT_311_INSTALLED;
    }
    PyErr_Clear();
    if (result == jit::Result::CODE_MOVED) {
      // Transient state of the FUNCTION, not a verdict about the code.
      // Routing it through the compile-failure reason below would spend
      // this code object's one automatic attempt on something it did not
      // do, and disable it for good.
      return kCodeMoved311;
    }
    if (const char* compiler_reason = Ci_JitShell311_TakeExecuteRefusal()) {
      return compiler_reason;
    }
    JIT_LOG(
        "execute compile returned {} for {}",
        static_cast<int>(result),
        functionName(func));
    return "SUPPORTED_OPCODE_FAILURE";
  } catch (const std::bad_alloc&) {
    PyErr_Clear();
    JIT_LOG("execute compile ran out of memory for {}", functionName(func));
    return "SUPPORTED_OPCODE_FAILURE";
  } catch (const std::exception& exc) {
    PyErr_Clear();
    JIT_LOG(
        "execute compile failed for {}: {}", functionName(func), exc.what());
    return "SUPPORTED_OPCODE_FAILURE";
  } catch (...) {
    PyErr_Clear();
    JIT_LOG(
        "execute compile failed for {}: unknown exception", functionName(func));
    return "SUPPORTED_OPCODE_FAILURE";
  }
}

} // namespace

extern "C" const char* Ci_JitShell311_RequestCompile(
    PyFunctionObject* raw_func,
    PyCodeObject* raw_expected_code) {
  BorrowedRef<PyFunctionObject> func{raw_func};
  BorrowedRef<PyCodeObject> expected_code{raw_expected_code};
  try {
    // Eligibility reads Python objects (co_names, flags, namespaces) and must
    // stay inside the same exception boundary as CompileShadow: a legal
    // CodeType can still raise from the C-API, and that must never escape
    // into the interpreted call.
    const char* reason = eligibilityReason(func);
    if (configuredExecuteMode()) {
      // Everything since the count allocated, so `__code__` may have
      // moved.  The attempt belongs to the code that earned it; a
      // function pointing elsewhere is not this attempt's subject.
      if (expected_code != nullptr && func->func_code != expected_code) {
        return CI_JIT_RESULT_311_DEFERRED;
      }
      // Ask before anything is spent: an allocation above can run a
      // finalizer that reaches the control plane, and that refusal says
      // nothing about this code object.
      if (Ci_JitShell311_DispatchDeferred()) {
        return CI_JIT_RESULT_311_DEFERRED;
      }
      // One attempt per code object; the verdict lives on the code so
      // every door sees it, and force_compile() stays available.  The
      // subject is the PINNED code, captured before the attempt: the
      // attempt runs arbitrary Python, a __code__ swap inside it must
      // neither be compiled on a threshold it did not earn nor recorded
      // as this code's verdict, and 3.11 has no watcher to notice.
      BorrowedRef<PyCodeObject> code{
          expected_code != nullptr
              ? expected_code
              : BorrowedRef<PyCodeObject>{func->func_code}};
      const char* verdict =
          reason != nullptr ? reason : requestExecute311(func, code);
      if (verdict == kCodeMoved311) {
        // The function moved mid-attempt.  Nothing was compiled and
        // nothing is judged: the code that earned the threshold keeps its
        // attempt, and the code that did not earn it gets no verdict.
        return CI_JIT_RESULT_311_DEFERRED;
      }
      if (std::strcmp(verdict, CI_JIT_RESULT_311_INSTALLED) != 0) {
        // The one site that writes the verdict.  A disable() landing
        // inside the attempt turns the answer into the capability
        // refusal, which says nothing about this code: report the attempt
        // withheld rather than making a reversible state permanent.
        if (Ci_JitShell311_DispatchDeferred()) {
          return CI_JIT_RESULT_311_DEFERRED;
        }
        if (isCodeScopedRefusal311(verdict)) {
          disableCodeAutoJit311(code);
        }
      }
      return verdict;
    }
    if (reason != nullptr) {
      return reason;
    }

    // Preloaders collect Python-object facts on this GIL-holding thread and
    // are destroyed before returning to the interpreter.
    jit::hir::IsolatedPreloaders isolated_preloaders;
    auto* context = static_cast<jit::CompilerContext<jit::Compiler>*>(
        cinderx::getModuleState()->jit_context.get());
    auto result = context->compiler().CompileShadow(func);
    if (!result.has_value()) {
      PyErr_Clear();
      JIT_LOG("shadow compile returned empty for {}", functionName(func));
      return "SUPPORTED_OPCODE_FAILURE";
    }
    jit::triggerStatsOnShadowCompile(
        result->code_size, result->specialized_opcodes);
    return "compiled";
  } catch (const std::exception& exc) {
    // Shadow compilation is observational: failures are reported through the
    // stable event reason and must never perturb the interpreted call.
    PyErr_Clear();
    JIT_LOG("shadow compile failed for {}: {}", functionName(func), exc.what());
    std::string_view what{exc.what()};
    if (what.find("RelocOffsetOutOfRange") != std::string_view::npos) {
      return "REFUSE_SHAPE_CODEGEN_SPAN";
    }
    if (what.find("REFUSE_SHAPE_INVALID_UTF8_NAME") != std::string_view::npos) {
      return "REFUSE_SHAPE_INVALID_UTF8_NAME";
    }
    return "SUPPORTED_OPCODE_FAILURE";
  } catch (...) {
    PyErr_Clear();
    JIT_LOG(
        "shadow compile failed for {}: unknown exception", functionName(func));
    return "SUPPORTED_OPCODE_FAILURE";
  }
}

extern "C" int Ci_JitShell311_CodeHasArtifact(PyCodeObject* code) {
  return code != nullptr && codeHasArtifact311(code) ? 1 : 0;
}

extern "C" int Ci_JitShell311_CodeAutoJitDisabled(PyCodeObject* code) {
  return code != nullptr && codeAutoJitDisabled311(code);
}

// The transient conditions shared by every scheduling door: under a trace
// or profile function a publication would immediately fall back and be
// reported as a failed attempt, and inside an import/setup scope nothing
// says the code is hot.  Neither says anything about the code object, so
// neither may spend an attempt or a budget.
static bool autoJitTransientHold311(void) {
  PyThreadState* tstate = PyThreadState_Get();
  if (tstate == nullptr || tstate->c_tracefunc != nullptr ||
      tstate->c_profilefunc != nullptr) {
    return true;
  }
  return jit::autoJitImportDepth() > 0 || jit::autoJitSetupDepth() > 0;
}

extern "C" int Ci_JitShell311_DispatchDeferred(void) {
  // Called only from the execute-mode dispatch, which gates on the
  // CONFIGURED mode.  This predicate must not gate on executeMode()
  // itself: that reads the live JIT state, which is exactly what a pause
  // changes -- asking it here would answer "nothing to defer" in the one
  // situation the deferral exists for.
  if (jit::isJitPaused() || !jit::isJitUsable()) {
    // A paused JIT refuses every request with the capability reason.
    // Dispatching into that would burn the code object's one attempt on a
    // state the next enable() undoes.
    return 1;
  }
  if (!Ci_EvalHook311_IsInstalled()) {
    // Without our evaluator nothing runs compiled, so an install would be
    // published and never entered.
    return 1;
  }
  return autoJitTransientHold311() ? 1 : 0;
}

namespace {

// Whether `needle` is a code object nested (up to `depth` levels down)
// inside `haystack`'s constants.  Identity only, no eligibility filter, no
// allocation: this runs on the scheduling path, once per hot nested code
// object and caller-chain entry.
bool codeContains311(
    BorrowedRef<PyCodeObject> haystack,
    BorrowedRef<PyCodeObject> needle,
    int depth) {
  BorrowedRef<> consts{haystack->co_consts};
  if (consts == nullptr || !PyTuple_CheckExact(consts)) {
    return false;
  }
  Py_ssize_t size = PyTuple_GET_SIZE(consts.get());
  for (Py_ssize_t i = 0; i < size; i++) {
    PyObject* item = PyTuple_GET_ITEM(consts.get(), i);
    if (!PyCode_Check(item)) {
      continue;
    }
    if (item == reinterpret_cast<PyObject*>(needle.get())) {
      return true;
    }
    if (depth > 1 &&
        codeContains311(
            reinterpret_cast<PyCodeObject*>(item), needle, depth - 1)) {
      return true;
    }
  }
  return false;
}

// A nested function is normally called by its outer or by something the
// outer called a few frames down; three levels cover a lambda inside a
// comprehension inside a function.
//
// This is a search, not an answer, and the distinction matters: 3.11 does
// not record which function object created another, so the anchor has to
// be recognised from the outside.  What the search cannot see is a nested
// function whose outer function is neither on the caller chain nor bound
// in the namespace -- a closure built by a factory, handed to a queue and
// called from an unrelated thread, say.  The consequence is bounded and
// benign: the instance that was compiled keeps its machine code, and the
// rest of the instances interpret.  Nothing is misattributed, because the
// anchor is only ever accepted when the candidate's own constants contain
// the code object.
//
// The exact answer is a lexical one -- stamp the creating function on the
// function object at MAKE_FUNCTION -- which means touching the vendored
// evaluator and the function layout, and is deliberately left as its own
// change rather than folded into the scheduler.
constexpr int kOuterWalkFrames = 8;
constexpr int kOuterWalkNesting = 3;

bool sameNamespace311(
    BorrowedRef<PyFunctionObject> candidate,
    BorrowedRef<PyFunctionObject> func) {
  return candidate->func_globals == func->func_globals &&
      candidate->func_builtins == func->func_builtins;
}

// The outer function of a nested code object, found where it normally
// lives: bound to a name in the module namespace, or a method in a class
// bound there.  A closure returned by a factory and called from elsewhere
// has no trace of its outer in the caller chain, but the factory itself is
// a module attribute (or a method) for as long as the module lives -- and
// that is exactly the lifetime the artifact should have.  The walk itself
// runs no Python, so the dictionaries cannot change underneath it; the
// result is returned OWNING, because what happens next (arming the death
// watch) allocates, and an allocation can collect and run a finalizer that
// drops the very binding this was found through.
Ref<PyFunctionObject> findOuterInNamespace311(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyCodeObject> code) {
  BorrowedRef<> globals{func->func_globals};
  if (globals == nullptr || !PyDict_CheckExact(globals)) {
    return {};
  }
  auto consider = [&](PyObject* value) -> BorrowedRef<PyFunctionObject> {
    if (value == nullptr) {
      return nullptr;
    }
    // A factory bound as a staticmethod or classmethod is the wrapper, not
    // the function: `C.factory` in the class dictionary is a descriptor
    // object.  Unwrap the two exact types -- their `__func__` is a plain
    // member, so reading it runs no Python and cannot disturb the walk.
    // (A subclass could override attribute access, so only exact types are
    // unwrapped; other wrappers -- functools.wraps and friends -- are not
    // reachable this way and rely on the caller chain.)
    Ref<> unwrapped;
    if (Py_TYPE(value) == &PyStaticMethod_Type ||
        Py_TYPE(value) == &PyClassMethod_Type) {
      unwrapped = Ref<>::steal(PyObject_GetAttrString(value, "__func__"));
      if (unwrapped == nullptr) {
        PyErr_Clear();
        return nullptr;
      }
      value = unwrapped.get();
    }
    if (!PyFunction_Check(value)) {
      return nullptr;
    }
    BorrowedRef<PyFunctionObject> candidate{value};
    BorrowedRef<PyCodeObject> candidate_code{candidate->func_code};
    if (candidate_code == nullptr || candidate_code == code ||
        !sameNamespace311(candidate, func) ||
        !codeContains311(candidate_code, code, kOuterWalkNesting)) {
      return nullptr;
    }
    return candidate;
  };
  Py_ssize_t pos = 0;
  PyObject* key = nullptr;
  PyObject* value = nullptr;
  while (PyDict_Next(globals, &pos, &key, &value)) {
    if (BorrowedRef<PyFunctionObject> outer = consider(value)) {
      return Ref<PyFunctionObject>::create(outer);
    }
    if (!PyType_Check(value)) {
      continue;
    }
    // Methods: one level into the class namespace, where `consider`
    // unwraps an exact staticmethod/classmethod binding.
    BorrowedRef<> type_dict{reinterpret_cast<PyTypeObject*>(value)->tp_dict};
    if (type_dict == nullptr || !PyDict_Check(type_dict)) {
      continue;
    }
    Py_ssize_t type_pos = 0;
    PyObject* member_key = nullptr;
    PyObject* member = nullptr;
    while (PyDict_Next(type_dict, &type_pos, &member_key, &member)) {
      if (BorrowedRef<PyFunctionObject> outer = consider(member)) {
        return Ref<PyFunctionObject>::create(outer);
      }
    }
  }
  return {};
}

} // namespace

// The outer-function walk proper; the exported entry point below is its
// exception boundary.  Registration allocates (the outer-function map, the
// nested-code walk's containers), and the caller is a C translation unit
// compiled without exception support: a std::bad_alloc crossing back into
// it would skip the observer's own cleanup and unwind into frames with no
// landing pads.  Losing the anchoring is survivable; escaping is not.
namespace {

void trackOuterFromFrameImpl311(
    PyFunctionObject* raw_func,
    _PyInterpreterFrame* frame) {
  // `frame` may be null (an explicit request without a frame to walk from):
  // the namespace lookup still runs, only the caller chain is skipped.
  if (!executeMode() || raw_func == nullptr || !PyFunction_Check(raw_func)) {
    return;
  }
  BorrowedRef<PyFunctionObject> func{raw_func};
  BorrowedRef<PyCodeObject> code{func->func_code};
  if ((code->co_flags & CO_NESTED) == 0) {
    // A top-level function's artifact is anchored by the function itself,
    // and a module keeps its functions alive; there is no outer to find.
    return;
  }
  // The namespace binding is the stable owner; the caller chain is the
  // fallback for an outer that is not bound anywhere (a nested factory, a
  // script's module body).  In the chain, prefer the outermost function
  // that contains the code: the intermediate levels (a comprehension
  // function, say) are themselves fresh per call and would anchor nothing
  // for long.
  // OWNING, and held until the death watch is armed: registration
  // allocates (a weak reference, its callback, the map rows), an
  // allocation can collect, and a finalizer can drop the binding this
  // outer was found through -- `del globals["factory"]` is enough.  The
  // rows recorded below are borrowed and only the watch makes them
  // honest, so the function must not be freed before the watch exists.
  Ref<PyFunctionObject> outer = findOuterInNamespace311(func, code);
  // The frame being entered is not linked yet: the evaluator sets
  // frame->previous when it starts running the frame, after this hook.
  // The caller chain therefore starts at the thread's current frame, and
  // the entering frame itself is skipped should it already be there.
  PyThreadState* tstate = PyThreadState_GET();
  _PyInterpreterFrame* caller =
      frame != nullptr && tstate != nullptr && tstate->cframe != nullptr
      ? tstate->cframe->current_frame
      : nullptr;
  if (caller != nullptr && caller == frame) {
    caller = caller->previous;
  }
  int walked = 0;
  for (; outer == nullptr && caller != nullptr && walked < kOuterWalkFrames;
       caller = caller->previous, walked++) {
    PyFunctionObject* candidate = caller->f_func;
    if (candidate == nullptr || !PyFunction_Check(candidate)) {
      continue;
    }
    BorrowedRef<PyFunctionObject> candidate_func{candidate};
    BorrowedRef<PyCodeObject> candidate_code{candidate->func_code};
    if (candidate_code == code || !sameNamespace311(candidate_func, func)) {
      // Recursion, or a namespace the artifact is not compiled for: the
      // publication would not anchor on it anyway.
      continue;
    }
    if (codeContains311(candidate_code, code, kOuterWalkNesting)) {
      // Keep walking: a containing caller further up is outer still.
      Ref<PyFunctionObject> outermost =
          Ref<PyFunctionObject>::create(candidate_func);
      for (_PyInterpreterFrame* above = caller->previous;
           above != nullptr && walked < kOuterWalkFrames;
           above = above->previous, walked++) {
        PyFunctionObject* higher = above->f_func;
        if (higher == nullptr || !PyFunction_Check(higher)) {
          continue;
        }
        BorrowedRef<PyFunctionObject> higher_func{higher};
        BorrowedRef<PyCodeObject> higher_code{higher->func_code};
        if (higher_code != code && sameNamespace311(higher_func, func) &&
            codeContains311(higher_code, code, kOuterWalkNesting)) {
          outermost = Ref<PyFunctionObject>::create(higher_func);
        }
      }
      outer = std::move(outermost);
      break;
    }
  }
  if (outer != nullptr) {
    jit::trackOuterFunction311(outer);
  }
}

} // namespace

extern "C" void Ci_JitShell311_TrackOuterFromFrame(
    PyFunctionObject* raw_func,
    _PyInterpreterFrame* frame) {
  try {
    trackOuterFromFrameImpl311(raw_func, frame);
  } catch (const std::exception& exc) {
    JIT_LOG("outer-function tracking failed: {}", exc.what());
  } catch (...) {
    JIT_LOG("outer-function tracking failed: unknown exception");
  }
}

// The attachment proper; the exported entry point below is its exception
// boundary (see trackOuterFromFrameImpl311 -- the eligibility scan and the
// publication both allocate, and the caller cannot catch).
namespace {

int attachFreshImpl311(PyFunctionObject* raw_func) {
  // The answer contract: 1 attached, 0 nothing this time, -1 never again
  // for this code object.  Everything before the publication is a cheap
  // read; the publication itself runs only for a function that will run
  // machine code afterwards.
  if (!executeMode() || raw_func == nullptr || !PyFunction_Check(raw_func)) {
    return 0;
  }
  if (jit::isJitPaused() || !jit::isJitUsable() ||
      !Ci_EvalHook311_IsInstalled()) {
    // A paused JIT re-attaches its parked members on enable(); a fresh
    // instance simply waits.  Without our evaluator nothing runs compiled.
    return 0;
  }
  BorrowedRef<PyFunctionObject> func{raw_func};
  auto code = reinterpret_cast<PyCodeObject*>(func->func_code);
  CodeExtra* extra = codeExtraIfExists(code);
  if (extra == nullptr) {
    return 0;
  }
  if (Ci_code_extra_jit311_auto_disabled(extra)) {
    return -1;
  }
  auto* compiled = reinterpret_cast<jit::CompiledFunction*>(
      _Py_atomic_load_ptr_acquire(&extra->jit_compiled));
  if (compiled == nullptr) {
    // Uncompiled, or the artifact died with its last anchor: nothing to
    // attach to (and nothing will be compiled again automatically).
    return 0;
  }
  if (compiled->functions().contains(func)) {
    // A member that fell back to the interpreter for this call (tracing,
    // a foreign evaluator in between) -- not a fresh instance.
    return 0;
  }
  if (extra->jit_globals != func->func_globals ||
      extra->jit_builtins != func->func_builtins) {
    // A namespace twin; the artifact is not compiled for it.
    return 0;
  }
  uint32_t budget = jit::getConfig().fresh_attach_budget;
  if (Ci_code_extra_jit311_attach_count(extra) >= budget) {
    return -1;
  }
  if (autoJitTransientHold311()) {
    // Fresh attachment is a scheduling door too, reached BEFORE the
    // threshold path; the same transient treatment applies, and nothing
    // is spent -- the next clean call attaches.
    return 0;
  }
  if (Ci_JitShell311_ExecuteRefusal(func) != nullptr) {
    return 0;
  }
  // Pin the artifact across the publication: finalizeFunc() allocates, an
  // allocation can collect, and a collection can drop the last anchor of
  // an artifact whose only members were fresh instances that are already
  // gone -- freeing the artifact mid-publication (the spike's use-after-
  // free).  The pin outlives every step below.
  Ref<jit::CompiledFunction> pin{Ref<jit::CompiledFunction>::create(compiled)};
  auto* ctx =
      static_cast<jit::Context*>(cinderx::getModuleState()->jit_context.get());
  if (ctx == nullptr) {
    return 0;
  }
  bool published = false;
  try {
    published = ctx->finalizeFunc(func, compiled);
  } catch (const std::exception& exc) {
    JIT_LOG(
        "fresh attachment failed for {}: {}", functionName(func), exc.what());
    published = false;
  } catch (...) {
    JIT_LOG(
        "fresh attachment failed for {}: unknown exception",
        functionName(func));
    published = false;
  }
  if (!published) {
    PyErr_Clear();
    return 0;
  }
  // finalizeFunc() reports a refusal as "nothing to do", so its answer
  // cannot distinguish a refusal from a publication; membership can, and
  // this function was not a member before the attempt.
  //
  // Membership, not the entry predicate.  The two say different things: a
  // disable() landing inside the publication -- finalizeFunc() allocates,
  // an allocation can collect, a finalizer can reach the control plane --
  // leaves the association standing while the entry predicate answers "not
  // runnable right now", and the next enable() makes this member run
  // machine code.  Charging the budget on runnability would let that
  // member in for free and lift the per-code cap by one for every
  // publication a disable() lands in.
  if (!compiled->functions().contains(func)) {
    return 0;
  }
  // The subject of an attachment is the code object whose artifact is
  // being attached and whose per-code budget pays for it.  Publication
  // allocates, an allocation can collect, and a finalizer can reassign
  // `__code__`, so the function may no longer be about this code at all
  // -- and `extra` is that code's block, so charging it here would spend
  // one of its lifetime attachments on a function that is not its
  // instance any more.  The entry point already answers by the current
  // code, so nothing runs wrong; the budget is what must not drift.
  if (reinterpret_cast<PyCodeObject*>(func->func_code) != code) {
    return 0;
  }
  Ci_code_extra_jit311_note_attach(extra);
  return 1;
}

} // namespace

extern "C" int Ci_JitShell311_AttachFresh(PyFunctionObject* raw_func) {
  try {
    return attachFreshImpl311(raw_func);
  } catch (const std::exception& exc) {
    PyErr_Clear();
    JIT_LOG("fresh attachment failed: {}", exc.what());
  } catch (...) {
    PyErr_Clear();
    JIT_LOG("fresh attachment failed: unknown exception");
  }
  // Nothing was attached, but the failure says nothing about the code
  // object: a later call may try again.
  return 0;
}

#endif // PY_VERSION_HEX < 0x030C0000
