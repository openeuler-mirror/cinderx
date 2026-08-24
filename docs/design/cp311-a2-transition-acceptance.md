# CPython 3.11 A2 transition acceptance implementation

A2 is a discovery-first gate over the clean A1 capability surface. It does
not expand the opcode whitelist and only A2-P forbids explicit compilation;
A2-T may use the private generator compile diagnostic because automatic
generator scheduling is deliberately disabled by A1 policy.

## Lanes

- **P** runs the frozen 72 modules as Stock, threshold 50 control, and genuine
  `PYTHONJITAUTO=1` aggressive mode. The hook only enables evidence ledgers;
  it never calls `force_compile` or Compile-All.
- **T** runs paired Stock/JIT T01-T10 witnesses and requires exact semantic
  journals plus pre-entry, transition-ledger and recovery proof.
- **R** executes six core transitions for 100 cycles and a separate
  first-publication footprint plateau probe.

## Private transition ledger

`_jit311_reset_transition_ledger()` and `_jit311_transition_ledger()` are
test-only. Rows are copied at real deopt or suspended-generator conversion and
contain filename, qualname, first line, transition, deopt reason, cause/resume
offset, forced and instrumentation flags. No Python object is retained. Any
allocation failure increments `dropped`; A2 fails when it is non-zero.

## Existing-test mapping

| A2 | Existing native/Python anchor |
|---|---|
| T01 | `DeoptSite311Test.ForcedAndOrganicEnterTheSameRestore` |
| T02 | attribute-cache mutation tests in `test_canary_execute_311` |
| T03 | `Exception311Test.*`, `UnhandledTracebackStopsAtTheFaultingUnit` |
| T04 | `ReplacingCodeStopsTheOldMachineCode` |
| T05 | `DefaultsStayInstalledAndBindLive`, `KwOnlyDefaultSurvivesKwdefaultsClear` |
| T06 | A1 tracing T1-T8 |
| T07 | `test_mid_frame_pep523_takeover_does_not_capture_the_deopt` |
| T08 | generator send/throw/yield-from and A1 native-C boundary probe |
| T09 | `CallDeliversAsyncExcBeforeNextStatement`, CALL_EX counterpart |
| T10 | `BindFailureAtRecursionLimitMatchesStock`, C-stack guard test |

The A2 probes organize those contracts into one machine-readable transition
matrix; they do not replace the native tests.

## Fail-closed policy

- No CPython testcase edits or new skip.
- No automatic compatibility-baseline generation.
- Adaptive-only differences require exact fingerprints and an independent
  semantic probe.
- Frame position, traceback, tracing, generator, signal, crash and hang
  differences are always blockers.
- Coverage uses A1's non-retaining exact per-code entry ledger.
- Official runs inherit A1's clean source and exact wheel/source SHA preflight.
