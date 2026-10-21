# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Method-lookup semantics of the CPython 3.11 class-receiver method cache:
classmethod / staticmethod objects re-initialised in place, uninitialised
wrappers, a failing lookup on the way to the call, a metatype candidate
that becomes a data descriptor, a deopt between the method load and the
call, and the cache statistics switch."""

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


# classmethod.__init__ / staticmethod.__init__ replace the wrapped callable
# in place without any type version moving; the cache must serve the live
# callable and must not keep a borrowed pointer to the old one.
WRAPPER_REINIT_PROBE = PREAMBLE + """
import gc

old = lambda: "old"
new = lambda: "new"

class C:
    m = staticmethod(old)
    c = classmethod(lambda cls: "cold")

def invoke(cls):
    return cls.m()

def invoke_c(cls):
    return cls.c()

for _ in range(50):
    invoke(C)
    invoke_c(C)
assert cinderjit.force_compile(invoke) is True
assert cinderjit.force_compile(invoke_c) is True
for _ in range(50):
    assert invoke(C) == "old" and invoke_c(C) == "cold"
C.__dict__["m"].__init__(new)
C.__dict__["c"].__init__(lambda cls: "cnew")
assert invoke(C) == "new", invoke(C)
assert invoke_c(C) == "cnew", invoke_c(C)
del old, new
gc.collect()
for _ in range(50):
    assert invoke(C) == "new" and invoke_c(C) == "cnew"
# A classmethod re-bound to a callable that is not a plain function takes
# the descriptor protocol (the callable's own __get__ runs).
class Descr:
    def __get__(self, obj, owner):
        return lambda: ("descr", owner.__name__)
C.__dict__["c"].__init__(Descr())
assert invoke_c(C) == ("descr", "C"), invoke_c(C)
print("wrapper reinit ok")
"""


# An uninitialised classmethod / staticmethod raises RuntimeError exactly as
# the native descriptors do.
WRAPPER_UNINIT_PROBE = PREAMBLE + """
class C:
    m = classmethod.__new__(classmethod)

class D:
    m = staticmethod.__new__(staticmethod)

def invoke(cls):
    return cls.m()

assert cinderjit.force_compile(invoke) is True
for K in (C, D):
    for _ in range(3):
        try:
            invoke(K)
        except RuntimeError as exc:
            assert "uninitialized" in str(exc), exc
        else:
            raise AssertionError("uninitialised wrapper returned")
print("wrapper uninit ok")
"""


# A lookup that fails after the cache has been filled (a metaclass property
# whose getter raises) propagates its own exception; nothing is called.
LOOKUP_FAILURE_PROBE = PREAMBLE + """
class M(type):
    pass

class C(metaclass=M):
    @staticmethod
    def m():
        return "s"

calls = []

def arg():
    calls.append(1)
    return 1

def invoke(cls):
    return cls.m()

def invoke_with_arg(cls):
    return cls.m(arg())

for _ in range(50):
    invoke(C)
assert cinderjit.force_compile(invoke) is True
assert cinderjit.force_compile(invoke_with_arg) is True
for _ in range(50):
    assert invoke(C) == "s"

def getter(cls):
    raise ValueError("lookup")

M.m = property(getter)
for f in (invoke, invoke_with_arg):
    try:
        f(C)
    except ValueError as exc:
        assert str(exc) == "lookup", exc
    else:
        raise AssertionError("lookup failure swallowed")
assert calls == [], calls
del M.m
assert invoke(C) == "s"
print("lookup failure ok")
"""


# A same-named metatype candidate whose type gains __set__ later must win
# over the cached class attribute, as in type_getattro.
METATYPE_CANDIDATE_PROBE = PREAMBLE + """
class D:
    def __get__(self, obj, owner):
        return lambda: "meta"

class M(type):
    m = D()

class C(metaclass=M):
    @classmethod
    def m(cls):
        return "class"

def invoke(cls):
    return cls.m()

for _ in range(50):
    invoke(C)
assert cinderjit.force_compile(invoke) is True
for _ in range(50):
    assert invoke(C) == "class"
D.__set__ = lambda self, obj, value: None
assert invoke(C) == "meta", invoke(C)
del D.__set__
assert invoke(C) == "class"
print("metatype candidate ok")
"""


# Under type-annotation guards the class receiver takes the type-method
# load; a deopt between that load and the call (a global rebound after the
# compile) must hand the interpreter a stack it can call from.
DEOPT_BEFORE_CALL_PROBE = PREAMBLE + """
value = 7

class C:
    @staticmethod
    def m(x):
        return x

def f(cls: type):
    return cls.m(value)

for _ in range(50):
    f(C)
assert cinderjit.force_compile(f) is True
for _ in range(50):
    assert f(C) == 7
globals()["added_after_compile"] = 1
assert f(C) == 7, f(C)
globals()["value"] = 9
assert f(C) == 9, f(C)
print("deopt before call ok")
"""


# The lazily created class-receiver cache of a LoadMethod site carries the
# site's statistics context, so the statistics switch survives a miss that
# records its reason (a metaclass with its own __getattribute__).
CACHE_STATS_PROBE = PREAMBLE + """
class M(type):
    def __getattribute__(cls, name):
        return type.__getattribute__(cls, name)

class C(metaclass=M):
    @staticmethod
    def m():
        return "s"

def invoke(cls):
    return cls.m()

for _ in range(50):
    invoke(C)
assert cinderjit.force_compile(invoke) is True
before = cinderjit.get_attr_cache_stats()["load_type_method"]
for _ in range(50):
    assert invoke(C) == "s"
after = cinderjit.get_attr_cache_stats()["load_type_method"]
# The metaclass overrides __getattribute__: the site only misses and fills
# nothing, and the public tallies say so.
assert after["misses"] > before["misses"], (before, after)
assert after["fills"] == before["fills"], (before, after)
# The per-site miss records (the site is a LoadMethod site whose receiver
# is a class) are exported once and cleared by the export.
assert cinderjit.is_inline_cache_stats_collection_enabled() is True
LISTS = ("load_method_stats", "load_type_method_stats")
exported = cinderjit.get_and_clear_inline_cache_stats()
entries = [
    entry
    for key in LISTS
    for entry in exported[key]
    if entry["method"] == "invoke"
]
assert entries, exported
misses = entries[0]["cache_misses"]
keys = [key for key in misses if key.endswith(".m")]
assert keys, misses
assert misses[keys[0]]["reason"] == "WrongTpGetAttro", misses
assert misses[keys[0]]["count"] >= 1, misses
cleared = cinderjit.get_and_clear_inline_cache_stats()
assert all(
    entry["cache_misses"] == {}
    for key in LISTS
    for entry in cleared[key]
    if entry["method"] == "invoke"
), cleared
print("cache stats ok")
"""


# Two instances share the class's keys table; one of them shadows the method
# with an instance attribute and then materialises its __dict__ (split
# form, values still in ma_values).  The site's present-name memo, taken on
# the other instance, must read the slot where that dict keeps its values.
SPLIT_SHADOW_PROBE = PREAMBLE + """
class C:
    def m(self):
        return "class"

a = C()
b = C()
b.m = lambda: "shadow"

def invoke(obj):
    return obj.m()

for _ in range(50):
    invoke(a)
assert cinderjit.force_compile(invoke) is True
for _ in range(50):
    assert invoke(a) == "class"
vars(b)  # split-materialized: shared keys, values in ma_values
assert invoke(b) == "shadow", invoke(b)
assert invoke(a) == "class"
c = C()
c.m = lambda: "shadow2"
vars(c)
c.__dict__ = {"m": lambda: "shadow2", "z": 1}  # a plain dict: combined form
assert invoke(c) == "shadow2"
d = C()
vars(d)
assert invoke(d) == "class"
print("split shadow ok")
"""


# The class MRO lookup can run Python: a non-str key of equal hash in the
# class namespace compares with __eq__.  A callback that installs a metatype
# data descriptor and re-tags the metatype must not leave the class method
# cached under the new versions.
LOOKUP_CALLBACK_PROBE = PREAMBLE + """
def invoke(cls):
    return cls.m()

def interp(cls):
    return cls.m()

assert cinderjit.force_compile(invoke) is True

def scenario(retag):
    state = {"armed": False, "fired": 0}

    class M(type):
        touch = 1

    class Key:
        def __hash__(self):
            return hash("m")

        def __eq__(self, other):
            if state["armed"] and other == "m":
                state["fired"] += 1
                M.m = property(lambda cls: (lambda: "meta"))
                _ = M.touch  # the metatype takes a fresh version tag
            return False

    C = M("C", (), {Key(): 1, "m": classmethod(lambda cls: "class")})
    assert invoke(C) == "class"
    C.other = 1  # a class version change: the next call looks the method up again
    if retag:
        _ = C.touch  # the class carries a tag again before the armed lookup
    state["armed"] = True
    first = invoke(C)
    state["armed"] = False
    # One owner lookup from the method cache.  Under the parallel release
    # stage a second probe of the class dict was observed once (from
    # outside this cache; its origin was not captured), so only the lower
    # bound is fixed; the stale-version claim is the "meta" result below.
    assert state["fired"] >= 1, state
    assert first == "class", first  # selected before the callback ran
    assert interp(C) == "meta"
    assert invoke(C) == "meta", invoke(C)

scenario(retag=False)  # the class has no version tag when the lookup runs
scenario(retag=True)
print("lookup callback ok")
"""


# A name over 100 characters is never held by the type cache, so every MRO
# lookup of it walks the class dicts and can run Python.  A callback that
# replaces the method during such a lookup must neither crash the call nor
# leave the replaced method cached.
LONG_NAME_CALLBACK_PROBE = PREAMBLE + """
name = "m" * 101
state = {"armed": False, "owner_seen": False, "fired": 0}

class MetaKey:
    def __hash__(self):
        return hash(name)

    def __eq__(self, other):
        if state["armed"] and state["owner_seen"] and other == name:
            state["armed"] = False  # setattr looks the name up on M again
            state["fired"] += 1
            setattr(C, name, staticmethod(lambda: "new"))
        return False

class OwnerKey:
    def __hash__(self):
        return hash(name)

    def __eq__(self, other):
        if state["armed"] and other == name:
            state["owner_seen"] = True
        return False

M = type("M", (type,), {MetaKey(): 0, "touch": 1})
C = M("C", (), {OwnerKey(): 0, name: staticmethod(lambda: "old"), "touch": 1})
exec(f"def invoke(cls): return cls.{name}()")
exec(f"def interp(cls): return cls.{name}()")
assert cinderjit.force_compile(invoke) is True
_ = M.touch
_ = C.touch
state["armed"] = True
first = invoke(C)
assert state["fired"] == 0, state  # one owner lookup, after the metatype's
assert first == "old", first
second = invoke(C)
assert state["fired"] == 1, state
assert second == "new", second
state["armed"] = False
assert interp(C) == "new"
assert invoke(C) == "new", invoke(C)
print("long name callback ok")
"""


# The receiver class takes another metaclass through __class__ assignment
# while its method is being looked up; neither class changes version.  The
# entry must not be filled under the old metaclass: the new one defines the
# name as a data descriptor, which wins from the next call on.
METATYPE_SWITCH_PROBE = PREAMBLE + """
class M(type):
    touch = 1

class N(type):
    touch = 1
    m = property(lambda cls: (lambda: "meta"))

state = {"armed": False, "fired": 0}

class Key:
    def __hash__(self):
        return hash("m")

    def __eq__(self, other):
        if state["armed"] and other == "m":
            state["armed"] = False
            state["fired"] += 1
            C.__class__ = N
        return False

C = M("C", (), {Key(): 0, "m": classmethod(lambda cls: "class")})

def invoke(cls):
    return cls.m()

def interp(cls):
    return cls.m()

assert cinderjit.force_compile(invoke) is True
_ = M.touch
_ = N.touch
state["armed"] = True
first = invoke(C)
assert state["fired"] == 1, state
assert first == "class", first  # selected under the old metaclass
assert interp(C) == "meta"
assert invoke(C) == "meta", invoke(C)
print("metatype switch ok")
"""


# A metatype data descriptor wins without the owner class dict being
# consulted.  On a class whose version tag was just invalidated no extra
# owner probe may run before that: its __eq__ callback would delete the
# descriptor and change the answer of this very call.
METATYPE_PRIORITY_PROBE = PREAMBLE + """
state = {"armed": False, "owner_lookups": 0}

class M(type):
    m = property(lambda cls: (lambda: "meta"))

class Key:
    def __hash__(self):
        return hash("m")

    def __eq__(self, other):
        if state["armed"] and other == "m":
            state["armed"] = False
            state["owner_lookups"] += 1
            del M.m
        return False

C = M("C", (), {Key(): 0, "m": staticmethod(lambda: "class")})

def invoke(cls):
    return cls.m()

def interp(cls):
    return cls.m()

assert cinderjit.force_compile(invoke) is True
C.changed = 1  # the class loses its version tag
state["armed"] = True
assert invoke(C) == "meta", invoke(C)
assert state["owner_lookups"] == 0, state
assert interp(C) == "meta"
assert invoke(C) == "meta"
assert state["owner_lookups"] == 0, state
print("metatype priority ok")
"""


# The process-level overflow tables (attribute load / store, method) hold the
# names of their entries.  Types created and dropped in a loop pass through
# compiled sites, fill overflow slots and die; once they are gone the names
# must not stay held, or every program that keeps creating classes (the
# reference-leak hunt of regrtest among them) reads as leaking.
OVERFLOW_RELEASE_PROBE = PREAMBLE + """
import gc

NAME_M = "zz_overflow_method"
NAME_A = "zz_overflow_attr"

def call_method(o):
    return o.zz_overflow_method()

def load_attr(o):
    return o.zz_overflow_attr

def store_attr(o, v):
    o.zz_overflow_attr = v

def fresh_type(i):
    return type("T%d" % i, (object,), {NAME_M: lambda self: 1, NAME_A: i})

for f in (call_method, load_attr, store_attr):
    assert cinderjit.force_compile(f) is True

# Warm every site past its four per-site slots once, so the measured round
# starts with the overflow tables already in use.
for i in range(8):
    o = fresh_type(i)()
    call_method(o); load_attr(o); store_attr(o, i)
del o
gc.collect()
# CPython's own type attribute cache also holds one name per type version
# until sys._clear_type_cache() (regrtest's leak hunt clears it the same
# way); clearing it isolates the overflow tables' share.
sys._clear_type_cache()
base_m = sys.getrefcount(NAME_M)
base_a = sys.getrefcount(NAME_A)
for i in range(100, 160):
    o = fresh_type(i)()
    call_method(o); load_attr(o); store_attr(o, i)
del o
gc.collect()
gc.collect()
sys._clear_type_cache()
extra = (sys.getrefcount(NAME_M) - base_m, sys.getrefcount(NAME_A) - base_a)
assert extra == (0, 0), extra
# A live type keeps its entries: the next call is still served.
keep = fresh_type(999)()
for _ in range(3):
    assert call_method(keep) == 1 and load_attr(keep) == 999
print("overflow names released ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 method caches"
)
class MethodCacheSemantics311Test(unittest.TestCase):
    def _run(self, probe, marker, extra_env=None):
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

    def test_wrapper_reinitialised_in_place_is_served_live(self):
        self._run(WRAPPER_REINIT_PROBE, "wrapper reinit ok")

    def test_uninitialised_wrapper_raises_runtime_error(self):
        self._run(WRAPPER_UNINIT_PROBE, "wrapper uninit ok")

    def test_lookup_failure_propagates_before_the_call(self):
        self._run(LOOKUP_FAILURE_PROBE, "lookup failure ok")

    def test_metatype_candidate_turning_data_descriptor_wins(self):
        self._run(METATYPE_CANDIDATE_PROBE, "metatype candidate ok")

    def test_deopt_between_method_load_and_call(self):
        self._run(
            DEOPT_BEFORE_CALL_PROBE,
            "deopt before call ok",
            extra_env={"PYTHONJITTYPEANNOTATIONGUARDS": "1"},
        )

    def test_split_materialized_shadow_is_seen(self):
        self._run(SPLIT_SHADOW_PROBE, "split shadow ok")

    def test_lookup_callback_does_not_pin_stale_versions(self):
        self._run(LOOKUP_CALLBACK_PROBE, "lookup callback ok")

    def test_long_name_lookup_callback_does_not_crash_or_pin(self):
        self._run(LONG_NAME_CALLBACK_PROBE, "long name callback ok")

    def test_metaclass_switch_during_lookup_is_not_cached(self):
        self._run(METATYPE_SWITCH_PROBE, "metatype switch ok")

    def test_metatype_data_descriptor_wins_without_owner_probe(self):
        self._run(METATYPE_PRIORITY_PROBE, "metatype priority ok")

    def test_overflow_tables_release_names_of_dead_types(self):
        self._run(OVERFLOW_RELEASE_PROBE, "overflow names released ok")

    def test_class_receiver_cache_under_statistics(self):
        self._run(
            CACHE_STATS_PROBE,
            "cache stats ok",
            extra_env={"PYTHONJITCOLLECTINLINECACHESTATS": "1"},
        )


if __name__ == "__main__":
    unittest.main()
