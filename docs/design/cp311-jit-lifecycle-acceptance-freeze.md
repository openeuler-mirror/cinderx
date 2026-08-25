# CPython 3.11 JIT Lifecycle Acceptance Freeze Record

```text
LIFECYCLE ACCEPTANCE FROZEN
```

```json
{
  "acceptance_domain": "lifecycle",
  "freeze_state": "FROZEN"
}
```

The freeze criteria (all of them, none waived):

| Criterion | Evidence |
|---|---|
| LIFECYCLE_OWNERSHIP PASS | L1 function death ×100 · L2 code swap ×100 · L3 observer smoke ×1000 · L4 generator anchor · invariants at every checkpoint, non-vacuity proven |
| SHUTDOWN_STABILITY PASS | S1 park/die/re-enable ×100 (phase evidence judged) · S2 multithread batch · S3 six-state exit matrix 200/200 poisoned+entropy, multithread-completed passing on its own |
| NATIVE_MEMORY_SAFETY PASS | seven targeted ASAN legs against a verified-instrumented extension, zero sanitizer reports; the ASAN executed arm also runs the full green-family population |
| STDLIB_AUTOJIT_REGRESSION PASS with JIT path proof | 72/72 module results present and passing, JIT active in 72/72 workers, scheduler uniformly threshold=50, 18 463 657 machine-code entries, 37 own-code modules, 0 dropped ledger/scheduler events, 0 unknown refusals, `MALLOC_PERTURB_` armed |
| execution regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged) |
| runtime-transition regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged); the lifecycle prerequisite validates the frozen evidence from the frozen commit itself (`git show <frozen>:<historical path>`), so the freeze pins history rather than present-day file names |
| lifecycle blockers | none |
| 3.14 reference differential | paused by operator decision for the closing cleanup rounds; the cleanup commits are CI-only and the product tree is unchanged since the semantic-naming commit, and the differential stays runnable at any time via `ci_pipeline/scripts/rt314_differential.sh <base> <work-dir>` |

## Frozen identity

```text
frozen_commit            9e1a1bbac44d363dee5e44e8ac438f335d92a88d
wheel                    cinderx-2026.8.25.0-cp311-cp311-linux_aarch64.whl
wheel_sha256             7e7cf10a4715a0673c01fe473f34da8c0f750dd2fe2ead310ad90ef719370d60
wheel_embedded_git_sha   9e1a1bbac44d363dee5e44e8ac438f335d92a88d
canonical_report         CP311_JIT_LIFECYCLE_REPORT.md
canonical_report_sha256  40096117433df1c9e36b4913f076ed59da0b0e51bfdf46a95e5c8993f39bad7f
canonical_result         lifecycle_result.json
canonical_result_sha256  ce0459e4d7b98dc4d2d71d95a3d95cf5e2ab6218f3624a099c59c3ceaa62592d
```

The MR closes the development phase.  The chain sits on the execute-mode
base as three delivery commits -- the execution acceptance, the
runtime-transition acceptance (the transition freeze anchor) and the
lifecycle acceptance with its product repairs -- followed by the
semantic-naming change and the closing cleanup: the campaign write-off
ledgers and the mirrored RuntimeTests registry retired behind
rt311_required_tests.txt, source-form meta-tests replaced by the
behavior-proving fail-close set, the runner scenarios owned elsewhere
removed with their owners named, the case vocabulary finished in every
runner, the lifecycle discovery orchestration folded into the
self-contained acceptance entry (`python -m
ci_pipeline.jit311.lifecycle_acceptance`), and the harness converged
per external review: the autocompile case proves only what nothing else
owns, the duplicate scheduler-configuration program and the per-probe
Markdown renderers are gone, the probe pairs run through one dual-probe
helper, and the discovery-only churn scenarios retired.  The probes the
acceptance drives -- the snapshot schema, the churn matrix, the
shutdown exit matrix and the census -- keep their semantics untouched.

The acceptance profile is hardened per review: the stdlib regression
fails without a JIT path proof and judges the semantic leg incomplete
unless all 72 module results are present, a subset or operator-skipped
run caps at NOT_FULLY_RUN, slab free-listing is best-effort noexcept
with an injected-failure regression and dies loudly on a double
recycle, the park/die/re-enable non-vacuity reads the recorded phase
populations under either pause model, and slab containment accepts
exact slot addresses only.

Standing follow-ups (recorded, not blockers): the
RESOURCE_STABILITY_DIAGNOSTIC readings, LSan off by the ASAN leg's
evidence contract, the DeoptStress census pathology inherited from the
runtime-transition chain (tracked separately) and the shadow-mode
codegen-runtime stranding noted in the identity audit.  The execute
chain's on-stack frame-deopt generator pin, previously tracked here, is
closed: the base chain now releases it at its safe site and pins the
regression (JITLifecycle311Test.InStackGeneratorDeoptDropsTheArtifactPin,
held in the required manifest), and the arena lifetime token the base
introduced is now the single storage-liveness source for
runtimeStorageAlive().

The lifecycle campaign is closed.  Regressions in this surface route to
the lifecycle acceptance entry (every lifecycle-sensitive change also
triggers the stdlib regression through the sensitive-path list);
resource-stability work and real-workload trends route to the
workload-stability domain.
