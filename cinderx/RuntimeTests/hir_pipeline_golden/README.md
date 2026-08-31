# HIR pipeline goldens (INFRA-1)

This directory holds the no-behavior-drift baseline for
`Compiler::runPasses` (`cinderx/Jit/compiler.cpp`).  Each file is the
execution trace of the pass pipeline for one corpus function under one
`PassConfig`, recorded as **fingerprints**: one line per executed pass —
sequence number, pass name with `#n` occurrence markers for repeated
call points, and an FNV-1a-64 hash of the normalized post-pass HIR
(with its length).

The `HIRPipelineGolden` suite (`cinderx/RuntimeTests/hir_pipeline_test.cpp`)
rebuilds each trace, asserts that two consecutive runs are identical
(determinism gate), and compares against the committed golden.  Any
reordering, duplication, omission or gating change of a pass — or a
content change at any intermediate stage, including ones the final HIR
converges away — fails the suite.  This is the acceptance gate for the
PassManager extraction (Phase B0 / increment I1).

Why fingerprints and not full dumps: a full-HIR-per-step baseline is
~95% redundant between adjacent steps; any intentional pipeline change
would produce thousands of unattributable golden lines and push people
toward blind regeneration.  Fingerprints keep the baseline compact and
each drift pinpointed to the step; diagnosing what changed behind a
fingerprint is on demand and local:

    HIR_PIPELINE_FULL_DUMP=1 <runtime_tests> --gtest_filter=HIRPipelineGolden.<Case>/<Config>

prints the full per-pass HIR of the actual trace on failure (never
committed).

## Layout

    <python-version>/<arch>/<CaseName>__<PassConfig>.txt

Traces are only comparable within one CPython version and one
architecture: the pass set itself differs (FloatComparisonSimplification
and PrimitiveBoxRemat are AArch64-only), so aarch64 traces carry two
more scheduling units than an x86_64 build would.

## Version & platform policy

Per the project README the supported platform is **Linux aarch64**
(README_CN.md: 运行环境 Linux aarch64, 仅提供 aarch64 预编译 whl), and
per the scenario-extension batch plan **CI and gates anchor Python 3.14
only**.  Accordingly:

- `3.14/aarch64` — anchored, gated.  Baseline generated on
  openEuler 24.03 aarch64 with CPython 3.14.3.
- x86_64 — not a supported product platform; no goldens committed and
  no CI line.  The suite itself is arch-neutral and would skip loudly
  if ever run there without goldens.
- 3.11/3.12/3.15 — extension capability only (SPI/runtime_abi
  negotiation stays version-aware); no goldens, not gated, not in CI.

## Regenerating

Intentional pipeline changes regenerate the baseline with:

    python3 cinderx/TestScripts/update_hir_pipeline_golden.py \
        <path-to-runtime_tests-binary>

(or set `UPDATE_HIR_PIPELINE_GOLDEN=1` and run the suite directly) on an
aarch64 host, then review the diff.  A legitimate change shows up as a
small set of changed fingerprint lines from the affected step onward —
each must be attributable to the code change in the same commit.
Regenerating is never a fix for an unexplained drift.
