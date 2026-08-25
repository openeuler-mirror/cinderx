"""有机 deopt-resume 形态定向用例（M9 第三轮蒸馏）。

覆盖三个真实现场蒸馏出的恢复形态，全部依赖"解释器充分量化 →
编译 → 中途类型/全局变异触发守卫失败 → 任意字节码位带活操作数栈
恢复"这一链条（checkpoint 强制去优化只覆盖调用边界，抓不到这类）：

1. LOAD_METHOD 非方法形态 pair（[callable, NULL]）活在栈上时 deopt：
   3.11 解释器约定 NULL 标志位在下槽，JIT 内部约定在上槽，恢复时
   必须按运行时形态交换（deltablue Strength.weakest_of 现场）。
2. LOAD_GLOBAL（oparg&1，NULL+值双推）守卫失败 deopt：守卫 FrameState
   必须是指令边界前状态，否则恢复重执行后栈上多一个 NULL 哨兵
   （sqlglot _quotes_to_format 现场）。
3. WITH_VALUES 方法快路径的多态接收者：接收者 GuardType 的类型依赖
   必须以 UseType 钉住，否则 GuardTypeRemoval 删除守卫后异类型接收者
   直拿缓存方法常量（raytrace intersectionTime 现场）。

用例自恢复：类/全局变异在 finally 中还原。
"""

import sys


class _Recv:
    def __init__(self):
        self.x = 3
        self.y = 4


def _mod_fn(a, b):
    return ("mod_fn", a, b)


class _WithCm:
    @classmethod
    def cm(cls, a, b):
        return (cls.__name__, a, b)


def _pair_window_call(r):
    # 本模块作为 module receiver：LOAD_METHOD _mod_fn 走非方法形态
    # （[callable, NULL] pair）；窗口内的 r.x/r.y 属性加载在量化后带
    # 版本守卫，类变异使其失败 → pair 活在栈上的有机 deopt。
    mod = sys.modules[__name__]
    return mod._mod_fn(r.x, r.y)


def case_deopt_live_method_pair_not_found_form():
    r = _Recv()
    out = []
    for _ in range(40):
        out.append(_pair_window_call(r))
    try:
        _Recv.mutated = 1  # tp_version_tag 失效 → 窗口内守卫失败
        out.append(_pair_window_call(r))
    finally:
        del _Recv.mutated
    return out[-2:]


case_deopt_live_method_pair_not_found_form.helpers = (_pair_window_call,)


def _classmethod_window_call(r):
    # 类接收者经 generic 路径拿到 bound classmethod（非方法形态 pair），
    # 窗口内属性守卫失败复现 deltablue 的双重前置 cls 形态。
    return _WithCm.cm(r.x, r.y)


def case_deopt_live_classmethod_pair():
    r = _Recv()
    out = []
    for _ in range(40):
        out.append(_classmethod_window_call(r))
    try:
        _Recv.mutated2 = 1
        out.append(_classmethod_window_call(r))
    finally:
        del _Recv.mutated2
    return out[-2:]


case_deopt_live_classmethod_pair.helpers = (_classmethod_window_call,)


def _global_window_call(r):
    # LOAD_GLOBAL _mod_fn（NULL+值双推）后接受守卫的属性加载；全局字典
    # 键结构变异使 LOAD_GLOBAL_MODULE 守卫失败 → 恢复重执行 LOAD_GLOBAL。
    return _mod_fn(r.x, r.y)


def case_deopt_load_global_null_pair():
    r = _Recv()
    out = []
    g = globals()
    added = []
    try:
        for i in range(40):
            out.append(_global_window_call(r))
            if i == 30:
                # 插入新全局键 → dk_version 变化 → 下次调用守卫失败
                name = "_dg_scratch_key"
                g[name] = i
                added.append(name)
        out.append(_global_window_call(r))
    finally:
        for name in added:
            g.pop(name, None)
    return out[-2:]


case_deopt_load_global_null_pair.helpers = (_global_window_call,)


class _Poly1:
    def __init__(self):
        self.v = 1

    def m(self):
        return ("P1", self.v)


class _Poly2:
    def __init__(self):
        self.v = 2

    def m(self):
        return ("P2", self.v)


def _poly_call(o):
    return o.m()


def case_polymorphic_with_values_method_dispatch():
    a = _Poly1()
    b = _Poly2()
    out = []
    for _ in range(64):
        out.append(_poly_call(a))  # 单态量化 → WITH_VALUES 特化
    out.append(_poly_call(b))  # 异类型接收者必须正确派发
    out.append(_poly_call(a))
    return out[-3:]


case_polymorphic_with_values_method_dispatch.helpers = (_poly_call,)


class _ExitCtx:
    # [P5] 现场蒸馏：__exit__ 体内剥离 exc 的最后一个 traceback 引用后，
    # tb 实参的借用生命周期只能依靠调用方（WITH_EXCEPT_START）持有的
    # 引用；补丁前该引用在调用发起前即被归还，编译版 __exit__ 的任意
    # 后续 deopt 物化都会对尸体增减引用（M9 全表面 SEGV 四案）。
    def __exit__(self, exc_type, exc_value, tb):
        exc_value.with_traceback(None)
        _mod_fn(1, 2)  # LOAD_GLOBAL 守卫窗口（deopt 触发位）
        return True

    def __enter__(self):
        return self


def _with_exit_deopt_call():
    with _ExitCtx():
        raise ValueError("boom")
    return "handled"


def case_deopt_exit_strips_traceback():
    out = []
    g = globals()
    added = []
    try:
        for i in range(40):
            out.append(_with_exit_deopt_call())
            if i == 30:
                name = "_dg_scratch_key_p5"
                g[name] = i  # dk_version 漂移 → __exit__ 内 LOAD_GLOBAL 守卫失败
                added.append(name)
        out.append(_with_exit_deopt_call())
    finally:
        for name in added:
            g.pop(name, None)
    return out[-2:]


case_deopt_exit_strips_traceback.helpers = (
    _with_exit_deopt_call,
    _ExitCtx.__exit__,
)
