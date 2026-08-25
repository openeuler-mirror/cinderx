"""Frame introspection / traceback / generator matrix, including forced-deopt
checkpoints.

diffgate_rt.checkpoint() is a no-op under interp/jit and force-uncompiles the
current case's functions under jit_deopt — so cases that call it mid-execution
(especially with a suspended generator alive) exercise the deopt/resume path
that produced real bugs on the 3.14 branch (generator deopt resume indexes).
"""

import sys
import traceback

import diffgate_rt


def depth3_names():
    def level2():
        def level1():
            names = []
            f = sys._getframe()
            while f is not None and len(names) < 6:
                names.append(f.f_code.co_name)
                f = f.f_back
            return names

        return level1()

    return level2()


def raise_nested():
    def inner(x):
        return 1 // x

    return inner(0)


def gen_counter(n):
    total = 0
    for i in range(n):
        total += i
        yield total


def gen_with_finally(log):
    try:
        yield "first"
        yield "second"
    finally:
        log.append("finally-ran")


def gen_delegating(n):
    result = yield from gen_counter(n)
    yield ("delegated-done", result)


def case_getframe_name_chain():
    return depth3_names()[:3]


def case_traceback_last_frames():
    try:
        raise_nested()
    except ZeroDivisionError as e:
        lines = traceback.format_exception(type(e), e, e.__traceback__)
        tail = "".join(lines[-4:]).strip().splitlines()
        return [ln.strip() for ln in tail]


def case_locals_snapshot():
    a, b = 1, "two"
    snap = sorted(locals().items())
    return snap


def case_exception_context_chain():
    try:
        try:
            raise KeyError("inner")
        except KeyError:
            raise ValueError("outer")
    except ValueError as e:
        return (str(e), type(e.__context__).__name__, str(e.__context__))


def case_exception_cause_chain():
    try:
        try:
            raise KeyError("inner")
        except KeyError as ke:
            raise ValueError("outer") from ke
    except ValueError as e:
        return (str(e), type(e.__cause__).__name__, e.__suppress_context__)


def case_exception_group_basic():
    try:
        raise ExceptionGroup("grp", [ValueError("v1"), TypeError("t1")])
    except* ValueError as eg:
        caught = [str(x) for x in eg.exceptions]
    return caught


def case_gen_resume_after_checkpoint():
    g = gen_counter(4)
    first = next(g)
    diffgate_rt.checkpoint()  # uncompile while suspended, then resume
    rest = list(g)
    return (first, rest)


def case_gen_throw_after_checkpoint():
    g = gen_counter(4)
    next(g)
    diffgate_rt.checkpoint()
    try:
        g.throw(RuntimeError("thrown-in"))
    except RuntimeError as e:
        return "RuntimeError: {}".format(e)


def case_gen_close_runs_finally():
    log = []
    g = gen_with_finally(log)
    first = next(g)
    diffgate_rt.checkpoint()
    g.close()
    return (first, log)


def case_gen_frame_lineno_while_suspended():
    g = gen_counter(3)
    next(g)
    frame = g.gi_frame
    rel = frame.f_lineno - g.gi_code.co_firstlineno
    return ("suspended-rel-line", rel)


def case_yield_from_across_checkpoint():
    g = gen_delegating(3)
    first = next(g)
    diffgate_rt.checkpoint()
    rest = list(g)
    return (first, rest)


def case_gen_return_value_stopiteration():
    def gen():
        yield 1
        return "final-value"

    g = gen()
    next(g)
    try:
        next(g)
    except StopIteration as e:
        return e.value


def case_coroutine_send_smoke():
    async def coro():
        return "coro-result"

    c = coro()
    try:
        c.send(None)
    except StopIteration as e:
        return e.value
    finally:
        c.close()


def case_recursion_error_type():
    limit = sys.getrecursionlimit()

    def recurse(n):
        return recurse(n + 1)

    try:
        sys.setrecursionlimit(120)
        try:
            recurse(0)
        except RecursionError:
            return "RecursionError"
    finally:
        sys.setrecursionlimit(limit)


def case_exc_info_inside_handler():
    try:
        raise LookupError("look")
    except LookupError:
        etype, evalue, tb = sys.exc_info()
        return (etype.__name__, str(evalue), tb is not None)


def case_frame_lineno_relative():
    frame = sys._getframe()
    return frame.f_lineno - frame.f_code.co_firstlineno


case_getframe_name_chain.helpers = [depth3_names]
case_traceback_last_frames.helpers = [raise_nested]
case_gen_resume_after_checkpoint.helpers = [gen_counter]
case_gen_throw_after_checkpoint.helpers = [gen_counter]
case_gen_close_runs_finally.helpers = [gen_with_finally]
case_gen_frame_lineno_while_suspended.helpers = [gen_counter]
case_yield_from_across_checkpoint.helpers = [gen_delegating, gen_counter]
