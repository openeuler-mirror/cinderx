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
| STDLIB_AUTOJIT_REGRESSION PASS with JIT path proof | 72/72 module results present and passing, JIT active in 72/72 workers, scheduler uniformly threshold=50, 18 474 211 machine-code entries, 37 own-code modules, 0 dropped ledger/scheduler events, 0 unknown refusals, `MALLOC_PERTURB_` armed |
| execution regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged) |
| runtime-transition regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged); the lifecycle prerequisite validates the frozen evidence from the frozen commit itself (`git show <frozen>:<historical path>`), so the freeze pins history rather than present-day file names |
| lifecycle blockers | none |
| 3.14 reference differential | paused by operator decision for the closing cleanup rounds; the cleanup commits are CI-only and the product tree is unchanged since the semantic-naming commit, and the differential stays runnable at any time via `ci_pipeline/scripts/rt314_differential.sh <base> <work-dir>` |

## Frozen identity

```text
frozen_commit            a41f535c97d091577d10e8bc9dda3118e1603775
wheel                    cinderx-2026.8.24.0-cp311-cp311-linux_aarch64.whl
wheel_sha256             780012afa03ea3ebac52a685dc12b4d06db3fdec1409e6b39c65643f3cc3d0ba
wheel_embedded_git_sha   a41f535c97d091577d10e8bc9dda3118e1603775
canonical_report         CP311_JIT_LIFECYCLE_REPORT.md
canonical_report_sha256  09159a02b2333317c9f8bc33a12266de74fa5d43190d2885c1c2ea3efe304ef5
canonical_result         lifecycle_result.json
canonical_result_sha256  779d0bc19cd3b6abe558b0eab72bcaa7ac1f5abf4ee946718916f4f7f90f3504
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
runner, and the lifecycle discovery orchestration folded into the
self-contained acceptance entry (`python -m
ci_pipeline.jit311.lifecycle_acceptance`), whose probes -- the snapshot
schema, the churn matrix, the shutdown exit matrix and the census --
are untouched.

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
runtime-transition chain (tracked separately), the shadow-mode
codegen-runtime stranding noted in the identity audit, and the execute
chain's unreleased generator pin on the on-stack frame deopt path
(tracked separately; an early release there is a proven use-after-free,
so the fix needs its own safe release site).

The lifecycle campaign is closed.  Regressions in this surface route to
the lifecycle acceptance entry (every lifecycle-sensitive change also
triggers the stdlib regression through the sensitive-path list);
resource-stability work and real-workload trends route to the
workload-stability domain.
