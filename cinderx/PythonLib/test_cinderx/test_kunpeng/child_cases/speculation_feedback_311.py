# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Child programs for test_speculation_feedback_311 (CPython 3.11, execute
mode).  Each case prints ``CASE_OK <case>`` when every assertion held."""

import array
import dis
import gc
import math
import sys


# Run by the stock interpreter before cinderx is imported (importing it
# installs an evaluator, which stops the stock specializer): the stock
# specializer gives this __getitem__ a small function version, which a
# BINARY_SUBSCR_GETITEM site specialized under cinderx then carries in its
# inline cache (case_array_record_leftover_version).
class StockVersioned:
    def __getitem__(self, i):
        return ("stock", i)


def stock_subscr(obj):
    for i in range(64):
        obj[i]


stock_subscr(StockVersioned())

import _cinderx  # noqa: E402
import cinderx  # noqa: E402

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit  # noqa: E402

assert cinderx.is_initialized() and cinderjit.is_enabled()
assert _cinderx.is_frame_evaluator_installed()

# Enough interpreted calls for the 3.11 adaptive interpreter to specialise.
WARMUP_CALLS = 20

ARITHMETIC = """
def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def div(a, b):
    return a / b

def iadd(a, b):
    a += b
    return a

def isub(a, b):
    a -= b
    return a

def imul(a, b):
    a *= b
    return a

def idiv(a, b):
    a /= b
    return a

def lt(a, b):
    return a < b

def le_branch(a, b):
    if a <= b:
        return 1
    return 2

def eq_branch(a, b):
    if a == b:
        return 1
    return 2

def ne(a, b):
    return a != b
"""

ACCUMULATE = """
def acc(xs):
    s = 0
    for x in xs:
        s += x
    return s
"""


class FloatSub(float):
    """A float subclass whose operators must never take a float fast path."""

    def __add__(self, other):
        return ("sub-add", float(self), other)

    __radd__ = __add__

    def __lt__(self, other):
        return "sub-lt"


SPECIAL_OPERANDS = [
    0.0,
    -0.0,
    1.5,
    -2.25,
    math.inf,
    -math.inf,
    math.nan,
    1e308,
    5e-324,
    3,
    0,
    -7,
    True,
    2**70,
    2**1100,
    FloatSub(2.5),
]


def build(source, tag):
    namespace = {}
    exec(compile(source, f"<speculation-{tag}>", "exec"), namespace)
    return namespace


def outcome(func, *args):
    try:
        return ("value", func(*args))
    except Exception as exc:  # the exception type is part of the contract
        return ("raise", type(exc))


def same_value(a, b):
    if type(a) is not type(b):
        return False
    if isinstance(a, float):
        if math.isnan(a):
            return math.isnan(b)
        return a == b and math.copysign(1.0, a) == math.copysign(1.0, b)
    if isinstance(a, tuple):
        return len(a) == len(b) and all(map(same_value, a, b))
    return a == b


def same(expected, actual):
    if expected[0] != actual[0]:
        return False
    if expected[0] == "raise":
        return expected[1] is actual[1]
    return same_value(expected[1], actual[1])


def compile_now(func):
    assert cinderjit.force_compile(func), func
    assert cinderjit.is_jit_compiled(func), func


def machine_code_entries():
    return _cinderx._get_trigger_stats()["machine_code_entries"]


def organic_deopts():
    return _cinderx._get_trigger_stats()["organic_deopt_hits"]


def case_forced():
    """Forced deopts are not evidence of bad speculation."""
    for literal, argument, result in ((1.0, 2.5, 3.5), (1, 2, 3)):
        ns = build(f"def target(x):\n    return x + {literal!r}\n", "forced")
        target = ns["target"]
        for _ in range(100):
            assert target(argument) == result
        compile_now(target)
        sites = [s for s in cinderjit.deopt_sites(target) if s["forceable"]]
        assert sites, cinderjit.deopt_sites(target)
        site = max(sites, key=lambda s: s["bc_offset"])
        before = _cinderx._get_trigger_stats()
        for i in range(32):
            assert cinderjit.force_deopt(target, site["id"])
            assert target(argument) == result
            assert cinderjit.is_jit_compiled(target), i
        after = _cinderx._get_trigger_stats()
        assert after["forced_deopt_hits"] - before["forced_deopt_hits"] == 32
        assert after["organic_deopt_hits"] == before["organic_deopt_hits"]
        assert after["machine_code_entries"] > before["machine_code_entries"]


def case_semantics():
    """Speculated float arithmetic and compares match the interpreter on
    signed zeros, infinities, NaN, overflow, zero division, ints, bools and
    float subclasses, whether the site was specialised (float warm-up: guards,
    fuse, diamond after recompile) or stuck (mixed warm-up: diamond)."""
    oracle = build(ARITHMETIC, "oracle")
    warmups = {
        "float": [(1.5, 2.25), (-0.5, 4.0)],
        "mixed": [(1, 2.5), (2.5, 1), (1.5, 2.0), (3, 4)],
    }
    names = [n for n in oracle if not n.startswith("__")]
    entries = machine_code_entries()
    for warm_name, warm_args in warmups.items():
        compiled = build(ARITHMETIC, f"jit-{warm_name}")
        for name in names:
            func = compiled[name]
            for i in range(WARMUP_CALLS):
                func(*warm_args[i % len(warm_args)])
            compile_now(func)
        for name in names:
            func = compiled[name]
            for a in SPECIAL_OPERANDS:
                for b in SPECIAL_OPERANDS:
                    if not cinderjit.is_jit_compiled(func):
                        # A blown fuse uncompiles; keep testing machine code.
                        compile_now(func)
                    expected = outcome(oracle[name], a, b)
                    actual = outcome(func, a, b)
                    assert same(expected, actual), (
                        warm_name, name, a, b, expected, actual)
    assert machine_code_entries() > entries

    oracle_acc = build(ACCUMULATE, "acc-oracle")["acc"]
    acc = build(ACCUMULATE, "acc-jit")["acc"]
    for _ in range(WARMUP_CALLS):
        acc([1.0, 2.5])
    compile_now(acc)
    for xs in ([], [1.0, 2.5], [1, 2.0], [2.0, 1], [-0.0], [0.0, -0.0],
               [math.inf, -math.inf], [math.nan], [True, 0.5], [2**1100, 1.0],
               [FloatSub(1.0), 1.0], [1.0, FloatSub(1.0)]):
        if not cinderjit.is_jit_compiled(acc):
            compile_now(acc)
        expected = outcome(oracle_acc, xs)
        actual = outcome(acc, xs)
        assert same(expected, actual), (xs, expected, actual)


def case_fuse():
    """A *_FLOAT guard that keeps failing at one site is recompiled into a
    type-check diamond there: eight organic deopts, then none."""
    ns = build("def scale(a, b):\n    return a * b\n", "fuse")
    scale = ns["scale"]
    for _ in range(WARMUP_CALLS):
        assert scale(1.5, 2.0) == 3.0
    compile_now(scale)
    before = organic_deopts()
    for _ in range(8):
        assert scale(3, 4) == 12
    assert organic_deopts() - before == 8, organic_deopts() - before
    if not cinderjit.is_jit_compiled(scale):
        compile_now(scale)
    after_fuse = organic_deopts()
    entries = machine_code_entries()
    for _ in range(100):
        assert scale(3, 4) == 12
        assert scale(1.5, 2.0) == 3.0
    assert organic_deopts() == after_fuse, organic_deopts() - after_fuse
    assert machine_code_entries() - entries == 200


FUSE_ARTIFACTS = """
def make():
    def scale(a, b):
        return a * b
    return scale
"""


def deopts_while(calls):
    before = organic_deopts()
    for call in calls:
        call()
    return organic_deopts() - before


def case_fuse_by_code():
    """A fuse that blows in a by-code artifact (fresh function objects over
    one code, compiled through the observer) retires that artifact: the
    deopts stop after the eighth."""
    ns = build(FUSE_ARTIFACTS, "fuse-by-code")
    make = ns["make"]
    entries = machine_code_entries()
    for _ in range(200):
        assert make()(1.5, 2.0) == 3.0
    assert machine_code_entries() > entries
    assert deopts_while([lambda: make()(3, 4)] * 8) == 8
    assert deopts_while([lambda: make()(3, 4)] * 12) == 0
    assert make()(1.5, 2.0) == 3.0


EXC_EDGE = """
def guarded(b):
    try:
        return 2.5 * b
    except TypeError:
        return -1
"""


def case_exc_edge():
    """A float site inside a try block (one operand known float, so it
    would get a type-check diamond) keeps its generic form once the exception
    edge is granted, so the compiled handler takes the error without a
    deopt."""
    guarded = build(EXC_EDGE, "exc-edge")["guarded"]
    compile_now(guarded)
    # Eight exception deopts earn the edge and recompile the code in place.
    assert deopts_while([lambda: guarded("x")] * 8) == 8
    assert cinderjit.is_jit_compiled(guarded)
    entries = machine_code_entries()
    assert deopts_while([lambda: guarded("x")] * 12) == 0
    assert machine_code_entries() - entries == 12
    assert guarded("x") == -1
    assert guarded(2.0) == 5.0


IDENTITY = """
def binary(x, rhs):
    a = x * 2.0
    return a, a {op} rhs

def inplace(x, rhs):
    a = x * 2.0
    b = a
    b {op}= rhs
    return a, b

def read_after(x, rhs):
    a = x * 2.0
    b = a {op} rhs
    return a is b
"""


class Reflect:
    """Reflected operators hand back the float operand itself."""

    def __radd__(self, other):
        return other

    __rsub__ = __rmul__ = __rtruediv__ = __radd__


def case_identity():
    """A boxed float that is still read elsewhere reaches the generic slow arm
    as the same object, not an equal copy."""
    rhs = Reflect()
    entries = machine_code_entries()
    for op in "+-*/":
        ns = build(IDENTITY.format(op=op), f"identity-{op}")
        for name in ("binary", "inplace", "read_after"):
            func = ns[name]
            for _ in range(WARMUP_CALLS):
                func(12.5, rhs)
            compile_now(func)
            result = func(12.5, rhs)
            if name == "read_after":
                assert result is True, (op, name)
            else:
                a, b = result
                assert a == 25.0 and a is b, (op, name, a, b)
    assert machine_code_entries() > entries


def case_lifetime():
    """Fuse state is created by concurrent compile workers and dropped with
    its code object."""
    def count():
        return cinderjit._jit311_lifecycle_snapshot()["jit"]["numeric_spec_codes"]

    def collect():
        for _ in range(3):
            gc.collect()

    baseline = count()
    for cycle in range(2):
        units = []
        for i in range(256):
            ns = build("def unit(a, b):\n    return a * b\n", f"unit-{cycle}-{i}")
            fn = ns["unit"]
            for _ in range(WARMUP_CALLS):
                fn(1.5, 2.0)
            assert cinderjit._jit311_register_for_compile(fn)
            units.append(fn)
        cinderjit._jit311_multithreaded_compile_test()
        assert all(cinderjit.is_jit_compiled(fn) for fn in units)
        assert count() == baseline + len(units), (count(), baseline)
        for fn in units:
            assert fn(1.5, 2.0) == 3.0
        for fn in units:
            assert cinderjit.force_uncompile(fn)
        del fn, ns, units
        collect()
        assert count() == baseline, (count(), baseline)


INT_ARITHMETIC = """
def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def iadd(a, b):
    a += b
    return a

def isub(a, b):
    a -= b
    return a

def imul(a, b):
    a *= b
    return a

def floordiv_pow2(a, b):
    return a // 2, a // 1024

def mod_pow2(a, b):
    return a % 8, a % 1024

def band(a, b):
    return a & b

def bor(a, b):
    return a | b

def bxor(a, b):
    return a ^ b

def iand(a, b):
    a &= b
    return a

def ior(a, b):
    a |= b
    return a

def ixor(a, b):
    a ^= b
    return a

def masked(a, b):
    return ((a >> 3) & 0xFF) ^ b

def truthy(a, b):
    if a & b:
        return 1
    return 2

def band_box(a, b):
    return (a + 1) & (b - 1)

def bor_box(a, b):
    return (a * 3) | b

def bxor_const(a, b):
    return (a ^ 0x2AAAAAAA) ^ b

def iand_box(a, b):
    a += 1
    a &= b
    return a

def truthy_const(a, b):
    if a & 0x40:
        return 1
    return 2
"""

INT_SHIFTS = """
def lshift(a, n):
    return a << n

def rshift(a, n):
    return a >> n

def ilshift(a, n):
    a <<= n
    return a

def irshift(a, n):
    a >>= n
    return a

def lshift_const(a, n):
    return a << 5, a << 63

def rshift_const(a, n):
    return a >> 5, a >> 63

def guarded_shift(a, n):
    try:
        return a << n
    except ValueError:
        return "negative"

def lshift_box(a, n):
    return (a + 1) << n

def rshift_box(a, n):
    return (a - 1) >> n
"""


class IntSub(int):
    """An int subclass whose operators must never take an int fast path."""

    def __and__(self, other):
        return ("sub-and", int(self), other)

    __rand__ = __and__

    def __lshift__(self, other):
        return ("sub-lshift", int(self), other)

    def __add__(self, other):
        return ("sub-add", int(self), other)

    __radd__ = __add__


class AndProtocol:
    """A user type reached through the generic number protocol."""

    def __and__(self, other):
        return "and-protocol"

    __rand__ = __and__


# Digit boundaries of the 30-bit representation and of int64.
INT_OPERANDS = [
    0,
    1,
    -1,
    7,
    -7,
    2**29,
    2**30 - 1,
    -(2**30 - 1),
    2**30,
    -(2**30),
    2**59,
    -(2**59),
    2**60 - 1,
    -(2**60 - 1),
    2**60,
    -(2**60),
    2**63 - 1,
    -(2**63),
    2**64,
    -(2**100),
    True,
    False,
    IntSub(6),
    1.5,
    frozenset({1, 2}),
    AndProtocol(),
]

SHIFT_COUNTS = [0, 1, 5, 29, 30, 31, 59, 60, 62, 63, 64, 65, 100, -1, -64,
                True, IntSub(3), 1.5]


def check_against_oracle(source, tag, warmups, second_operands):
    oracle = build(source, f"{tag}-oracle")
    names = [n for n in oracle if not n.startswith("__")]
    entries = machine_code_entries()
    for warm_name, warm_args in warmups.items():
        compiled = build(source, f"{tag}-jit-{warm_name}")
        for name in names:
            func = compiled[name]
            for i in range(WARMUP_CALLS):
                # Mixed warm-up operands raise for the bitwise operators.
                outcome(func, *warm_args[i % len(warm_args)])
            compile_now(func)
        for name in names:
            func = compiled[name]
            for a in INT_OPERANDS:
                for b in second_operands:
                    if not cinderjit.is_jit_compiled(func):
                        # A blown fuse uncompiles; keep testing machine code.
                        compile_now(func)
                    expected = outcome(oracle[name], a, b)
                    actual = outcome(func, a, b)
                    assert same(expected, actual), (
                        warm_name, name, a, b, expected, actual)
    assert machine_code_entries() > entries


def case_int_semantics():
    """Unboxed int arithmetic, floor division and modulo by powers of two,
    bitwise operators and shifts match the interpreter across digit and int64
    boundaries, negatives, overflow into arbitrary precision, bools, int
    subclasses, the user number protocol and raising operands."""
    warmups = {
        "int": [(3, 5), (7, 2), (-4, 9)],
        "mixed": [(3, 5), (1.5, 2), (2**70, 3), (True, 1)],
    }
    check_against_oracle(INT_ARITHMETIC, "int", warmups, INT_OPERANDS)
    warmups = {"int": [(3, 2), (-5, 7), (255, 1)]}
    check_against_oracle(INT_SHIFTS, "shift", warmups, SHIFT_COUNTS)


def case_int_fuse():
    """An unboxed int operation deopts on an operand it cannot handle, and a
    site that keeps doing so is recompiled with the generic operation."""
    for source, good, good_result, bad, result in (
        ("def op(a, b):\n    return a & 0xFF\n",
         (6, 3), 6, (2**70 + 5, 0), 5),
        ("def op(a, b):\n    return a + b\n",
         (6, 3), 9, (2**40, 1), 2**40 + 1),
    ):
        op = build(source, "int-fuse")["op"]
        for _ in range(WARMUP_CALLS):
            op(*good)
        compile_now(op)
        before = organic_deopts()
        for _ in range(8):
            assert op(*bad) == result
        assert organic_deopts() - before == 8, (source, organic_deopts() - before)
        if not cinderjit.is_jit_compiled(op):
            compile_now(op)
        after_fuse = organic_deopts()
        entries = machine_code_entries()
        for _ in range(100):
            assert op(*bad) == result
            assert op(*good) == good_result
        assert organic_deopts() == after_fuse, (source, organic_deopts() - after_fuse)
        assert machine_code_entries() - entries >= 100

    # A constant that does not fit in int64 leaves the operation generic: an
    # operand of any type runs without deoptimizing.
    big = build("def big(a):\n    return a & 0x10000000000000000000\n", "int-big")["big"]
    for _ in range(WARMUP_CALLS):
        big(5)
    compile_now(big)
    before = organic_deopts()
    for _ in range(20):
        assert big(AndProtocol()) == "and-protocol"
        assert big(2**70 + 1) == 2**70 & 0x10000000000000000000
    assert organic_deopts() == before, organic_deopts() - before


def case_int_wide_operands():
    """Ints of unknown origin may be arbitrarily wide, so an operator with no
    small-int operand stays generic: 128-bit masks never deoptimize."""
    source = """
def masked(address, mask):
    packed = int(address)
    return (packed & int(mask)) != packed

class Net:
    def __init__(self, ip):
        self._ip = ip

    def within(self, mask):
        return self._ip & mask, self._ip ^ mask
"""
    ns = build(source, "int-wide")
    masked, net = ns["masked"], ns["Net"](2**100 + 7)
    for _ in range(WARMUP_CALLS):
        masked(3, 1)
        net.within(1)
    compile_now(masked)
    compile_now(ns["Net"].within)
    before = organic_deopts()
    entries = machine_code_entries()
    ip = net._ip
    for address, mask in ((2**100, 2**128 - 1), (2**100 + 1, 2**100),
                          (-2**100, -1), (2**32 - 1, 2**32 - 1), (5, 4)):
        assert masked(address, mask) == ((address & mask) != address)
        assert net.within(mask) == (ip & mask, ip ^ mask), mask
    assert organic_deopts() == before, organic_deopts() - before
    assert machine_code_entries() - entries == 10


def case_int_slow_arm_identity():
    """An int kept on registers that also reaches the generic arm of an
    array.array store is passed there as the same object a later deopt
    rebuilds the local from: `is` keeps its interpreter result."""
    source = """
def store(c, i, n, z):
    j = i * n + 1
    c[j] = 0
    return j + z
"""

    class Rec:
        key = None

        def __setitem__(self, key, value):
            Rec.key = key

    class Probe:
        def __radd__(self, other):
            return other is Rec.key

    store = build(source, "int-identity")["store"]
    arr = array.array("l", [0] * 4000)
    for _ in range(WARMUP_CALLS):
        store(arr, 30, 30, 1)
    compile_now(store)
    entries = machine_code_entries()
    assert store(Rec(), 30, 30, Probe()) is True
    assert machine_code_entries() - entries == 1


def case_int_deopt_values():
    """A deopt after unboxed intermediates rebuilds them exactly: the boxes
    the JIT never allocated are recreated from the register values."""
    source = """
def chain(a, b, c):
    x = (a << 4) | b
    y = x ^ c
    return x, y

def index(i, n, c):
    j = i * n + 1
    return j, j + c
"""
    oracle = build(source, "remat-oracle")
    compiled = build(source, "remat-jit")
    for name, warm in (("chain", (3, 1, 2)), ("index", (3, 4, 5))):
        func = compiled[name]
        for _ in range(WARMUP_CALLS):
            func(*warm)
        compile_now(func)
        for args in ((3, 1, 2**100), (3, 1, IntSub(2)), (3, 1, 1.5),
                     (2**58, 3, 2**100), (2**59, 3, 1), (-(2**58), -1, 2**100),
                     (2**29, 2**29, 2**100), (-(2**29), 2**29, 1.5),
                     (2**30, 2, 1)):
            if not cinderjit.is_jit_compiled(func):
                compile_now(func)
            expected = outcome(oracle[name], *args)
            actual = outcome(func, *args)
            assert same(expected, actual), (name, args, expected, actual)


INT64_ARITHMETIC = """
def mask(a, b):
    return (1 << (b & 63)) - 1

def combine(a, b):
    return (a << 24) + b

def spread(a, b):
    return (a << 31) - (b << 31)

def lt_wide(a, b):
    return (a << 20) < (b << 20)

def eq_wide(a, b):
    if (a << 33) == (b << 33):
        return 1
    return 2

def ge_wide(a, b):
    return (a << 3) >= b

def accumulate32(a, b):
    s = a
    for _ in range(4):
        s = (s + b) & 0xFFFFFFFF
    return s
"""

MIXED_ARITHMETIC = """
def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def div(a, b):
    return a / b

def scale(a, b):
    return a * 2.5 + b

def offset(a, b):
    return a + 3 - b

def iadd(a, b):
    a += b
    return a
"""


class NumberProtocol:
    """A user type reached through the generic number protocol."""

    def __add__(self, other):
        return "number-protocol"

    __radd__ = __sub__ = __rsub__ = __mul__ = __rmul__ = __add__
    __truediv__ = __rtruediv__ = __add__


MIXED_OPERANDS = [
    0,
    1,
    -3,
    2**30 - 1,
    2**30,
    -(2**30),
    2**62,
    2**200,
    True,
    0.0,
    -0.0,
    1.5,
    -2.25,
    math.inf,
    -math.inf,
    math.nan,
    1e308,
    IntSub(4),
    FloatSub(2.5),
    NumberProtocol(),
]


def case_int64_semantics():
    """Wide int64 add, subtract, compare and accumulate: overflow past int64
    falls back to arbitrary precision, negatives and the digit boundaries
    match the interpreter, and operands it cannot handle take the generic
    path."""
    warmups = {"int": [(3, 5), (7, 2), (-4, 9), (1, 31)]}
    check_against_oracle(INT64_ARITHMETIC, "int64", warmups, INT_OPERANDS)


def case_mixed_semantics():
    """Int/float mixed arithmetic in the float diamonds matches the
    interpreter: exact conversion of compact ints, signed zeros, infinities,
    NaN, division by zero, non-compact ints, bools, int and float subclasses
    and the user number protocol."""
    oracle = build(MIXED_ARITHMETIC, "mixed-oracle")
    names = [n for n in oracle if not n.startswith("__")]
    warmups = {
        "float": [(1.5, 2.25), (-0.5, 4.0)],
        "mixed": [(1, 2.5), (2.5, 1), (1.5, 2.0), (3, 4)],
    }
    entries = machine_code_entries()
    for warm_name, warm_args in warmups.items():
        compiled = build(MIXED_ARITHMETIC, f"mixed-jit-{warm_name}")
        for name in names:
            func = compiled[name]
            for i in range(WARMUP_CALLS):
                outcome(func, *warm_args[i % len(warm_args)])
            compile_now(func)
        for name in names:
            func = compiled[name]
            for a in MIXED_OPERANDS:
                for b in MIXED_OPERANDS:
                    if not cinderjit.is_jit_compiled(func):
                        compile_now(func)
                    expected = outcome(oracle[name], a, b)
                    actual = outcome(func, a, b)
                    assert same(expected, actual), (
                        warm_name, name, a, b, expected, actual)
    assert machine_code_entries() > entries


def case_mixed_fuse():
    """A float guard that keeps failing on ints is recompiled into a diamond
    that also dispatches int/float and int/int: no further deopts for any of
    the three pairings, and it stays compiled."""
    scale = build("def scale(a, b):\n    return a * b\n", "mixed-fuse")["scale"]
    for _ in range(WARMUP_CALLS):
        assert scale(1.5, 2.0) == 3.0
    compile_now(scale)
    before = organic_deopts()
    for _ in range(8):
        assert scale(3, 2.0) == 6.0
    assert organic_deopts() - before == 8, organic_deopts() - before
    if not cinderjit.is_jit_compiled(scale):
        compile_now(scale)
    after_fuse = organic_deopts()
    for _ in range(100):
        assert scale(3, 2.0) == 6.0
        assert scale(1.5, 2.0) == 3.0
        assert scale(3, 4) == 12
    assert organic_deopts() == after_fuse, organic_deopts() - after_fuse
    assert cinderjit.is_jit_compiled(scale)


ARRAY_ACCESS = """
def total(arr, n):
    s = arr[0] - arr[0]
    for i in range(n):
        s = s + arr[i]
    return s

def item(arr, i):
    # `i + 0` gives the index int evidence (an untyped index is never
    # speculated) and keeps every index's meaning, bad ones included.
    return arr[i + 0]
"""


def case_array_typecode():
    """Subscripts of array.array sites whose interpreter specialisation failed
    read the element by the one typecode the site has seen.  Another typecode,
    a subclass, a look-alike type, a list, a user container and bad indices
    deopt and match the interpreter; a site that keeps seeing another typecode
    is recompiled without the speculation."""
    import array

    class ArraySub(array.array):
        pass

    # Same name and instance size as array.array, different layout: neither
    # the interpreter's record nor the JIT's exact-type guard takes it for an
    # array (case_array_record_identity covers a non-zero layout).
    LookAlike = type(
        "array.array", (), {"__slots__": ("a", "b", "c", "d", "e", "f"),
                            "__getitem__": lambda self, i: ("look-alike", i)})

    class UserSeq:
        def __getitem__(self, i):
            return ("user", i)

    oracle = build(ARRAY_ACCESS, "arrtc-oracle")
    for tc, values, other_tc, other_values in (
        ("d", [0.5, -1.25, 1e300, -0.0, math.inf], "i", [3, -7, 2**31 - 1, 0, 1]),
        ("i", [3, -7, 2**31 - 1, -(2**31), 0], "d", [0.5, -1.25, 2.0, 3.0, 4.0]),
    ):
        ns = build(ARRAY_ACCESS, f"arrtc-{tc}")
        total, item = ns["total"], ns["item"]
        warm = array.array(tc, values)
        for _ in range(WARMUP_CALLS):
            total(warm, len(warm))
            for i in range(len(warm)):
                item(warm, i)
        compile_now(total)
        compile_now(item)

        # The speculated path: no deopt for the warm typecode.  (Only the
        # first two elements are summed: the large ones would leave the
        # compact-int range of the accumulator, which deopts by design.)
        before = organic_deopts()
        for _ in range(50):
            assert same(outcome(oracle["total"], warm, 2), outcome(total, warm, 2))
        assert organic_deopts() == before, (tc, "total", organic_deopts() - before)
        for _ in range(50):
            for i in (0, 1, 4):
                assert same(outcome(oracle["item"], warm, i), outcome(item, warm, i))
        assert organic_deopts() == before, (tc, "item", organic_deopts() - before)

        other = array.array(other_tc, other_values)
        cases = [
            (warm, 0), (warm, 4), (warm, -5), (warm, 5), (warm, -6),
            (warm, True), (warm, 2**40), (warm, 1.0), (warm, IntSub(2)),
            (other, 2), (ArraySub(tc, values), 2), (LookAlike(), 2),
            ([1.5, 2.5, 3.5], 2), (UserSeq(), 2), ((1, 2, 3), 1),
        ]
        for container, index in cases:
            if not cinderjit.is_jit_compiled(item):
                compile_now(item)
            expected = outcome(oracle["item"], container, index)
            actual = outcome(item, container, index)
            assert same(expected, actual), (tc, container, index, expected, actual)

        # A site that keeps seeing another typecode is taken down; the
        # recompiled site handles both without deoptimizing.
        fresh = build(ARRAY_ACCESS, f"arrtc-fuse-{tc}")["item"]
        for _ in range(WARMUP_CALLS):
            for i in range(len(warm)):
                fresh(warm, i)
        compile_now(fresh)
        before = organic_deopts()
        for _ in range(8):
            assert same(("value", other[2]), outcome(fresh, other, 2))
        assert organic_deopts() - before == 8, (tc, organic_deopts() - before)
        if not cinderjit.is_jit_compiled(fresh):
            compile_now(fresh)
        after_fuse = organic_deopts()
        for _ in range(50):
            assert same(("value", other[2]), outcome(fresh, other, 2))
            assert same(("value", warm[1]), outcome(fresh, warm, 1))
        assert organic_deopts() == after_fuse, (tc, organic_deopts() - after_fuse)


ARRTC_TAG = 0x8000
ARRTC_OTHER = 0x80FE


def subscr_cache(func):
    """(opcode name, func_version cache entry) of func's only BINARY_SUBSCR."""
    code = func.__code__
    (offset,) = [
        ins.offset for ins in dis.get_instructions(code)
        if ins.opname == "BINARY_SUBSCR"
    ]
    raw = code._co_code_adaptive
    # Inline cache: counter (1 unit), type_version (2), func_version (1).
    entry = raw[offset + 8:offset + 10]
    return dis._all_opname[raw[offset]], int.from_bytes(entry, sys.byteorder)


FAKE_ARRAY_READ = """
def read(o):
    return o[0]
"""


def case_array_record_identity():
    """The interpreter's subscript record reads an array descriptor only for
    the array.array type itself: a type with the same name and instance size
    but another layout, whose bytes at the descriptor offset are not a
    pointer, is recorded as another container without being read."""
    import itertools

    Fake = type("array.array", (itertools.product,), {
        "__slots__": ("a", "b"),
        "__getitem__": lambda self, key: 42,
    })
    assert Fake.__basicsize__ == array.array.__basicsize__
    fake = Fake([])
    list(fake)  # exhausted: the field at the descriptor offset is set
    read = build(FAKE_ARRAY_READ, "arrtc-fake")["read"]
    for _ in range(100):
        assert read(fake) == 42
    cache = subscr_cache(read)
    assert cache == ("BINARY_SUBSCR_ADAPTIVE", ARRTC_OTHER), cache

    real = build(ARRAY_ACCESS, "arrtc-real")["item"]
    warm = array.array("d", [0.5, 1.5])
    for _ in range(100):
        assert real(warm, 1) == 1.5
    cache = subscr_cache(real)
    assert cache == ("BINARY_SUBSCR_ADAPTIVE", ARRTC_TAG | ord("d")), cache


def case_array_record_leftover_version():
    """A BINARY_SUBSCR_GETITEM site that deopts back to the adaptive form keeps
    its function version in the cache entry that holds the record.  The
    record treats it as empty: the site then records its arrays' typecode,
    and the JIT speculates on it."""
    name, stock_version = subscr_cache(stock_subscr)
    assert name == "BINARY_SUBSCR_GETITEM", name
    assert 0 < stock_version < ARRTC_TAG, stock_version

    item = build(ARRAY_ACCESS, "arrtc-leftover")["item"]
    obj = StockVersioned()
    for i in range(WARMUP_CALLS):
        assert item(obj, i) == ("stock", i)
    cache = subscr_cache(item)
    assert cache == ("BINARY_SUBSCR_GETITEM", stock_version), cache

    StockVersioned.changed = True  # new type version: the site misses
    warm = array.array("d", [0.5, 1.5])
    for _ in range(1000):
        assert item(warm, 1) == 1.5
    cache = subscr_cache(item)
    assert cache == ("BINARY_SUBSCR_ADAPTIVE", ARRTC_TAG | ord("d")), cache

    compile_now(item)
    before = organic_deopts()
    for _ in range(50):
        assert item(warm, 1) == 1.5
    assert organic_deopts() == before, organic_deopts() - before
    # The speculation is there: another typecode fails its guard.
    other = array.array("i", [3, 4])
    for _ in range(8):
        assert item(other, 1) == 4
    assert organic_deopts() - before == 8, organic_deopts() - before


INT64_INPLACE_FUSE = """
def binary(x):
    a = x << 61
    return a + 4611686018427387904

def inplace(x):
    a = x << 61
    a += 4611686018427387904
    return a
"""


def case_int64_inplace_fuse():
    """A wide add whose result leaves int64 deopts; after the fuse takes the
    site down, the recompiled binary and in-place forms both run the generic
    operation and no longer deoptimize."""
    ns = build(INT64_INPLACE_FUSE, "int64-inplace-fuse")
    for name in ("binary", "inplace"):
        fn = ns[name]
        for _ in range(100):
            assert fn(1) == 3 * 2**61
        compile_now(fn)
        before = organic_deopts()
        for _ in range(8):
            assert fn(2) == 2**63
        assert organic_deopts() - before == 8, (name, organic_deopts() - before)
        if not cinderjit.is_jit_compiled(fn):
            compile_now(fn)
        before = organic_deopts()
        entries = machine_code_entries()
        for _ in range(10):
            assert fn(1) == 3 * 2**61
            assert fn(2) == 2**63
        entered = machine_code_entries() - entries
        assert entered == 20, (name, entered)
        assert organic_deopts() == before, (name, organic_deopts() - before)


INT64_MASKED = """
def and_add(x):
    return ((x << 2) & 0x7fffffffffffffff) + 4

def and_sub(x):
    return -4 - ((x << 2) & 0x7fffffffffffffff)

def and_left_add(x):
    return (0x7fffffffffffffff & (x << 2)) + 4

def xor_add(x, y):
    return ((x << 2) ^ (y << 2)) + ((x << 2) ^ (y << 2))

def rshift_add(x):
    return ((x << 2) >> 1) + ((x << 2) >> 1)
"""


def case_int64_bitwise_bounds():
    """Value bounds through &, ^ and >> hold for negative operands: an add or
    subtract they feed keeps its overflow check when a result can leave
    int64, and matches the interpreter on fresh code."""
    oracle = build(INT64_MASKED, "int64-masked-oracle")
    args = {
        "and_add": [(-1,), (-2,), (-(2**60),), (1,), (2**60 - 1,), (0,)],
        "and_sub": [(-1,), (-3,), (-(2**60),), (1,), (2**60 - 1,), (0,)],
        "and_left_add": [(-1,), (-(2**59),), (1,), (2**60 - 1,)],
        "xor_add": [(2**60 - 1, -1), (-1, 2**60 - 1), (1, 2), (-(2**60), 3)],
        "rshift_add": [(-(2**60),), (-(2**60) + 1,), (-7,), (2**60 - 1,), (5,)],
    }
    for name, calls in args.items():
        fn = build(INT64_MASKED, f"int64-masked-{name}")[name]
        for _ in range(WARMUP_CALLS):
            fn(*calls[-1])
        compile_now(fn)
        for call in calls:
            expected = outcome(oracle[name], *call)
            actual = outcome(fn, *call)
            assert same(expected, actual), (name, call, expected, actual)


FRAME_IDENTITY = """
def int_site(x, rhs):
    a = x * 1000
    return a + rhs

def float_site(x, rhs):
    a = x * 1000.5
    return a + rhs
"""


class MarkerError(Exception):
    pass


class Reflected:
    def __init__(self):
        self.raise_it = False
        self.seen = None

    def __radd__(self, other):
        self.seen = other
        if self.raise_it:
            raise MarkerError(other)
        return other


def case_slow_arm_frame_identity():
    """The generic arm of a numeric diamond passes its operand object to a
    reflected method; in the traceback of an exception that method raises,
    the caller's local holding that value is the same object."""
    for name, x in (("int_site", 3), ("float_site", 3.0)):
        fn = build(FRAME_IDENTITY, f"frame-identity-{name}")[name]
        rhs = Reflected()
        for _ in range(5 * WARMUP_CALLS):
            fn(x, rhs)
        compile_now(fn)
        entries = machine_code_entries()
        result = fn(x, rhs)
        assert result is rhs.seen, (name, result, rhs.seen)
        rhs.raise_it = True
        try:
            fn(x, rhs)
        except MarkerError as exc:
            err = exc
        else:
            raise AssertionError(name)
        assert rhs.seen is err.args[0], name
        frames = []
        tb = err.__traceback__
        while tb is not None:
            frames.append(tb.tb_frame)
            tb = tb.tb_next
        (frame,) = [f for f in frames if f.f_code is fn.__code__]
        assert frame.f_locals["a"] is err.args[0], (name, frame.f_locals)
        entered = machine_code_entries() - entries
        assert entered == 2, (name, entered)


SUCCESSOR_IDENTITY = """
def int_site(x, rhs, after):
    a = x * 1000
    result = a + rhs
    after()
    return result

def float_site(x, rhs, after):
    a = x * 1000.5
    result = a + rhs
    after()
    return result
"""


class RaiseLater:
    """Returns normally until switched, then raises the object the reflected
    method kept."""

    def __init__(self, rhs):
        self.rhs = rhs
        self.raise_it = False

    def __call__(self):
        if self.raise_it:
            raise MarkerError(self.rhs.seen)


def case_slow_arm_successor_identity():
    """After the generic arm of a numeric diamond returns normally, a later
    call that raises shows, in the caller's traceback frame, the very object
    the reflected method received and kept."""
    for name, x in (("int_site", 3), ("float_site", 3.0)):
        fn = build(SUCCESSOR_IDENTITY, f"successor-identity-{name}")[name]
        rhs = Reflected()
        after = RaiseLater(rhs)
        for _ in range(5 * WARMUP_CALLS):
            fn(x, rhs, after)
        compile_now(fn)
        entries = machine_code_entries()
        result = fn(x, rhs, after)
        assert result is rhs.seen, (name, result, rhs.seen)
        after.raise_it = True
        try:
            fn(x, rhs, after)
        except MarkerError as exc:
            err = exc
        else:
            raise AssertionError(name)
        assert err.args[0] is rhs.seen, name
        frames = []
        tb = err.__traceback__
        while tb is not None:
            frames.append(tb.tb_frame)
            tb = tb.tb_next
        (frame,) = [f for f in frames if f.f_code is fn.__code__]
        assert frame.f_locals["a"] is err.args[0], (name, frame.f_locals)
        entered = machine_code_entries() - entries
        assert entered == 2, (name, entered)


WIDE_COMPACT_FUSE = """
def cmp_jump(a, b):
    if a < b:
        return 1
    return 0

def iadd(a, b):
    a += b
    return a
"""


def case_compact_compare_inplace_fuse():
    """Plain int objects too wide for the compact guard: a compare fused with
    its jump and an in-place add each take their site down after eight guard
    failures, and the recompiled code stays in machine code without
    deoptimizing."""
    small, wide = (3, 5), (10**20, 10**21)
    for name, small_result, wide_result in (
        ("cmp_jump", 1, 1),
        ("iadd", 8, 10**20 + 10**21),
    ):
        fn = build(WIDE_COMPACT_FUSE, f"compact-fuse-{name}")[name]
        for _ in range(2 * WARMUP_CALLS):
            assert fn(*small) == small_result
        compile_now(fn)
        before = organic_deopts()
        for _ in range(8):
            assert fn(*wide) == wide_result
        assert organic_deopts() - before == 8, (name, organic_deopts() - before)
        if not cinderjit.is_jit_compiled(fn):
            compile_now(fn)
        before = organic_deopts()
        entries = machine_code_entries()
        for _ in range(10):
            assert fn(*small) == small_result
            assert fn(*wide) == wide_result
        entered = machine_code_entries() - entries
        assert entered == 20, (name, entered)
        assert organic_deopts() == before, (name, organic_deopts() - before)


# Ints the two-digit unbox takes (|v| < 2^60), at the digit and int64
# boundaries the shifted forms of INT64_ARITHMETIC cross.
INT64_BOUNDARY_OPERANDS = [
    0, 1, -1, 2**29, 2**30, -(2**30), 2**59, -(2**59), 2**60 - 1, -(2**60 - 1),
]


def case_int64_boundaries_unfused():
    """Every int64 boundary pair runs as the first machine-code call of a
    freshly compiled code object, so its result comes from the int64 lowering
    (or the deopt of its overflow check), never from a site an earlier input
    already took down."""
    oracle = build(INT64_ARITHMETIC, "int64-boundary-oracle")
    names = [n for n in oracle if not n.startswith("__")]
    warm = [(3, 5), (7, 2), (-4, 9), (1, 31)]
    for name in names:
        for a in INT64_BOUNDARY_OPERANDS:
            for b in INT64_BOUNDARY_OPERANDS:
                func = build(INT64_ARITHMETIC, f"int64-boundary-{name}")[name]
                for i in range(WARMUP_CALLS):
                    func(*warm[i % len(warm)])
                compile_now(func)
                entries = machine_code_entries()
                expected = outcome(oracle[name], a, b)
                actual = outcome(func, a, b)
                assert same(expected, actual), (name, a, b, expected, actual)
                assert machine_code_entries() - entries == 1, (name, a, b)


CASES = {
    "forced": case_forced,
    "semantics": case_semantics,
    "fuse": case_fuse,
    "fuse_by_code": case_fuse_by_code,
    "exc_edge": case_exc_edge,
    "identity": case_identity,
    "lifetime": case_lifetime,
    "int_semantics": case_int_semantics,
    "int_fuse": case_int_fuse,
    "int_deopt_values": case_int_deopt_values,
    "int_wide_operands": case_int_wide_operands,
    "int_slow_arm_identity": case_int_slow_arm_identity,
    "int64_semantics": case_int64_semantics,
    "mixed_semantics": case_mixed_semantics,
    "mixed_fuse": case_mixed_fuse,
    "array_typecode": case_array_typecode,
    "array_record_identity": case_array_record_identity,
    "array_record_leftover_version": case_array_record_leftover_version,
    "int64_inplace_fuse": case_int64_inplace_fuse,
    "int64_bitwise_bounds": case_int64_bitwise_bounds,
    "slow_arm_frame_identity": case_slow_arm_frame_identity,
    "slow_arm_successor_identity": case_slow_arm_successor_identity,
    "compact_compare_inplace_fuse": case_compact_compare_inplace_fuse,
    "int64_boundaries_unfused": case_int64_boundaries_unfused,
}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in CASES:
        raise SystemExit(f"usage: {sys.argv[0]} {{{'|'.join(CASES)}}}")
    CASES[sys.argv[1]]()
    print(f"CASE_OK {sys.argv[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
