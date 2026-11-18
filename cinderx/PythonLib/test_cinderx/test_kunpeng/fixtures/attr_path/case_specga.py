"""Observable semantics, including warmed caches and hostile dictionary keys."""
import dis
import json
import os
import sys
from pathlib import Path

import _key_probe

cases = {}

def capture(fn):
    try:
        return fn()
    except Exception as error:
        return {"error": type(error).__name__, "message": str(error)}

def warm(fn, obj, count=2000):
    for _ in range(count):
        fn(obj)

def attr(o): return o.x
def method(o): return o.f()

class Hook:
    def __getattr__(self, name):
        return "fallback:" + name
    def f(self):
        return "method"

obj = Hook()
original_getattr = Hook.__getattr__
obj.x = 11
warm(attr, obj)
cases["instance_value"] = attr(obj)
vars(obj)
warm(attr, obj)
cases["materialized_split"] = attr(obj)
obj.x = 12
cases["value_reassignment"] = attr(obj)
del obj.x
cases["delete_to_fallback"] = attr(obj)
Hook.__getattr__ = lambda self, name: "changed:" + name
cases["replace_getattr"] = attr(obj)
Hook.__getattr__ = original_getattr
cases["restore_getattr_first_read"] = attr(obj)
obj.x = 13
warm(attr, obj)
Hook.__getattribute__ = lambda self, name: 99 if name == "x" else object.__getattribute__(self, name)
cases["replace_getattribute"] = attr(obj)
del Hook.__getattribute__
cases["restore_getattribute_first_read"] = attr(obj)
warm(attr, obj)
Hook.x = property(lambda self: 14)
cases["late_data_descriptor"] = attr(obj)
del Hook.x
cases["remove_descriptor_first_read"] = attr(obj)

class Alternate(Hook):
    @property
    def x(self):
        return 15
warm(attr, obj)
obj.__class__ = Alternate
cases["change_class"] = attr(obj)
obj.__class__ = Hook
cases["restore_class_first_read"] = attr(obj)
original_dict = obj.__dict__
obj.__dict__ = {"x": 16}
assert _key_probe.dict_layout(obj.__dict__) == "combined_unicode"
cases["replace_dict_first_read"] = attr(obj)
warm(attr, obj)
cases["replace_dict"] = attr(obj)
obj.__dict__ = original_dict
cases["restore_dict_first_read"] = attr(obj)
obj.__dict__ = {"x": 17, "f": lambda: "instance-method"}
cases["method_instance_shadow"] = method(obj)
del obj.__dict__["f"]
cases["method_unshadow_first_call"] = method(obj)
warm(method, obj)
cases["method_unshadow"] = method(obj)
obj.f = lambda: "late-shadow"
cases["method_late_shadow"] = method(obj)

class Raise(Hook):
    @property
    def x(self):
        raise AttributeError("inside-property")
cases["property_attribute_error"] = attr(Raise())
class RaiseOther(Hook):
    @property
    def x(self):
        raise ValueError("inside-property")
cases["property_other_error"] = capture(lambda: attr(RaiseOther()))

class Slots:
    __slots__ = ("x",)
    def __getattr__(self, name): return "unset:" + name
    def f(self): return 20
s = Slots()
s.x = 18
warm(attr, s)
cases["slot_set"] = attr(s)
del s.x
cases["slot_unset"] = attr(s)
warm(method, s)
cases["method_no_dict"] = method(s)

class BaseA:
    def f(self): return "A"
class BaseB:
    def f(self): return "B"
class Derived(BaseA, Hook): pass
d = Derived()
warm(method, d)
Derived.__bases__ = (BaseB, Hook)
cases["change_bases"] = method(d)
Derived.__bases__ = (BaseA, Hook)
cases["restore_bases_first_call"] = method(d)

class Custom(Hook):
    def __getattribute__(self, name):
        return 123 if name == "x" else object.__getattribute__(self, name)
c = Custom()
c.__dict__["x"] = 999
warm(attr, c)
cases["custom_getattribute"] = attr(c)
poly = [Hook(), Custom(), Slots()]
poly[0].x = 21
poly[2].x = 22
cases["polymorphic"] = [attr(poly[i % 3]) for i in range(12)]

# Replace a warm hinted dictionary by one with a colliding non-string key
# inserted BEFORE the real attribute. Equality calls are observable even if
# the identity-matching attribute happens to occupy the old hint index.
for raises in (False, True):
    events = []
    class Collision:
        def __hash__(self): return hash("x")
        def __eq__(self, other):
            events.append(str(other))
            if raises: raise LookupError("collision")
            return False
    o = Hook()
    o.pad = 0
    o.x = 23
    vars(o)
    warm(attr, o)
    # Build while equality is benign; turn error mode on only for reads.
    desired = raises
    raises = False
    dictionary = {Collision(): 0, "x": 24}
    raises = desired
    o.__dict__ = dictionary
    assert _key_probe.dict_layout(o.__dict__) == "combined_general"
    events.clear()
    first = capture(lambda: attr(o))
    # Attempt enough reads to trigger specialization retries.
    results = [capture(lambda: attr(o)) for _ in range(100)]
    cases["mixed_keys_error" if desired else "mixed_keys"] = {
        "first": first, "last": results[-1], "comparisons": len(events)}

# A string subclass can have equality side effects as well.
events = []
class StringKey(str):
    __hash__ = str.__hash__
    def __eq__(self, other):
        events.append(str(other))
        return super().__eq__(other)
o = Hook()
o.__dict__ = {StringKey("x"): 25}
values = [attr(o) for _ in range(100)]
cases["string_subclass_key"] = [values[-1], len(events)]

trace = []
o = Hook()
o.x = 26
warm(attr, o)
def tracer(frame, event, arg):
    if frame.f_code in (attr.__code__, method.__code__):
        trace.append((frame.f_code.co_name, event))
    return tracer
sys.settrace(tracer)
try:
    result = [attr(o), method(o)]
finally:
    sys.settrace(None)
cases["trace"] = [result, trace]

class Materialized(Hook):
    def __init__(self):
        self.x = 27
        vars(self)


class InlineValues(Hook):
    pass


def split_read(value):
    return value.x


def inline_read(value):
    return value.x


def slot_read(value):
    return value.x


def dict_method(value):
    return value.f()


def nodict_method(value):
    return value.f()


def inline_method(value):
    return value.f()


def attribute_caches(fn):
    instructions = dis.get_instructions(fn, adaptive=True, show_caches=True)
    caches = []
    active = False
    for instruction in instructions:
        if instruction.opname != "CACHE":
            active = instruction.opname.startswith(("LOAD_ATTR", "LOAD_METHOD"))
        if active:
            caches.append((instruction.opname, instruction.argrepr))
    return caches


materialized = Materialized()
assert _key_probe.dict_layout(materialized.__dict__) == "split"
inline = InlineValues()
inline.x = 30
slot = Slots()
slot.x = 31

cx = None
split_enabled = False
if os.environ["ATTR_PATH_MODE"] == "cinderx":
    import _cinderx as cx
    split_enabled = bool(cx._attr_path_status()["split_dict"])
    assert split_enabled == (os.environ.get("ATTR_PATH_SPLIT_HINT", "1") == "1")


def check_path(fn, value, expected):
    warm(fn, value)
    if cx is None:
        return
    before = attribute_caches(fn)
    hits = cx._attr_path_counters()["attribute_hits"]
    warm(fn, value, 16)
    generic_hits = cx._attr_path_counters()["attribute_hits"] - hits
    after = attribute_caches(fn)
    if fn is split_read:
        assert generic_hits == (0 if split_enabled else 16), (split_enabled, generic_hits, after)
        if not split_enabled:
            return
    assert expected in [opname for opname, _ in after], (fn.__name__, after)
    # The specialized opcode must execute without consuming miss counters.
    assert before == after, (fn.__name__, before, after)


for fn, value, expected in (
    (split_read, materialized, "LOAD_ATTR_WITH_HINT"),
    (inline_read, inline, "LOAD_ATTR_INSTANCE_VALUE"),
    (slot_read, slot, "LOAD_ATTR_SLOT"),
    (dict_method, materialized, "LOAD_METHOD_WITH_DICT"),
    (nodict_method, slot, "LOAD_METHOD_NO_DICT"),
    (inline_method, inline, "LOAD_METHOD_WITH_VALUES"),
):
    check_path(fn, value, expected)

original_dict = materialized.__dict__
materialized.__dict__ = {"x": 28}
cases["warmed_split_replaced_first_read"] = split_read(materialized)
materialized.__dict__ = original_dict
cases["warmed_split_restored_first_read"] = split_read(materialized)
check_path(split_read, materialized, "LOAD_ATTR_WITH_HINT")
cases["warmed_split_restored_rewarmed"] = split_read(materialized)

if "_cinderx" in sys.modules:
    import _cinderx
    assert not any(_cinderx._get_trigger_stats().values())

Path(os.environ["ATTR_PATH_TEST_OUTPUT"]).write_text(json.dumps(cases), encoding="utf-8")
