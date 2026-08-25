"""Hot-loop cases: sustained execution with verifiable checksums.

Fills the "heat" gap that both the one-behavior matrices and Lib/test leave
open: counters, inline-cache repatching, and deopt-under-heat only misbehave
after many iterations. Iteration counts are chosen to keep the whole module
under ~30s in interpreter mode.
"""

import diffgate_rt

N = 3_000_000


def hot_int_accumulate(n):
    total = 0
    for i in range(n):
        total += i * 3 - (i >> 1)
    return total


def hot_float_kernel(n):
    s = 0.0
    x = 1.000001
    for _ in range(n // 10):
        s = s + x * x - 0.5
        x = x + 1e-9
    return round(s, 6)


def hot_str_build(n):
    parts = []
    for i in range(n // 100):
        parts.append(str(i & 0xFF))
    joined = ",".join(parts)
    return (len(joined), hash(joined) & 0xFFFF == hash(joined) & 0xFFFF,
            joined[:20], joined[-20:])


def hot_exception_every_k(n, k):
    caught = 0
    total = 0
    for i in range(n // 30):
        try:
            if i % k == 0:
                raise ValueError(i)
            total += i
        except ValueError as e:
            caught += e.args[0]
    return (total, caught)


def hot_gen_pipeline(n):
    def source(m):
        for i in range(m):
            yield i

    def double(it):
        for v in it:
            yield v * 2

    total = 0
    for v in double(source(n // 30)):
        total += v
    return total


def case_hot_int_accumulate():
    return hot_int_accumulate(N)


def case_hot_float_kernel():
    return hot_float_kernel(N)


def case_hot_str_build():
    return hot_str_build(N)


def case_hot_exception_every_k():
    # 高温 + 异常路径的组合：已知问题 ⓪ 类的加压探针
    return hot_exception_every_k(N, 7)


def case_hot_gen_pipeline():
    return hot_gen_pipeline(N)


def case_hot_deopt_midway():
    # 跑热后强制反优化（jit_deopt 模式下），再继续跑，校验和必须一致
    first = hot_int_accumulate(N // 2)
    diffgate_rt.checkpoint()
    second = hot_int_accumulate(N // 2)
    return (first, second, first == second)


case_hot_int_accumulate.helpers = [hot_int_accumulate]
case_hot_float_kernel.helpers = [hot_float_kernel]
case_hot_str_build.helpers = [hot_str_build]
case_hot_exception_every_k.helpers = [hot_exception_every_k]
case_hot_gen_pipeline.helpers = [hot_gen_pipeline]
case_hot_deopt_midway.helpers = [hot_int_accumulate]
