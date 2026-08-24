"""A1 T1-T8 tracing/profile fallback acceptance probe for CPython 3.11."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import threading
import unittest


def target(value):
    return value + 1


def fresh(value):
    return value + 2


def run() -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()

    def entries():
        return int(_cinderx._get_trigger_stats()["machine_code_entries"])

    def creations():
        return int(_cinderx._get_trigger_stats()["compiled_function_creations"])

    target_diag = cinderjit._jit311_compile_diagnostic(target)
    before_plain = entries()
    assert target(4) == 5
    plain_entry_delta = entries() - before_plain

    trace_events = []

    def tracer(frame, event, arg):
        if frame.f_code is target.__code__:
            trace_events.append(event)
        return tracer

    sys.settrace(tracer)
    before_trace = entries()
    assert target(4) == 5
    trace_entry_delta = entries() - before_trace
    before_trace_compile = creations()
    trace_compile_diag = cinderjit._jit311_compile_diagnostic(fresh)
    force_compile_while_tracing = cinderjit.force_compile(fresh)
    trace_compile_creation_delta = creations() - before_trace_compile
    sys.settrace(None)

    before_resume = entries()
    assert target(4) == 5
    resume_entry_delta = entries() - before_resume
    fresh_after_trace_diag = cinderjit._jit311_compile_diagnostic(fresh)
    before_fresh = entries()
    assert fresh(4) == 6
    fresh_entry_delta = entries() - before_fresh

    profile_events = []

    def profiler(frame, event, arg):
        if frame.f_code is target.__code__:
            profile_events.append(event)

    sys.setprofile(profiler)
    before_profile = entries()
    assert target(4) == 5
    profile_entry_delta = entries() - before_profile
    sys.setprofile(None)
    before_profile_resume = entries()
    assert target(4) == 5
    profile_resume_delta = entries() - before_profile_resume

    from test import test_sys_settrace as trace_tests

    compile_diagnostics = {}
    for function in (trace_tests.settrace_and_return, trace_tests.settrace_and_raise):
        compile_diagnostics[function.__qualname__] = cinderjit._jit311_compile_diagnostic(function)

    classes = (
        trace_tests.TraceTestCase,
        trace_tests.SkipLineEventsTraceTestCase,
        trace_tests.TraceOpcodesTestCase,
    )
    suite = unittest.TestSuite()
    for cls in classes:
        error_method = cls.test_testcapi_settrace_error
        compile_diagnostics[f"{cls.__name__}.test_testcapi_settrace_error"] = (
            cinderjit._jit311_compile_diagnostic(error_method)
        )
        for name in (
            "test_08_settrace_and_return",
            "test_09_settrace_and_raise",
            "test_testcapi_settrace_error",
        ):
            suite.addTest(cls(name))
    exact = unittest.TestResult()
    suite.run(exact)

    def multithreaded_transition(second_kind: str) -> dict:
        active = [threading.Event(), threading.Event()]
        release = [threading.Event(), threading.Event()]
        cleared = [threading.Event(), threading.Event()]

        def trace_callback(frame, event, arg):
            return trace_callback

        def profile_callback(frame, event, arg):
            return None

        def worker(index: int, kind: str) -> None:
            if kind == "trace":
                sys.settrace(trace_callback)
            else:
                sys.setprofile(profile_callback)
            active[index].set()
            release[index].wait(30)
            if kind == "trace":
                sys.settrace(None)
            else:
                sys.setprofile(None)
            cleared[index].set()

        threads = [
            threading.Thread(target=worker, args=(0, "trace")),
            threading.Thread(target=worker, args=(1, second_kind)),
        ]
        for thread in threads:
            thread.start()
        if not active[0].wait(30) or not active[1].wait(30):
            raise AssertionError("instrumented worker did not become active")
        paused_with_both = not cinderjit.is_enabled()
        release[0].set()
        if not cleared[0].wait(30):
            raise AssertionError("first instrumented worker did not clear")
        paused_after_first_clear = not cinderjit.is_enabled()
        release[1].set()
        if not cleared[1].wait(30):
            raise AssertionError("second instrumented worker did not clear")
        for thread in threads:
            thread.join(30)
            if thread.is_alive():
                raise AssertionError("instrumented worker did not exit")
        enabled_after_final_clear = cinderjit.is_enabled()
        return {
            "second_kind": second_kind,
            "paused_with_both": paused_with_both,
            "paused_after_first_clear": paused_after_first_clear,
            "enabled_after_final_clear": enabled_after_final_clear,
            "pass": paused_with_both
            and paused_after_first_clear
            and enabled_after_final_clear,
        }

    t8_trace_trace = multithreaded_transition("trace")
    t8_trace_profile = multithreaded_transition("profile")

    exact_failures = [
        {"test": str(test), "diagnostic": diagnostic}
        for test, diagnostic in exact.failures
    ]
    exact_errors = [
        {"test": str(test), "diagnostic": diagnostic}
        for test, diagnostic in exact.errors
    ]
    checks = {
        "T1_trace_active_before_call": trace_entry_delta == 0
        and "call" in trace_events
        and "return" in trace_events,
        "T2_trace_blocks_new_compile": trace_compile_diag["reason"] == "TRACING_ACTIVE"
        and not trace_compile_diag["compiled"]
        and force_compile_while_tracing is False
        and trace_compile_creation_delta == 0,
        "T3_midflight_return": not any("test_08_settrace_and_return" in item["test"] for item in exact_failures + exact_errors),
        "T4_midflight_raise": not any("test_09_settrace_and_raise" in item["test"] for item in exact_failures + exact_errors),
        "T5_c_trace_error_delivery": not any("test_testcapi_settrace_error" in item["test"] for item in exact_failures + exact_errors),
        "T6_profile_fallback": profile_entry_delta == 0
        and "call" in profile_events
        and "return" in profile_events,
        "T7_monitoring_removal_recovers": plain_entry_delta > 0
        and resume_entry_delta > 0
        and profile_resume_delta > 0
        and fresh_after_trace_diag["compiled"]
        and fresh_entry_delta > 0,
        "T8_multithread_final_callback": t8_trace_trace["pass"]
        and t8_trace_profile["pass"],
    }
    return {
        "result": "PASS" if all(checks.values()) else "FAIL",
        "checks": checks,
        "target_diagnostic": target_diag,
        "trace": {
            "events": trace_events,
            "machine_entries_delta": trace_entry_delta,
            "compile_diagnostic": trace_compile_diag,
            "force_compile_return": force_compile_while_tracing,
            "compiled_function_creations_delta": trace_compile_creation_delta,
        },
        "profile": {
            "events": profile_events,
            "machine_entries_delta": profile_entry_delta,
        },
        "recovery": {
            "trace_machine_entries_delta": resume_entry_delta,
            "profile_machine_entries_delta": profile_resume_delta,
            "fresh_machine_entries_delta": fresh_entry_delta,
        },
        "exact_tests": {
            "run": exact.testsRun,
            "failures": exact_failures,
            "errors": exact_errors,
        },
        "compile_diagnostics": compile_diagnostics,
        "multithreaded": {
            "trace_trace": t8_trace_trace,
            "trace_profile": t8_trace_profile,
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
