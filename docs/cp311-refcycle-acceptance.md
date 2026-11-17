# CPython 3.11 generator refcycle acceptance

The execution Compile-All lane runs the original
`test.test_generators.FinalizationTest.test_refcycle` in machine code. Slice
admission in `591e1fb2` made its opening `gc.garbage[:]` eligible; the daily
source `9a0699ec` consequently fails `assertGreater(sys.getrefcount(g), 2)`.
This does not justify accepting failure of the entire testcase.

## Reference ownership

After `g.send(g)`, the suspended interpreted generator has a self-reference.
The caller also owns `g`. CPython's `LOAD_FAST` adds a temporary argument-stack
reference when calling `sys.getrefcount`, so it observes 3. The compiled
caller's HIR uses its existing owned register as a borrowed `VectorCall`
argument, without an `Incref`, and observes 2. The final HIR retains this owned
register until the `Decref` at `del g`; the count difference is not evidence
that the self-reference or the caller's ownership is missing.

On local aarch64 openEuler with Python 3.11.6-34, an isolated Release build of
`9a0699ec` reproduced the original failure three times. Stock and instrumented
arms observed 3. Letting only this assertion proceed in JIT observed 2 and
completed all later assertions. These results use a local build, not the
official daily wheel; the parent commit has not been rebuilt for attribution.

## Assertion contract

`semantic_conformance_adapters.refcycle_refcount_contract` applies only to
CPython 3.11, this exact testcase ID, and a successful JIT compile diagnostic.
For this invocation's first `assertGreater` only, the integer pair `(2, 2)` is
checked as `(3, 2)` to account for the interpreter's extra temporary reference.
Counts below 2 still fail. An interpreted/runtime-fallback result of 3 retains
the original assertion. Other testcases, thresholds and later assertions are
unchanged. The original test method and its code identity remain intact.

The override is instance-local and restored even on failure. A JSONL
`assertion-contract` event records the observed and interpreter-equivalent
counts; compilation and native-entry evidence continue to identify the
original method. The testcase must return successfully, including its
`assertFalse(finalized)`, `assertTrue(finalized)` after collection, and
`assertEqual(gc.garbage, old_garbage)`. No testcase-level deviation is added.

Regression coverage includes rejected lower counts, scope and restoration,
and downstream assertion failures. The real compiled CPython test first fails
without the adapter, then passes with it. With automatic GC disabled, injected
no-op collection and a polluted garbage list must still fail; each run must
enter the original method's machine code.

The fixed harness passed the complete `test_generators` module in Stock,
instrumented and Compile-All arms using `libtest_diff_311.py`. The focused
adapter tests, execution-report tests, gate configuration tests and execution
infrastructure tests passed together (48 tests). This is targeted local
validation, not a rerun of the complete daily gate or its official wheel.

## Full Compile-All scope audit (2026-11-11)

All 72 frozen modules were run with the same `9a0699ec` native build in Stock,
instrumented, and fixed Compile-All arms. An additional complete Compile-All
run used the original hook, without the assertion adapter. The fixed three-arm
run was repeated with per-process provenance recording; testcase outcomes were
identical. All 94 recorded JIT processes loaded
`/refcycle-build-gcc14/_cinderx.so`, SHA-256
`8128bec644c1cd64974784b7977245601fca61ac9c650784bc59e13aae1668ab`.
Stock and instrumented processes did not load `_cinderx`.

| Arm | Passed cases | Failed cases | Skipped cases |
| --- | ---: | ---: | ---: |
| Stock | 6424 | 0 | 88 |
| Instrumented | 6424 | 0 | 88 |
| Original Compile-All | 6418 | 6 | 88 |
| Fixed Compile-All | 6419 | 5 | 88 |

The only outcome changed by the adapter is `test_refcycle`: failure to pass.
The remaining five failures exactly match the existing deviation ledger:
four adaptive-disassembly cases and
`test.test_enumerate.TestReversed.test_bug1229429` (`2 != 3`). No new deviations
were added. Stock versus instrumented is `PASS`; instrumented versus fixed
Compile-All is `PASS_WITH_APPROVED_DEVIATIONS`. Classification is `PASS`, with
all 72 modules entering their own machine code, 5296 discovered functions,
3867 compiled, 3539 entered, and 1429 expected refusals. There are no unexpected
refusals or dropped entry-ledger records. Tracing T1-T8, generator execution and
the disassembly semantic-control probes also pass.

### Reference-count checks inside the frozen surface

Static AST inventory found 34 references to `getrefcount`, `gettotalrefcount`
or `refcount_test`, in 14 methods across 8 modules. These are source references,
not 34 independent tests. Runtime evidence separates the methods as follows:

| Methods | Count | Fixed-run evidence |
| --- | ---: | --- |
| `CoroutineTest.test_for_2`, traceback `test_no_refs_to_exception_and_traceback_objects` | 2 | Passed, original methods entered JIT |
| `TestReversed.test_bug1229429` | 1 | Entered JIT; existing reference-count deviation remains |
| `FinalizationTest.test_refcycle` | 1 | Entered JIT; assertion contract observed 2; passed |
| `CoroutineTest.test_for_3`, `test_for_4`, `test_for_6` | 3 | Passed in interpreter; refused with `REFUSE_SHAPE_EXECUTE_SURFACE` |
| `GCTests.test_frame`, `test_get_count`, `test_collect_generations` | 3 | Passed; no native entry recorded for the original test body |
| Two descriptor initializer leak checks, property initializer leak check, union reference-cycle check | 4 | Skipped: this Release Python lacks `gettotalrefcount` |

Thus the observed reference-count incompatibilities in the unmodified frozen
suite are two methods, not just `test_refcycle`. The adapter addresses only
one of them. Even before/after delta checks can expose a difference, as the
existing `test_bug1229429` demonstrates; they must not be assumed safe solely
because they compare two measurements.

### Inventory boundaries

The broader stdlib AST search found 116 source references in 27 files; some
are outside the frozen surface or belong to test infrastructure. Repository
text inventory also includes subprocess source strings (121 matching lines
across 28 Python files), covering argument/attribute/cache ownership, generator
artifact release, deoptimization, immortalization and reference-drift probes.
These textual counts include comments and feature guards. Native RuntimeTests
have a separate candidate inventory for `Py_REFCNT`/reference-count assertions.

The complete frozen Compile-All surface was executed, not every CPython or
CinderX test. Outside-surface checks, refused functions, skipped Debug-Python
checks and the C++ suite do not acquire JIT coverage from this run. This is a
local build/harness validation, not an official-wheel or full daily-gate result.

## Why `test_bug1229429` observes `2 != 3`

An unchanged-method diagnostic on the same native build records one native
entry and one organic deoptimization. The deopt event identifies
`TestReversed.test_bug1229429`, line 241, bytecode offset 144 (`CALL`), reason
`UnhandledException`, description `VectorCall`: the first `reversed(f)` raises
the expected TypeError because `f.__reversed__` is not callable. The JIT exits
to the interpreter to handle this exception, although the Python test catches
it normally. "UnhandledException" here describes the JIT exit, not an uncaught
Python exception.

The first `getrefcount(r)` executes in JIT code: `r` has one caller-owned
reference and one owner in `f.__reversed__`, and vectorcall borrows its argument,
so the result is 2. The final measurement executes in the interpreter after
deoptimization. Its temporary argument-stack reference makes the result 3.
Both calls have borrowed argument passing in final HIR; the second compiled
call is not reached on this exception path. Thus this is specifically a
cross-execution-mode comparison, not evidence of a leaked reference.

Controlled variants retain the original setup and error loop, replacing only
the final assertion with a return of the two counts and the objects for
external inspection. A weak-referenceable payload variant additionally tests
destruction. Each variant is explicitly compiled and native/deopt counters are
checked. Automatic cyclic GC is disabled throughout.

| Exception-loop iterations | Interpreter counts | JIT-started counts | JIT deopts |
| ---: | --- | --- | ---: |
| 0 | 3, 3 | 2, 2 | 0 |
| 1 | 3, 3 | 2, 3 | 1 |
| 10 | 3, 3 | 2, 3 | 1 |
| 1000 | 3, 3 | 2, 3 | 1 |
| 100000 | 3, 3 | 2, 3 | 1 |

The exact `object()` payload and weak-referenceable variant agree. Measured
from the same interpreted caller after return, both arms have exactly 3
references (local, function attribute, temporary argument). Deleting the
function immediately clears its weakref and lowers the payload count to 2;
deleting the last payload variable immediately clears its weakref too.

A further 200 fresh compiled frames, each executing 1000 failing calls, record
exactly 200 machine entries and 200 deopts. Every pair is `(2, 3)`, external
counts stay balanced, and every function/payload weakref dies without GC.
Fresh frames prevent JIT deopt backoff from silently turning this stress run
into an interpreted-only check. The stock control produces `(3, 3)` in all
200 rounds with the same immediate destruction behavior.

These checks support a reference-count observable difference, not a payload
reference leak on the tested path. They do not establish leak freedom for all
runtime allocations or other deoptimization routes. The existing deviation
entry and runtime implementation were not changed by this investigation.

## Running the regression from CI

The CP311 PR gate invokes `test_execution_infra_311` from
`cinderx/PythonLib`, where the repository's `ci_pipeline` package is not on
the default import path. A local run that adds the repository root to
`PYTHONPATH` hides this dependency. The refcycle regression now locates its
harness root relative to the test file (supporting both checkout and staged
layouts), and adds only that directory to this child's import path. Other
children retain their existing environment; the source runtime package
directory is not added.

With `PYTHONPATH` unset, all 7 execution infrastructure tests pass from both
the CI working directory and an independent staged tree. The 41 adapter,
execution report and gate configuration tests also pass.
