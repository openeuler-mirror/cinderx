# CPython 3.11 CinderX JIT A2 Execution Report

Final: **PASS_WITH_APPROVED_DEVIATIONS**

## A2-P Aggressive penetration

- Target modules: 72
- Worker JIT active: 72/72
- Classified: 72/72
- OWN_CODE_JIT: 65
- PUBLISHED_NO_REENTRY: 7
- EXPECTED_SAFE_REFUSAL: 0
- COVERAGE_GAP: 0
- Unknown refusals: 0
- Differential: PASS_WITH_APPROVED_DEVIATIONS
- Unexpected differences: 0

## A2-T Transition matrix

| Transition | Pre JIT | Trigger | Stock match | Recovery | Result |
|---|---|---|---|---|---|
| T01 guard-miss-specialized-numeric-type | True | guard_miss | True | {"reentered": true, "result": 3} | PASS |
| T02 dynamic-object-model-method-replacement | True | object_model | True | {"reentered": true} | PASS |
| T03 exception-deopt-restore | True | exceptions | True | {"reentered": true, "result": "clean"} | PASS |
| T04 function-code-mutation | True | code_mutation | True | {"compiled": true, "entry_rows": [{"entries": 1, "filename": "/Users/thedk/.codex/worktrees/260e/cinderx/ci_pipeline/jit311/a2_transition_probe.py", "firstlineno": 63, "qualname": "t04_new_code"}], "machine_entry_proven": true} | PASS |
| T05 defaults-and-kwdefaults-mutation | True | defaults_mutation | True | {"reentered": true, "result": 200} | PASS |
| T06 runtime-tracing-profile-transition | True | instrumentation | True | {"reentered": true, "result": 3} | PASS |
| T07 foreign-pep523-evaluator-transition | True | foreign_evaluator | True | {"reentered": true, "result": 5} | PASS |
| T08 generator-suspend-deopt-resume | True | generator | True | {"interpreter_resume": true, "policy": "interpreter-resume", "semantic_correct": true, "stale_machine_entry": false} | PASS |
| T09 pending-async-exception-boundary | True | pending_event | True | {"reentered": true, "result": 10} | PASS |
| T10 recursion-c-stack-boundary | True | recursion | True | {"policy": "interpreter-after-backoff", "reentered": false, "result": 4, "semantic_correct": true, "stale_machine_entry": false} | PASS |

Aggressive tracing/C-trace regression: **PASS**

Frame-position matrix: **PASS**
Recursion-boundary matrix: **PASS**
Native C recursion boundary: **PASS**

## A2-R Repetition

| Transition | Cycles | Semantic failures | State failures |
|---|---:|---:|---:|
| guard-miss | 100 | 0 | 0 |
| trace-attach-detach | 100 | 0 | 0 |
| profile-attach-detach | 100 | 0 | 0 |
| code-swap-recompile | 100 | 0 | 0 |
| attribute-mutation | 100 | 0 | 0 |
| generator-suspend-deopt-resume | 100 | 0 | 0 |

## Footprint plateau

- Result: PASS
- Strict plateau: True
- Classification: APPROVED_STRESS_MODE_DEVIATION
- Delta: `{"first_publication_gc_objects": 5, "first_publication_object_types": {"builtins.CompiledFunction": 1, "builtins.builtin_function_or_method": 1, "builtins.dict": 1, "builtins.tuple": 1, "weakref.ReferenceType": 1}, "steady_10_to_1000_gc_objects": 0, "steady_10_to_1000_object_types": {}, "steady_compiled_function_creations": 0, "steady_resident_code_buffers": 0}`

## Approved deviations

- Result: PASS
- Exact differences: `["test.test_descr.ClassPropertiesAndMethods.test_slots", "test.test_dis.DisTests.test_super_instructions", "test.test_dis.DisWithFileTests.test_super_instructions"]`
- Proof errors: `[]`

## Freeze hardening

- FH-1 native recursion: PASS
- FH-2 no re-entry proof: PASS
- FH-3 footprint fingerprint: PASS
- Conclusion: **A2 FROZEN**

## Blockers

- None
