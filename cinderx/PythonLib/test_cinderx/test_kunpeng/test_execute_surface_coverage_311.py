# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Execute-surface coverage added for CPython 3.11: in-function imports
(IMPORT_NAME, and IMPORT_FROM through the 3.11 port of ceval's import_from),
`del obj.attr`, closure-cell and global writes, the and/or short-circuit
jumps, and the rich-compare helper's exact-float and identity fast paths.
Every probe runs the compiled function against the interpreter's answer."""

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


def compiled(fn):
    assert cinderjit.force_compile(fn) is True, fn
    assert cinderjit.is_jit_compiled(fn), fn
    return fn
"""


IMPORT_PROBE = PREAMBLE + """
import types


@compiled
def use_import(x):
    import json
    return json.dumps(x)


@compiled
def use_import_from(a, b):
    from os import path
    return path.join(a, b)


@compiled
def missing_module():
    import module_that_does_not_exist_xyz  # noqa: F401


@compiled
def missing_name():
    from os import name_that_does_not_exist_xyz  # noqa: F401


@compiled
def from_fake(which):
    if which == "sub":
        from fakepkg_xyz import sub  # noqa: F401
        return sub
    from fakepkg_xyz import missing  # noqa: F401


assert use_import([1, "a"]) == '[1, "a"]'
assert use_import_from("a", "b") == "a/b"

try:
    missing_module()
except ModuleNotFoundError as exc:
    assert exc.name == "module_that_does_not_exist_xyz", exc
else:
    raise AssertionError("missing module imported")

try:
    missing_name()
except ImportError as exc:
    assert exc.name == "os", exc
    assert str(exc).startswith("cannot import name 'name_that_does_not_exist_xyz' from 'os'"), exc
    assert exc.path is not None, exc
else:
    raise AssertionError("missing name imported")

# The sys.modules fallback for circular relative imports, and the
# partially-initialized message: both ceval import_from shapes.
pkg = types.ModuleType("fakepkg_xyz")
pkg.__file__ = "/tmp/fakepkg_xyz/__init__.py"
sub = types.ModuleType("fakepkg_xyz.sub")
sys.modules["fakepkg_xyz"] = pkg
sys.modules["fakepkg_xyz.sub"] = sub
assert from_fake("sub") is sub
pkg.__spec__ = types.SimpleNamespace(_initializing=True)
try:
    from_fake("missing")
except ImportError as exc:
    assert "partially initialized module 'fakepkg_xyz'" in str(exc), exc
    assert "circular import" in str(exc), exc
else:
    raise AssertionError("missing attribute imported")
print("imports ok")
"""


COVERAGE_PROBE = PREAMBLE + """
class Box:
    pass


@compiled
def delete_attr(obj):
    del obj.value


box = Box()
box.value = 1
delete_attr(box)
assert not hasattr(box, "value")
try:
    delete_attr(box)
except AttributeError as exc:
    assert "value" in str(exc), exc
else:
    raise AssertionError("second delete succeeded")


def make_counter():
    count = 0

    def inc():
        nonlocal count
        count += 1
        return count

    return inc


inc = make_counter()
compiled(inc)
assert [inc(), inc(), inc()] == [1, 2, 3]

shared = 0


@compiled
def bump_global(n):
    global shared
    shared = shared + n
    return shared


assert bump_global(2) == 2 and bump_global(3) == 5 and shared == 5


class Truthy:
    calls = 0

    def __init__(self, value):
        self.value = value

    def __bool__(self):
        Truthy.calls += 1
        return self.value


@compiled
def both(a, b):
    return a and b


@compiled
def either(a, b):
    return a or b


f, t = Truthy(False), Truthy(True)
assert both(f, t) is f and both(t, f) is f and both(t, t) is t
assert either(f, t) is t and either(t, f) is t and either(f, f) is f
assert Truthy.calls == 6, Truthy.calls
print("coverage ok")
"""


COMPARE_PROBE = PREAMBLE + """
import math
import operator

OPS = [operator.lt, operator.le, operator.eq, operator.ne, operator.gt, operator.ge]


@compiled
def cmp_lt(a, b):
    return a < b


@compiled
def cmp_le(a, b):
    return a <= b


@compiled
def cmp_eq(a, b):
    return a == b


@compiled
def cmp_ne(a, b):
    return a != b


@compiled
def cmp_gt(a, b):
    return a > b


@compiled
def cmp_ge(a, b):
    return a >= b


FUNCS = [cmp_lt, cmp_le, cmp_eq, cmp_ne, cmp_gt, cmp_ge]
nan = float("nan")
values = [0.0, -0.0, 1.5, -2.25, nan, math.inf, -math.inf]
for a in values:
    for b in values:
        for fn, op in zip(FUNCS, OPS):
            assert fn(a, b) is op(a, b), (fn.__name__, a, b)


class Plain:
    pass


class Custom:
    def __init__(self):
        self.calls = 0

    def __eq__(self, other):
        self.calls += 1
        return True


class Sub(Plain):
    def __ne__(self, other):
        return "ne-called"


p, q = Plain(), Plain()
assert cmp_eq(p, p) is True and cmp_eq(p, q) is False
assert cmp_ne(p, q) is True and cmp_ne(p, p) is False
c = Custom()
assert cmp_eq(c, p) is True and cmp_eq(p, c) is True and c.calls == 2
assert cmp_ne(Sub(), p) == "ne-called"
assert cmp_eq(p, 1) is False and cmp_eq(1, 1.0) is True
assert cmp_eq("ab", "a" + "b") is True
print("compare ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 execute surface"
)
class ExecuteSurfaceCoverage311Test(unittest.TestCase):
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
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self.assertIn(marker, proc.stdout)

    def test_in_function_imports_compile_and_raise_like_the_interpreter(self):
        self._run(IMPORT_PROBE, "imports ok")

    def test_delete_attr_cell_and_global_writes_and_short_circuit_jumps(self):
        self._run(COVERAGE_PROBE, "coverage ok")

    def test_rich_compare_fast_paths_match_the_interpreter(self):
        self._run(COMPARE_PROBE, "compare ok")


if __name__ == "__main__":
    unittest.main()
