# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Attribute-protocol semantics of the CPython 3.11 attribute caches under
reentrancy and late shape changes: the AArch64 descriptor stub under a
__getattr__ hook, the shared overflow table overwritten from inside a
lookup, the metatype hook pinned across a descriptor call, the late-added
kDict fill next to a values-live sibling, the specialized STORE_ATTR HIR
paths, and the class-attribute constant fold under a payload __class__
change."""

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


# A property's AttributeError on a receiver whose type carries __getattr__
# routes to the hook; the inline stub must hand such receivers to the helper
# before the getter runs (the getter has a side effect counter: twice is a
# failure as much as never).
HOOK_DESCRIPTOR_PROBE = PREAMBLE + """
state = {"miss": False, "getter_calls": 0}

class C:
    @property
    def value(self):
        state["getter_calls"] += 1
        if state["miss"]:
            raise AttributeError("getter miss")
        return 7

    def __getattr__(self, name):
        return ("fallback", name)

def read(obj):
    return obj.value

assert cinderjit.force_compile(read) is True
c = C()
assert read(c) == 7
for _ in range(50):
    assert read(c) == 7
state["miss"] = True
state["getter_calls"] = 0
assert read(c) == ("fallback", "value"), read(c)
assert state["getter_calls"] == 1, state
for _ in range(50):
    assert read(c) == ("fallback", "value")
assert state["getter_calls"] == 51, state
state["miss"] = False
assert read(c) == 7
print("hook descriptor ok")
"""


# Two (type, name) pairs that share a slot of the process-wide overflow
# table: the inner read, run from a dict key's __eq__ during the outer
# read's instance probe, refills the slot; the outer read must still answer
# with the descriptor it entered with.
OVERFLOW_REENTRANCY_PROBE = PREAMBLE + """
M64 = (1 << 64) - 1

def mega_index(tp, name):
    h = ((id(tp) >> 4) * 0x9E3779B97F4A7C15) & M64
    h ^= ((id(name) >> 4) * 0xC2B2AE3D27D4EB4F) & M64
    return (h >> 40) & 2047

class D:
    def __init__(self, tag):
        self.tag = tag

    def __get__(self, obj, owner):
        return self.tag

def read_x(obj):
    return obj.x

def read_y(obj):
    return obj.y

assert cinderjit.force_compile(read_x) is True
assert cinderjit.force_compile(read_y) is True

# Fill both sites' per-site entries so the classes below land in the
# overflow table.
fillers = []
for site, name in ((read_x, "x"), (read_y, "y")):
    for i in range(4):
        cls = type("F%s%d" % (name, i), (), {name: i})
        fillers.append(cls)
        assert site(cls()) == i

name_x = read_x.__code__.co_names[0]
name_y = read_y.__code__.co_names[0]
assert name_x == "x" and name_y == "y"

class A:
    x = D("A.x")

target = mega_index(A, name_x)
keep = []
B = None
for i in range(60000):
    cls = type("B%d" % i, (), {"y": D("B.y")})
    keep.append(cls)
    if mega_index(cls, name_y) == target:
        B = cls
        break
assert B is not None, "no overflow-slot collision found"

a = A()
b = B()
assert read_y(b) == "B.y"
hits = {"eq": 0}

class Key:
    def __hash__(self):
        return hash(name_x)

    def __eq__(self, other):
        hits["eq"] += 1
        # Refill the shared slot while the outer read_x is in flight.
        assert read_y(b) == "B.y"
        return False

a.__dict__[Key()] = 1  # general keys: the instance probe compares keys
assert read_x(a) == "A.x"
before = hits["eq"]
for _ in range(20):
    assert read_x(a) == "A.x", read_x(a)
assert hits["eq"] > before, hits
print("overflow reentrancy ok")
"""


# The metatype __getattr__ hook is pinned before the class attribute's
# descriptor runs; replacing the hook from inside the descriptor affects the
# next access, not this one.
METATYPE_HOOK_PROBE = PREAMBLE + """
armed = {"on": False}

class M(type):
    def __getattr__(cls, name):
        return "old:" + name

    @property
    def x(cls):
        if armed["on"]:
            M.__getattr__ = lambda cls, name: "new:" + name
            raise AttributeError("descriptor raised")
        return 17

class C(metaclass=M):
    pass

def read(cls):
    return cls.x

assert cinderjit.force_compile(read) is True
for _ in range(20):
    assert read(C) == 17
armed["on"] = True
assert read(C) == "old:x", read(C)
assert read(C) == "new:x", read(C)
print("metatype hook ok")
"""


# A late-added attribute living in one instance's materialized combined
# dict fills kDict; a sibling that still uses inline values and gains the
# attribute afterwards reads its own value through the same site.
LATE_DICT_SIBLING_PROBE = PREAMBLE + """
class C:
    pass

a = C()
b = C()
a.__dict__[1] = 0
a.x = 1

def read(obj):
    return obj.x

assert cinderjit.force_compile(read) is True
for _ in range(20):
    assert read(a) == 1
b.x = 2
assert read(b) == 2
for _ in range(20):
    assert read(a) == 1
    assert read(b) == 2
c = C()
try:
    read(c)
except AttributeError:
    pass
else:
    raise AssertionError("missing attribute was returned")
print("late dict sibling ok")
"""


# The specialized STORE_ATTR forms reach the builder.  Each probe compiles
# exactly one function under PYTHONJITDUMPFINALHIR so the dump on stderr is
# that function's HIR: a site quickened to STORE_ATTR_INSTANCE_VALUE carries
# the inline overwrite (StoreField) and the insert helper call (CallStatic);
# a site quickened to STORE_ATTR_WITH_HINT carries the hint helper call.
# Both keep the generic StoreAttr as their slow path.
STORE_ATTR_VALUES_PROBE = PREAMBLE + """
class Box:
    def __init__(self):
        self.x = 0

def store_values(obj, value):
    obj.x = value

values_box = Box()
for i in range(100):
    store_values(values_box, i)
assert cinderjit.force_compile(store_values) is True

sentinel = object()
for i in range(100):
    store_values(values_box, i)
    assert values_box.x == i
store_values(values_box, sentinel)
assert values_box.x is sentinel
refs = sys.getrefcount(sentinel)
for _ in range(100):
    store_values(values_box, sentinel)
assert sys.getrefcount(sentinel) == refs
fresh = Box.__new__(Box)
store_values(fresh, 5)  # insert into an unset slot
assert fresh.x == 5
store_values(fresh, 6)
assert fresh.x == 6
print("store attr values ok")
"""

STORE_ATTR_HINT_PROBE = PREAMBLE + """
class Box:
    def __init__(self):
        self.x = 0

def store_hint(obj, value):
    obj.x = value

# A combined dict with unicode keys: the layout the hint helper serves in
# place (the general-keys layout below is the helper's refusal + fallback).
hint_box = Box()
hint_box.__dict__ = {"x": 0}
for i in range(100):
    store_hint(hint_box, i)
assert cinderjit.force_compile(store_hint) is True

sentinel = object()
for i in range(100):
    store_hint(hint_box, i)
    assert hint_box.x == i
store_hint(hint_box, sentinel)
assert hint_box.x is sentinel
refs = sys.getrefcount(sentinel)
for _ in range(100):
    store_hint(hint_box, sentinel)
assert sys.getrefcount(sentinel) == refs
old = object()
store_hint(hint_box, old)
old_refs = sys.getrefcount(old)
store_hint(hint_box, sentinel)
assert sys.getrefcount(old) == old_refs - 1  # the replaced value is released
assert list(hint_box.__dict__) == ["x"] and hint_box.__dict__["x"] is sentinel
hint_box.__dict__["y"] = 1  # a key added behind the hint: the index moves
store_hint(hint_box, 7)
assert hint_box.__dict__ == {"x": 7, "y": 1}

general_box = Box()
general_box.__dict__[42] = 0  # general keys: the helper refuses, fallback
del general_box.__dict__[42]
for i in range(100):
    store_hint(general_box, i)
    assert general_box.x == i
other = Box()
store_hint(other, 9)
assert other.x == 9
print("store attr hint ok")
"""


# A class attribute folded to a constant behind the owner's version is also
# guarded on the payload's live type: a __class__ assignment that turns the
# payload into a descriptor changes neither version tag.
CLASS_CONST_PAYLOAD_PROBE = PREAMBLE + """
import types

class Value:
    pass

class Descriptor:
    def __get__(self, instance, owner):
        return 42

value = Value()

class Owner:
    x = value

def template():
    return None.x

code = template.__code__.replace(co_consts=(Owner,))
read = types.FunctionType(code, globals(), "read")
assert read() is value
assert cinderjit.force_compile(read) is True
for _ in range(20):
    assert read() is value
value.__class__ = Descriptor
assert read() == 42, read()
assert Owner.x == 42
value.__class__ = Value
assert read() is value
print("class const payload ok")
"""


# A temporary receiver (`factory().x = value`) through the inline
# STORE_ATTR_INSTANCE_VALUE form: stock stores the value, releases the old
# one, then releases the receiver, so a __del__ observes the new value.
STORE_ATTR_TEMPORARY_RECEIVER_PROBE = PREAMBLE + """
import gc

log = []

class Box:
    def __init__(self):
        self.x = 0

    def __del__(self):
        log.append(("del", self.x))

class Old:
    def __init__(self, box):
        self.box = box

    def __del__(self):
        # The receiver is still alive and already carries the new value.
        log.append(("old", self.box.x))

def write(factory, value):
    factory().x = value

keep = [Box() for _ in range(4)]
for i in range(200):
    write(lambda b=keep[i % 4]: b, i)
log.clear()
assert cinderjit.force_compile(write) is True
for i in range(1, 6):
    write(Box, 100 + i)
assert log == [("del", 101), ("del", 102), ("del", 103), ("del", 104),
               ("del", 105)], log
log.clear()
box = Box()
box.x = Old(box)
write(lambda: box, "new")
assert log == [("old", "new")], log
del box
gc.collect()
assert log[-1] == ("del", "new"), log

# The same order through the STORE_ATTR_SLOT form: the old value's
# finalizer still finds the receiver alive (weak reference).
import weakref

seen = []

class SlotOld:
    def __init__(self, owner):
        self.owner = weakref.ref(owner)

    def __del__(self):
        seen.append(self.owner() is not None)

class SlotBox:
    __slots__ = ("x", "__weakref__")

    def __init__(self):
        self.x = SlotOld(self)

def write_slot(factory, value):
    factory().x = value

keep_slots = [SlotBox() for _ in range(4)]
for i in range(200):
    write_slot(lambda b=keep_slots[i % 4]: b, i)
seen.clear()
assert cinderjit.force_compile(write_slot) is True
for i in range(5):
    write_slot(SlotBox, i)
assert seen == [True] * 5, seen
print("temporary receiver ok")
"""


# A class attribute served from the type-receiver cache while the metatype
# holds a same-named non-data descriptor: that descriptor's type gaining
# __set__ later changes neither the metatype nor the class version, and the
# metatype candidate must then win as in type_getattro.
METATYPE_CANDIDATE_PROBE = PREAMBLE + """
class MetaDescr:
    def __get__(self, obj, owner):
        return "meta"

class M(type):
    x = MetaDescr()

class C(metaclass=M):
    @classmethod
    def x(cls):
        return "class"

class Plain(metaclass=M):
    x = "plain"

def read(cls):
    return cls.x

assert cinderjit.force_compile(read) is True
for _ in range(50):
    assert read(C)() == "class"
    assert read(Plain) == "plain"
MetaDescr.__set__ = lambda self, obj, value: None
assert read(C) == "meta", read(C)
assert read(Plain) == "meta", read(Plain)
del MetaDescr.__set__
assert read(C)() == "class"
assert read(Plain) == "plain"

# A candidate of an immutable type may still change type: a module object
# may take a module subclass through __class__ assignment.
import types

candidate = types.ModuleType("candidate")

class M2(type):
    x = candidate

class D(metaclass=M2):
    @classmethod
    def x(cls):
        return "class"

class ModuleDescr(types.ModuleType):
    __slots__ = ()

    def __get__(self, obj, owner):
        return "meta"

    def __set__(self, obj, value):
        pass

for _ in range(50):
    assert read(D)() == "class"
candidate.__class__ = ModuleDescr
assert read(D) == "meta", read(D)
candidate.__class__ = types.ModuleType
assert read(D)() == "class"
print("metatype candidate ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 attribute caches"
)
class AttrCacheSemantics311Test(unittest.TestCase):
    def _run(self, probe, marker, extra_env=None, stderr_contains=()):
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

    def test_descriptor_stub_routes_attribute_error_to_the_hook(self):
        self._run(HOOK_DESCRIPTOR_PROBE, "hook descriptor ok")

    def test_overflow_entry_refilled_during_the_lookup(self):
        self._run(OVERFLOW_REENTRANCY_PROBE, "overflow reentrancy ok")

    def test_metatype_hook_pinned_across_the_descriptor(self):
        self._run(METATYPE_HOOK_PROBE, "metatype hook ok")

    def test_late_dict_fill_next_to_a_values_sibling(self):
        self._run(LATE_DICT_SIBLING_PROBE, "late dict sibling ok")

    def test_store_attr_instance_value_path_is_emitted(self):
        self._run(
            STORE_ATTR_VALUES_PROBE,
            "store attr values ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("StoreField", "CallStatic", "StoreAttr"),
        )

    def test_store_attr_with_hint_path_is_emitted(self):
        self._run(
            STORE_ATTR_HINT_PROBE,
            "store attr hint ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("CallStatic", "StoreAttr"),
        )

    def test_class_constant_fold_guards_the_payload_type(self):
        self._run(CLASS_CONST_PAYLOAD_PROBE, "class const payload ok")

    def test_inline_store_keeps_a_temporary_receiver_alive(self):
        # The HIR dump proves both specialised stores were taken: the values
        # form (a StoreField on the BitCast values array) and the slot form
        # (its version guard is described as STORE_ATTR_SLOT).
        self._run(
            STORE_ATTR_TEMPORARY_RECEIVER_PROBE,
            "temporary receiver ok",
            extra_env={"PYTHONJITDUMPFINALHIR": "1"},
            stderr_contains=("STORE_ATTR_SLOT", "BitCast", "StoreField<x@0>"),
        )

    def test_metatype_candidate_turning_data_descriptor_wins(self):
        self._run(METATYPE_CANDIDATE_PROBE, "metatype candidate ok")


if __name__ == "__main__":
    unittest.main()
