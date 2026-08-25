# CPython 3.11 interpreter

This directory holds the CPython 3.11.6 evaluator that CinderX runs Python
frames on. It is built into `_cinderx` as part of the main build and takes
over frame evaluation through PEP 523 when `install_frame_evaluator()` is
called. Nothing in this directory executes machine code: the JIT's 3.11
runtime modes (`observe.c`) hang off the frame entry, and compiled code is
entered only through a function's vectorcall entry point.

The vendored files under `upstream/` and the interpreter-local Borrow output
are derived from the `Python-3.11.6` tree produced by rpmbuild of
`python3-3.11.6-34.oe2403sp3.src.rpm`. Their pinned digests are recorded in
`upstream/SHA256SUMS`; source-consistency enforcement belongs in a future CI
gate.

## Deviations from the pristine sources

`upstream/` is never edited. Every difference from the interpreter CPython
would otherwise run is expressed in a wrapper translation unit and listed
here.

| Deviation | Where | What | Why |
|---|---|---|---|
| Evaluator entry point renamed | `ceval_wrapper.c` | `_PyEval_EvalFrameDefault` → `Ci_EvalFrameDefault_311` | Keeps the vendored loop distinct from libpython's own entry point, including its recursive self-calls |
| Frame symbols renamed | `ceval_wrapper.c`, `frame_wrapper.c` | `_PyFrame_*` → `Ci_PyFrame_*_311` | The vendored frame implementation must not collide with the real-name copy the Borrow library provides for pycore_frame.h's inline helpers |
| Dict version stream | `ceval_wrapper.c` | `DICT_NEXT_VERSION()` → `Cix_PyDict_NextVersion()` | libpython does not export `_pydict_global_version`; the Borrow copy draws from the real stream instead of keeping a second counter |
| Version allocators | `specialize_wrapper.c` | Keys and function versions come from the top half of the 32-bit range | The stock allocators are file-static and unexported. Top-half numbers stay clear of stock's stream in any real process (2^31 first-time allocations away) and deliberately decline the specializations whose caches hold a version in 16 bits (`BINARY_SUBSCR_GETITEM`, `LOAD_GLOBAL_BUILTIN`) -- fail-closed; wraparound reports 0 and declines as well |
| Trusted evaluator for specialization | `specialize_wrapper.c` | CALL and BINARY_SUBSCR specialization proceeds while our evaluator is installed; at execution the vendored loop's own eval-frame checks still deopt those specialized forms to the generic path | Upstream declines under any PEP 523 hook because it cannot know what the hook does. Ours is the hash-locked stock loop, and the caches stay valid under it because the specialized Python-callee forms never run inline here: the handler's own check deopts them to the generic CALL, which reaches a callee through its vectorcall entry -- the JIT's entry when the callee is compiled (execute mode), the interpreter's otherwise. Any other evaluator keeps upstream's refusal. Specialized state thereby matches stock-without-a-hook for introspection, while executing the inlined fast paths under the hook is execution-side trust that belongs to the performance milestone |
| Private thin helpers forwarded | `ceval_shims.c` | Private wrappers forwarded to their public protocol or type-slot equivalents | Their upstream implementations are thin calls into file-static machinery the target does not export |
| DTrace probes disabled | `ceval_wrapper.c` | `#undef WITH_DTRACE` | The devel package defines `WITH_DTRACE` but ships no generated `pydtrace_probes.h`; CinderX's own USDT instrumentation is unaffected |

## Standalone build

The directory also builds on its own, without the rest of CinderX, which is
useful when working on the evaluator in isolation:

```sh
cmake -S cinderx/Interpreter/3.11 -B build/interpreter-311 \
  -DPython_EXECUTABLE=/usr/bin/python3.11
cmake --build build/interpreter-311 --target \
  cinderx_cpython311_interpreter -j
```

That target leaves the integration-time dependencies declared in
`interpreter_dependencies.h` unresolved; the main build supplies them from the
Borrow library.
