"""Control-flow matrix: with statement (BEFORE_WITH / WITH_EXCEPT_START),
try/finally interactions, match patterns, boolean short-circuit variants,
set/dict display opcodes, assert, and an EXTENDED_ARG synthetic case.

The with/finally block is the main road of 3.11's exception-table unwinding —
the highest-risk area for a JIT that must replicate zero-cost exception
semantics (known-issue family ⓪).
"""

import contextlib


class Ctx:
    """Context manager with scriptable behavior, logging every step."""

    def __init__(self, log, tag, swallow=False, raise_on_exit=None,
                 raise_on_enter=None):
        self.log, self.tag = log, tag
        self.swallow, self.raise_on_exit = swallow, raise_on_exit
        self.raise_on_enter = raise_on_enter

    def __enter__(self):
        self.log.append("enter-" + self.tag)
        if self.raise_on_enter:
            raise self.raise_on_enter
        return self.tag

    def __exit__(self, etype, evalue, tb):
        self.log.append("exit-{}-{}".format(
            self.tag, etype.__name__ if etype else "clean"))
        if self.raise_on_exit:
            raise self.raise_on_exit
        return self.swallow


# ---- with 语句族 ----

def case_with_clean():
    log = []
    with Ctx(log, "a") as v:
        log.append("body-" + v)
    return log


def case_with_exception_propagates():
    log = []
    try:
        with Ctx(log, "a"):
            raise ValueError("inside")
    except ValueError as e:
        log.append("caught-" + str(e))
    return log


def case_with_exit_swallows():
    log = []
    with Ctx(log, "a", swallow=True):
        raise KeyError("swallowed")
    log.append("after")
    return log


def case_with_exit_raises_new():
    log = []
    try:
        with Ctx(log, "a", raise_on_exit=RuntimeError("from-exit")):
            raise ValueError("original")
    except RuntimeError as e:
        return (log, str(e), type(e.__context__).__name__)


def case_with_enter_raises():
    log = []
    try:
        with Ctx(log, "a", raise_on_enter=OSError("enter-fail")):
            log.append("never")
    except OSError as e:
        return (log, str(e))


def case_with_return_inside():
    log = []

    def f():
        with Ctx(log, "a"):
            return "returned"

    return (f(), log)


def case_with_break_inside_loop():
    log = []
    for i in range(3):
        with Ctx(log, str(i)):
            if i == 1:
                break
    return log


def case_with_multiple_managers():
    log = []
    with Ctx(log, "a"), Ctx(log, "b", swallow=True):
        raise IndexError("multi")
    return log


def case_with_nested_finally():
    log = []
    try:
        with Ctx(log, "outer"):
            try:
                raise ValueError("v")
            finally:
                log.append("finally")
    except ValueError:
        log.append("caught")
    return log


def case_with_contextlib_suppress():
    with contextlib.suppress(ZeroDivisionError):
        return 1 // 0
    return "suppressed"


# ---- try/finally 交互 ----

def case_finally_runs_on_return():
    log = []

    def f():
        try:
            return "from-try"
        finally:
            log.append("finally")

    return (f(), log)


def case_return_in_finally_overrides():
    def f():
        try:
            return "from-try"
        finally:
            return "from-finally"  # noqa: B012

    return f()


def case_finally_swallows_by_break():
    out = []
    for i in range(3):
        try:
            if i == 1:
                raise ValueError("gone")
        finally:
            if i == 1:
                break  # noqa: B012  吞掉在途异常
        out.append(i)
    return out


def case_continue_in_finally():
    out = []
    for i in range(3):
        try:
            out.append(i)
        finally:
            continue  # noqa: B012
        out.append("never")  # noqa: F841
    return out


def case_nested_finally_order():
    log = []
    try:
        try:
            raise KeyError("k")
        finally:
            log.append("inner")
    except KeyError:
        log.append("caught")
    finally:
        log.append("outer")
    return log


def case_full_ladder():
    log = []
    try:
        log.append("try")
    except ValueError:
        log.append("except")
    else:
        log.append("else")
    finally:
        log.append("finally")
    return log


def case_exception_in_else():
    log = []
    try:
        try:
            log.append("try")
        except KeyError:
            log.append("except")
        else:
            raise ValueError("from-else")
    except ValueError:
        log.append("caught")
    return log


def case_bare_reraise():
    try:
        try:
            raise OSError("orig")
        except OSError:
            raise
    except OSError as e:
        return str(e)


def case_raise_from_none():
    try:
        try:
            raise KeyError("inner")
        except KeyError:
            raise ValueError("outer") from None
    except ValueError as e:
        return (str(e), e.__cause__, e.__suppress_context__,
                type(e.__context__).__name__)


# ---- assert ----

def case_assert_pass():
    x = 1
    assert x == 1, "unreachable"
    return "ok"


def case_assert_fail_with_msg():
    x = 2
    assert x == 1, "x should be {}".format(1)


def case_assert_fail_no_msg():
    assert []


# ---- match 模式 ----

def case_match_mapping():
    value = {"kind": "point", "x": 1, "y": 2}
    match value:
        case {"kind": "point", "x": x, **rest}:
            return ("point", x, sorted(rest.items()))
        case _:
            return "no"


def case_match_sequence():
    match [1, 2, 3, 4]:
        case [first, *middle, last]:
            return (first, middle, last)


def case_match_guard_and_or():
    def m(v):
        match v:
            case int() | float() if v > 10:
                return "big-number"
            case int():
                return "small-int"
            case [x] if x is None:
                return "single-none"
            case _:
                return "other"

    return [m(3), m(11), m(2.5), m([None]), m("s")]


def case_match_class_positional():
    class P:
        __match_args__ = ("x", "y")

        def __init__(self, x, y):
            self.x, self.y = x, y

    match P(1, 2):
        case P(a, b):
            return (a, b)


# ---- 布尔短路与回跳分支 ----

def case_and_or_chain_values():
    a, b = 0, "yes"
    return (a and b, a or b, b and a, b or a,
            None or [] or "last", 1 and 2 and 3)


def case_ternary_chain():
    x = 5
    return ("big" if x > 10 else "mid" if x > 3 else "small",
            [v if v else "z" for v in (0, 1, "", "s")])


def case_while_is_none_backjump():
    slots = [None, None, "found"]
    i = 0
    cur = slots[0]
    while cur is None:
        i += 1
        cur = slots[i]
    return (i, cur)


def case_while_not_none_backjump():
    node = {"v": 1, "next": {"v": 2, "next": {"v": 3, "next": None}}}
    out = []
    while node is not None:
        out.append(node["v"])
        node = node["next"]
    return out


def case_while_else_and_bool_or_pop():
    n, hits = 0, []
    while n < 5 and (n % 2 == 0 or n == 3):
        hits.append(n)
        n += 1
    else:
        hits.append("else")
    return hits


# ---- 容器构建 opcode ----

def case_set_literal_and_comprehension():
    s = {3, 1, 2, 1}
    t = {x * 2 for x in range(4) if x != 2}
    return (sorted(s), sorted(t), sorted({*s, *t, 99}))


def case_dict_merge_operators():
    a, b = {"x": 1, "y": 2}, {"y": 20, "z": 30}
    merged = a | b
    a |= b
    return (sorted(merged.items()), sorted(a.items()))


def case_star_displays():
    a, b = [1, 2], (3, 4)
    return ([*a, *b, 5], (*a, *b), [*"ab"],
            {**{"k": 1}, **{"j": 2}})


def case_frozenset_const():
    x = 2
    return x in {1, 2, 3}  # 常量 frozenset 优化路径


# ---- EXTENDED_ARG 合成用例（>255 个局部变量，oparg 超一字节） ----

_lines = ["def _extended_arg_fn():"]
_lines += ["    v{} = {}".format(i, i) for i in range(300)]
_lines += ["    if v299 > v0:",
           "        return v256 + v257",
           "    return -1"]
_EXT_SRC = "\n".join(_lines)
_ns = {}
exec(compile(_EXT_SRC, "<diffgate-extarg>", "exec"), _ns)
_extended_arg_fn = _ns["_extended_arg_fn"]


def case_extended_arg_locals():
    return _extended_arg_fn()


case_extended_arg_locals.helpers = [_extended_arg_fn]

