# CPython 3.11 Lifecycle Product Execution Report

The lifecycle fix round: product repairs for
the discovery blockers B7-EXEC, B7-RUNTIME and B8/B9, verified by the full
product-lifecycle rerun, the execution and runtime-transition
regressions and the strengthened shutdown
gate.  Covers the per-MR reports the task book names
(the multithread-shutdown fix, executable reclamation, CodeRuntime
recycling and shutdown-acceptance reports).

Fix head: `b16afbb7` (wheel `73f23e77…`, embedded SHA matches).
Environment: openEuler dev container, CPython 3.11.6, aarch64.

## Verdict

```text
Lifecycle round:  DISCOVERY_PASS  (C1-C8, ownership and shutdown lanes PASS, 0 blockers)
Execution regression:  PASS_WITH_APPROVED_DEVIATIONS  (register unchanged)
Runtime-transition regression:  PASS_WITH_APPROVED_DEVIATIONS  (register unchanged)
Final blocker list: no remaining product lifecycle blocker.
```

## The four product changes

1. **`cp311: finalize orphaned JIT artifacts before context
   teardown`** — B8/B9.  Orphans from `clearForMultithreadedCompileTest()`
   left every registry, so `~Context`'s severing walks never reached them;
   a function-dictionary anchor kept them alive past the context and the
   exit GC walked `tp_traverse`/`tp_clear` into freed slab storage.  The
   destructor now severs orphans in the same window as every other
   artifact.  Root cause dossier:
   `cp311-lifecycle-multithread-shutdown-root-cause.md` (core+gdb stacks on both
   the `CodeRuntime::releaseReferences` and `CodeRuntime::traverse` arms;
   `MALLOC_PERTURB_` poisoning turned the surviving layout into a 3/3
   deterministic fault).
2. **`cp311: reclaim retired JIT executable code`** — B7-EXEC.
   CPython 3.11 now routes to the asmjit-backed reclaiming allocator at
   configuration time (the configuration always reads the effective
   policy; an explicit `PYTHONJITHUGEPAGES` request is answered with a log
   line and a `false` reading).  Accounting is span-symmetric on both
   sides and `~CompiledFunction` fails closed on a refused release.
3. **`cp311: recycle retired CodeRuntime storage`** —
   B7-RUNTIME.  `SlabArena` reuses cleared husks through a free list;
   `Context::recycleCodeRuntime()` purges the one address-keyed side table
   (`deopt_stats_`) first.  The identity audit
   (`cp311-lifecycle-coderuntime-identity-audit.md`) surfaced that the leak had
   been load-bearing, so two ownership repairs land with it: the
   generator footer pins its artifact for the generator's lifetime, and
   registry retirement (`retire()`) leaves the runtime whole for pins —
   gutting and hand-back happen only at destruction/GC/finalization.
4. **`cp311: sever a survivor artifact's runtime after its context dies`**
   — caught by the runtime-transition threshold-50 regression arm, not by the churn
   matrix: a *retired* artifact anchored in a module cycle outlives the
   context and the exit GC walked its runtime pointer into freed slabs
   (2/72 stdlib modules crashed in one memory layout; 51/72 with
   poisoning armed).  The GC hooks now prove the storage is alive before
   touching it (`runtimeStorageAlive()`: a live owner vouches; an
   ownerless artifact asks the module context, which answers only for
   slots it owns).  After the fix the same 72-module arm passes 72/72
   *with poisoning still armed*.

## Product lifecycle rerun (wheel-first)

C lane — every scenario PASS at full scale, machine-entry proven, all
live-ownership gauges at baseline (empty `gauge_drift` everywhere):

| Scenario | entries | `code_runtimes_allocated` | `code_allocator_used_bytes` |
|---|---:|---|---|
| C1 ×1000 | 1 001 | 1 → 1 (was 101 → 1 001) | 320 → 320 (was 361 464 → 3 579 864) |
| C2 ×10000 | 10 010 | 5 → 5 (was 1 006 → 10 006) | 11 136 → 11 136 (was 3.59 MB → 35.78 MB) |
| C3 ×1000 | 1 002 | plateau | plateau |
| C4 ×1000 (pop 4) | 6 001 | 4 → 4 | 320 → 320 |
| C5 ×1000 | 1 004 | plateau | plateau |
| C6 ×1000 ×4 modes | 10 001 | plateau | plateau |
| C7 ×1000 (fault inj.) | 1 001 | 1 → 1 (was 201 → 2 001) | plateau |
| C8 ×100 (batch 8) | 801 | 8 → 8 (was 81 → 801) | plateau |

Ownership lane: PASS (three refcount-matrix corpora, interp vs jit
differential).

Shutdown gate (strengthened: per-state quotas, `MALLOC_PERTURB_`, layout
entropy, core capture): **3000/3000 clean exits** —
`installed/parked/function-death/code-death/failure-unwind` 200/200 each,
`multithread-completed` **2000/2000** (0 SIGSEGV / 0 SIGABRT / 0 timeout /
0 forbidden stderr).  Before the orphan-finalize fix the same gate recorded 2000/2000
SIGSEGV on that state.

## Regressions on the fix head

- Execution (S/C/W): PASS_WITH_APPROVED_DEVIATIONS — compile-all over the
  72-module surface (5 296 functions discovered, 0 unexpected refusals,
  0 unexpected differences, deviation register unchanged).
- Runtime transition (P/T/R): PASS_WITH_APPROVED_DEVIATIONS — the penetration coverage
  counts are byte-identical to the frozen baseline (35/37 control gap,
  7/65 threshold-1 gap, `test_descr`/`test_dis` fails unchanged), and the
  control arm's crash column is empty again.
- rt311 census: the registered population **minus the DeoptStressTest
  family** runs to completion on the final tree; every failure sits in the
  historically red capability families, none in the lifecycle/allocator
  surface or the new tests.  The DeoptStress exclusion is an inherited
  runtime-transition-chain condition, not this round's: on the pre-round frozen tree
  (`0ebc04c2`) the same tests already grind past 300 s at >1.4 GB RSS
  (this round's control build), where the last daily census recorded them
  as 3 ms fast-fails — that chain's compile-surface widening made them
  runnable and pathological, and the full census now exceeds this
  machine's 15.65 GiB VM.  Flagged for the daily-gate owners.
- Green families / canary RuntimeTests (dev build): 225/225 and full
  lifecycle+allocator suites green, including the four new tests
  (`OrphanedArtifactOutlivesContextTeardown`,
  `PinnedRetirementKeepsTheRuntimeWhole`,
  `RetiredCodeRuntimeStorageIsRecycled`, the two allocator reclaim
  tests) and the Python-level
  `test_suspended_generator_pins_its_artifact`.

## Sanitizer battery

- S1 ASAN: PASS — green families (225) and the mode-gated canary population (117, including every new lifecycle/allocator test) run clean under the instrumented RuntimeTests; the instrumented `_cinderx.so` (42 `__asan` symbols, libasan NEEDED verified) passes the canary execution smoke (1 023 machine-code entries) and the quick churn subset C1/C2/C5/C7/C8. 0 UAF / heap-buffer-overflow / double-free / invalid-free / sanitizer crashes.
- S2 LSan: off by the evidence contract of the ASAN leg
  (`detect_leaks=0`): CPython's own finalization leaks are upstream
  noise, and gtest's discovery step trips LSan's ptrace detection.
  Addressability, use-after-free and double-free remain the gate.
  Recorded per the task book as unavailable-with-reason rather than
  silently disabled.
- S3 refleak: PASS — the Py_DEBUG 3.11.6 arm (`run_refleak_311.sh`, product startup path with per-process attestation) reports `-R 3:3 over 10 modules, no leaks`; 1 004 436 machine-code entries inside the -R run itself, and the only block-figure movement was verified as the known quickened-counter artifact.  Note: the leg builds CinderX from the source tree and needs a writable checkout.
- S4 native RuntimeTests lifecycle canary: green (see census / green
  gates above).
- S5 sanitizer shutdown repetition: PASS — 55/55 clean exits under the instrumented extension (multithread-completed ×30, the other five states ×5 each), forbidden-stderr scan (including the AddressSanitizer token) empty.

## Known residuals (documented, non-blocking)

- Shadow-mode compiles strand their codegen-time CodeRuntime husks (no
  artifact exists to hand them back); shadow is a diagnostics mode
  outside the execute surface (identity audit, "load-bearing leak").
- `Context::deopt_metadata_` is a dead declaration; candidate removal.
- A private test context's storage is never adopted by the module
  fallback; artifacts outliving such a context sever untouched, and the
  arena's own destruction releases the husks.
