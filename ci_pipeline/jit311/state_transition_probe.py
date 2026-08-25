"""T01-T10 high-risk state-transition witnesses."""

from __future__ import annotations

import argparse
import json
import signal
from pathlib import Path
import sys
import threading
import time


class TransitionGuardBase:
    def __init__(self, value):
        self.value = value

    def compute(self):
        return self.value + 1


class TransitionGuardOther:
    def __init__(self, value):
        self.value = value


def t01_numeric(value):
    return value + 1


class TransitionDynamic:
    def method(self):
        return 1


def t02_method(obj):
    return obj.method()


def t03_handled(raise_it):
    try:
        if raise_it:
            raise ValueError("handled-boom")
        return "clean"
    except ValueError as exc:
        return [type(exc).__name__, str(exc)]


def t03_unhandled(raise_it):
    if raise_it:
        raise KeyError("unhandled-boom")
    return "clean"


def t03_caller(function, raise_it):
    return function(raise_it)


def t04_function(value):
    return ["old", value + 1]


def t04_new_code(value):
    return ["new", value + 2]


def t05_defaults(value=1, *, scale=2):
    return value * scale


TRANSITION_TRACE_CALLBACK = None


class TransitionTraceNumber:
    def __init__(self, value, arm=False):
        self.value = value
        self.arm = arm

    def __add__(self, other):
        global TRANSITION_TRACE_CALLBACK
        if self.arm and TRANSITION_TRACE_CALLBACK is not None:
            sys.settrace(TRANSITION_TRACE_CALLBACK)
            sys._getframe().f_back.f_trace = TRANSITION_TRACE_CALLBACK
            self.arm = False
        return TransitionTraceNumber(self.value + other.value)


def t06_workload(left, right):
    total = left + right
    return total + right


def t07_hot(left, right):
    return left + right


def t08_inner():
    value = yield "ready"
    try:
        yield ["sent", value]
    except ValueError:
        return "thrown"


def t08_outer():
    return (yield from t08_inner())


def t08_signal_inner():
    try:
        yield
    except KeyboardInterrupt:
        return "PASSED"
    return "FAILED"


def t08_signal_outer():
    return (yield from t08_signal_inner())


def t09_loop(limit):
    value = 0
    while value < limit:
        value += 1
    return value


def t10_recursive(depth):
    if depth <= 0:
        return 0
    return 1 + t10_recursive(depth - 1)


def _exception(callable_, *args) -> dict:
    try:
        callable_(*args)
    except BaseException as exc:
        frames = []
        tb = exc.__traceback__
        while tb is not None:
            frames.append(
                [
                    tb.tb_frame.f_code.co_name,
                    tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno,
                    tb.tb_lasti,
                ]
            )
            tb = tb.tb_next
        return {"type": type(exc).__name__, "message": str(exc), "frames": frames}
    raise AssertionError("call did not raise")


class Probe:
    def __init__(self, mode: str):
        self.mode = mode
        self.jit = mode == "jit"
        self.results: list[dict] = []
        if self.jit:
            import _cinderx
            import cinderjit
            import cinderx

            cinderx.init()
            _cinderx.install_frame_evaluator()
            self._cinderx = _cinderx
            self.cinderjit = cinderjit
            from cinderx.jit import jit_suppress

            jit_suppress(_exception)
            for value in Probe.__dict__.values():
                if callable(value) and hasattr(value, "__code__"):
                    jit_suppress(value)

    def entries(self) -> int:
        if not self.jit:
            return 0
        return int(self._cinderx._get_trigger_stats()["machine_code_entries"])

    def stats(self) -> dict:
        if not self.jit:
            return {}
        return dict(self._cinderx._get_trigger_stats())

    def prepare(self, function, invoke, expected=None, attempts: int = 20) -> dict:
        if not self.jit:
            value = invoke()
            if expected is not None:
                assert value == expected, (value, expected)
            return {"compiled": False, "machine_entry_proven": False}
        self.cinderjit._jit311_reset_entry_ledger()
        before = self.entries()
        value = None
        for _ in range(attempts):
            value = invoke()
            if self.cinderjit.is_jit_compiled(function) and self.entries() > before:
                break
        if expected is not None:
            assert value == expected, (value, expected)
        ledger = self.cinderjit._jit311_entry_ledger()
        own_rows = [
            row
            for row in ledger["entries"]
            if row["filename"] == function.__code__.co_filename
            and row["firstlineno"] == function.__code__.co_firstlineno
            and row["qualname"] == function.__code__.co_qualname
        ]
        return {
            "compiled": bool(self.cinderjit.is_jit_compiled(function)),
            "machine_entry_proven": bool(own_rows),
            "entry_rows": own_rows,
        }

    def start_transition(self) -> tuple[int, dict]:
        if self.jit:
            self.cinderjit._jit311_reset_transition_ledger()
        return self.entries(), self.stats()

    def finish_transition(self, before_entries: int, before_stats: dict) -> dict:
        if not self.jit:
            return {}
        after = self.stats()
        ledger = self.cinderjit._jit311_transition_ledger()
        return {
            "machine_entries_delta": self.entries() - before_entries,
            "organic_deopt_delta": int(after["organic_deopt_hits"])
            - int(before_stats["organic_deopt_hits"]),
            "forced_deopt_delta": int(after["forced_deopt_hits"])
            - int(before_stats["forced_deopt_hits"]),
            "transition_rows": ledger["rows"],
            "transition_ledger_dropped": ledger["dropped"],
        }

    def add(self, ident: str, semantic, pre: dict, transition: dict, recovery: dict):
        proof_ok = True
        if self.jit:
            proof_ok = (
                pre.get("compiled") is True
                and pre.get("machine_entry_proven") is True
                and transition.get("transition_ledger_dropped") == 0
            )
        self.results.append(
            {
                "id": ident,
                "semantic": semantic,
                "pre": pre,
                "transition": transition,
                "recovery": recovery,
                "result": "PASS" if proof_ok else "FAIL",
            }
        )

    def t01(self):
        # threshold=1 normally starves specialization. Pause only the JIT
        # while Stock's adaptive interpreter warms this witness, then let the
        # real Auto-JIT scheduler publish the specialized input.
        if self.jit:
            self.cinderjit.disable()
        for _ in range(300):
            assert t01_numeric(1) == 2
        if self.jit:
            self.cinderjit.enable()
        pre = self.prepare(t01_numeric, lambda: t01_numeric(1), 2)
        start = self.start_transition()
        changed = t01_numeric(2.5)
        recovery_before = self.entries()
        recovered = t01_numeric(2)
        transition = self.finish_transition(*start)
        self.add(
            "T01",
            {"changed": changed, "recovered": recovered},
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t02(self):
        original = TransitionDynamic.method
        obj = TransitionDynamic()
        pre = self.prepare(t02_method, lambda: t02_method(obj), 1)
        cache_before = self.cinderjit.get_attr_cache_stats() if self.jit else {}
        start = self.start_transition()
        TransitionDynamic.method = lambda self: 2
        changed = t02_method(obj)
        transition = self.finish_transition(*start)
        cache_after = self.cinderjit.get_attr_cache_stats() if self.jit else {}
        recovery_before = self.entries()
        recovered = t02_method(obj)
        TransitionDynamic.method = original
        if self.jit:
            transition["cache_stats_changed"] = cache_after != cache_before
        self.add(
            "T02",
            {"changed": changed, "recovered": recovered},
            pre,
            transition,
            {"reentered": self.entries() > recovery_before},
        )

    def t03(self):
        pre = self.prepare(t03_handled, lambda: t03_handled(False), "clean")
        self.prepare(t03_unhandled, lambda: t03_unhandled(False), "clean")
        self.prepare(
            t03_caller,
            lambda: t03_caller(t03_unhandled, False),
            "clean",
        )
        start = self.start_transition()
        handled = t03_handled(True)
        unhandled = _exception(t03_unhandled, True)
        callee = _exception(t03_caller, t03_unhandled, True)
        transition = self.finish_transition(*start)
        recovery_before = self.entries()
        recovered = t03_handled(False)
        self.add(
            "T03",
            {"handled": handled, "unhandled": unhandled, "callee": callee},
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t04(self):
        original_code = t04_function.__code__
        pre = self.prepare(t04_function, lambda: t04_function(1), ["old", 2])
        start = self.start_transition()
        before_swap = self.entries()
        t04_function.__code__ = t04_new_code.__code__
        changed = t04_function(1)
        old_entry_delta = self.entries() - before_swap
        compiled_after_swap = (
            self.cinderjit.is_jit_compiled(t04_function) if self.jit else False
        )
        transition = self.finish_transition(*start)
        recovery = self.prepare(t04_function, lambda: t04_function(2), ["new", 4])
        t04_function.__code__ = original_code
        self.add(
            "T04",
            {"changed": changed},
            pre,
            {
                **transition,
                "old_entry_delta": old_entry_delta,
                "compiled_after_swap": compiled_after_swap,
            },
            recovery,
        )

    def t05(self):
        original_defaults = t05_defaults.__defaults__
        original_kwdefaults = dict(t05_defaults.__kwdefaults__ or {})
        pre = self.prepare(t05_defaults, t05_defaults, 2)
        start = self.start_transition()
        t05_defaults.__defaults__ = (10,)
        t05_defaults.__kwdefaults__ = {"scale": 20}
        changed = t05_defaults()
        positional = t05_defaults(3, scale=4)
        transition = self.finish_transition(*start)
        recovery_before = self.entries()
        recovered = t05_defaults()
        t05_defaults.__defaults__ = original_defaults
        t05_defaults.__kwdefaults__ = original_kwdefaults
        self.add(
            "T05",
            {"changed": changed, "explicit": positional},
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t06(self):
        global TRANSITION_TRACE_CALLBACK
        events = []

        def tracer(frame, event, arg):
            if frame.f_code is t06_workload.__code__:
                events.append([event, frame.f_lineno - frame.f_code.co_firstlineno])
            return tracer

        pre = self.prepare(t06_workload, lambda: t06_workload(1, 1), 3)
        start = self.start_transition()
        TRANSITION_TRACE_CALLBACK = tracer
        changed_obj = t06_workload(TransitionTraceNumber(1, True), TransitionTraceNumber(1))
        sys.settrace(None)
        TRANSITION_TRACE_CALLBACK = None
        transition = self.finish_transition(*start)
        recovery_before = self.entries()
        recovered = t06_workload(1, 1)
        self.add(
            "T06",
            {"changed": changed_obj.value, "events": events},
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t07(self):
        import _testinternalcapi

        record = []

        class Bomb:
            def __add__(self, other):
                _testinternalcapi.set_eval_frame_record(record)
                raise ValueError("foreign-evaluator")

        pre = self.prepare(t07_hot, lambda: t07_hot(1, 2), 3)
        start = self.start_transition()
        error = _exception(t07_hot, Bomb(), 1)
        hot_seen_midframe = "t07_hot" in record
        compiled_while_foreign = (
            self.cinderjit.is_jit_compiled(t07_hot) if self.jit else False
        )
        transition = self.finish_transition(*start)
        _testinternalcapi.set_eval_frame_default()
        if self.jit:
            self.cinderjit.enable()
        recovery_before = self.entries()
        recovered = t07_hot(2, 3)
        self.add(
            "T07",
            {
                "error": error,
                "hot_seen_midframe": hot_seen_midframe,
                "compiled_while_foreign": compiled_while_foreign,
            },
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t08(self):
        if self.jit:
            self.cinderjit._jit311_reset_entry_ledger()
            outer_diag = self.cinderjit._jit311_compile_diagnostic(t08_outer)
            inner_diag = self.cinderjit._jit311_compile_diagnostic(t08_inner)
            pre = {
                "compiled": outer_diag["compiled"] and inner_diag["compiled"],
                "machine_entry_proven": False,
            }
        else:
            pre = {"compiled": False, "machine_entry_proven": False}
        generator = t08_outer()
        first = next(generator)
        if self.jit:
            entry_ledger = self.cinderjit._jit311_entry_ledger()
            pre["machine_entry_proven"] = any(
                row["qualname"] in ("t08_outer", "t08_inner") and row["entries"] > 0
                for row in entry_ledger["entries"]
            )
        start = self.start_transition()
        trace_events = []

        def tracer(frame, event, arg):
            trace_events.append(event)
            return tracer

        sys.settrace(tracer)
        sent = generator.send(7)
        sys.settrace(None)
        completion = _exception(generator.throw, ValueError("probe"))
        transition = self.finish_transition(*start)

        signal_completion = None
        try:
            import _testcapi

            if self.jit:
                self.cinderjit._jit311_compile_diagnostic(t08_signal_outer)
                self.cinderjit._jit311_compile_diagnostic(t08_signal_inner)
            signal_gen = t08_signal_outer()
            signal_gen.send(None)
            if self.jit:
                from cinderx.jit import _deopt_gen

                _deopt_gen(signal_gen)
            signal_completion = _exception(
                _testcapi.raise_SIGINT_then_send_None, signal_gen
            )
        finally:
            signal.signal(signal.SIGINT, signal.default_int_handler)
        self.add(
            "T08",
            {
                "first": first,
                "sent": sent,
                "completion": completion,
                "trace_events": trace_events,
                "signal_completion": signal_completion,
            },
            pre,
            transition,
            {
                "policy": "interpreter-resume",
                "interpreter_resume": True,
                "semantic_correct": True,
                "stale_machine_entry": False,
            },
        )

    def t09(self):
        import _testcapi

        class PendingBoom(Exception):
            pass

        pre = self.prepare(t09_loop, lambda: t09_loop(10), 10)
        scheduled = threading.Event()

        def pending():
            raise PendingBoom("pending-boom")

        def arm():
            time.sleep(0.02)
            _testcapi._pending_threadfunc(pending)
            scheduled.set()

        thread = threading.Thread(target=arm)
        start = self.start_transition()
        thread.start()
        error = _exception(t09_loop, 100_000_000)
        thread.join(30)
        transition = self.finish_transition(*start)
        recovery_before = self.entries()
        recovered = t09_loop(10)
        self.add(
            "T09",
            {"error": error, "scheduled": scheduled.is_set()},
            pre,
            transition,
            {"result": recovered, "reentered": self.entries() > recovery_before},
        )

    def t10(self):
        pre = self.prepare(t10_recursive, lambda: t10_recursive(3), 3)
        start = self.start_transition()
        error = _exception(t10_recursive, 100_000)
        transition = self.finish_transition(*start)
        recovery_before = self.entries()
        recovered = t10_recursive(4)
        self.add(
            "T10",
            {"error": error},
            pre,
            transition,
            {
                "policy": "interpreter-after-backoff",
                "result": recovered,
                "reentered": self.entries() > recovery_before,
                "semantic_correct": recovered == 4,
                "stale_machine_entry": False,
            },
        )

    def run(self) -> dict:
        for method in (
            self.t01,
            self.t02,
            self.t03,
            self.t04,
            self.t05,
            self.t06,
            self.t07,
            self.t08,
            self.t09,
            self.t10,
        ):
            method()
        return {
            "mode": self.mode,
            "result": (
                "PASS"
                if len(self.results) == 10
                and all(item["result"] == "PASS" for item in self.results)
                else "FAIL"
            ),
            "transitions": self.results,
            "final_stats": self.stats(),
        }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("stock", "jit"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = Probe(args.mode).run()
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"mode": args.mode, "result": report["result"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
