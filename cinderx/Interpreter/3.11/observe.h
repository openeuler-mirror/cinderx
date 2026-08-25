// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// The reason every CPython 3.11 observe-mode compile request terminates with:
// the capability gate fires before any bytecode is read.  Shadow mode uses
// the same scheduling path but continues through the discard-only backend.
#define CI_OBSERVE_311_REFUSAL "CINDERX311_JIT_EXEC_DISABLED"

// Non-zero once a counting mode (observe, shadow or execute) is configured
// on.  Read directly on the frame entry so the JIT-off hot path pays one
// predictable flag test and nothing else.
extern int Ci_Observe311_Enabled;

// The CPython 3.11 runtime modes.  The state machine is
//   off     -> no counting, no scheduling, no compilation;
//   observe -> counting and scheduling, every request refused at the
//              capability gate (CI_OBSERVE_311_REFUSAL);
//   shadow  -> counting and scheduling, requests compile through the whole
//              backend and the artifact is discarded;
//   execute -> counting and scheduling, requests compile, install and run
//              machine code (the product auto-JIT, MR-11).
// "canary" is the test-only spelling of execute kept for the MR-04..MR-10
// gate legs: same machinery, same policy, reported under its own name.
typedef enum {
  CI_JIT_MODE_311_OFF = 0,
  CI_JIT_MODE_311_OBSERVE,
  CI_JIT_MODE_311_SHADOW,
  CI_JIT_MODE_311_EXECUTE,
} Ci_JitMode311;

// Resolve CINDERX_JIT_MODE into a mode without recording anything.  The
// spelling (for diagnostics) is returned through *spelling when non-NULL.
// PYTHONJITDISABLE / CINDERX_JIT_DISABLE turn execute into off: they are
// the product's "no machine code" switch and must win over the mode
// selector.  An unaccepted spelling sets an exception and returns -1.
int Ci_Observe311_ResolveMode(Ci_JitMode311* mode, const char** spelling);

// Report that a code object is being destroyed.  The pointer is a
// registry key arriving from code_dealloc and must never be dereferenced.
// (Why the observer has no weak reference: see observe.c's preamble.)
void Ci_Observe311_OnCodeDeath(PyCodeObject* code);

// The configured mode; CI_JIT_MODE_311_OFF before configuration.
Ci_JitMode311 Ci_Observe311_Mode(void);

// Parse CINDERX_JIT_MODE (off, observe, shadow, execute or canary), the
// PYTHONJITAUTO threshold and CINDERX_JIT_OBSERVE_FILE.  A successful parse
// is recorded once and kept; a failed one records nothing, sets an
// exception and returns -1, and a later call parses again.
int Ci_Observe311_Configure(void);

// Publish the final CPython 3.11 threshold resolved by the shared JIT flag
// processor. Execute/shadow mode calls this before installing the evaluator,
// so the frame scheduler consumes the same final value (including option
// ordering) rather than reparsing the environment independently.
void Ci_Observe311_SetResolvedAutoJitConfig(
    int configured,
    uint64_t threshold,
    int auto_classify,
    int valid);

// Frame-entry hot counting: one scheduling request per code object crossing
// the threshold, walked into Ci_JitShell311_RequestCompile with the real
// function object.  In execute mode a later frame of an already-dispatched
// code object is offered to Ci_JitShell311_AttachFresh, which hands a fresh
// function object the code's published artifact (3.11 has no
// function-creation watcher).  Never changes what the frame computes.
// `frame` is the frame being entered; its caller chain locates the outer
// function that anchors a nested code's artifact.
struct _PyInterpreterFrame;
//
// `code` is the code the frame is actually running, which the caller reads
// from the frame itself.  It is NOT always `func->func_code`: a frame
// holds its own strong reference to the code it was built for, and
// `function.__code__` can be reassigned afterwards while a suspended
// generator keeps resuming the frame it already has.  Passing it in keeps
// the decision with the only party that can see the frame layout.
void Ci_Observe311_OnFrame(
    PyFunctionObject* func,
    PyCodeObject* code,
    struct _PyInterpreterFrame* frame);

// Snapshot dict for tests and diagnostics: enabled, mode, requested_mode,
// threshold, codes_seen, events_dropped, fresh_attachments,
// post_publication_interpreted_frames,
// auto_jit_disabled_codes, and the bounded event list (qualname, filename,
// count, result, post_publication_interpreted_frames).
PyObject* Ci_Observe311_Stats(void);

// Allocation-free numeric census used by the private lifecycle snapshot.
void Ci_Observe311_GetLifecycleState(
    uint64_t* watched_codes,
    uint64_t* keyed_slots,
    uint64_t* table_capacity,
    uint64_t* events,
    uint64_t* post_publication_interpreted_frames);

// Read the scheduler slot for one live code object without creating it.
// Returns 1 when a slot exists, 0 otherwise. Output pointers are optional.
int Ci_Observe311_GetCodeState(
    PyCodeObject* code,
    uint64_t* count,
    int* dispatched,
    int* attachable);

// Release all observer-owned weakrefs, event references, tables and files.
// This also resets configuration so a later interpreter can configure anew.
void Ci_Observe311_Finalize(void);

// The JIT shell's unified compile entry point.  Observe mode returns a stable
// capability refusal; shadow mode synchronously compiles and discards the
// artifact without installing an entry point; execute mode compiles,
// installs and reports CI_JIT_RESULT_311_INSTALLED.  Every other result is a
// registered refusal reason or SUPPORTED_OPCODE_FAILURE, and in execute
// mode it disables automatic compilation of the code object for good.
#define CI_JIT_RESULT_311_INSTALLED "installed"

// The attempt was withheld, not made: the JIT stopped being usable inside
// the attempt, which describes the process, not the code object.
#define CI_JIT_RESULT_311_DEFERRED "deferred"
//
// In execute mode this is the code object's one automatic attempt; any
// verdict other than INSTALLED is final and recorded on the code object.
// `expected_code` is the code the scheduler counted: the request refuses
// with DEFERRED if the function no longer holds it (the attempt belongs
// to the code that earned the threshold).  NULL for explicit requests.
const char* Ci_JitShell311_RequestCompile(
    PyFunctionObject* func,
    PyCodeObject* expected_code);

// Whether automatic compilation has been disabled for this code object
// (its one scheduling attempt failed or was refused).  C-callable view of
// the code-extra verdict bit.
int Ci_JitShell311_CodeAutoJitDisabled(PyCodeObject* code);

// The refusal that says "this code HAS an artifact, but it is not this
// function's".  It describes the function's namespace, not the code, so
// it leaves the code's own automatic attempt unspent and leaves the
// artifact available to the instances that do share its namespace.
#define CI_JIT_RESULT_311_PUBLISHED_ELSEWHERE \
  "REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED"

// Whether this code object already carries a published artifact.
//
// Later function objects over the same code are offered for attachment on
// this fact rather than on what the dispatch's verdict said: a refusal
// that describes the FUNCTION -- a namespace twin, say -- leaves the
// artifact in place for the instances it does belong to.
int Ci_JitShell311_CodeHasArtifact(PyCodeObject* code);

// Non-zero when a scheduling request would fail right now for a reason
// that says nothing about the code object: a trace or profile function is
// active on this thread, the JIT is paused, or the frame-evaluator entry
// point is not ours.  The one-attempt rule burns a code object's only
// chance, so the dispatch waits for such a condition to pass instead of
// spending it -- and a function that only ever runs under one (a workload
// that lives entirely inside a coverage tracer) simply stays interpreted.
int Ci_JitShell311_DispatchDeferred(void);

// Execute mode: attach a fresh function object to the artifact already
// published for its code object, within the per-code attachment budget.
// Returns 1 when the function now runs machine code, 0 when nothing was
// done this time (no artifact, a different namespace, the JIT paused,
// tracing active, or the function is already a member), and -1 when the
// code object can never attach again (budget exhausted or automatic
// compilation disabled), so the caller stops asking.
int Ci_JitShell311_AttachFresh(PyFunctionObject* func);

// Execute mode: before a nested code object is scheduled, find the outer
// function whose code contains it in the caller chain of `frame` and
// register it as the code's outer function.  A nested artifact is then
// anchored by that outer function -- the same residency CPython 3.12+
// establishes through the function-creation watcher -- so it outlives the
// fresh function objects that come and go and later instances can attach.
void Ci_JitShell311_TrackOuterFromFrame(
    PyFunctionObject* func,
    struct _PyInterpreterFrame* frame);

// The MR-04 execute-surface predicate: the registered refusal reason for
// this function, or NULL when machine code may be installed for it.  The
// compile choke point, the eligibility report and the canary control plane
// all ask this one predicate, so a refusal is reported with the same reason
// wherever it surfaces.
const char* Ci_JitShell311_ExecuteRefusal(PyFunctionObject* func);

// Detail for the immediately preceding ExecuteRefusal call on this thread.
// opcode/offset are -1 unless the reason is an ordinary execute-whitelist
// refusal with an exact first rejected instruction.
void Ci_JitShell311_GetExecuteRefusalDetail(int* opcode, int* offset);

// The artifact currently installed for this function, or NULL.  "Installed"
// means the next call runs machine code: the code object, globals, builtins,
// argument shape and artifact ownership all still match what was compiled.
// Returned as void* because the artifact type is C++; callers in the JIT
// cast it back to jit::CompiledFunction*.
void* Ci_JitShell311_InstalledArtifact(PyFunctionObject* func);

// The artifact pinned for the in-flight GuardedEntry invocation, or NULL.
// Keyword binding can run arbitrary Python, so helpers must not re-query
// InstalledArtifact after they have started; this snapshot is the code
// that this call already committed to.
void* Ci_JitShell311_InvocationArtifact(void);

// The vectorcall entry installed on a function whose artifact may execute.
// It re-checks the predicate above on every call and falls back to the
// interpreter entry when anything has moved, so a function that leaves the
// execute surface stops running machine code without needing a watcher --
// 3.11 has none.
PyObject* Ci_JitShell311_GuardedEntry(
    PyObject* func,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

#ifdef __cplusplus
}
#endif
