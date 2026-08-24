"""Test-only semantic-conformance Compile-All hook; installed through staged sitecustomize.

The hook does not edit CPython tests.  It discovers Python functions owned by
the regrtest target module, asks the private 3.11 diagnostic to compile or
return a typed refusal, and records test/doctest execution windows as JSONL.
"""

from __future__ import annotations

import atexit
import dis
import doctest
import functools
import json
import os
from pathlib import Path
import sys
import types
import unittest


_MODE = os.environ["A1_COMPILE_ALL_MODE"]
_LOG_DIR = Path(os.environ["A1_COMPILE_ALL_LOG_DIR"])
_LOG_DIR.mkdir(parents=True, exist_ok=True)
_SCANNED: set[str] = set()
_COMPILE_RESULTS: dict[int, dict] = {}


def _worker_target_module() -> str | None:
    for index, arg in enumerate(sys.argv):
        encoded = None
        if arg == "--worker-args" and index + 1 < len(sys.argv):
            encoded = sys.argv[index + 1]
        elif arg.startswith("--worker-args="):
            encoded = arg.partition("=")[2]
        if encoded is None:
            continue
        try:
            _namespace, test_name = json.loads(encoded)
        except Exception:
            return None
        test_name = str(test_name)
        return test_name if test_name.startswith("test.") else "test." + test_name
    return None


def _write(event: dict) -> None:
    payload = {"pid": os.getpid(), "mode": _MODE, **event}
    path = _LOG_DIR / f"{os.getpid()}.jsonl"
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(payload, sort_keys=True) + "\n")


def _target_file(module) -> str | None:
    filename = getattr(module, "__file__", None)
    if not filename:
        return None
    if filename.endswith((".pyc", ".pyo")):
        filename = filename[:-1]
    return os.path.realpath(filename)


def _functions_from_value(value):
    if isinstance(value, types.FunctionType):
        yield value
    elif isinstance(value, (staticmethod, classmethod)):
        yield value.__func__
    elif isinstance(value, property):
        for function in (value.fget, value.fset, value.fdel):
            if isinstance(function, types.FunctionType):
                yield function


def _discover(module) -> list[types.FunctionType]:
    target_file = _target_file(module)
    found: dict[int, types.FunctionType] = {}
    if target_file is None:
        return []

    def add(function) -> None:
        if not isinstance(function, types.FunctionType):
            return
        if os.path.realpath(function.__code__.co_filename) != target_file:
            return
        found[id(function)] = function

    for value in vars(module).values():
        for function in _functions_from_value(value):
            add(function)
        if isinstance(value, type) and value.__module__ == module.__name__:
            for member in vars(value).values():
                for function in _functions_from_value(member):
                    add(function)
    return sorted(found.values(), key=lambda function: function.__qualname__)


def _runtime_fallback() -> str | None:
    if sys.gettrace() is not None:
        return "TRACING_ACTIVE"
    if sys.getprofile() is not None:
        return "PROFILING_ACTIVE"
    return None


def _compile(module_name: str, function) -> dict:
    cached = _COMPILE_RESULTS.get(id(function))
    if cached is not None:
        return cached

    if _MODE != "jit":
        result = {
            "status": "instrumented-only",
            "eligible": None,
            "compiled": False,
            "phase": None,
            "reason": None,
        }
    elif (reason := _runtime_fallback()) is not None:
        result = {
            "status": "runtime-fallback",
            "eligible": True,
            "compiled": False,
            "phase": "runtime",
            "reason": reason,
        }
    else:
        import cinderjit

        try:
            diagnostic = cinderjit._jit311_compile_diagnostic(function)
        except BaseException as exc:
            result = {
                "status": "hook-error",
                "eligible": None,
                "compiled": False,
                "phase": "api",
                "reason": f"{type(exc).__name__}: {exc}",
            }
        else:
            result = dict(diagnostic)
            opcode = diagnostic.get("opcode")
            result["opcode_name"] = (
                dis.opname[opcode]
                if isinstance(opcode, int) and 0 <= opcode < len(dis.opname)
                else None
            )
            if diagnostic["compiled"]:
                result["status"] = "compiled"
            elif diagnostic["phase"] == "runtime":
                result["status"] = "runtime-fallback"
            elif diagnostic["reason"] is not None:
                result["status"] = "refused"
            else:
                result["status"] = "unknown-refusal"

    _COMPILE_RESULTS[id(function)] = result
    _write(
        {
            "type": "compile",
            "module": module_name,
            "qualname": function.__qualname__,
            "filename": os.path.realpath(function.__code__.co_filename),
            "firstlineno": function.__code__.co_firstlineno,
            **result,
        }
    )
    return result


def _scan(module_name: str) -> None:
    if module_name in _SCANNED:
        return
    _SCANNED.add(module_name)
    module = sys.modules.get(module_name)
    candidates = _discover(module) if module is not None else []
    statuses: dict[str, int] = {}
    reasons: dict[str, int] = {}
    for function in candidates:
        result = _compile(module_name, function)
        status = result["status"]
        statuses[status] = statuses.get(status, 0) + 1
        reason = result.get("reason")
        if reason is not None:
            reasons[reason] = reasons.get(reason, 0) + 1
    _write(
        {
            "type": "module-scan",
            "module": module_name,
            "filename": _target_file(module) if module is not None else None,
            "discovered": len(candidates),
            "statuses": statuses,
            "reasons": reasons,
        }
    )


def _entries() -> int:
    if _MODE != "jit":
        return 0
    import _cinderx

    return int(_cinderx._get_trigger_stats()["machine_code_entries"])


_ORIGINAL_CALL_TEST_METHOD = unittest.TestCase._callTestMethod
_ORIGINAL_LOAD_TESTS_FROM_MODULE = unittest.TestLoader.loadTestsFromModule


def _patched_load_tests_from_module(self, module, *args, **kwargs):
    # unittest calls an optional module-level load_tests() inside the original
    # method.  Scan before delegating so that function's own first invocation
    # can enter machine code and be attributed by the exact per-code ledger.
    _scan(module.__name__)
    return _ORIGINAL_LOAD_TESTS_FROM_MODULE(self, module, *args, **kwargs)


def _patched_call_test_method(self, method):
    module_name = self.__class__.__module__
    target_module = _worker_target_module()
    _scan(module_name)
    if target_module is not None:
        _scan(target_module)

    function = getattr(method, "__func__", method)
    compile_result = _compile(module_name, function)
    before = _entries()
    try:
        return _ORIGINAL_CALL_TEST_METHOD(self, method)
    finally:
        after = _entries()
        _write(
            {
                "type": "test-call",
                "module": module_name,
                "target_module": target_module,
                "test": self.id(),
                "method_qualname": getattr(function, "__qualname__", repr(function)),
                "filename": os.path.realpath(function.__code__.co_filename)
                if hasattr(function, "__code__")
                else None,
                "firstlineno": getattr(getattr(function, "__code__", None), "co_firstlineno", -1),
                "compile_status": compile_result["status"],
                "compile_reason": compile_result.get("reason"),
                "machine_entries_delta": after - before,
            }
        )


_ORIGINAL_DOCTEST_RUN = doctest.DocTestRunner.run


def _patched_doctest_run(self, test, *args, **kwargs):
    target_module = _worker_target_module()
    if target_module is not None:
        _scan(target_module)
    before = _entries()
    try:
        return _ORIGINAL_DOCTEST_RUN(self, test, *args, **kwargs)
    finally:
        _write(
            {
                "type": "doctest-call",
                "target_module": target_module,
                "test": getattr(test, "name", repr(test)),
                "machine_entries_delta": _entries() - before,
            }
        )


def _install_generator_capi_adapter() -> None:
    """Bridge the one CPython test C API that insists on PyGen_Type.

    JIT generators use CinderX's extended object layout.  The supported C API
    boundary is to deopt a suspended instance before handing it to an API that
    parses ``O! &PyGen_Type``.  The ordinary CinderX lib-test runner already
    applies this adapter; the semantic-conformance case stages the same policy explicitly so Compile-All
    cannot bypass it.
    """
    if _MODE != "jit":
        return
    try:
        import _testcapi
        from cinderx.jit import _deopt_gen
    except (ImportError, AttributeError):
        return
    original = getattr(_testcapi, "raise_SIGINT_then_send_None", None)
    if original is None or getattr(original, "__cinderx_generator_adapter__", False):
        return

    @functools.wraps(original)
    def adapted(generator):
        if type(generator).__module__ == "builtins" and type(generator).__name__ == "generator":
            _deopt_gen(generator)
        return original(generator)

    adapted.__cinderx_generator_adapter__ = True
    _testcapi.raise_SIGINT_then_send_None = adapted
    _write({"type": "generator-capi-adapter-installed"})


def _initialize_runtime_evidence() -> None:
    if _MODE != "jit":
        return
    import cinderjit

    cinderjit._jit311_reset_entry_ledger()
    numeric = list(cinderjit._jit311_execute_surface())
    _write(
        {
            "type": "execute-surface",
            "opcodes": numeric,
            "opcode_names": [dis.opname[opcode] for opcode in numeric],
        }
    )


def _summary() -> None:
    payload = {
        "type": "process-summary",
        "target_module": _worker_target_module(),
        "modules_scanned": sorted(_SCANNED),
    }
    if _MODE == "jit":
        try:
            import _cinderx
            import cinderjit

            stats = _cinderx._get_trigger_stats()
            ledger = cinderjit._jit311_entry_ledger()
            rows = [
                {
                    **row,
                    "filename": os.path.realpath(row["filename"]),
                }
                for row in ledger["entries"]
            ]
            payload.update(
                machine_code_entries=stats["machine_code_entries"],
                compiled_function_creations=stats["compiled_function_creations"],
                entry_ledger=rows,
                entry_ledger_dropped=ledger["dropped"],
            )
        except BaseException as exc:
            payload["summary_error"] = f"{type(exc).__name__}: {exc}"
    _write(payload)


unittest.TestCase._callTestMethod = _patched_call_test_method
unittest.TestLoader.loadTestsFromModule = _patched_load_tests_from_module
doctest.DocTestRunner.run = _patched_doctest_run
_initialize_runtime_evidence()
_install_generator_capi_adapter()
atexit.register(_summary)
_write({"type": "hook-installed", "target_module": _worker_target_module()})
