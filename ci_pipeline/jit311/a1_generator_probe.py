"""Targeted A1 synchronous-generator semantics and C-API boundary probe."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import types


def normal_inner():
    yield 1
    return 2


def normal_outer():
    return (yield from normal_inner())


def send_inner():
    value = yield "ready"
    return value


def send_outer():
    return (yield from send_inner())


def throw_inner():
    try:
        yield "ready"
    except ValueError:
        return "caught"


def throw_outer():
    return (yield from throw_inner())


def signal_inner():
    try:
        yield
    except KeyboardInterrupt:
        return "PASSED"
    return "FAILED"


def signal_outer():
    return (yield from signal_inner())


def _stop_value(operation):
    try:
        operation()
    except StopIteration as exc:
        return {"type": type(exc).__name__, "value": exc.value}
    raise AssertionError("generator did not complete with StopIteration")


def run() -> dict:
    import _cinderx
    import _testcapi
    import cinderjit
    import cinderx
    from cinderx.jit import _deopt_gen

    cinderx.init()
    _cinderx.install_frame_evaluator()

    functions = (
        normal_inner,
        normal_outer,
        send_inner,
        send_outer,
        throw_inner,
        throw_outer,
        signal_inner,
        signal_outer,
    )
    diagnostics = {
        function.__name__: cinderjit._jit311_compile_diagnostic(function)
        for function in functions
    }
    if not all(item["compiled"] for item in diagnostics.values()):
        raise AssertionError(diagnostics)

    start_entries = int(_cinderx._get_trigger_stats()["machine_code_entries"])

    normal = normal_outer()
    first = next(normal)
    normal_done = _stop_value(lambda: next(normal))

    sent = send_outer()
    send_first = next(sent)
    send_done = _stop_value(lambda: sent.send("sent"))

    thrown = throw_outer()
    throw_first = next(thrown)
    throw_done = _stop_value(lambda: thrown.throw(ValueError("probe")))

    # JitGen has a trailing footer and is intentionally a distinct type.  A C
    # extension that parses ``O! &PyGen_Type`` must receive a suspended object
    # deoptimized at that boundary.  This is the same narrow adapter used by
    # CinderX's established CPython lib-test runner.
    signalled = signal_outer()
    signalled.send(None)
    signal_entries = int(_cinderx._get_trigger_stats()["machine_code_entries"])
    deopted = bool(_deopt_gen(signalled))
    native_type_after_deopt = type(signalled) is types.GeneratorType
    signal_done = _stop_value(
        lambda: _testcapi.raise_SIGINT_then_send_None(signalled)
    )

    end_entries = int(_cinderx._get_trigger_stats()["machine_code_entries"])
    checks = {
        "normal_yield_from": first == 1 and normal_done == {"type": "StopIteration", "value": 2},
        "send_through_yield_from": send_first == "ready" and send_done == {"type": "StopIteration", "value": "sent"},
        "throw_through_yield_from": throw_first == "ready" and throw_done == {"type": "StopIteration", "value": "caught"},
        "signal_boundary_deopted": deopted and native_type_after_deopt,
        "signal_exception_semantics": signal_done == {"type": "StopIteration", "value": "PASSED"},
        "machine_entry_proven": signal_entries > start_entries and end_entries > start_entries,
    }
    return {
        "result": "PASS" if all(checks.values()) else "FAIL",
        "checks": checks,
        "diagnostics": diagnostics,
        "machine_entries_delta": end_entries - start_entries,
        "normal": {"first": first, "completion": normal_done},
        "send": {"first": send_first, "completion": send_done},
        "throw": {"first": throw_first, "completion": throw_done},
        "signal": {
            "deopted_before_native_c_api": deopted,
            "native_type_after_deopt": native_type_after_deopt,
            "completion": signal_done,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run()
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"], **report["checks"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
