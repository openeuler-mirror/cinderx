# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Instrumentation polls of CPython 3.11 compiled code: a frame that was
running when sys.settrace / sys.setprofile was switched on leaves machine
code where the interpreter would next report an event (the start of a
source line, the return), so the events a tracer sees from the transition
on are the interpreter's; a C trace function's error is delivered at that
next event; a frame that is not traced pays the polls without ever leaving
machine code."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import _cinderx
import cinderx
import sys

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
"""


# Tracing switched on inside a call made from the middle of a compiled
# frame: the events reported for that frame from then on are the
# interpreter's (the next line, the return), and the frame left machine
# code through an Instrumentation deopt.
MIDFLIGHT_EVENTS_PROBE = PREAMBLE + """
TRACER = None

class Number:
    def __init__(self, value, arm=False):
        self.value = value
        self.arm = arm

    def __add__(self, other):
        global TRACER
        if self.arm and TRACER is not None:
            sys.settrace(TRACER)
            sys._getframe().f_back.f_trace = TRACER
            self.arm = False
        return Number(self.value + other.value)

def workload(left, right):
    total = left + right
    return total + right

def run(func):
    global TRACER
    events = []
    def tracer(frame, event, arg):
        if frame.f_code is func.__code__:
            events.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
        return tracer
    TRACER = tracer
    result = func(Number(1, True), Number(1))
    sys.settrace(None)
    TRACER = None
    return result.value, events

# The interpreter's answer, from an uncompiled twin of the same code.
twin = type(workload)(workload.__code__, globals(), "twin")
expected = run(twin)
assert expected == (3, [("line", 2), ("return", 2)]), expected

assert cinderjit.force_compile(workload) is True
for _ in range(20):
    assert workload(Number(1), Number(1)).value == 3
cinderjit.get_and_clear_runtime_stats()
compiled = run(workload)
assert compiled == expected, (compiled, expected)
deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
reasons = {entry["normal"]["reason"] for entry in deopts}
assert "Instrumentation" in reasons, deopts
print("midflight events ok")
"""


# A C trace function registered from inside a compiled frame raises at the
# next event, the next line of that frame: the exception carries the frame
# exactly once (test_sys_settrace.test_testcapi_settrace_error, compiled).
C_TRACE_ERROR_PROBE = PREAMBLE + """
import _testcapi

def body():
    _testcapi.settrace_to_error([])
    len([])
    return "ran past the trace error"

assert cinderjit.force_compile(body) is True
try:
    try:
        result = body()
    except Exception as exc:
        count = 0
        tb = exc.__traceback__
        while tb:
            if tb.tb_frame.f_code is body.__code__:
                count += 1
            tb = tb.tb_next
        assert count == 1, count
    else:
        raise AssertionError(result)
finally:
    sys.settrace(None)
print("c trace error ok")
"""


# Tracing switched on by a callee: the compiled caller reports what the
# interpreter reports from there on -- the line that follows the call and
# its return (the same code interpreted gives the expectation).
MIDFLIGHT_RETURN_PROBE = PREAMBLE + """
events = []
watched = None

def tracer(frame, event, arg):
    if frame.f_code is watched:
        events.append(event)
    return tracer

def switch_on():
    sys.settrace(tracer)
    sys._getframe().f_back.f_trace = tracer

def caller():
    switch_on()
    return 1

def run(func):
    global watched
    watched = func.__code__
    sys.settrace(None)
    events.clear()
    result = func()
    sys.settrace(None)
    return result, list(events)

twin = type(caller)(caller.__code__, globals(), "twin")
expected = run(twin)
assert expected == (1, ["line", "return"]), expected
assert cinderjit.force_compile(caller) is True
for _ in range(20):
    assert run(caller) == expected, (run(caller), expected)
print("midflight return ok")
"""


# Without a tracer the polls never fire: every call runs in machine code
# to completion.
STEADY_STATE_PROBE = PREAMBLE + """
def loop(n):
    total = 0
    for i in range(n):
        total += i
    return total

assert cinderjit.force_compile(loop) is True
stats = _cinderx._get_trigger_stats()
entries = stats["machine_code_entries"]
deopts = stats["organic_deopt_hits"]
for _ in range(2000):
    assert loop(50) == 1225
stats = _cinderx._get_trigger_stats()
assert stats["machine_code_entries"] - entries == 2000, stats
assert stats["organic_deopt_hits"] == deopts, stats
print("steady state ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 instrumentation polls"
)
class InstrumentationPolls311Test(unittest.TestCase):
    def _run(self, probe, marker):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(probe)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn(marker, proc.stdout)

    def test_midflight_tracing_reports_the_interpreter_events(self):
        self._run(MIDFLIGHT_EVENTS_PROBE, "midflight events ok")

    def test_c_trace_error_is_delivered_at_the_next_line(self):
        self._run(C_TRACE_ERROR_PROBE, "c trace error ok")

    def test_midflight_tracing_reports_the_return(self):
        self._run(MIDFLIGHT_RETURN_PROBE, "midflight return ok")

    def test_untraced_frames_never_leave_machine_code(self):
        self._run(STEADY_STATE_PROBE, "steady state ok")


if __name__ == "__main__":
    unittest.main()
