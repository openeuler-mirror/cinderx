# CPython 3.11 JIT Lifecycle Inventory

This is the source inventory for the lifecycle discovery. It describes the state before any product-lifecycle fix. The runtime-transition domain is frozen at `0ebc04c2` and its runner is not reused as the lifecycle implementation surface.

## Ownership structures

1. Borrowed `PyFunctionObject*` registries:
   - `Context::compiled_funcs_`: installed function to borrowed `CompiledFunction`.
   - `Context::associated_funcs_` (3.11): authoritative function-to-artifact claim, including parked and code-swapped functions.
   - `Context::deopted_funcs_`: parked functions.
   - `CompiledFunction::functions_`: reverse borrowed member set.
   - `ModuleState::registered_compilation_units`: borrowed function/code units awaiting compilation.
   - `ModuleState::perf_trampoline_worklist`: borrowed functions.
   - `Context::code_outer_funcs_`: borrowed code-to-outer-function relation.

2. Borrowed-pointer lifetime protection:
   - On 3.11, installed, associated, parked and registered functions must first be represented in `func_death_watch_`; the externally rooted weak-reference callback enters `funcDestroyedInContext()` and removes every borrowed record.
   - `compiled_codes_` keys are borrowed code/globals/builtins identities kept alive by the corresponding `CodeRuntime`; its values are borrowed artifacts whose owning references live in function dictionaries, dedup/orphan storage, or temporary teardown pins.
   - Code death arrives through the CinderX code-extra free callback because CPython 3.11 has no code watcher.
   - Perf worklist cleanup is routed through the function-destroy notification path when prefork compilation is active.

3. Strong owners in lifecycle structures:
   - Function dictionaries hold `CompiledFunction` anchors.
   - `func_death_watch_` strongly owns weak-reference objects, not their referents.
   - `deferred_anchor_releases_`, `orphaned_compiled_codes_`, dedup donor entries, `references_`, completed-compile `ThreadedRef`s and deferred-finalization `ThreadedRef`s own their payloads.
   - `ModuleState` owns the context, code allocator, generator allocator/types, caches and control-plane module references.

4. `CompiledFunction` ownership:
   - Normal installed ownership is the function dictionary anchor.
   - A single artifact may serve multiple borrowed function members.
   - Dedup donors, multithread teardown orphan storage, external Python references and temporary teardown vectors may pin it.
   - Context maps are identity indices by borrow; `CompiledFunction::owner()` identifies the live owning context and is nulled on retirement.

5. `CodeRuntime` ownership:
   - Allocated from `Context::code_runtimes_` and lives until context destruction.
   - It strongly owns the code/globals/builtins objects required by compiled code.
   - `isCleared()` distinguishes live runtime state from arena high-water entries.

6. Executable code-buffer ownership:
   - `ModuleState::code_allocator` owns executable mappings.
   - A live `CompiledFunction` owns its allocated code span; destruction releases it through the allocator.
   - `resident_code_buffers` is the physical live-buffer gauge; allocator `usedBytes()` is a byte gauge, while arena/chunk capacity is high-water state.

## Death and cleanup paths

7. Function death:
   - The 3.11 weak death-watch callback calls `funcDestroyedInContext()`.
   - It increments `function_destroyed_notifications`, unregisters function/code units, calls `Context::funcDestroyed()`, removes entry caches, associations, members, parked/installed records and watches.

8. Code death:
   - The code-extra free function enters `codeDestroyed()` and `Ci_Observe311_OnCodeDeath()`.
   - It increments `code_destroyed_notifications`, erases registered units and `code_outer_funcs_`, then marks the observer slot dead without dereferencing the code address.

9. Observer tombstones:
   - A dead slot retains only its key so open-addressed probe chains remain valid.
   - Reuse of the same address resets count, dispatch, attach, publication, event index and post-publication evidence.
   - Dead tombstones are swept when table growth rehashes; `watched_codes` is live population, `keyed_slots` and `table_capacity` are high-water/capacity evidence.

10. Generator JIT data:
    - `GenDataFooter` is stored with the generator's heap suspend data and points to the generator and `CodeRuntime` by borrow.
    - The generator object/frame owns Python live values; JIT traversal visits owned spill values while suspended.
    - Deallocation clears/deopts footer state and returns memory through `jit_gen_free_list` or `PyObject_GC_Del`.
    - CPython 3.11 deliberately bypasses the fixed freelist and currently has no allocation-free live native gauge. The lifecycle gate reports `GENERATOR_NATIVE_GAUGE_NOT_AVAILABLE` and uses weakref/GC census plus registry/residency evidence.

11. Parked/deopted cleanup:
    - `disable()` removes installed entries and records surviving associations in `deopted_funcs_`.
    - The death watch erases a function that dies while parked.
    - `enable()` snapshots/revalidates survivors and reattaches only live, still-associated functions.

12. Deferred anchors:
    - Publication/reassociation parks displaced dictionary anchors in `deferred_anchor_releases_` so arbitrary Python cannot run inside a registry transaction.
    - `drainDeferredAnchorReleases()` drains one item at a time at explicit control-plane boundaries and re-reads the queue after reentry.

13. Multithread compile state:
    - `active_compiles_` owns compilation keys for work in flight.
    - `completed_compiles_` owns finished data and a `ThreadedRef` to the function until finalization.
    - `deferred_finalizations_` holds functions whose existing artifact must be installed on the main thread.
    - `finalizeMultiThreadedCompile()` consumes these; `clearForMultithreadedCompileTest()` pins artifacts, detaches borrowed registries, clears caches and moves pins to orphan storage before releasing references.

14. Finalize order (3.11):
    - Enter one-way `State::kFinalizing`; reject reentrant enable.
    - Drain deferred anchors, poison/nil the death-watch owner token and clear code-extra artifact caches.
    - Strongly pin every artifact, restore interpreted entries, sever artifact member sets, and empty installed/associated/parked borrowed registries.
    - Clear artifacts under pins, then clear death watches.
    - Clear deopt stats, Context references, registered compilation units, code-outer relations, context/allocator/module hooks and observer state.
    - Repeated finalize is idempotent; production JIT is not reinitialized for testing.

15. Existing lifecycle RuntimeTest anchors include:
    - `FunctionDeathIsReported`, `FunctionDeathInsideACycleIsReported`, `ParkedFunctionDeathUnparksIt`.
    - `EnableKeepsParkedFunctionsAcrossFailures`, reentrant publication/enable tests and last-reference uncompile tests.
    - `PublicationUnwindsOnAllocationFailure`, `StaleArtifactDeathPreservesSameKeySuccessor`.
    - `FinalizeRefusesReentrantEnable`, `FinalizeIsRepeatable`, installed/parked registry cleanup and `MultithreadedTeardownLeavesNoDeadKeys`.
    - Observer death/address reuse/table-compaction and resident code-extra/buffer tests.

## Snapshot schema and field classes

Schema: `cp311-jit-lifecycle-v1`.

| Section.field | Class | Meaning |
|---|---|---|
| `jit.compiled_codes` | gauge | code/globals/builtins artifact index |
| `jit.installed_functions` | gauge | calls currently installed on machine code |
| `jit.associated_functions` | gauge | authoritative function/artifact claims |
| `jit.parked_functions` | gauge | deopted/parked functions |
| `jit.watched_functions` | gauge | live 3.11 death watches |
| `jit.artifact_members` | gauge | reverse borrowed member population |
| `jit.deferred_anchor_releases` | gauge | pending reference-release transactions |
| `jit.active_compiles` | gauge | in-flight compile keys |
| `jit.completed_compiles` | gauge | completed worker results awaiting consumption |
| `jit.deferred_finalizations` | gauge | main-thread finalizations pending |
| `jit.orphaned_compiled_codes` | gauge | multithread-test teardown pins |
| `jit.code_dedup_entries` | gauge | strong structural donor entries |
| `jit.code_outer_functions` | gauge | borrowed nested-code ownership relation |
| `jit.context_references` | gauge | strong objects retained by emitted code |
| `jit.code_runtimes_live` | gauge | uncleared runtime records |
| `jit.code_runtimes_allocated` | capacity/high-water | slab allocations retained to context death |
| `module.registered_compilation_units` | gauge | borrowed pending units |
| `module.perf_trampoline_worklist` | gauge | borrowed prefork functions |
| `module.unit_deletion_tracking_failed` | invariant boolean | must remain false |
| `module.code_allocator_used_bytes` | capacity/high-water | `CodeAllocatorCinder::releaseCode()` is intentionally a no-op; bytes are reclaimed only with allocator teardown |
| `runtime.resident_code_buffers` | gauge | physical live code buffers |
| `runtime.resident_code_extra_blocks` | gauge | live CodeExtra blocks |
| `runtime.compiled_function_creations` | cumulative | artifact creation proof |
| `runtime.function_destroyed_notifications` | cumulative | delivered function deaths |
| `runtime.code_destroyed_notifications` | cumulative | delivered code deaths |
| `runtime.executable_alloc_calls/bytes` | cumulative | executable allocator activity |
| `runtime.machine_code_entries` | cumulative | machine execution proof |
| `observer.watched_codes` | gauge | observer entries keyed to live code |
| `observer.keyed_slots` | capacity/high-water | live and tombstone keys |
| `observer.table_capacity` | capacity/high-water | allocated table slots |
| `observer.events` | cumulative/bounded evidence | scheduling-decision records |
| `observer.post_publication_interpreted_frames` | cumulative | fallback/no-reentry evidence |
| `generator.native_gauge_available` | availability boolean | false in v0.1 |

## Invariant checker scope

The checker is allocation-only reporting and does not repair state. It verifies I1/I2 exact installed/association/member identity, I3 live death-watch coverage for borrowed Context registries, I4 empty stable compile transactions, I5 empty deferred-anchor queue, I6 attached artifact ownership, I7 CodeExtra cache identity and I8 unpoisoned unit-deletion tracking. Missing or violated evidence returns `ok=false` with exact string errors.
