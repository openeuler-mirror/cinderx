# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Global pins, static argument binding and super lookup semantics."""

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


KWSTATIC_PROBE = PREAMBLE + """
def target(a, b=2, c=3):
    return (a, b, c)

def call():
    return target(1, c=5)

def other(a, b=1, c=0):
    return ("other", a, b, c)

# Every global the probe assigns exists before the compile, and none is
# removed afterwards, so the deopts observed below come from the binder
# guards alone and not from a global re-pin.  (An `except ... as name`
# handler deletes its name on exit, so the handlers bind none.)
_ = message = deopts = None
assert cinderjit.force_compile(call) is True
for _ in range(100):
    assert call() == (1, 2, 5)
target.__defaults__ = (20, 30)
assert call() == (1, 20, 5)
# The defaults guard fails and the call resumes in the interpreter at
# KW_NAMES: c=5 stays a keyword argument, so b is missing.
target.__defaults__ = (7,)
cinderjit.get_and_clear_runtime_stats()
try:
    call()
except TypeError:
    message = str(sys.exception())
    assert "missing 1 required positional argument: 'b'" in message, message
else:
    raise AssertionError("missing positional accepted after a defaults change")
deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
assert deopts and all(
    "LOAD_GLOBAL" not in entry["normal"]["description"] for entry in deopts
), deopts  # the binder's guard failed, not the global load's
target.__defaults__ = (2, 3)
assert call() == (1, 2, 5)
# The code guard fails the same way: under the new code (a, b, c) with the
# function's own defaults (2, 3), the keyword c=5 lands on c and b takes
# its default -- a positional (1, 5) would have put 5 on b instead.
target.__code__ = other.__code__
cinderjit.get_and_clear_runtime_stats()
assert call() == ("other", 1, 2, 5), call()
deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
assert deopts and all(
    "LOAD_GLOBAL" not in entry["normal"]["description"] for entry in deopts
), deopts
target = lambda a, *, c: ("lambda", a, c)
assert call() == ("lambda", 1, 5), call()
del target
try:
    call()
except NameError:
    pass
else:
    raise AssertionError("deleted global still called")
print("kwstatic ok")
"""


DEFPIN_PROBE = PREAMBLE + """
def target(a, b=2, c=3):
    return (a, b, c)

def call():
    return target(1)

assert cinderjit.force_compile(call) is True
for _ in range(100):
    assert call() == (1, 2, 3)
target.__defaults__ = (20, 30)
assert call() == (1, 20, 30)
target.__defaults__ = None
try:
    call()
except TypeError as exc:
    assert "missing 2 required positional arguments" in str(exc), exc
else:
    raise AssertionError("missing arguments accepted")
target = lambda a, b=0, c=0: ("lambda", a, b, c)
assert call() == ("lambda", 1, 0, 0)

# The same function object re-coded in place: the identity pin and the
# defaults guard both hold for the re-coded function, so the completed call
# guards the code object as well and the new code sees only the arguments
# the caller wrote.  Every helper exists before the compile: a module
# global added afterwards would deopt the pinned load by itself (the
# globals keys version) and prove nothing about the completion.
def target2(a, b=10):
    return (a, b)

def replacement(a, *rest):
    return (a, rest)

def call2():
    return target2(1)

assert cinderjit.force_compile(call2) is True
for _ in range(100):
    assert call2() == (1, 10)
target2.__code__ = replacement.__code__
assert call2() == (1, ()), call2()
for _ in range(50):
    assert call2() == (1, ())
print("defpin ok")
"""


BUILTIN_PIN_PROBE = PREAMBLE + """
import builtins

def f(x):
    return (len(x), isinstance(x, list), type(x))

assert cinderjit.force_compile(f) is True
for _ in range(100):
    assert f([1, 2]) == (2, True, list)
    assert f("abc") == (3, False, str)
real_len = builtins.len
builtins.len = lambda x: 99
try:
    assert f([1, 2]) == (99, True, list), f([1, 2])
finally:
    builtins.len = real_len
assert f([1, 2]) == (2, True, list)
globals()["len"] = lambda x: -1
assert f([1, 2]) == (-1, True, list), f([1, 2])
del globals()["len"]
assert f([1, 2]) == (2, True, list)
print("builtin pin ok")
"""


SUPER_PROBE = PREAMBLE + """
class A:
    def m(self):
        return "A"
    @property
    def p(self):
        return "A.p"
    @classmethod
    def cm(cls):
        return ("A.cm", cls.__name__)

class C(A):
    pass

class B(C):
    def m(self):
        return super().m() + "B"
    def p2(self):
        return super().p
    @classmethod
    def cm(cls):
        return super().cm() + ("B.cm",)

for f in (B.m, B.p2, B.cm.__func__):
    assert cinderjit.force_compile(f) is True

b = B()
for _ in range(100):
    assert b.m() == "AB"
    assert b.p2() == "A.p"
    assert B.cm() == ("A.cm", "B", "B.cm")
C.m = lambda self: "C"
assert b.m() == "CB", b.m()
del C.m
assert b.m() == "AB"
A.m = lambda self: "Ax"
assert b.m() == "AxB"
A.p = property(lambda self: "A.p2")
assert b.p2() == "A.p2"
class D(B):
    pass
assert D().m() == "AxB"

class Sub(B):
    pass
holder = {}
def make_fake():
    class fake:
        def __init__(self, *args):
            pass
        def m(self):
            return "fake"
    return fake
holder["super"] = super
globals()["super"] = make_fake()
try:
    assert b.m() == "fakeB", b.m()
finally:
    globals()["super"] = holder["super"]
assert b.m() == "AxB"
print("super ok")
"""


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 global call optimizations")
class GlobalCallOptimizations311Test(unittest.TestCase):
    def _run(self, probe, marker, extra_env=None, stderr_contains=(),
             stderr_excludes=()):
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
        env.update(extra_env or {})
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(probe)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn(marker, proc.stdout)
        for needle in stderr_contains:
            self.assertIn(needle, proc.stderr)
        for needle in stderr_excludes:
            self.assertNotIn(needle, proc.stderr)


    def test_static_keyword_binding_behind_the_pinned_global(self):
        self._run(
            KWSTATIC_PROBE,
            "kwstatic ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("LOAD_GLOBAL_MODULE func",),
            stderr_excludes=("kwnames",),
        )


    def test_default_completion_behind_the_pinned_global(self):
        self._run(
            DEFPIN_PROBE,
            "defpin ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("LOAD_GLOBAL_MODULE func",),
        )


    def test_builtins_pins_open_len_isinstance_type(self):
        self._run(
            BUILTIN_PIN_PROBE,
            "builtin pin ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("LOAD_GLOBAL_BUILTIN", "GetLength"),
        )


    def test_super_site_cache_tracks_mro_changes(self):
        self._run(SUPER_PROBE, "super ok")


if __name__ == "__main__":
    unittest.main()
