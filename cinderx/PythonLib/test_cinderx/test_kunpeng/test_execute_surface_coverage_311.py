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


IMPORT_BUILTINS_PROBE = PREAMBLE + """
import builtins
import types

sentinel = object()


# One code object per namespace: an artifact belongs to one (code, globals,
# builtins) key, and a second namespace over the same code is refused.
def template_plain():
    import math
    return math


def template_custom():
    import math
    return math


def template_bare():
    import math
    return math


custom_builtins = dict(vars(builtins))
custom_builtins["__import__"] = lambda *args, **kwargs: sentinel
with_custom = types.FunctionType(
    template_custom.__code__,
    {"__builtins__": custom_builtins, "__name__": "custom_scope"},
)
without_import = types.FunctionType(
    template_bare.__code__,
    {"__builtins__": {k: v for k, v in vars(builtins).items() if k != "__import__"},
     "__name__": "bare_scope"},
)
compiled(template_plain)
compiled(with_custom)
compiled(without_import)

# The evaluator resolves __import__ from the frame's f_builtins: a function
# over a private builtins dict sees its own __import__, and one whose
# builtins lack it raises ImportError.
assert template_plain() is __import__("math")
assert with_custom() is sentinel, with_custom()
try:
    without_import()
except ImportError as exc:
    assert "__import__ not found" in str(exc), exc
else:
    raise AssertionError("import without __import__ succeeded")
for fn in (template_plain, with_custom, without_import):
    assert cinderjit.is_jit_compiled(fn), fn


# The evaluator hands __import__ the frame's locals dict when one has been
# materialized (a prior f_locals read) and None otherwise.
def template_locals(probe_locals):
    if probe_locals:
        seen = sys._getframe().f_locals
    else:
        seen = None
    import math
    return math, seen


received = []
locals_builtins = dict(vars(builtins))
locals_builtins["__import__"] = (
    lambda name, g=None, local_ns=None, fromlist=(), level=0: received.append(local_ns) or sentinel
)
with_locals = types.FunctionType(
    template_locals.__code__,
    {"__builtins__": locals_builtins, "__name__": "locals_scope", "sys": sys},
)
compiled(with_locals)
for probe_locals in (False, True, False, True):
    module, seen = with_locals(probe_locals)
    assert module is sentinel
    if probe_locals:
        # Identity is the contract: the hook gets the dict the frame already
        # materialized.  (Its contents for a live frame without exception
        # edges follow the execute surface's frame-observability rules,
        # covered by test_exception_handler_semantics_311.)
        assert received[-1] is seen and isinstance(seen, dict), (received[-1], seen)
    else:
        assert received[-1] is None, received[-1]
assert cinderjit.is_jit_compiled(with_locals)


# The __import__ lookup itself can run Python: a non-string key that hashes
# and compares equal to "__import__".  (a) An exception raised by the key's
# comparison propagates unchanged (not turned into "__import__ not found");
# (b) the key's comparison may be the first to read the importing frame's
# f_locals, and __import__ then receives that freshly materialized dict.
exposed = []


def template_key():
    import placeholder_mod
    return placeholder_mod


key_code = template_key.__code__


class ImportKey:
    def __init__(self, fail):
        self.fail = fail

    def __hash__(self):
        return hash("__import__")

    def __eq__(self, other):
        if other != "__import__":
            return False
        if self.fail:
            raise RuntimeError("lookup failed")
        frame = sys._getframe(1)
        if frame.f_code is key_code:
            exposed.append(frame.f_locals)
        return True


def key_importer(name, g=None, local_ns=None, fromlist=(), level=0):
    return local_ns


def key_builtins(fail):
    d = dict(vars(builtins))
    del d["__import__"]
    d[ImportKey(fail)] = key_importer
    return d


raising_key = types.FunctionType(
    template_key.__code__,
    {"__builtins__": key_builtins(True), "__name__": "raising_key_scope", "sys": sys},
)
compiled(raising_key)
try:
    raising_key()
except RuntimeError as exc:
    assert str(exc) == "lookup failed", exc
else:
    raise AssertionError("lookup exception was swallowed")
assert cinderjit.is_jit_compiled(raising_key)

# A second namespace over the same code is refused, so the materialization
# probe gets its own code object.
def template_key2():
    import placeholder_mod
    return placeholder_mod


key_code = template_key2.__code__
materializing_key = types.FunctionType(
    template_key2.__code__,
    {"__builtins__": key_builtins(False), "__name__": "materializing_key_scope", "sys": sys},
)
compiled(materializing_key)
result = materializing_key()
assert exposed and result is exposed[-1] and isinstance(result, dict), (result, exposed)
assert cinderjit.is_jit_compiled(materializing_key)
print("import builtins ok")
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

    def test_import_name_uses_the_functions_own_builtins(self):
        self._run(IMPORT_BUILTINS_PROBE, "import builtins ok")

    def test_delete_attr_cell_and_global_writes_and_short_circuit_jumps(self):
        self._run(COVERAGE_PROBE, "coverage ok")

    def test_rich_compare_fast_paths_match_the_interpreter(self):
        self._run(COMPARE_PROBE, "compare ok")


if __name__ == "__main__":
    unittest.main()
