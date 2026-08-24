# CPython 3.11 A3 Multithread Shutdown Root Cause

Status: evidence dossier for MR-A3-04 (`cp311: finalize orphaned JIT artifacts
before context teardown`). The product fix is not part of this change; this
records the reproduction, the native stacks and the mechanism confirmation
that the fix must satisfy.

Blockers covered: `B8` (multithread compile lifetime) and `B9`
(finalize/shutdown crash). They are one root cause.

## Observed

- The `a3_shutdown` child that builds the `multithread-completed` state
  (register 8 functions, run `_jit311_multithreaded_compile_test()`, verify
  the batch, root everything to module teardown) prints its full readiness
  evidence — `READY_FOR_NORMAL_EXIT`, machine-code entries, a clean
  invariant report, `orphaned_compiled_codes: 2` — and then exits with
  SIGSEGV during `Py_FinalizeEx`.
- The crash is **deterministic per memory layout and flaky across
  layouts**. Byte-identical commands crash 20/20 under one environment
  block and exit 0 20/20 under another; the discovery gate's fixed
  environment sampled a passing layout and reported 100/100 clean while
  the crash was one environment away. This is why the first round
  classified B9 as "low probability".
- The other five shutdown states (`installed`, `parked`, `function-death`,
  `code-death`, `failure-unwind`) exit clean in every tested layout,
  including under freed-memory poisoning (5/5 each).

## Native stacks

Core dump 1 (release wheel at `62ecd1ac`, GC clear during finalize):

```text
#0 libc                          (inside free/deallocate)
#1 std::_Hashtable<jit::ThreadedRef<_object>, ...>::~_Hashtable()
#2 jit::CodeRuntime::releaseReferences()
#3 jit::CompiledFunction::clear(bool)
#4 jit::(anonymous namespace)::compiledfunc_clear(_object*)   ; tp_clear
#5..#8 libpython GC / module teardown internals
#9 Py_FinalizeEx
```

Core dump 2 (same state, different layout, GC traverse during finalize):

```text
#0 jit::CodeRuntime::traverse(int (*)(_object*, void*), void*)
#1..#5 libpython GC internals
```

Both entries dereference the artifact's `code_runtime_`.

## Confirmed use-after-free

glibc `MALLOC_PERTURB_=165` (freed memory poisoned on `free()`) converts
the surviving layout into a deterministic crash: the identical command
that exits 0 without poisoning crashes 3/3 with it. A read of freed heap
memory is the only behaviour that matches poison-sensitivity plus
layout-sensitivity plus the two stacks above.

## Mechanism

1. `clearForMultithreadedCompileTest()` detaches the batch artifacts from
   the borrowed registries and moves their pins into
   `orphaned_compiled_codes_` (the child's snapshot records 2 of them).
   The functions' dictionary anchors keep the same `CompiledFunction`
   Python objects alive independently, and the workload roots those
   functions to module teardown.
2. JIT finalize clears the artifacts it can still reach through the
   installed/associated registries. The orphaned artifacts are no longer
   in those registries, so nothing severs their `code_runtime_` pointers.
3. `~Context` destroys `code_runtimes_` (`SlabArena<CodeRuntime>` frees
   its slabs wholesale; a `CodeRuntime` lives exactly as long as its
   context).
4. Later inside `Py_FinalizeEx`, the GC reaches the still-alive
   `CompiledFunction` objects: `tp_traverse` enters
   `CodeRuntime::traverse()` and `tp_clear` enters
   `CompiledFunction::clear()` → `CodeRuntime::releaseReferences()`,
   both through the dangling pointer into the freed slab.

Whether the read faults is decided by what the allocator did with the
slab pages afterwards — hence deterministic-per-layout, flaky-across-layout.

## Fix contract (for MR-A3-04, not this change)

Any release that can run Python `DECREF`/finalizers, and any pointer the
GC can walk, must be severed while the context's teardown support is
intact: orphaned artifacts must be cleared (their `code_runtime_`
detached) before `~Context` destroys the `CodeRuntime` storage, exactly
as the finalize path already does for registry-reachable artifacts.
Forbidden shapes: leaking `orphaned_compiled_codes_`, skipping the
`CompiledFunction` destructor, `_exit()`, catching SIGSEGV, or shrinking
the shutdown gate.

## Reproduction

```bash
# Deterministic in any layout: poison freed memory.
export PYTHONPATH=<staged harness>
export CINDERX_JIT_MODE=canary PYTHONJITAUTO=1000000 PYTHONJITGENERATOR=1
export PYTHONJITMULTITHREADEDCOMPILETEST=1 PYTHONJITBATCHCOMPILEWORKERS=4
export MALLOC_PERTURB_=165
<venv python> -m ci_pipeline.jit311.a3_shutdown --child --state multithread-completed
# → SIGSEGV after READY_FOR_NORMAL_EXIT

# Gate form (per-state ledger, poisoning and layout entropy are defaults):
<venv python> -m ci_pipeline.jit311.a3_shutdown \
    --only-state multithread-completed --repetitions 1000 --out result.json
```

The A3-F lane records the per-state ledger, keeps the first cores and
extracts the gdb backtrace automatically; see `a3_shutdown.py` for the
detector defaults.
