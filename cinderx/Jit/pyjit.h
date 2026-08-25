// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit_result.h"

namespace jit {

/*
 * This defines the global public API for the JIT that is consumed by the
 * runtime.
 *
 * These methods assume that the GIL is held unless it is explicitly stated
 * otherwise.
 */

/*
 * Initialize any global state required by the JIT.
 *
 * This must be called before attempting to use the JIT.
 *
 * Returns 0 on success, -1 on error, or -2 if we just printed the JIT args.
 */
int initialize();

/*
 * Clean up any resources allocated by the JIT.
 *
 * This is intended to be called at interpreter shutdown in Py_Finalize().
 */
void finalize();

/*
 * Overwrite the entry point of a function so that it tries to JIT-compile
 * itself in the future.
 *
 * By default this will trigger the JIT the next time the function is called,
 * unless AutoJIT is enabled, in that case the function will compile after it is
 * called more times than the AutoJIT threshold.  Before that it will run
 * through the interpreter.
 *
 * Return true if the function was successfully scheduled for compilation, or if
 * it is already compiled.
 */
bool scheduleJitCompile(BorrowedRef<PyFunctionObject> func);

#if PY_VERSION_HEX < 0x030C0000
// CPython 3.11 execute mode: register `outer` as the outer function of the
// nested code objects in its constants, arming the death watch that keeps
// the registration honest.  A nested artifact compiled afterwards is
// anchored by `outer`'s __cinderx_nested_compiled_funcs__ list -- the
// residency 3.12+ gets from the function-creation watcher -- so it outlives
// the fresh function objects that come and go.
void trackOuterFunction311(BorrowedRef<PyFunctionObject> outer);
#endif

void recordDeoptForRoiBackoff(
    CodeRuntime* code_runtime,
    DeoptReason reason,
    bool is_instrumentation_deopt);

bool roiBackoffAllowsCompile(BorrowedRef<PyCodeObject> code);

/*
 * JIT compile func and patch its entry point.
 *
 * On success, positional only calls to func will use the JIT compiled version.
 */
// Compile `func`.
//
// `expected_code` fixes the subject: the compile refuses with CODE_MOVED
// the moment the function stops holding it, at every boundary that can
// run Python.  Pass nullptr to compile whatever the function holds, which
// pins it at entry instead -- the caller then has no opinion about which
// code object it asked for, but the compile still may not switch subjects
// halfway through.
Result compileFunction(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<PyCodeObject> expected_code = nullptr);

void uncompile(BorrowedRef<PyFunctionObject> func);

/*
 * Preload a function, along with any functions that it calls that we might want
 * to compile afterwards as well.  This is to support inlining and faster
 * invokes for Static Python functions.
 *
 * Setting the `forcePreload` will bypass the "might want to compile" logic and
 * force all the preloads to happen unconditionally.
 *
 * Return a list of preloaders that were created.  There should be at least one
 * preloader in the list, if it's empty then there was a preloading failure.
 */
std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func,
    bool forcePreload = false);

/*
 * Inform the JIT that a code, function, or type object is being modified or
 * destroyed.
 */
void codeDestroyed(BorrowedRef<PyCodeObject> code);
void funcDestroyed(BorrowedRef<PyFunctionObject> func);

/*
 * funcDestroyed() minus the death-notification counter: administrative
 * unpublication (force_uncompile of a live function) must not claim a
 * death.
 */
void funcUnpublished(BorrowedRef<PyFunctionObject> func);

/*
 * Context-explicit forms: a death belongs to the context whose watch
 * delivered it, never to whichever context the module currently holds.
 */
void funcUnpublishedInContext(Context* ctx, BorrowedRef<PyFunctionObject> func);
void funcDestroyedInContext(Context* ctx, BorrowedRef<PyFunctionObject> func);

/*
 * Record that a unit-deletion notification may have been lost (the record's
 * allocation failed inside a death callback, where nothing may throw).  The
 * batch-compile entry points consume the mark and fail conservatively: a
 * batch whose deleted-units view is incomplete must not compile.
 */
void poisonUnitDeletionTracking();

/*
 * Read and clear the poison mark.  The batch-compile entry points call this
 * before trusting their deleted-units view; fault-injection tests call it to
 * assert containment happened.
 */
bool consumeUnitDeletionTrackingPoison();

/*
 * Test-only: invoked between force_uncompile()'s unpublication and its
 * artifact retirement, so a native case can drop the last external
 * reference in the middle of the operation.
 */
void setUncompileMidpointHookForTest(void (*hook)());

/*
 * Test-only: drive the registration path (register a function for future
 * compilation without compiling it), which is otherwise reachable only
 * through entry points the canary does not publish.
 */
bool registerFunctionForTest(BorrowedRef<PyFunctionObject> func);
void funcModified(BorrowedRef<PyFunctionObject> func);
void typeDestroyed(BorrowedRef<PyTypeObject> type);
void typeModified(BorrowedRef<PyTypeObject> type);
void typeNameModified(BorrowedRef<PyTypeObject> type);

// Exposed for unit tests
Result compilePreloaderImpl(
    jit::CompilerContext<Compiler>* jit_ctx,
    const hir::Preloader& preloader,
    BorrowedRef<PyFunctionObject> func);

} // namespace jit
