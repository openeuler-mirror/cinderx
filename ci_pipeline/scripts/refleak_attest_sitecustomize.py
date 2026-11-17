"""Isolate CPython's -R measurement and attest JIT work inside that call.

The product .pth owns evaluator installation. The unmodified upstream judge
and cache-reset code stay interpreted; test functions remain JIT-eligible.
"""
import atexit
import os
import sys

_measurement_functions = ()
_measured_module = "-"
_entries = 0
_creations = 0


def _isolate_measurement():
    # Never initialize CinderX here: broken product startup must stay red.
    jit = sys.modules.get("cinderjit")
    if jit is None:
        return ()
    import _cinderx
    from test.libregrtest import refleak

    original = refleak.dash_R
    cleanup = refleak.dash_R_cleanup
    jit.jit_suppress(original)
    jit.jit_suppress(cleanup)

    def measured(ns, test_name, test_func):
        global _measured_module, _entries, _creations
        before = _cinderx._get_trigger_stats()
        result = original(ns, test_name, test_func)
        after = _cinderx._get_trigger_stats()
        _entries += after["machine_code_entries"] - before["machine_code_entries"]
        _creations += after["compiled_function_creations"] - before["compiled_function_creations"]
        _measured_module = test_name
        return result

    # The observer must never manufacture the machine-code entry it proves.
    jit.jit_suppress(measured)
    refleak.dash_R = measured
    runner = sys.modules.get("test.libregrtest.runtest")
    if runner is not None and getattr(runner, "dash_R", None) is original:
        runner.dash_R = measured
    return (original, cleanup, measured)


def _attest():
    ledger = os.environ.get("CINDERX_REFLEAK_LEDGER")
    if not ledger:
        return
    try:
        import _cinderx

        jit = sys.modules.get("cinderjit")
        isolated = bool(_measurement_functions) and jit is not None and all(
            not jit.is_jit_compiled(function) for function in _measurement_functions
        )
        line = "%d %s %d %d %s %s\n" % (
            os.getpid(), bool(_cinderx.is_frame_evaluator_installed()),
            _entries, _creations, isolated, _measured_module,
        )
    except Exception:
        line = "%d unavailable 0 0 False -\n" % os.getpid()
    try:
        with open(ledger, "a", encoding="utf-8") as fh:
            fh.write(line)
    except OSError:
        pass


# CPython swallows sitecustomize errors. Register before anything that can
# fail so partial isolation still records False rather than omitting a row.
atexit.register(_attest)
_jit = sys.modules.get("cinderjit")
if _jit is not None:
    _jit.jit_suppress(_attest)
    _jit.jit_suppress(_isolate_measurement)
_measurement_functions = _isolate_measurement()
