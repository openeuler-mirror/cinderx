"""Sync-generator protocol matrix (M8): send/throw/close/yield-from,
return-value, PEP 479, interleaving, dealloc-runs-finally, suspension-point
deopt via diffgate_rt.checkpoint(), and hot resume loops.

Async generators / coroutines are deliberately absent: on 3.11 they are
refused at eligibility (D6) and never enter the JIT; their behavior is
covered by the refusal smoke, not by this differential module.
"""

import diffgate_rt


def gen_acc(n):
    total = 0
    for i in range(n):
        got = yield i
        if got:
            total += got
    return total


def gen_finally(log):
    try:
        yield "a"
        yield "b"
    finally:
        log.append("finally")


def gen_catches(n):
    caught = []
    for i in range(n):
        try:
            yield i
        except ValueError as exc:
            caught.append(str(exc))
    return caught


def gen_delegate(n):
    inner_result = yield from gen_acc(n)
    yield ("inner-done", inner_result)


def gen_recursive(n):
    if n > 0:
        yield n
        yield from gen_recursive(n - 1)


def gen_many_locals(base):
    a, b, c, d, e = base, base + 1, base + 2, base + 3, base + 4
    f, g, h, i, j = base + 5, base + 6, base + 7, base + 8, base + 9
    yield a + b
    c += a
    d += b
    yield c + d
    e += c
    f += d
    yield e + f
    return a + b + c + d + e + f + g + h + i + j


def gen_cellvar(mult):
    def scale(x):
        return x * mult

    yield scale(1)
    yield scale(2)


def gen_pep479():
    yield 1
    raise StopIteration("smuggled")


def gen_hot(n):
    total = 0
    for i in range(n):
        total += i
        yield total


def case_basic_send_and_return():
    it = gen_acc(3)
    first = next(it)
    second = it.send(10)
    third = it.send(20)
    try:
        it.send(30)
    except StopIteration as exc:
        return (first, second, third, exc.value)


case_basic_send_and_return.helpers = (gen_acc,)


def case_send_to_unstarted():
    it = gen_acc(3)
    try:
        it.send(42)
    except TypeError as exc:
        # The invalid first send is rejected before generator machine code
        # can run.  Exercise a fresh instance normally as the per-case JIT
        # entry witness while preserving the negative-path result.
        assert next(gen_acc(1)) == 0
        return type(exc).__name__


case_send_to_unstarted.helpers = (gen_acc,)


def case_throw_caught_inside():
    it = gen_catches(3)
    next(it)
    a = it.throw(ValueError("v1"))
    b = it.throw(ValueError("v2"))
    try:
        it.send(None)
        it.send(None)
    except StopIteration as exc:
        return (a, b, exc.value)


case_throw_caught_inside.helpers = (gen_catches,)


def case_throw_uncaught():
    it = gen_acc(3)
    next(it)
    try:
        it.throw(KeyError("boom"))
    except KeyError as exc:
        return ("propagated", str(exc))


case_throw_uncaught.helpers = (gen_acc,)


def case_close_runs_finally():
    log = []
    it = gen_finally(log)
    next(it)
    result = it.close()
    return (result, log)


case_close_runs_finally.helpers = (gen_finally,)


def case_dealloc_runs_finally():
    log = []
    it = gen_finally(log)
    next(it)
    del it
    return log


case_dealloc_runs_finally.helpers = (gen_finally,)


def case_break_out_of_for():
    log = []
    for value in gen_finally(log):
        break
    return (value, log)


case_break_out_of_for.helpers = (gen_finally,)


def case_yield_from_return_value():
    values = []
    it = gen_delegate(3)
    try:
        v = next(it)
        while True:
            values.append(v)
            v = it.send(5)
    except StopIteration:
        return values


case_yield_from_return_value.helpers = (gen_delegate, gen_acc)


def case_yield_from_throw_passthrough():
    it = gen_delegate(5)
    next(it)
    try:
        it.throw(KeyError("through"))
    except KeyError as exc:
        return ("through", str(exc))


case_yield_from_throw_passthrough.helpers = (gen_delegate, gen_acc)


def case_recursive_yield_from():
    return list(gen_recursive(6))


case_recursive_yield_from.helpers = (gen_recursive,)


def case_many_locals_across_yields():
    it = gen_many_locals(100)
    seen = [next(it), next(it), next(it)]
    try:
        next(it)
    except StopIteration as exc:
        return (seen, exc.value)


case_many_locals_across_yields.helpers = (gen_many_locals,)


def case_cellvar_in_gen():
    return list(gen_cellvar(7))


case_cellvar_in_gen.helpers = (gen_cellvar,)


def case_pep479_stopiteration_to_runtimeerror():
    it = gen_pep479()
    next(it)
    try:
        next(it)
    except RuntimeError as exc:
        return ("pep479", type(exc.__cause__).__name__)


case_pep479_stopiteration_to_runtimeerror.helpers = (gen_pep479,)


def case_interleaved_instances():
    g1 = gen_acc(3)
    g2 = gen_acc(3)
    order = [next(g1), next(g2), g1.send(1), g2.send(2), g1.send(3)]
    return order


case_interleaved_instances.helpers = (gen_acc,)


def case_genexpr_consumed():
    return sum(x * x for x in range(10))


def case_gi_frame_lineno_at_suspension():
    it = gen_acc(3)
    next(it)
    fr = it.gi_frame
    rel = fr.f_lineno - gen_acc.__code__.co_firstlineno
    return (rel, it.gi_running, it.gi_yieldfrom)


case_gi_frame_lineno_at_suspension.helpers = (gen_acc,)


def case_gi_frame_locals_at_suspension():
    # Known-gap probe: register-resident locals of a suspended JIT frame.
    it = gen_acc(3)
    next(it)
    it.send(10)
    loc = it.gi_frame.f_locals
    return sorted((k, loc[k]) for k in loc)


case_gi_frame_locals_at_suspension.helpers = (gen_acc,)


def case_hot_resume_loop():
    total = 0
    for value in gen_hot(500):
        total ^= value
    return total


case_hot_resume_loop.helpers = (gen_hot,)


def case_resume_after_checkpoint():
    it = gen_acc(4)
    first = next(it)
    diffgate_rt.checkpoint()  # uncompile while suspended, then resume
    rest = [it.send(1), it.send(2), it.send(3)]
    try:
        it.send(4)
    except StopIteration as exc:
        return (first, rest, exc.value)


case_resume_after_checkpoint.helpers = (gen_acc,)


def case_throw_after_checkpoint():
    it = gen_catches(2)
    next(it)
    diffgate_rt.checkpoint()
    caught = it.throw(ValueError("post-deopt"))
    try:
        it.send(None)
    except StopIteration as exc:
        return (caught, exc.value)


case_throw_after_checkpoint.helpers = (gen_catches,)


def case_close_after_checkpoint():
    log = []
    it = gen_finally(log)
    next(it)
    diffgate_rt.checkpoint()
    it.close()
    return log


case_close_after_checkpoint.helpers = (gen_finally,)


def case_yield_from_across_checkpoint():
    it = gen_delegate(4)
    values = [next(it)]
    diffgate_rt.checkpoint()  # outer+inner both uncompiled while suspended
    try:
        while True:
            values.append(it.send(3))
    except StopIteration:
        return values


case_yield_from_across_checkpoint.helpers = (gen_delegate, gen_acc)


def gen_boom():
    yield 1
    raise ValueError("boom")


def case_gen_exception_exit_state():
    # M8"完成路径运行态残留"案的蒸馏：异常退出后生成器必须为完成态
    # （后续 next 得 StopIteration 而非 "generator already executing"），
    # 且析构/close 不产生 Exception ignored。
    g = gen_boom()
    out = [next(g)]
    try:
        next(g)
    except ValueError as exc:
        out.append(("ve", str(exc)))
    try:
        next(g)
        out.append("no-exc!?")
    except StopIteration:
        out.append("exhausted")
    except ValueError as exc:
        out.append(("already-executing", str(exc)))
    return out


case_gen_exception_exit_state.helpers = (gen_boom,)
