"""Global-name cache hits, version invalidation and ownership."""

import gc
import os
import sys
import types
import weakref

import _cinderx as cx
import _key_probe as probe


cx.install_frame_evaluator()
assert cx._attr_path_status()["global_cache"]


def lookup():
    return token


def warm(fn):
    return probe.call_n(fn, 60)


builtins = {"token": 7}
namespace = {"__builtins__": builtins}
read = types.FunctionType(lookup.__code__, namespace)
assert warm(read) == 7
before = cx._attr_path_counters()["global_hits"]
assert warm(read) == 7
assert cx._attr_path_counters()["global_hits"] >= before + 50
assert probe.snapshot(builtins)["version"] >= 2**31

builtins["token"] = 9
assert read() == 9
namespace["token"] = 13
assert read() == 13
del namespace["token"]
assert read() == 9
del builtins["token"]
try:
    read()
except NameError:
    pass
else:
    raise AssertionError("NameError lost")
builtins["token"] = 17
assert read() == 17

# Code identity alone cannot distinguish different globals or builtins.
other_builtins = {"token": 19}
namespace["__builtins__"] = other_builtins
read_other_builtins = types.FunctionType(lookup.__code__, namespace)
other_namespace = {"__builtins__": other_builtins, "token": 23}
read_other_globals = types.FunctionType(lookup.__code__, other_namespace)
for _ in range(60):
    assert (read(), read_other_builtins(), read_other_globals()) == (17, 19, 23)


def write():
    global token
    token += 1


# STORE_GLOBAL must invalidate the borrowed cached value.
assert warm(read_other_globals) == 23
types.FunctionType(write.__code__, other_namespace)()
assert read_other_globals() == 24


class Value:
    pass


other_namespace["token"] = value = Value()
assert warm(read_other_globals) is value
ref = weakref.ref(value)
del value
del other_namespace["token"]
gc.collect()
assert ref() is None
assert read_other_globals() == 19

events = []


class Key:
    def __hash__(self):
        return hash("token")

    def __eq__(self, other):
        events.append(other)
        return other == "token"


mixed = types.FunctionType(lookup.__code__, {"__builtins__": {Key(): 29}})
assert warm(mixed) == 29
assert events == ["token"] * 60


class BuiltinsDict(dict):
    def __getitem__(self, key):
        return 31 if key == "token" else super().__getitem__(key)


subclass = types.FunctionType(lookup.__code__, {"__builtins__": BuiltinsDict(token=0)})
assert warm(subclass) == 31


def trace(frame, event, arg):
    if frame.f_code is lookup.__code__ and event == "call":
        other_namespace["token"] = 37
    return trace


sys.settrace(trace)
try:
    assert read_other_globals() == 37
finally:
    sys.settrace(None)

if hasattr(os, "fork"):
    pid = os.fork()
    if pid == 0:
        other_namespace["token"] = 41
        os._exit(0 if read_other_globals() == 41 else 1)
    assert os.waitpid(pid, 0)[1] == 0
    assert read_other_globals() == 37

cx.remove_frame_evaluator()
other_namespace["token"] = 43
assert read_other_globals() == 43
cx.install_frame_evaluator()
assert read_other_globals() == 43
assert not any(cx._get_trigger_stats().values())
