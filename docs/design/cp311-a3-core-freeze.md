# CPython 3.11 A3-Core Freeze Record

```text
A3-Core FROZEN
```

The freeze criteria of the A3-Core review (all of them, none waived):

| Criterion | Evidence |
|---|---|
| LIFECYCLE PASS | L1 function death ×100 · L2 code swap ×100 · L3 observer smoke ×1000 · L4 generator anchor · invariants I1–I5, non-vacuity proven |
| SHUTDOWN PASS | S1 park/die/re-enable ×100 (phase evidence judged) · S2 multithread batch · S3 six-state exit matrix 200/200 poisoned+entropy, multithread-completed passing on its own |
| MEMSAFE PASS | seven targeted ASAN legs against a verified-instrumented extension (42 `__asan` symbols), zero sanitizer reports; the ASAN executed arm also runs the full 228-test green-family population |
| REALWORLD-PENETRATION-HOOK PASS with JIT path proof | 72/72 module results present and passing, JIT active in 72/72 workers, scheduler uniformly threshold=50, 18 445 515 machine-code entries, 37 own-code modules, 0 dropped ledger/scheduler events, 0 unknown refusals, `MALLOC_PERTURB_` armed |
| A1 regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged) |
| A2 regression | PASS_WITH_APPROVED_DEVIATIONS (frozen deviation register unchanged) |
| A3-Core blockers | none |

## Frozen identity

```text
frozen_commit            de73e262ba5970139239493ccbdb6e57fc0cd332
wheel                    cinderx-2026.8.24.0-cp311-cp311-linux_aarch64.whl
wheel_sha256             e0b998ef17180824cd3194ff9acfe6069f3b232aef5ed8856f2ada0aa2613ab2
wheel_embedded_git_sha   de73e262ba5970139239493ccbdb6e57fc0cd332
canonical_report         CP311_JIT_A3_CORE_REPORT.md
canonical_report_sha256  f121a75670c26eeeebd13bfa1b7d45dce444fb9e2b48bff344b5dbe43976a6f9
canonical_result         a3_core_result.json
canonical_result_sha256  3c85e2732e2a5ffa6be258ae4735ab9bf5608d1ace6b980fbbe16aab6e7d384e
```

The freeze-hardening change (`cp311: harden final A3-Core acceptance`)
closed the review's P0-1/P0-2/P0-3 and P1-1/P1-2 plus the two re-review
additions, without widening the surface: the penetration hook fails
without a JIT path proof and judges the semantic leg incomplete unless
all 72 module results are present, a subset or operator-skipped run caps
at NOT_FULLY_RUN, slab free-listing is best-effort noexcept with an
injected-failure regression and dies loudly on a double recycle, C4's
non-vacuity reads the recorded phase populations under either pause
model, and slab containment accepts exact slot addresses only.

Standing follow-ups (recorded, not blockers): the
RESOURCE_STABILITY_DIAGNOSTIC readings, LSan off by the ASAN leg's
evidence contract, the DeoptStress census pathology inherited from the
A2 chain (tracked separately), and the shadow-mode codegen-runtime
stranding noted in the identity audit.

A3 is closed.  Regressions in this surface route to `--profile core`
(every lifecycle-sensitive change also triggers the penetration hook by
the sensitive-path list); resource-stability work and real-workload
trends route to A4.
