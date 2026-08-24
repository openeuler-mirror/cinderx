# CPython 3.11 A3 CodeRuntime Identity Audit

Mechanical audit of every long-term store of `CodeRuntime*` /
`const CodeRuntime*`, performed before slot recycling (MR-A3-06) so the
slot's next tenant cannot inherit the previous tenant's state (ABA) and
no borrower can outlive the storage.

## Long-term stores and their verdicts

| Store | Kind | Verdict |
|---|---|---|
| `CompiledFunctionData::runtime` | owner's forward pointer | Severed by `clear()`; retirement (`retire()`) deliberately leaves it whole because pins still read it. |
| `Context::deopt_stats_` (`UnorderedMap<const CodeRuntime*, ...>`) | address-keyed side state | **The ABA hazard.** Purged under `deopt_stats_mutex_` in `Context::recycleCodeRuntime()` before the slot enters the free list. |
| `GenDataFooter::code_rt` | suspended-generator borrow | **Was an unprotected borrow.** The footer now holds a strong reference to the invocation's artifact (`GenDataFooter::compiled`, CPython 3.11, captured from the guarded entry's invocation-artifact slot at generator creation), so the runtime and its machine code live at least as long as any generator that can resume into them. Released when the object stops being a JIT generator (`clearGeneratorCompiledFunction`: deopt, completion, destruction); visited by `jitgen_traverse` so generator cycles stay collectable. |
| `Context::deopt_metadata_` | *dead declaration* | No writers or readers; metadata actually lives inside `CodeRuntime::deopt_metadatas_`, which dies with the husk. Candidate for removal. |
| `codegen::Environ::code_rt`, `gen_asm` locals, JITRT call arguments | transient (compile/invocation scope) | In scope only while the compile or the pinned invocation runs. |

`site_id` (`DeoptMetadata::site_id`) hashes code identity + bytecode
offset + site kind + inline path — not the runtime's address — so slot
reuse cannot alias deopt site identities. This supersedes the M-07-era
argument that relied on the arena never recycling.

## The load-bearing leak

Before this round, three consumers survived on the fact that nothing was
ever freed:

1. A **suspended JIT generator** resumes into the artifact's machine code
   and reads `code_rt->getDeoptMetadata()` at resume, traversal and
   deallocation, with no ownership link. The buffer stayed mapped because
   `CodeAllocatorCinder::releaseCode()` was a no-op (B7-EXEC), and the
   husk stayed valid because the slab never recycled (B7-RUNTIME).
2. A **retired artifact still pinned** by an in-flight invocation kept
   working because `forgetCodeEntry()`'s `clear()` released the runtime's
   references but left the metadata vectors and the (leaked) buffer.
3. **Orphaned artifacts** (`clearForMultithreadedCompileTest()`) leaned on
   the same permanence until `~Context` freed the slabs under them (B9).

Reclaiming B7 therefore required making the ownership explicit first:

- the generator's footer pins the artifact (this audit's row 3);
- registry retirement was split out of `clear()` into `retire()`, which
  performs the identity-guarded owner-side bookkeeping only — the runtime
  is gutted and its storage handed back exclusively at artifact
  destruction, GC collection of an unreachable artifact, or context
  finalization, all of which prove no pin remains;
- `~Context` severs orphans before the slabs die (MR-A3-04).

## Recycling boundaries (R1–R5)

- **R1** normal death → slot reuse: `RetiredCodeRuntimeStorageIsRecycled`
  (compile → death → recompile keeps `code_runtimes_allocated` flat).
- **R2** external pin defers reuse: `PinnedRetirementKeepsTheRuntimeWhole`
  (uncompile + function death under a pin keeps the runtime whole; the
  pin's release recycles), plus the Python-level
  `test_suspended_generator_pins_its_artifact` for the generator pin.
- **R3** context dies first, artifact after:
  `OrphanedArtifactOutlivesContextTeardown`; destruction after `~Context`
  skips the hand-back because `ModuleState::jit_context` is already null
  (`unique_ptr::reset` clears the pointer before destroying) and a
  private test context never passes `ownsCodeRuntime()`.
- **R4** publication failure: the failed transaction's runtime is
  released with its artifact through the same destruction path; C7's
  per-iteration transaction-residue checks and plateau hold the line.
- **R5** no side state across reuse: the `deopt_stats_` purge in
  `recycleCodeRuntime()`; all other per-runtime state lives inside the
  husk and is destroyed by the reusing `allocate()`.

Ownerless hand-backs route through `ModuleState::jit_context` and are
accepted only after `ownsCodeRuntime()` proves the slot lies in that
context's slabs, so a recycled or test-private address can never be
adopted into the wrong arena.
