# CPython 3.11 attribute access

These changes optimize attribute access in the CinderX interpreter without
depending on a model framework or replacing its attribute handlers.
The validated target is Linux AArch64, CPython 3.11.6, the main interpreter,
and JIT-off execution.

## Design

- For recognized `__getattr__` hook types with the default `__getattribute__`,
  suppress the temporary missing-attribute exception and invoke the original
  handler with CPython's descriptor binding and final exception context.
- Allow eligible getattr-only types to specialize ordinary instance and method
  reads. Read materialized split-dictionary values from `ma_values`; retain
  version, name, index, descriptor and shadowing guards.
- Reduce interpreter overhead with a global-name cache guarded by full dictionary
  content versions, and the upstream inline-frame call path guarded by evaluator
  ownership, tracing/profiling state and JIT-off state.

Qualification must not introduce additional user callbacks. A full MRO guard
rejects general-key type dictionaries before extra type lookups. The helper may
skip that scan only when the default getter, non-null handler and actual target
name all match the current interpreter's type cache. A cached missing target is
allowed. No Python callback runs between these checks and the target type lookup.
An already cached nondefault getter delegates the whole request to the original
API. Instance-storage guards and handler ownership remain in place in every case.

The handler is held strongly before any operation that can run Python. Managed
instance dictionaries with general keys, dictionary subclasses and non-managed
instance-dictionary layouts use the original attribute API. This preserves key
comparison side effects and the distinction between ordinary lookup misses and
exceptions raised by user code.

Tracked upstream files remain unchanged. See the
[patch series](../cinderx/Interpreter/3.11/attr-patches/README.md) for build-time
application and the affected opcode paths. Other Python versions and JIT-on
execution are outside the attribute mode described here.

## Use

Install a wheel built from this source, then select the mode before starting Python:

```bash
ATTR_PATH_MODE=off python workload.py
ATTR_PATH_MODE=cinderx_control python workload.py
ATTR_PATH_MODE=cinderx python workload.py
ATTR_PATH_MODE=cinderx python -c 'import _cinderx_auto; print(_cinderx_auto._verify())'
```

`off` does not install the evaluator. `cinderx_control` keeps the interpreter
support paths but disables the two attribute optimizations. `cinderx` enables
them. Explicit attribute modes disable JIT and abort startup if installation or
resolved configuration checks fail. Defaults remain process-local; child
processes inherit only settings explicitly supplied by the caller.

For targeted comparisons, `ATTR_PATH_NOEXC`, `ATTR_PATH_SPECIALIZE`,
`ATTR_PATH_SPLIT_HINT`, `ATTR_PATH_GLOBAL_CACHE`, `ATTR_PATH_INLINE_CALL` and
`ATTR_PATH_MRO_CACHE` accept `0/1` overrides. The last switch selects full MRO
qualification or guarded type-cache reuse. Global-name caching and guarded inline
calls default on whenever the custom interpreter runs JIT-off, independently
of `ATTR_PATH_MODE`; their defaults are off in other JIT modes. Split-dictionary
reads default on only in `ATTR_PATH_MODE=cinderx`. Explicit overrides still apply. `ATTR_PATH_GLOBAL_ROUTE` defaults to
`compact`; other supported routes are `helper`, `early`, `slot` and `tick`.
Do not combine these paths with a separate plugin replacing `tp_getattro`.

`_attr_path_status()` reports configuration. Optional `_attr_path_counters()`
and `_attr_mro_stats()` diagnostics require `ATTR_PATH_DIAGNOSTICS=1`; keep it
disabled while timing. Configuration flags alone are not proof of a taken fast
path; regression tests also inspect opcodes and hit/callback counters.

## Regression tests

The Kunpeng `all_test_cinderx` suite discovers the attribute, global-name
cache, call, startup and patch build tests in `test_kunpeng`. From a source
checkout with an installed CPython 3.11.6 CinderX wheel, run:

```bash
export PYTHONPATH="$PWD/cinderx/PythonLib${PYTHONPATH:+:$PYTHONPATH}"
python -m unittest -v test_cinderx.test_kunpeng.test_attr_path_interpreter_311
python -m unittest discover -s cinderx/PythonLib/test_cinderx/test_kunpeng -t cinderx/PythonLib -p 'test_interp_*_311.py' -v
```

The attribute tests compare stock and optimized behavior using standard-library
fixtures, and check that warmed attribute and method opcodes hit their caches.
Subprocess fixtures and C probes live in `test_kunpeng/fixtures/attr_path/`.
Runtime tests require Linux and CPython 3.11.6. Probes need matching Python
headers and a C compiler. The patch build test needs CMake, Make and Git.

The CPython `test_call`, `test_generators`, `test_coroutines`,
`test_sys_settrace` and `test_sys_setprofile` suites provide additional call-path
regression coverage.

## PGO builds

This feature does not install a separate PGO trainer or change the repository's
PGO policy. `setup.py` continues to select
`cinderx/TestScripts/pgo_train_workloads.py` unless a workload is explicitly
supplied through the existing `CINDERX_PGO_WORKLOAD` option. The default trainer
currently exercises JIT execution; training JIT-off attribute paths is a separate
coverage concern. Results from a custom-training build must not be treated as
measurements of a default-training rebuild.
