# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Runtime dispatch, binding and lifetime semantics for CPython 3.11."""

import json
import os
import signal
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


# Construction through a compiled site: the plain-class fast path must
# produce what type_call -> object_new -> slot_tp_init produces, including
# the TypeError for a non-None __init__ result, and must step aside for an
# overridden __new__, an abstract class, a replaced __init__ and keywords.
CTOR_PROBE = PREAMBLE + """
import abc

class P:
    def __init__(self, a, b=10):
        self.a = a
        self.b = b

class BadInit:
    def __init__(self):
        return 1

class Other:
    pass

class NewOverride:
    def __new__(cls, *args):
        return Other()
    def __init__(self, *args):
        raise AssertionError("__init__ must not run for a foreign instance")

class Abstract(abc.ABC):
    @abc.abstractmethod
    def m(self):
        pass

class Slotted:
    __slots__ = ("v",)
    def __init__(self, v):
        self.v = v

def make2(a, b):
    return P(a, b)

def make1(a):
    return P(a)

def make_kw(a):
    return P(b=a, a=a)

def construct(cls, *args):
    return cls(*args)

def construct_fixed(cls, x):
    return cls(x)

for f in (make2, make1, make_kw, construct, construct_fixed):
    assert cinderjit.force_compile(f) is True

for i in range(300):
    p = make2(i, i + 1)
    assert (p.a, p.b) == (i, i + 1) and type(p) is P
    q = make1(i)
    assert (q.a, q.b) == (i, 10)
    r = make_kw(i)
    assert (r.a, r.b) == (i, i)
    s = construct_fixed(Slotted, i)
    assert s.v == i and not hasattr(s, "__dict__")

try:
    construct(BadInit)
except TypeError as exc:
    assert "__init__() should return None, not 'int'" in str(exc), exc
else:
    raise AssertionError("non-None __init__ result accepted")
for _ in range(50):
    assert type(construct(NewOverride, 1)) is Other
try:
    construct(Abstract)
except TypeError as exc:
    assert "abstract" in str(exc), exc
else:
    raise AssertionError("abstract class instantiated")

# A replaced __init__ invalidates the cached resolution through the version
# tag; a function __init__ replaced by a non-function takes the stock path.
def init2(self, a, b=10):
    self.a = a * 2
    self.b = b
P.__init__ = init2
p = make2(3, 4)
assert (p.a, p.b) == (6, 4), (p.a, p.b)
P.__init__ = lambda self, *args: None
p = make2(3, 4)
assert not hasattr(p, "a")
class Counter:
    calls = 0
    def __call__(self, obj, *args):
        Counter.calls += 1
P.__init__ = Counter()
make2(1, 2)
assert Counter.calls == 1, Counter.calls
P.__init__ = init2
assert make2(5, 6).a == 10

# A class whose __init__ raises leaves no half-built instance behind.
class Raises:
    def __init__(self, x):
        raise ValueError(x)
refs = sys.getrefcount(Raises)
for _ in range(50):
    try:
        construct_fixed(Raises, 7)
    except ValueError as exc:
        assert exc.args == (7,)
    else:
        raise AssertionError("ValueError lost")
assert sys.getrefcount(Raises) == refs
print("ctor ok")
"""


# The generic-call C-entry shapes answer exactly like the stock call.
CALLFAST_PROBE = PREAMBLE + """
import math

def call1(f, x):
    return f(x)

def call2(f, x, y):
    return f(x, y)

def call0(f):
    return f()

for f in (call0, call1, call2):
    assert cinderjit.force_compile(f) is True

class S:
    def __str__(self):
        return "S!"

class BadStr:
    def __str__(self):
        return 1

class Truthy:
    def __bool__(self):
        raise RuntimeError("no truth")

for _ in range(100):
    assert call1(str, 12) == "12" and call1(str, S()) == "S!"
    assert call1(int, "12") == 12 and call1(int, 3.9) == 3 and call1(int, True) == 1
    assert call1(float, "2.5") == 2.5 and call1(float, 2) == 2.0
    assert call1(bool, []) is False and call1(bool, [0]) is True
    assert call1(tuple, (i for i in range(3))) == (0, 1, 2)
    assert call1(tuple, [1, 2]) == (1, 2)
for bad, exc_type in ((lambda: call1(str, BadStr()), TypeError),
                      (lambda: call1(int, "x"), ValueError),
                      (lambda: call1(float, object()), TypeError),
                      (lambda: call1(bool, Truthy()), RuntimeError),
                      (lambda: call1(tuple, 5), TypeError)):
    try:
        bad()
    except exc_type:
        pass
    else:
        raise AssertionError("expected %s" % exc_type.__name__)

# min / max with two positional arguments: the first argument is kept on a
# false or NaN comparison, a raising comparison propagates, a rebinding of
# the builtin takes the new object.
nan = float("nan")
class Lt:
    def __init__(self, v):
        self.v = v
    def __lt__(self, other):
        raise ZeroDivisionError("lt")
    def __gt__(self, other):
        raise ZeroDivisionError("gt")
a, b = 1, 1.0
for _ in range(100):
    assert call2(min, 3, 2) == 2 and call2(max, 3, 2) == 3
    assert call2(min, a, b) is a and call2(max, a, b) is a
    assert call2(min, nan, 1) is nan and call2(min, 1, nan) == 1
    assert call2(max, nan, 1) is nan and call2(max, 1, nan) == 1
    assert call2(min, "b", "a") == "a"
try:
    call2(min, Lt(1), Lt(2))
except ZeroDivisionError:
    pass
else:
    raise AssertionError("raising __lt__ swallowed")
import builtins
real_min = builtins.min
builtins.min = lambda *args: ("patched",) + args
try:
    assert call2(real_min, 3, 2) == 2
    assert call2(builtins.min, 3, 2) == ("patched", 3, 2)
finally:
    builtins.min = real_min
assert call2(min, 9, 8) == 8

# object.__init__(self) through a super() chain and directly.
class Base:
    def __init__(self):
        super().__init__()
        self.base = True
class Derived(Base):
    def __init__(self):
        super().__init__()
        self.derived = True
assert cinderjit.force_compile(Base.__init__) is True
assert cinderjit.force_compile(Derived.__init__) is True
for _ in range(100):
    d = Derived()
    assert d.base and d.derived
obj = object()
for _ in range(100):
    assert call1(object.__init__, obj) is None
try:
    call2(object.__init__, obj, 1)
except TypeError:
    pass
else:
    raise AssertionError("object.__init__ accepted an argument")

# A plain-class __call__ that is a Python function, replaced at runtime.
class Callable:
    def __call__(self, x, y=5):
        return ("call", x, y)
c = Callable()
for _ in range(100):
    assert call1(c, 1) == ("call", 1, 5)
    assert call2(c, 1, 2) == ("call", 1, 2)
Callable.__call__ = lambda self, x, y=0: ("new", x, y)
assert call1(c, 1) == ("new", 1, 0)
Callable.__call__ = staticmethod(lambda x: ("static", x))
assert call1(c, 1) == ("static", 1)
print("callfast ok")
"""


# The keyword-call helper's positional shapes: *args/**kwargs callees get
# the tail tuple and a fresh dict, keyword-only defaults are memoised per
# dict version and owned across the call.
KWCALL_PROBE = PREAMBLE + """
def va(a, b, *rest, **kw):
    return (a, b, rest, kw)

def kwonly(a, *, k="default", j=None):
    return (a, k, j)

def required(a, *, k):
    return (a, k)

def plain(a, b):
    return (a, b)

for f in (va, kwonly, required, plain):
    assert cinderjit.force_compile(f) is True

for i in range(100):
    assert va(1, 2) == (1, 2, (), {})
    assert va(1, 2, 3, 4) == (1, 2, (3, 4), {})
    assert va(1, 2, x=3) == (1, 2, (), {"x": 3})
    d = va(1, 2)[3]
    d["leak"] = i
    assert va(1, 2)[3] == {}
try:
    va(1)
except TypeError as exc:
    assert "missing 1 required positional argument" in str(exc), exc
else:
    raise AssertionError("missing positional accepted")
try:
    plain(1, 2, 3)
except TypeError as exc:
    assert "takes 2 positional arguments but 3 were given" in str(exc), exc
else:
    raise AssertionError("oversupplied call accepted")

sentinel = object()
refs = sys.getrefcount(sentinel)
kwonly.__kwdefaults__["k"] = sentinel
for _ in range(100):
    assert kwonly(1) == (1, sentinel, None)
    assert kwonly(1, j=2) == (1, sentinel, 2)
assert sys.getrefcount(sentinel) == refs + 1, (sys.getrefcount(sentinel), refs)
kwonly.__kwdefaults__["k"] = "second"
assert kwonly(1) == (1, "second", None)
kwonly.__kwdefaults__ = {"k": "third", "j": 3}
assert kwonly(1) == (1, "third", 3)
assert sys.getrefcount(sentinel) == refs, (sys.getrefcount(sentinel), refs)
try:
    required(1)
except TypeError as exc:
    assert "missing 1 required keyword-only argument: 'k'" in str(exc), exc
else:
    raise AssertionError("missing keyword-only accepted")
required.__kwdefaults__ = {"k": "late"}
assert required(1) == (1, "late")

# The defaults dict cleared from inside the call: the bound values are
# owned for the duration of the call.
class Boom:
    pass
def clear_kw():
    victim.__kwdefaults__.clear()
def victim(*, first=Boom(), second=Boom()):
    clear_kw()
    return first, second
assert cinderjit.force_compile(victim) is True
for _ in range(20):
    victim.__kwdefaults__ = {"first": Boom(), "second": Boom()}
    got = victim()
    assert type(got[0]) is Boom and type(got[1]) is Boom
print("kwcall ok")
"""


# A keyword call of a pinned module-global function is bound at compile
# time; the __code__ identity and the defaults shape are re-proved per call.
# The compiled caller's HIR (dumped on stderr) carries the pin guard and no
# keyword-call form.


# A positional call with fewer arguments than parameters completes the
# defaults at compile time behind the same pin.


# Keyword-only defaults are memoised per (code, __kwdefaults__) only for a
# unicode-keyed dict, where the lookups run no Python.  A dict with a
# str-subclass key compares with __eq__, and such a key can pop another
# parameter's default while the defaults are being collected: the generic
# bind owns each default as it finds it.
KWDEFAULTS_CALLBACK_PROBE = PREAMBLE + """
state = {"freed": False}

class Victim:
    def __del__(self):
        state["freed"] = True

def f(*, a=None, b=None):
    return (type(a).__name__, b)

class Key(str):
    __hash__ = str.__hash__

    def __eq__(self, other):
        if other == "b":
            f.__kwdefaults__.pop("a", None)
        return str.__eq__(self, other)

def call():
    return f()

assert cinderjit.force_compile(f) is True
assert cinderjit.force_compile(call) is True
f.__kwdefaults__ = {"a": Victim(), Key("b"): 0}
assert call() == ("Victim", 0), call()
assert state["freed"] is True  # the dict was its only owner
f.__kwdefaults__ = {"a": Victim(), "b": 1}
state["freed"] = False
assert call() == ("Victim", 1)
assert state["freed"] is False
print("kwdefaults callback ok")
"""


# The allocation inside a constructor fast path can collect (the GC link
# runs a collection synchronously on 3.11), and a collection runs callbacks:
# one that constructs another class through the same site refills the
# site's cache, one that replaces __init__ re-versions the class.  Either
# way the instance already allocated is initialised the way type_call
# would initialise it, by the initialiser current after the allocation.
CTOR_GC_REENTRY_PROBE = PREAMBLE + """
import gc

class C:
    def __init__(self, x):
        self.x = x
        self.kind = "C"

class D:
    def __init__(self, x):
        self.x = x
        self.kind = "D"

def make(cls, x):
    return cls(x)

assert cinderjit.force_compile(make) is True
for _ in range(50):
    make(C, 1)
state = {"mode": None, "fired": 0}

def callback(phase, info):
    if phase != "start" or state["mode"] is None:
        return
    mode = state["mode"]
    state["mode"] = None
    state["fired"] += 1
    if mode == "reenter":
        state["inner"] = make(D, 2)
    else:
        def new_init(self, x):
            self.x = x
            self.kind = "C-new"
        C.__init__ = new_init

gc.callbacks.append(callback)
gc.set_threshold(1)
try:
    fired = 0
    for i in range(8):
        gc.collect()
        pad = [] if i % 2 else None  # flips the allocation parity
        state["mode"] = "reenter"
        obj = make(C, 1)
        if state["mode"] is None:  # the collection landed inside the call
            fired += 1
        assert (type(obj).__name__, obj.kind) == ("C", "C"), (obj.kind, i)
    assert fired >= 1, fired
    assert state["inner"].kind == "D"
    for _ in range(50):
        make(C, 1)
    fired = 0
    for i in range(8):
        gc.collect()
        pad = [] if i % 2 else None
        state["mode"] = "replace"
        obj = make(C, 1)
        if state["mode"] is None:
            fired += 1
        expected = "C-new" if fired else "C"
        assert obj.kind == expected, (obj.kind, expected, i)
    assert fired >= 1, fired
finally:
    gc.set_threshold(700)
    gc.callbacks.remove(callback)
print("ctor gc reentry ok")
"""


# A constructor site whose cached target is the initialiser's direct entry:
# that entry carries no __code__ check of its own, and assigning __code__
# changes neither the class version nor the function's vectorcall, so the
# site must pin the code identity itself.
CTOR_CODE_SWAP_PROBE = PREAMBLE + """
class C:
    def __init__(self, x):
        self.x = x + 1

def replacement(self, x):
    self.x = x + 100

def make(cls, x):
    return cls(x)

assert cinderjit.force_compile(C.__init__) is True
assert cinderjit.force_compile(make) is True
for _ in range(100):
    assert make(C, 1).x == 2
C.__init__.__code__ = replacement.__code__
assert make(C, 1).x == 101, make(C, 1).x
for _ in range(50):
    assert make(C, 1).x == 101
print("ctor code swap ok")
"""


# The min/max shortcut of the generic-call helper recognises the builtins
# module's own functions by implementation (their method definition is an
# entry of the builtins module's own table), never by what the name "min"
# or "max" holds in builtins, and never by an object's name, module and
# calling convention: an extension can build a C function named "min",
# bound to the builtins module, with the builtin's flags.
MINMAX_IDENTITY_PROBE = PREAMBLE + """
import builtins
import ctypes

Impl = ctypes.CFUNCTYPE(
    ctypes.py_object, ctypes.py_object, ctypes.py_object, ctypes.c_void_p
)

def impl(self, args, kwargs):
    return 777

keep = Impl(impl)

class PyMethodDef(ctypes.Structure):
    _fields_ = [
        ("ml_name", ctypes.c_char_p),
        ("ml_meth", ctypes.c_void_p),
        ("ml_flags", ctypes.c_int),
        ("ml_doc", ctypes.c_char_p),
    ]

METH_VARARGS, METH_KEYWORDS = 0x0001, 0x0002
md = PyMethodDef(
    b"min",
    ctypes.cast(keep, ctypes.c_void_p).value,
    METH_VARARGS | METH_KEYWORDS,
    None,
)
new_cfunction = ctypes.pythonapi.PyCFunction_NewEx
new_cfunction.restype = ctypes.py_object
new_cfunction.argtypes = [ctypes.c_void_p, ctypes.py_object, ctypes.py_object]
fake_min = new_cfunction(ctypes.addressof(md), builtins, builtins)
assert fake_min(1, 2) == 777
assert type(fake_min) is type(min) and fake_min.__self__ is builtins

real_min = builtins.min
builtins.min = builtins.max

def call2(f, a, b):
    return f(a, b)

assert cinderjit.force_compile(call2) is True
assert call2(max, 1, 2) == 2, call2(max, 1, 2)
assert call2(builtins.min, 1, 2) == 2  # the object bound as min is max
builtins.min = real_min
assert call2(min, 1, 2) == 1
assert call2(max, 1, 2) == 2
assert call2(pow, 2, 3) == 8
assert call2(lambda a, b: "py", 1, 2) == "py"
assert call2(fake_min, 1, 2) == 777, call2(fake_min, 1, 2)
assert call2(min, 1, 2) == 1
assert call2(fake_min, 2, 1) == 777
print("minmax identity ok")
"""


# A fresh function object over observer-dispatched code is attached within
# the attach budget, from the frame hook when the interpreter calls it and
# from the by-code entry when a compiled site does.  The attachment allocates and
# can collect; a collection callback that uncompiles the original member
# retires the artifact mid-publication.  The call then returns through the
# function's own entry, and the artifact stays allocated for the duration
# of the call that pinned it.
BY_CODE_ATTACH_GC_PROBE = PREAMBLE + """
import gc
import types

def f1(x):
    return x + 1

def caller(g):
    return g(1)

for _ in range(30):
    caller(f1)
assert cinderjit.is_jit_compiled(f1) is True
assert cinderjit.is_jit_compiled(caller) is True
state = {"armed": False, "fired": 0}

def callback(phase, info):
    if phase == "start" and state["armed"]:
        state["armed"] = False
        state["fired"] += 1
        cinderjit.force_uncompile(f1)

gc.callbacks.append(callback)
gc.set_threshold(1)
try:
    for i in range(4):
        fresh = types.FunctionType(f1.__code__, globals(), "fresh")
        gc.collect()
        pad = [] if i % 2 else None  # flips the allocation parity
        state["armed"] = True
        if i < 2:
            # From the interpreter: a Python-to-Python call pushes the frame
            # inline and never reaches the by-code entry, so the attachment
            # here is offered by the frame hook (Ci_Observe311_OnFrame) and
            # pinned by the attach implementation itself; the by-code
            # entry's own pin is exercised by the compiled-site calls.
            assert fresh(1) == 2
        else:
            assert caller(fresh) == 2
        state["armed"] = False
        assert fresh(1) == 2
        assert caller(fresh) == 2
        if not cinderjit.is_jit_compiled(f1):
            assert cinderjit.is_jit_compiled(fresh) is False
finally:
    gc.set_threshold(700)
    gc.callbacks.remove(callback)
assert state["fired"] >= 1, state
print("by code attach gc ok")
"""


# A compiled frame does not own its function; the caller keeps it alive
# for the whole call.  The plain-instance __call__ fast path must hold the
# function it dispatches to: the class dict can be its only owner, and the
# body can delete it.
CALL_LIFETIME_PROBE = PREAMBLE + """
import weakref

state = {}

def helper(self):
    del type(self).__call__
    state["alive"] = state["ref"]() is not None

class C:
    def __call__(self, x):
        helper(self)
        return x + 1

def call1(f, a):
    return f(a)

assert cinderjit.force_compile(C.__call__) is True
assert cinderjit.force_compile(call1) is True
state["ref"] = weakref.ref(C.__call__)
assert call1(C(), 1) == 2
assert state["alive"] is True, state
print("call lifetime ok")
"""


# The invocation artifact a C entry publishes answers for the function it
# was published for: a nested call of the same code under other globals
# (the artifact retired while still running, a twin compiled since) binds
# through its own artifact and reads its own globals.
INVOCATION_NAMESPACE_PROBE = PREAMBLE + """
import types

MARK = "A"

def shared(callback, *args):
    if callback is not None:
        callback()
    return MARK

def call1(f, x):
    return f(x)

# Every global the probe assigns below exists before the compile: a key
# added to the module afterwards fails the pinned global load's keys-version
# guard on every call, and the ROI backoff would retire the artifact.
how = outer = ledger = shared_entries = _ = None
assert cinderjit.force_compile(shared) is True
assert cinderjit.force_compile(call1) is True
fresh_a = types.FunctionType(shared.__code__, globals(), "fresh_a")
globals_b = {"MARK": "B", "__builtins__": __builtins__, "__name__": "b"}
func_b = types.FunctionType(shared.__code__, globals_b, "func_b")
state = {}

def callback():
    cinderjit.force_uncompile(shared)
    assert cinderjit.force_compile(func_b) is True
    state["inner"] = (func_b(None), func_b(None, 1))

# "site": the outer call enters through the by-code entry, which publishes
# the artifact of A as the invocation artifact for the whole call.
# "interpreted": the outer call runs in the interpreter (an inline frame
# push, no by-code entry); the nested calls must bind the same way.
for how in ("site", "interpreted"):
    state.clear()
    outer = call1(fresh_a, callback) if how == "site" else fresh_a(callback)
    assert outer == "A", (how, outer)
    assert state["inner"] == ("B", "B"), (how, state)
    cinderjit.force_uncompile(func_b)
    assert cinderjit.force_compile(shared) is True

# The snapshot still answers for the function it was published for: an
# unattached twin of the *args function, called through the compiled site,
# binds its arguments and re-enters machine code every time.
cinderjit._jit311_reset_entry_ledger()
for _ in range(50):
    assert call1(fresh_a, None) == "A"
ledger = cinderjit._jit311_entry_ledger()["entries"]
shared_entries = [e["entries"] for e in ledger if e["qualname"] == "shared"]
assert shared_entries == [50], ledger
print("invocation namespace ok")
"""


# A call site that first saw a function object before its code was
# compiled keeps dispatching through the by-code entry, so the artifact
# published later serves the site's unattached callees.
NEUTRAL_UPGRADE_PROBE = PREAMBLE + """
def entries():
    return _cinderx._get_trigger_stats()["machine_code_entries"]

def make(k):
    def inner(x):
        return x + k
    return inner

def call1(f, x):
    return f(x)

assert cinderjit.force_compile(call1) is True
seed = make(0)
assert call1(seed, 1) == 1  # the site is warmed while the code has no artifact
assert cinderjit.force_compile(seed) is True
fresh = make(1)
before = entries()
for _ in range(100):
    assert call1(fresh, 1) == 2
assert entries() - before >= 200, entries() - before  # caller and callee
print("neutral upgrade ok")
"""


# The keyword-only defaults memo holds the code object its values are laid
# out for.  Releasing the code a rewrite displaces can run Python (a weak
# reference callback runs when a code object dies), so it is released only
# after the caller owns the defaults it took from the shared entry: a
# callback that binds another signature through the same defaults dict
# refills the entry, a callback that empties the dict drops the defaults.
# The function whose code the memo ends up owning is created by exec: a
# code object defined at module level is a constant of the module's code
# and never dies.  Slots are keyed by code object, so the displacing
# function's code is placed on the retired code's slot by construction.
KWDEFAULTS_RETIRE_PROBE = PREAMBLE + """
import gc
import weakref

# The memo chooses its slot by code object, so displacing an entry (and
# releasing the code it held) takes another code object on the same slot:
# the displacing function is defined by exec until its code lands there.
def memo_slot(code):
    return (id(code) >> 4) & 255

_keep = []
def colliding(target_code, src, **env):
    for _ in range(20000):
        ns = dict(env, __builtins__=__builtins__)
        exec(src, ns)
        _keep.append(ns)
        if memo_slot(ns["g"].__code__) == memo_slot(target_code):
            return ns
    raise AssertionError("no code object shares the memo slot")

class V:
    def __init__(self, n):
        self.n = n

def noop():
    pass

def retire(ns):
    cinderjit.force_uncompile(ns["f"])
    cinderjit.force_uncompile(ns["callf"])
    del ns["f"], ns["callf"]
    noop()  # an evaluator frame sweeps the retired artifacts
    gc.collect()
    noop()

# 1. The callback binds another signature through the same dict.
D = {"a": 11, "b": 22}
ns = {"D": D, "__builtins__": __builtins__}
exec("def f(*, a): return a\\ndef callf(): return f()\\nf.__kwdefaults__ = D", ns)

gns = colliding(
    ns["f"].__code__,
    "def g(*, a): return a\\ndef callg(): return g()\\ng.__kwdefaults__ = D",
    D=D,
)
callg = gns["callg"]

def h(*, b):
    return b

h.__kwdefaults__ = D

def callh():
    return h()

for fn in (ns["f"], ns["callf"], gns["g"], callg, h, callh):
    assert cinderjit.force_compile(fn) is True
assert ns["callf"]() == 11  # the memo holds the code of f
state = {"fired": 0}

def refill(ref):
    state["fired"] += 1
    state["h"] = callh()

ref = weakref.ref(ns["f"].__code__, refill)
retire(ns)
assert ref() is not None  # the memo is the last owner
assert callg() == 11, callg()
assert state["fired"] == 1 and state["h"] == 22, state
assert callg() == 11

# 2. The callback empties the dict that was the default's only owner.
E = {"a": V(11)}
ns = {"D": E, "__builtins__": __builtins__}
exec("def f(*, a): return a.n\\ndef callf(): return f()\\nf.__kwdefaults__ = D", ns)

kns = colliding(
    ns["f"].__code__,
    "def g(*, a): return a.n\\ndef callg(): return g()\\ng.__kwdefaults__ = D",
    D=E,
)
callk = kns["callg"]

for fn in (ns["f"], ns["callf"], kns["g"], callk):
    assert cinderjit.force_compile(fn) is True
assert ns["callf"]() == 11
state = {"fired": 0}

def empty(ref):
    state["fired"] += 1
    E.clear()
    state["junk"] = [V(i) for i in range(5000)]

ref = weakref.ref(ns["f"].__code__, empty)
retire(ns)
assert ref() is not None
assert callk() == 11, callk()
assert state["fired"] == 1, state
print("kwdefaults retire ok")
"""


# A code object the memo displaces is released only when the keyword call
# is over: the argument space, the binding and the re-entry are all derived
# from the callee's code at entry, and the release can run Python that
# re-codes the function (here: a required keyword-only parameter has no
# default, the memo cannot serve the call, and the callback that the
# release runs gives the function a forty-parameter signature).
KWDEFAULTS_RECODE_PROBE = PREAMBLE + """
import gc
import weakref

# The memo chooses its slot by code object, so displacing an entry (and
# releasing the code it held) takes another code object on the same slot:
# the displacing function is defined by exec until its code lands there.
def memo_slot(code):
    return (id(code) >> 4) & 255

_keep = []
def colliding(target_code, src, **env):
    for _ in range(20000):
        ns = dict(env, __builtins__=__builtins__)
        exec(src, ns)
        _keep.append(ns)
        if memo_slot(ns["g"].__code__) == memo_slot(target_code):
            return ns
    raise AssertionError("no code object shares the memo slot")

def noop():
    pass

D = {"a": 11}
ns = {"D": D, "__builtins__": __builtins__}
exec(
    "def f(*, a): return a\\n"
    "def callf(): return f()\\n"
    "f.__kwdefaults__ = D",
    ns,
)

# g's dict is non-empty, but holds no default for `missing`.
gns = colliding(
    ns["f"].__code__,
    "def g(*, missing): return missing\\ndef callg(): return g()\\n"
    "g.__kwdefaults__ = D",
    D=D,
)
g = gns["g"]
callg = gns["callg"]

for fn in (ns["f"], ns["callf"], g, callg):
    assert cinderjit.force_compile(fn) is True
assert ns["callf"]() == 11  # the memo holds the code of f
params = ", ".join(f"k{i}=None" for i in range(40))
exec(f"def wide(*, {params}): return (k0, k39)", ns)
state = {"fired": 0}

def recode(ref):
    state["fired"] += 1
    g.__code__ = ns["wide"].__code__
    g.__kwdefaults__ = {f"k{i}": i for i in range(40)}

ref = weakref.ref(ns["f"].__code__, recode)
cinderjit.force_uncompile(ns["f"])
cinderjit.force_uncompile(ns["callf"])
del ns["f"], ns["callf"]
noop()
gc.collect()
noop()
assert ref() is not None  # the memo is the last owner
try:
    callg()
except TypeError as exc:
    assert "missing" in str(exc), exc
else:
    raise AssertionError("the call bound without its required parameter")
assert state["fired"] == 1, state
assert callg() == (0, 39), callg()
print("kwdefaults recode ok")
"""


# A call-site entry is matched by the addresses of the callee's code and
# vectorcall; the caller's artifact owns every code object its sites name,
# so a code object that died cannot be succeeded at its address by one of
# another signature while the artifact lives.  Before that ownership, the
# first replacement code object of the same size took the freed address and
# a two-parameter function entered through the one-parameter direct entry.
ENTRY_CACHE_CODE_LIFETIME_PROBE = PREAMBLE + """
import gc
import weakref

def noop():
    pass

def caller(f, x):
    return f(x)

assert cinderjit.force_compile(caller) is True
ns = {"__builtins__": __builtins__}
exec("def f(x):\\n    return x + 1", ns)
assert cinderjit.force_compile(ns["f"]) is True
for _ in range(10):
    assert caller(ns["f"], 1) == 2  # the site fills the one-parameter direct entry
ref = weakref.ref(ns["f"].__code__)
cinderjit.force_uncompile(ns["f"])
del ns["f"]
noop()
gc.collect()
noop()
assert ref() is not None  # owned by the caller's artifact
for _ in range(50):
    exec("def g(x, y):\\n    return x + y", ns)
    assert cinderjit.force_compile(ns["g"]) is True
    try:
        caller(ns["g"], 1)
    except TypeError as exc:
        assert "y" in str(exc), exc
    else:
        raise AssertionError("a two-parameter function ran with one argument")
    cinderjit.force_uncompile(ns["g"])
    del ns["g"]
    gc.collect()
print("entry cache code lifetime ok")
"""


# len / isinstance / type loaded from builtins are pinned so the simplifier
# opens them; shadowing at module level or rebinding in builtins deopts.


# The per-site super() cache replays the (instance type, start class)
# resolution and is invalidated by every class-dict change along the MRO;
# attribute and classmethod forms, and a shadowed `super` global, keep
# stock semantics.


# The call-site entry cache: compiled, interpreted, builtin, class and bound
# callees through one site; a __code__ swap, a defaults change and a
# recompilation are all honoured.
ENTRY_CACHE_PROBE = PREAMBLE + """
def call1(f, x):
    return f(x)

def callee(x):
    return x * 2

def callee_defaults(x, y=1):
    return (x, y)

def other(x):
    return ("other", x)

class K:
    def __init__(self, x):
        self.x = x
    def m(self, x):
        return (self.x, x)

for f in (call1, callee, callee_defaults):
    assert cinderjit.force_compile(f) is True

k = K(5)
for i in range(300):
    assert call1(callee, i) == i * 2
    assert call1(callee_defaults, i) == (i, 1)
    assert call1(abs, -i) == i
    assert call1(K, i).x == i
    assert call1(k.m, i) == (5, i)
    assert call1(len, [i]) == 1
callee.__code__ = other.__code__
assert call1(callee, 1) == ("other", 1)
callee.__code__ = (lambda x: x * 2).__code__
assert call1(callee, 1) == 2
callee_defaults.__defaults__ = (9,)
assert call1(callee_defaults, 1) == (1, 9)
try:
    call1(callee_defaults.__code__.replace, 1)
except TypeError:
    pass
else:
    raise AssertionError("code.replace accepted a positional argument")

# A callee that is compiled after the site has cached its interpreted
# entry: the vectorcall identity changes, the site refills with the direct
# entry.
def late(x):
    return x + 100
for i in range(50):
    assert call1(late, i) == i + 100
assert cinderjit.force_compile(late) is True
for i in range(100):
    assert call1(late, i) == i + 100

print("entry cache ok")
"""


# A function object never attached to its code's artifact (a closure made
# per call, the attach budget set to zero) still runs the artifact: the
# machine-code entry counter advances once per call, through a compiled
# call site and through the interpreter alike.
BY_CODE_PROBE = PREAMBLE + """
def make(k):
    def inner(i):
        return i * 3 + k
    return inner

def call1(f, x):
    return f(x)

assert cinderjit.force_compile(call1) is True
first = make(0)
assert cinderjit.force_compile(first) is True
assert call1(first, 2) == 6

def entries():
    return _cinderx._get_trigger_stats()["machine_code_entries"]

# The caller's own entries, measured against an interpreted callee of
# another code object: one per call.
def plain(x):
    return x

before = entries()
for _ in range(200):
    assert call1(plain, 2) == 2
caller_only = entries() - before
assert caller_only == 200, caller_only
# Through the compiled site: the caller's entry plus the callee's by-code
# entry per call.
before = entries()
for k in range(1, 201):
    fresh = make(k)
    assert cinderjit.is_jit_compiled(fresh) is False
    assert call1(fresh, 2) == 6 + k
site = entries() - before
assert site - caller_only >= 200, (site, caller_only)
# From the interpreter the frame is pushed inline (no vectorcall, so no
# by-code entry): correct, interpreted.
for k in range(1, 201):
    fresh = make(k)
    assert fresh(3) == 9 + k
    assert cinderjit.is_jit_compiled(fresh) is False

# While a by-code call is in flight, a nested call of another compiled
# function whose prologue binds through the C helpers (*args, a keyword
# call, a defaults completion) must re-enter its own artifact, not the one
# the by-code entry published for the outer function.
def star(self, *params):
    return (self, params)

def kw(a, b=2):
    return (a, b)

for f in (star, kw):
    assert cinderjit.force_compile(f) is True

def make_caller(k):
    def caller(x):
        return (star(x, k, k + 1), kw(x, b=k), kw(k))
    return caller

seed = make_caller(0)
assert cinderjit.force_compile(seed) is True
for k in range(1, 101):
    fresh = make_caller(k)
    assert cinderjit.is_jit_compiled(fresh) is False
    assert fresh(7) == ((7, (k, k + 1)), (7, k), (k, 2)), fresh(7)
    assert call1(fresh, 8) == ((8, (k, k + 1)), (8, k), (k, 2))
print("by code ok")
"""


# The traceback WITH_EXCEPT_START passes to a compiled __exit__ is held for
# the duration of the call: `val.with_traceback(None)` inside __exit__ drops
# the exception's reference, and the frame materialised for the deopt that
# the inner exception causes must not see a freed object.
HELD_TRACEBACK_PROBE = PREAMBLE + """
import gc

class Ctx:
    def __enter__(self):
        return self
    def __exit__(self, typ, val, tb):
        val.with_traceback(None)
        try:
            raise KeyError("inner")
        except KeyError:
            pass
        return tb is not None and tb.tb_frame is not None

def body():
    with Ctx():
        raise ValueError("outer")
    return "swallowed"

assert cinderjit.force_compile(Ctx.__exit__) is True
for _ in range(300):
    assert body() == "swallowed"
    gc.collect()
print("held traceback ok")
"""


# Keep the workload identical in A and B. A's version guard must deopt at
# the nested function's LOAD_GLOBAL; B's per-site cell re-pins without deopt.
# The expected count is chosen by the branch's test, never by observed stats.
def fresh_function_global_change_probe(expected_deopts):
    return PREAMBLE + f"EXPECTED_GLOBAL_DEOPTS = {expected_deopts}\n" + r"""
import dis
import types

filename = "mr4_fresh_global_guard.py"
source = "def outer(x):\n    def inner(y):\n        return VALUE + y\n    return inner(x)\n"

def new_namespace():
    ns = {"VALUE": 10, "__builtins__": __builtins__}
    exec(compile(source, filename, "exec"), ns)
    return ns

# Obtain the reference with stock evaluation, before compiling either
# function. Separate code objects/namespaces prevent by-code dispatch from
# accidentally running the reference through the candidate's artifact.
_cinderx.remove_frame_evaluator()
assert not _cinderx.is_frame_evaluator_installed()
reference = new_namespace()
reference_initial = reference["outer"](7)
reference["VALUE"] = 20
reference_rebound = reference["outer"](7)
reference["EXTRA"] = True
reference["VALUE"] = 30
reference_changed = reference["outer"](7)
assert (reference_initial, reference_rebound, reference_changed) == (17, 27, 37)
assert not cinderjit.is_jit_compiled(reference["outer"])
_cinderx.install_frame_evaluator()
assert _cinderx.is_frame_evaluator_installed()

candidate = new_namespace()
outer = candidate["outer"]
inner_code = next(
    c for c in outer.__code__.co_consts if isinstance(c, types.CodeType)
)
# Quicken the shared inner code before compilation, while auto-compilation
# remains disabled by the probe's high threshold. Keep candidate globals
# unchanged from here until the deliberate mutations below.
for _ in range(16):
    assert outer(7) == reference_initial
prototype = types.FunctionType(inner_code, candidate)
assert cinderjit.force_compile(prototype)
assert cinderjit.force_compile(outer)
assert cinderjit.is_jit_compiled(prototype)
assert cinderjit.is_jit_compiled(outer)
load_offset = next(
    instruction.offset for instruction in dis.get_instructions(inner_code)
    if instruction.opname == "LOAD_GLOBAL" and instruction.argval == "VALUE"
)

# Each call creates a fresh inner function: the compiled prototype itself
# is never called here. With fresh-attach budget zero, the inner ledger and
# both machine-code entries prove dispatch through its code artifact.
def invoke(expected, expected_deopts):
    cinderjit._jit311_reset_entry_ledger()
    cinderjit.get_and_clear_runtime_stats()
    before = _cinderx._get_trigger_stats()
    actual = outer(7)
    after = _cinderx._get_trigger_stats()
    deopts = cinderjit.get_and_clear_runtime_stats()["deopt"]
    ledger = cinderjit._jit311_entry_ledger()["entries"]
    assert actual == expected, (actual, expected)
    entered = after["machine_code_entries"] - before["machine_code_entries"]
    assert entered == 2, ("machine-code entries", entered, ledger)
    for code in (outer.__code__, inner_code):
        entries = [
            row["entries"] for row in ledger
            if row["qualname"] == code.co_qualname
        ]
        assert entries == [1], (code.co_qualname, ledger)
    assert after["forced_deopt_hits"] == before["forced_deopt_hits"]
    organic = after["organic_deopt_hits"] - before["organic_deopt_hits"]
    assert organic == expected_deopts, ("organic deopts", organic, expected_deopts, deopts)
    if expected_deopts == 0:
        assert deopts == [], deopts
    else:
        assert len(deopts) == 1, deopts
        event = deopts[0]
        assert event["normal"]["filename"] == filename, event
        assert event["normal"]["func_qualname"] == inner_code.co_qualname, event
        assert event["normal"]["reason"] == "GuardFailure", event
        assert event["normal"]["description"] == "LOAD_GLOBAL_MODULE: VALUE", event
        assert event["int"]["bc_offset"] == load_offset, event
        assert event["int"]["opcode"] == dis.opmap["LOAD_GLOBAL"], event
        assert event["int"]["count"] == 1, event

invoke(reference_initial, 0)
# Rebinding an existing value must be observed without a key-version miss.
candidate["VALUE"] = 20
invoke(reference_rebound, 0)
# Inserting a new key invalidates the keys-version guard. Rebind VALUE as
# well so returning a stale cached value cannot satisfy the result check.
candidate["EXTRA"] = True
candidate["VALUE"] = 30
invoke(reference_changed, EXPECTED_GLOBAL_DEOPTS)
print("fresh global change ok")
"""


# Exercise the actual non-function vectorcall slot with CPython's own
# malformed-C-return fixtures. Both oracle and JIT run in fresh children;
# Py_DEBUG deliberately aborts in _Py_CheckFunctionResult in either mode.
def invalid_c_return_probe(function_name, compiled):
    return (
        PREAMBLE
        + f"FUNCTION_NAME = {function_name!r}\nCOMPILED = {compiled!r}\n"
        + r"""
import _testcapi
import json

def invoke(fn, events):
    result = fn()
    events.append("continued")
    return result

if COMPILED:
    assert cinderjit.force_compile(invoke)
    assert cinderjit.is_jit_compiled(invoke)
else:
    _cinderx.remove_frame_evaluator()
    assert not _cinderx.is_frame_evaluator_installed()

# Cover both legal result/error combinations before the malformed one:
# a successful C vectorcall and a NULL return carrying a real TypeError.
events = []
assert invoke(dict, events) == {}
assert events == ["continued"]
events.clear()
try:
    invoke(len, events)
except TypeError:
    pass
else:
    raise AssertionError("len() must reject the missing argument")
assert events == []
if COMPILED:
    assert cinderjit.is_jit_compiled(invoke)

fn = getattr(_testcapi, FUNCTION_NAME)
before = _cinderx._get_trigger_stats()["machine_code_entries"]
print("calling invalid C function", flush=True)
try:
    invoke(fn, events)
except SystemError as error:
    outcome = {
        "type": type(error).__name__,
        "message": str(error),
        "cause": None if error.__cause__ is None else (
            type(error.__cause__).__name__, str(error.__cause__)
        ),
        "events": events,
    }
else:
    raise AssertionError("malformed C return did not raise SystemError")
after = _cinderx._get_trigger_stats()["machine_code_entries"]
assert after - before == int(COMPILED), (before, after, COMPILED)
assert events == [], events
print("OUTCOME " + json.dumps(outcome), flush=True)
"""
    )


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 call dispatch"
)
class CallDispatchSemantics311Test(unittest.TestCase):
    def _run(self, probe, marker, extra_env=None, stderr_contains=(),
             stderr_excludes=(), expected_returncode=0):
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
        self.assertEqual(proc.returncode, expected_returncode, proc.stderr[-2000:])
        self.assertIn(marker, proc.stdout)
        for needle in stderr_contains:
            self.assertIn(needle, proc.stderr)
        for needle in stderr_excludes:
            self.assertNotIn(needle, proc.stderr)
        return proc

    def _check_invalid_c_return(self, name, diagnostic):
        debug_python = hasattr(sys, "gettotalrefcount")
        outcomes = []
        for compiled in (False, True):
            with self.subTest(compiled=compiled):
                proc = self._run(
                    invalid_c_return_probe(name, compiled),
                    "calling invalid C function",
                    expected_returncode=-signal.SIGABRT if debug_python else 0,
                    stderr_contains=(name, diagnostic) if debug_python else (),
                )
                if not debug_python:
                    line = next(
                        line for line in proc.stdout.splitlines()
                        if line.startswith("OUTCOME ")
                    )
                    outcome = json.loads(line[len("OUTCOME "):])
                    self.assertEqual(outcome["type"], "SystemError")
                    self.assertEqual(outcome["events"], [])
                    self.assertIn(name, outcome["message"])
                    self.assertIn(diagnostic, outcome["message"])
                    outcomes.append(outcome)
        if not debug_python and len(outcomes) == 2:
            self.assertEqual(outcomes[0], outcomes[1])

    def test_c_vectorcall_null_without_exception_matches_interpreter(self):
        self._check_invalid_c_return(
            "return_null_without_error", "returned NULL without setting an exception"
        )

    def test_c_vectorcall_result_with_exception_matches_interpreter(self):
        self._check_invalid_c_return(
            "return_result_with_error", "returned a result with an exception set"
        )

    def test_fresh_known_function_uses_its_code_artifact(self):
        self._run(
            PREAMBLE + """
import types
import _cinderx

def outer(x):
    def inner(y):
        return y + 1
    return inner(x)

code = next(c for c in outer.__code__.co_consts if isinstance(c, types.CodeType))
prototype = types.FunctionType(code, globals())
assert cinderjit.force_compile(prototype)
assert cinderjit.force_compile(outer)
assert cinderjit.is_jit_compiled(prototype)
assert cinderjit.is_jit_compiled(outer)
before = _cinderx._get_trigger_stats()["machine_code_entries"]
for i in range(100):
    assert outer(i) == i + 1
after = _cinderx._get_trigger_stats()["machine_code_entries"]
assert after - before >= 200, (before, after)
print("fresh known function ok")
""",
            "fresh known function ok",
            extra_env={"PYTHONJITFRESHATTACHBUDGET": "0"},
        )

    def test_fresh_function_global_change_matches_interpreter(self):
        self._run(
            fresh_function_global_change_probe(expected_deopts=1),
            "fresh global change ok",
            extra_env={"PYTHONJITFRESHATTACHBUDGET": "0"},
        )

    def test_construction_fast_path_matches_type_call(self):
        self._run(CTOR_PROBE, "ctor ok")

    def test_generic_call_c_entry_shapes(self):
        self._run(CALLFAST_PROBE, "callfast ok")

    def test_keyword_call_helper_positional_shapes(self):
        self._run(KWCALL_PROBE, "kwcall ok")





    def test_call_site_entry_cache_tracks_the_callee(self):
        self._run(ENTRY_CACHE_PROBE, "entry cache ok")

    def test_by_code_dispatch_beyond_the_attach_budget(self):
        self._run(
            BY_CODE_PROBE,
            "by code ok",
            extra_env={"PYTHONJITFRESHATTACHBUDGET": "0"},
        )

    def test_keyword_only_defaults_collected_under_a_callback(self):
        self._run(KWDEFAULTS_CALLBACK_PROBE, "kwdefaults callback ok")

    def test_keyword_only_defaults_memo_releases_the_old_code_last(self):
        self._run(KWDEFAULTS_RETIRE_PROBE, "kwdefaults retire ok")

    def test_keyword_only_defaults_memo_release_outlives_the_call(self):
        self._run(KWDEFAULTS_RECODE_PROBE, "kwdefaults recode ok")

    def test_entry_cache_keeps_the_named_code_alive(self):
        self._run(ENTRY_CACHE_CODE_LIFETIME_PROBE, "entry cache code lifetime ok")

    def test_construction_revalidated_after_a_collecting_allocation(self):
        self._run(CTOR_GC_REENTRY_PROBE, "ctor gc reentry ok")

    def test_construction_site_pins_the_initialiser_code(self):
        self._run(CTOR_CODE_SWAP_PROBE, "ctor code swap ok")

    def test_min_max_shortcut_recognises_the_builtins_by_identity(self):
        self._run(MINMAX_IDENTITY_PROBE, "minmax identity ok")

    def test_call_fast_path_holds_the_function(self):
        self._run(CALL_LIFETIME_PROBE, "call lifetime ok")

    def test_invocation_artifact_answers_for_its_own_namespace(self):
        self._run(
            INVOCATION_NAMESPACE_PROBE,
            "invocation namespace ok",
            extra_env={"PYTHONJITFRESHATTACHBUDGET": "0"},
        )

    def test_neutral_site_takes_the_artifact_published_later(self):
        self._run(
            NEUTRAL_UPGRADE_PROBE,
            "neutral upgrade ok",
            extra_env={"PYTHONJITFRESHATTACHBUDGET": "0"},
        )

    def test_by_code_attach_survives_an_uncompile_during_publication(self):
        self._run(
            BY_CODE_ATTACH_GC_PROBE,
            "by code attach gc ok",
            extra_env={
                "PYTHONJITAUTO": "4",
                "PYTHONJITFRESHATTACHBUDGET": "8",
            },
        )

    def test_traceback_held_across_compiled_exit(self):
        self._run(HELD_TRACEBACK_PROBE, "held traceback ok")


if __name__ == "__main__":
    unittest.main()
