# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Semantics of compiled `except` handlers on CPython 3.11: a raising call,
binary operation, subscript or store inside a try block takes a CFG edge
into the compiled handler once its site is granted (after eight exception
deopts), and the function stays compiled across the grant.  Every probe
prints a transcript that is compared between the JIT and the interpreter:
handler entry, sys.exc_info() inside and after the handler, __context__
chaining, bare re-raise, handler-side return/continue/break, nested try,
finally, traceback line numbers through a handler-free frame, and the
settrace fallback.  The last transcript entry is the compiled state: a
function whose every raise sits at an edge-capable site must stay compiled
past the deopt budget (the edge is taken, nothing deopts), and one that
re-raises or raises inside its handler must be withdrawn by that budget (the
raise statement and the handler body keep the deopt exit)."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import json
import os
import sys
import traceback

JIT = os.environ.get("CINDERX_TEST_REQUIRE_JIT", "1") == "1"
if JIT:
    import _cinderx
    import cinderx
    cinderx.init()
    assert cinderx.is_initialized(), "CinderX must be initialized"
    _cinderx.install_frame_evaluator()
    assert _cinderx.is_frame_evaluator_installed(), "evaluator must be installed"
    import cinderjit
    assert cinderjit.is_enabled(), "JIT must be enabled"
else:
    cinderjit = None

LOG = []


def compiled(fn):
    if JIT:
        assert cinderjit.force_compile(fn) is True, fn
        assert cinderjit.is_jit_compiled(fn), fn
    return fn


def still_compiled(fn):
    return cinderjit.is_jit_compiled(fn) if JIT else True


def compiled_state(edge_functions, deopt_functions=()):
    # Edge-capable functions stay compiled past the budget; functions that
    # deopt on every round (re-raise, raise inside a handler) are withdrawn
    # by it.  Both groups are asserted so the transcript cannot pass with
    # the edges never taken.
    if not JIT:
        return True
    return all(still_compiled(f) for f in edge_functions) and not any(
        still_compiled(f) for f in deopt_functions
    )


def exc_summary(exc):
    return [type(exc).__name__, str(exc)]


def tb_lines(exc):
    return [(f.name, f.lineno - BASE) for f in traceback.extract_tb(exc.__traceback__)]


def exc_type():
    # Read from a helper: a subscript outside a handler would keep the
    # probe function off the execute surface (subscripts are admitted in
    # numeric kernels only), while one inside a handler is translated.
    return sys.exc_info()[0]


# The execute surface admits BINARY_SUBSCR/STORE_SUBSCR in numeric kernels
# only (a loop or a store plus float arithmetic); the subscript probes carry
# a one-iteration float loop so their subscripts are compiled.


# Every probe repeats its scenario past the eighth raise, so the sites are
# granted and the function recompiled with the edge while it runs.  The
# count also exceeds the deopt budget (32 per code object): a site whose
# exit kept deopting after its grant would have the machine code withdrawn,
# and the compiled-state line at the end of each transcript would say so.
ROUNDS = 64
"""


HANDLER_SHAPES_PROBE = PREAMBLE + """
BASE = 0


def boom(x):
    raise ValueError("boom %r" % (x,))


@compiled
def call_site(x):
    try:
        return boom(x)
    except ValueError as exc:
        return ("caught", exc_summary(exc), sys.exc_info()[0] is ValueError)


@compiled
def binop_site(a, b):
    try:
        return a + b
    except TypeError as exc:
        # No slicing in a handler: BUILD_SLICE is outside the execute
        # whitelist and would keep the handler interpreted.
        return ("type", str(exc))


@compiled
def subscr_sites(d, lst, s, key):
    out = []
    acc = 0.5
    for _ in range(1):
        acc = acc * 1.5
    try:
        out.append(d[key])
    except KeyError as exc:
        out.append(("KeyError", exc.args))
    try:
        out.append(lst[key])
    except (IndexError, TypeError) as exc:
        out.append(exc_summary(exc))
    try:
        out.append(s[key])
    except (IndexError, TypeError) as exc:
        out.append(exc_summary(exc))
    return out


@compiled
def store_site(buf, i, v):
    acc = 0.5
    for _ in range(1):
        acc = acc * 1.5
    try:
        buf[i] = v
        return "stored"
    except IndexError as exc:
        return exc_summary(exc)
    except ValueError as exc:
        return exc_summary(exc)


@compiled
def after_handler(x):
    try:
        boom(x)
    except ValueError:
        inner = sys.exc_info()[0]
    return (inner is ValueError, exc_type())


d = {"k": 1}
lst = [10, 20]
s = "ab"
buf = bytearray(2)
for i in range(ROUNDS):
    LOG.append(call_site(i))
    LOG.append(binop_site(1, None))
    LOG.append(binop_site(1, 2))
    LOG.append(subscr_sites(d, lst, s, "k"))
    LOG.append(subscr_sites(d, lst, s, 5))
    LOG.append(subscr_sites(d, lst, s, -1))
    LOG.append(store_site(buf, 5, 1))
    LOG.append(store_site(buf, 0, 300))
    LOG.append(store_site(buf, 1, 7))
    LOG.append(after_handler(i))
LOG.append(compiled_state((call_site, binop_site, subscr_sites, store_site, after_handler)))
print(json.dumps(LOG, default=str))
print("handler shapes ok")
"""


INPLACE_OPS_PROBE = PREAMBLE + """
BASE = 0


@compiled
def inplace_add(value, other):
    try:
        value += other
    except TypeError as exc:
        return ("type", str(exc))
    return ("value", value)


@compiled
def inplace_divide(value):
    try:
        value /= 0.0
    except ZeroDivisionError as exc:
        return ("zero", str(exc))
    return ("value", value)


@compiled
def literal_float_divide():
    value = 1.0
    try:
        value /= 0.0
    except ZeroDivisionError as exc:
        return ("zero", str(exc))
    return ("value", value)


@compiled
def exact_float_divide(value, divisor):
    value = float(value)
    divisor = float(divisor)
    try:
        value /= divisor
    except ZeroDivisionError as exc:
        return ("zero", str(exc))
    return ("value", value)


@compiled
def exact_int_divide(value, divisor):
    value = int(value)
    divisor = int(divisor)
    try:
        value //= divisor
    except ZeroDivisionError as exc:
        return ("zero", str(exc))
    return ("value", value + 0.0)


class BadAdd:
    def __init__(self):
        self.calls = 0

    def __iadd__(self, other):
        self.calls += 1
        raise ValueError("custom inplace")


@compiled
def custom_inplace(value):
    try:
        value += 1
    except ValueError as exc:
        return ("custom", str(exc), value.calls)
    return ("unexpected",)


obj = BadAdd()
functions = (inplace_add, inplace_divide, literal_float_divide,
             exact_float_divide, exact_int_divide, custom_inplace)
for i in range(ROUNDS):
    if JIT:
        before = _cinderx._get_trigger_stats()["machine_code_entries"]
    LOG.append(inplace_add(1, None))
    LOG.append(inplace_add(1, 2))
    LOG.append(inplace_divide(1.0))
    LOG.append(literal_float_divide())
    LOG.append(exact_float_divide(1.0, 0.0))
    LOG.append(exact_float_divide(8.0, 2.0))
    LOG.append(exact_int_divide(1, 0))
    LOG.append(exact_int_divide(8, 2))
    LOG.append(custom_inplace(obj))
    assert obj.calls == i + 1  # The failed __iadd__ must not be replayed.
    if JIT:
        stats = _cinderx._get_trigger_stats()
        assert stats["machine_code_entries"] > before
        assert all(cinderjit.is_jit_compiled(fn) for fn in functions)
        if i == 15:
            # Each raising site has passed the eighth-exception grant.
            settled_deopts = stats["organic_deopt_hits"]
        elif i > 15:
            assert stats["organic_deopt_hits"] == settled_deopts, stats
LOG.append(compiled_state(functions))
print(json.dumps(LOG, default=str))
print("inplace ops ok")
"""


CONTROL_FLOW_PROBE = PREAMBLE + """
BASE = 0


def boom(tag):
    raise KeyError(tag)


@compiled
def chaining(tag):
    try:
        try:
            boom(tag)
        except KeyError as inner:
            raise ValueError("outer") from None
    except ValueError as exc:
        return (
            exc_summary(exc),
            exc_summary(exc.__context__) if exc.__context__ else None,
            exc.__suppress_context__,
        )


@compiled
def implicit_chain(tag):
    try:
        try:
            boom(tag)
        except KeyError:
            1 / 0
    except ZeroDivisionError as exc:
        return (exc_summary(exc), exc_summary(exc.__context__))


@compiled
def reraise(tag):
    try:
        boom(tag)
    except KeyError:
        raise


@compiled
def not_matching(tag):
    try:
        boom(tag)
    except ValueError:
        return "wrong handler"


@compiled
def loop_control(n):
    out = []
    for i in range(n):
        try:
            if i % 3 == 0:
                boom(i)
            out.append(i)
        except KeyError:
            if i == 3:
                continue
            if i == 6:
                break
            out.append(("caught", i))
    return out


@compiled
def handler_return(tag):
    try:
        boom(tag)
    except KeyError as exc:
        return ("returned", exc.args)
    return "fell through"


@compiled
def with_finally(tag, log):
    try:
        boom(tag)
    except KeyError:
        log.append("except")
    finally:
        log.append("finally")
    return list(log)


@compiled
def nested_exc_info(tag):
    try:
        boom(tag)
    except KeyError:
        a = sys.exc_info()[0].__name__
        try:
            1 / 0
        except ZeroDivisionError:
            b = sys.exc_info()[0].__name__
        c = sys.exc_info()[0].__name__
    return (a, b, c, exc_type())


for i in range(ROUNDS):
    LOG.append(chaining(i))
    LOG.append(implicit_chain(i))
    try:
        reraise(i)
    except KeyError as exc:
        LOG.append(("reraised", exc.args))
    try:
        not_matching(i)
    except KeyError as exc:
        LOG.append(("propagated", exc.args))
    LOG.append(loop_control(8))
    LOG.append(handler_return(i))
    LOG.append(with_finally(i, []))
    LOG.append(nested_exc_info(i))
# nested_exc_info raises inside its handler too, but into a nested try whose
# handler is compiled: that site takes an edge and the function stays.
LOG.append(compiled_state(
    (loop_control, handler_return, with_finally, nested_exc_info),
    (chaining, implicit_chain, reraise, not_matching),
))
print(json.dumps(LOG, default=str))
print("control flow ok")
"""


TRACEBACK_PROBE = PREAMBLE + """
BASE = sys._getframe().f_lineno


def boom():
    raise RuntimeError("deep")


@compiled
def no_handler_call(x):
    y = x + 1
    return boom()


@compiled
def no_handler_binop(a, b):
    return a + b


@compiled
def no_handler_subscr(d, k):
    acc = 0.5
    for _ in range(1):
        acc = acc * 1.5
    return d[k]


@compiled
def no_handler_store(buf, i):
    acc = 0.5
    for _ in range(1):
        acc = acc * 1.5
    buf[i] = 1
    return buf


@compiled
def middle(x):
    return no_handler_call(x)


for i in range(ROUNDS):
    for fn, args in (
        (middle, (i,)),
        (no_handler_binop, (1, None)),
        (no_handler_subscr, ({}, "k")),
        (no_handler_store, (bytearray(1), 5)),
    ):
        try:
            fn(*args)
        except Exception as exc:  # noqa: BLE001
            LOG.append((fn.__name__, exc_summary(exc), tb_lines(exc)))
# No handler in these frames: the raise keeps the deopt exit (the evaluator
# materializes the frame's locals before the traceback captures it), so the
# budget withdraws them all.
LOG.append(compiled_state((), (no_handler_call, no_handler_binop, no_handler_subscr, no_handler_store, middle)))
print(json.dumps(LOG, default=str))
print("traceback ok")
"""


CALL_VARIANTS_PROBE = PREAMBLE + """
BASE = 0


class Widget:
    def __init__(self, tag):
        raise KeyError(tag)

    def method(self, tag):
        raise ValueError(tag)


def kw_target(*, tag):
    raise TypeError(tag)


widget = Widget.__new__(Widget)


@compiled
def method_site(tag):
    try:
        return widget.method(tag)
    except ValueError as exc:
        return ("method", exc.args)


@compiled
def kwargs_site(tag):
    try:
        return kw_target(tag=tag)
    except TypeError as exc:
        return ("kw", exc.args)


@compiled
def ctor_site(tag):
    try:
        return Widget(tag)
    except KeyError as exc:
        return ("ctor", exc.args)


@compiled
def builtin_site(tag):
    try:
        return int(tag)
    except ValueError as exc:
        return ("builtin", str(exc))


@compiled
def propagating_site(tag):
    # The callee raises and this frame has no handler: the deopt exit stays.
    return widget.method(tag)


for i in range(ROUNDS):
    LOG.append(method_site(i))
    LOG.append(kwargs_site(i))
    LOG.append(ctor_site(i))
    LOG.append(builtin_site("x%d" % i))
    try:
        propagating_site(i)
    except ValueError as exc:
        LOG.append(("propagated", exc.args, tb_lines(exc)))
LOG.append(compiled_state((method_site, kwargs_site, ctor_site, builtin_site), (propagating_site,)))
print(json.dumps(LOG, default=str))
print("call variants ok")
"""


FRESH_FUNCTIONS_PROBE = PREAMBLE + """
BASE = 0


def outer(tag):
    # A fresh function object per outer call, attached to the code's artifact
    # by the observer on its first frame and run there for the rest of the
    # loop.  The raise is the operation's own (a raise statement in a callee
    # would be a function withdrawn by the budget).
    def inner(t):
        try:
            return t + None
        except TypeError as exc:
            return ("inner", str(exc))
    out = []
    for i in range(16):
        out.append(inner(tag + i))
    return inner, out


# The threshold compiles both; the eighth raise grants the edge and
# recompiles through the deopting activation's function object (the current
# inner), and the fresh function objects that follow attach to the new
# artifact.  24 x 16 raises pass the deopt budget many times over: without
# the edge the code would be withdrawn and the last inner not compiled.
for r in range(24):
    inner, out = outer(r)
    LOG.append(out)
LOG.append(compiled_state((outer, inner)))
print(json.dumps(LOG, default=str))
print("fresh functions ok")
"""


FRAME_LOCALS_PROBE = PREAMBLE + """
BASE = 0
import gc
import weakref


class Payload:
    pass


def boom(tag):
    raise ValueError(tag)


@compiled
def caught(token, tag):
    only_here = Payload()
    try:
        boom(tag)
    except ValueError as exc:
        caught_here = tag
        return exc
    return None


@compiled
def caught_loop(token, fail):
    # The body opens with `while True:`: the entry bytecode block holds
    # only RESUME and the loop's NOP, so the arguments are never the target
    # of a store and must be seeded into the frame at entry.
    while True:
        try:
            fail()
        except ValueError as exc:
            return exc


# The traceback created on entry into the compiled handler exposes this
# frame: its locals must be complete (the values at the raise, and the
# handler's own stores), and the frame keeps its locals alive while the
# traceback is held, as the evaluator's frame does.
tokens = []
for i in range(ROUNDS):
    token = Payload()
    exc = caught(token, i)
    frame = exc.__traceback__.tb_frame
    while frame is not None and frame.f_code is not caught.__code__:
        frame = frame.f_back
    names = sorted(k for k, v in frame.f_locals.items() if v is not None)
    only_ref = weakref.ref(frame.f_locals["only_here"])
    LOG.append((frame.f_code.co_name, names, frame.f_locals["token"] is token,
                frame.f_locals["tag"] == i, frame.f_locals["caught_here"] == i))
    gc.collect()
    LOG.append(("alive while traceback held", only_ref() is not None))
    del exc, frame
    gc.collect()
    LOG.append(("released with traceback", only_ref() is None))
def boom_loop():
    raise ValueError("loop")


for i in range(ROUNDS):
    token = Payload()
    exc = caught_loop(token, boom_loop)
    frame = exc.__traceback__.tb_frame
    while frame is not None and frame.f_code is not caught_loop.__code__:
        frame = frame.f_back
    LOG.append(("loop", frame.f_locals["token"] is token, frame.f_locals["fail"] is boom_loop,
                sorted(k for k, v in frame.f_locals.items() if v is not None)))
    del exc, frame
LOG.append(compiled_state((caught, caught_loop)))
print(json.dumps(LOG, default=str))
print("frame locals ok")
"""


TRACING_MIDCALL_PROBE = PREAMBLE + """
BASE = sys._getframe().f_lineno
TRACE_INSIDE = [False]


def tracer(frame, event, arg):
    return tracer


def boom_with_trace(tag):
    if TRACE_INSIDE[0]:
        sys.settrace(tracer)
    raise ValueError(tag)


class Adder:
    def __add__(self, other):
        if TRACE_INSIDE[0]:
            sys.settrace(tracer)
        raise TypeError("no add")


@compiled
def guarded_call(tag):
    try:
        return boom_with_trace(tag)
    except ValueError as exc:
        return exc


@compiled
def guarded_binop(a, b):
    try:
        # The operation spans two lines: the BINARY_OP's line differs from
        # the preceding code unit's, which is the default line-poll
        # placement's condition.
        return (a
                + b)
    except TypeError as exc:
        return exc


def positions(exc, name):
    tb = exc.__traceback__
    while tb is not None and tb.tb_frame.f_code.co_name != name:
        tb = tb.tb_next
    return (name, tb.tb_lineno - BASE, tb.tb_lasti) if tb is not None else (name, None, None)


# Tracing off: both sites pass the grant threshold and are recompiled with
# their exception edges (past the budget: the compiled state below proves
# the edges are taken).
for i in range(ROUNDS):
    LOG.append(positions(guarded_call(i), "guarded_call"))
    LOG.append(positions(guarded_binop(Adder(), i), "guarded_binop"))
LOG.append(compiled_state((guarded_call, guarded_binop)))
# Now the raising callee / __add__ switches tracing on while the caller is
# already in machine code at a granted site: the exception position it
# reports must be the interpreter's.  Tracing is switched off right after
# each probe so the next one enters machine code again.
TRACE_INSIDE[0] = True
for i in range(3):
    assert sys.gettrace() is None
    LOG.append(positions(guarded_call(i), "guarded_call"))
    sys.settrace(None)
    assert sys.gettrace() is None
    LOG.append(positions(guarded_binop(Adder(), i), "guarded_binop"))
    sys.settrace(None)
# (The compiled state recorded before the tracing rounds is the one that
# proves the edges; this final one is the harness's required last entry.)
LOG.append(compiled_state((guarded_call, guarded_binop)))
print(json.dumps(LOG, default=str))
print("tracing midcall ok")
"""


SETTRACE_PROBE = PREAMBLE + """
BASE = 0


def boom(x):
    raise ValueError(x)


@compiled
def guarded(x):
    try:
        return boom(x)
    except ValueError as exc:
        return ("caught", exc.args)


for i in range(ROUNDS):
    LOG.append(guarded(i))

events = []


def tracer(frame, event, arg):
    if event == "exception" and frame.f_code.co_name in ("guarded", "boom"):
        events.append((frame.f_code.co_name, arg[0].__name__))
    return tracer


sys.settrace(tracer)
try:
    for i in range(3):
        LOG.append(guarded(i))
finally:
    sys.settrace(None)
LOG.append(sorted(set(events)))
for i in range(3):
    LOG.append(guarded(i))
LOG.append(compiled_state((guarded,)))
print(json.dumps(LOG, default=str))
print("settrace ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 compiled exception handlers"
)
class ExceptionHandlerSemantics311Test(unittest.TestCase):
    def _run_once(self, probe, jit, threshold="1000000", **extra):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        if jit:
            env.update(
                CINDERX_JIT_MODE="execute",
                PYTHONJITAUTO=threshold,
                PYTHONJITLIGHTWEIGHTFRAME="0",
                **extra,
            )
        else:
            env.update(CINDERX_JIT_MODE="off")
        env.update(
            CINDERX_PLUGIN_ENABLE="0",
            CINDERX_TEST_REQUIRE_JIT="1" if jit else "0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(probe)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        return proc.stdout

    def _run(self, probe, marker, threshold="1000000", **extra):
        jit_out = self._run_once(probe, jit=True, threshold=threshold, **extra)
        interp_out = self._run_once(probe, jit=False)
        self.assertIn(marker, jit_out)
        self.assertIn(marker, interp_out)
        # The transcript (everything but the compiled-state line) must match
        # the interpreter's; the JIT run's last entry says the functions
        # stayed compiled across the grants.
        import json

        jit_log = json.loads(jit_out.splitlines()[0])
        interp_log = json.loads(interp_out.splitlines()[0])
        self.assertEqual(jit_log[:-1], interp_log[:-1])
        self.assertIs(jit_log[-1], True)

    def test_inplace_operations_take_the_exception_edge(self):
        self._run(INPLACE_OPS_PROBE, "inplace ops ok")

    def _unavailable_runtime_probes(self):
        import_failure = """
import builtins
original_import = builtins.__import__
def unavailable(name, *args, **kwargs):
    if name == "_cinderx":
        raise ImportError("injected native import failure")
    return original_import(name, *args, **kwargs)
builtins.__import__ = unavailable
"""
        init_failure = """
import sys
import types
fake = types.ModuleType("cinderx")
def broken_init():
    raise RuntimeError("injected initialization failure")
fake.init = broken_init
sys.modules["cinderx"] = fake
"""
        disabled = """
import sys
import types
cx = types.ModuleType("cinderx")
cx.init = lambda: None
cx.is_initialized = lambda: True
native = types.ModuleType("_cinderx")
native.install_frame_evaluator = lambda: None
native.is_frame_evaluator_installed = lambda: True
jit = types.ModuleType("cinderjit")
jit.is_enabled = lambda: False
sys.modules.update(cinderx=cx, _cinderx=native, cinderjit=jit)
"""
        return (
            ("import", import_failure, "injected native import failure"),
            ("init", init_failure, "injected initialization failure"),
            ("disabled", disabled, "JIT must be enabled"),
        )

    def test_jit_probe_rejects_unavailable_runtime(self):
        for name, setup, message in self._unavailable_runtime_probes():
            with self.subTest(name=name):
                with self.assertRaisesRegex(AssertionError, message):
                    self._run_once(setup + PREAMBLE, jit=True)

    def test_interpreter_probe_does_not_require_jit(self):
        for name, setup, _ in self._unavailable_runtime_probes():
            with self.subTest(name=name):
                output = self._run_once(
                    setup + PREAMBLE + "\nprint('interpreter ok')", jit=False
                )
                self.assertIn("interpreter ok", output)

    def test_handler_shapes_at_call_binop_subscript_and_store_sites(self):
        self._run(HANDLER_SHAPES_PROBE, "handler shapes ok")

    def test_handler_control_flow_matches_the_interpreter(self):
        self._run(CONTROL_FLOW_PROBE, "control flow ok")

    def test_handler_free_frames_keep_the_deopt_exit_and_traceback(self):
        self._run(TRACEBACK_PROBE, "traceback ok")

    def test_settrace_falls_back_to_the_evaluator(self):
        self._run(SETTRACE_PROBE, "settrace ok")

    def test_handler_entry_exposes_complete_frame_locals(self):
        self._run(FRAME_LOCALS_PROBE, "frame locals ok")

    def test_tracing_switched_on_inside_the_raising_callee(self):
        # Both poll placements: the per-bytecode polls and the default
        # line polls must not treat the exception stub as a resume point.
        self._run(TRACING_MIDCALL_PROBE, "tracing midcall ok")
        self._run(
            TRACING_MIDCALL_PROBE,
            "tracing midcall ok",
            PYTHONJITSUPPORTINSTRUMENTATION="1",
        )

    def test_call_variants_take_the_edge(self):
        self._run(CALL_VARIANTS_PROBE, "call variants ok")

    def test_fresh_function_objects_re_earn_the_edge(self):
        # The production threshold, with every fresh function object
        # attached (an unlimited attach budget): the grant recompiles through
        # the deopting activation's own function object.
        self._run(
            FRESH_FUNCTIONS_PROBE,
            "fresh functions ok",
            threshold="4",
            PYTHONJITFRESHATTACHBUDGET="1000000",
        )


if __name__ == "__main__":
    unittest.main()
