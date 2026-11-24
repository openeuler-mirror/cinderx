# CPython 3.11 JIT-off entry and PGO coverage

The default evaluator entry skips JIT-only recursion, reclamation and observer
checks only after the configured mode is frozen to `off`, the compiler context
and code allocator are absent, and there are no active or retired JIT frames.
This qualification is revoked before compiler initialization (including test
force-initialization) and when the owning interpreter removes its evaluator.
Pausing an execute-mode JIT never qualifies it. Tracing, profiling, evaluator
ownership and the exact function/vectorcall checks remain dynamic for inline
calls. Normal argument binding and evaluator semantics are unchanged.

`CINDERX_INTERP_OFF_ENTRY=0` selects the existing guarded entry for an ablation;
`1` or an unset variable requests the qualified entry. The private
`_cinderx._interp_off_entry_enabled()` reports qualification and current evaluator
ownership. This is independent of `ATTR_PATH_MODE`. Observe/shadow/execute modes
retain their original entry and heat accounting.

The existing `cinderx/TestScripts/pgo_train_workloads.py` keeps all twelve JIT
execute workloads and their repetition counts. Its default plan adds six generic
interpreter workloads and one framework-independent attribute-protocol workload,
each in a fresh process with checked extension provenance and zero JIT activity.
It does not import pyperformance benchmark code. Build-only
`CINDERX_PGO_INTERPRETER_TRAINING=0` retains the twelve-phase training plan for
comparison; the default `1` selects nineteen phases. Training uses zero startup
grace so the existing immediate compilation assertions are deterministic.
Performance measurements must run in separate JIT-off processes.
