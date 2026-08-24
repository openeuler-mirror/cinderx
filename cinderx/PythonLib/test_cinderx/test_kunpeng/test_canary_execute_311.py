# Copyright (c) Meta Platforms, Inc. and affiliates.
"""MR-04 canary execution gate (CPython 3.11).

The default build keeps machine code structurally unreachable; the canary
mode flips exactly that, in a child process, and this gate holds both
sides: the child genuinely executes machine code with a closed ledger and
the attribute-cache default stays off, while the parent (default mode)
counters remain zero.
"""

import json
import os
import subprocess
import sys
import textwrap
import unittest

def _capability_clean_env():
    """A child environment with every JIT-capability variable removed.

    Dual-arm oracles must fail closed: an interp/stock arm that inherits
    CINDERX_JIT_MODE=canary from the parent would silently compare the jit
    arm against itself.  Mirrors the prefix scrub the jit311 runners apply
    to their children.
    """
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
    }


CHILD = textwrap.dedent(
    """
    import json
    import _cinderx, cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    assert cinderjit.is_attr_caches_enabled() is True, (
        "3.11 attribute caches default on since the MR-09 acceptance")

    def hot(a, b, one):
        total = a - a
        i = total
        while i < b:
            total = total + a
            i = i + one
        return total

    expected = [hot(i, 7, 1) for i in range(32)]
    assert cinderjit.force_compile(hot) is True
    assert cinderjit.is_jit_compiled(hot)
    for i in range(32):
        assert hot(i, 7, 1) == expected[i], i

    stats = _cinderx._get_trigger_stats()
    print(json.dumps(stats))
    """
)


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the canary execute surface targets the vendored CPython 3.11.6",
)
class CanaryExecute311Test(unittest.TestCase):
    def test_canary_child_executes_and_default_stays_zero(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        proc = subprocess.run(
            [sys.executable, "-c", CHILD],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        stats = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertGreaterEqual(stats["compiled_function_creations"], 1)
        self.assertGreaterEqual(stats["executable_alloc_calls"], 1)
        self.assertGreater(stats["executable_alloc_bytes"], 0)
        self.assertGreaterEqual(stats["machine_code_entries"], 32)
        self.assertEqual(stats["shadow_compile_success"], 0)

    def test_canary_rejects_functions_outside_the_execute_surface(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def uses_subscr(a):
                return a[0]

            try:
                cinderjit.force_compile(uses_subscr)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit(
                    "execute surface failed to refuse BINARY_SUBSCR")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_canary_refuses_importlib_bootstrap(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1"
        probe = textwrap.dedent(
            """
            import importlib
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            bootstrap = sys.modules["_frozen_importlib"]
            try:
                cinderjit.force_compile(bootstrap._find_and_load)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("importlib bootstrap was compiled")

            importlib.import_module("test.test_grammar")
            frozen = (
                "_frozen_importlib",
                "_frozen_importlib_external",
                "importlib._bootstrap",
                "importlib._bootstrap_external",
            )
            for fn in cinderjit.get_compiled_functions():
                assert fn.__module__ not in frozen, fn
                assert "importlib._bootstrap" not in fn.__code__.co_filename, fn
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_canary_load_method_miss_calls_type_attribute(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def uniq(items):
                return list(dict.fromkeys(items))

            expected = uniq(["a", "a", "b"])
            assert cinderjit.force_compile(uniq) is True
            assert cinderjit.is_jit_compiled(uniq)
            assert uniq(["a", "a", "b"]) == expected == ["a", "b"]
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_canary_calling_none_raises_typeerror(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def call_local(x):
                return x()

            def call_const():
                return None()

            class Box:
                x = None

            def call_attr(obj):
                return obj.x()

            def star_kw(fn, kw):
                return fn(**kw)

            def star_args(fn, args):
                return fn(*args)

            for func in (
                call_local, call_const, call_attr, star_kw, star_args,
            ):
                assert cinderjit.force_compile(func) is True, func.__name__

            def expect_typeerror(fn, *args):
                try:
                    fn(*args)
                except TypeError as exc:
                    return type(exc), str(exc)
                raise SystemExit("expected TypeError from %s" % fn.__name__)

            for fn, args in (
                (call_local, (None,)),
                (call_const, ()),
                (call_attr, (Box(),)),
            ):
                exc_t, msg = expect_typeerror(fn, *args)
                assert exc_t is TypeError, (fn.__name__, exc_t, msg)
                assert "NoneType" in msg and "not callable" in msg, (
                    fn.__name__, msg)

            exc_t, msg = expect_typeerror(star_kw, lambda **k: k, 1)
            assert exc_t is TypeError, (exc_t, msg)
            assert "mapping" in msg, msg

            exc_t, msg = expect_typeerror(star_args, lambda *a: a, 1)
            assert exc_t is TypeError, (exc_t, msg)
            assert "iterable" in msg, msg

            print("calling None and CallEx TypeErrors held")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("calling None and CallEx TypeErrors held", proc.stdout)

    def test_canary_executes_call_family_with_local_callees(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def positional(fn, x):
                return fn(x)

            class Box:
                def add(self, value):
                    return value + 1

            def method_call(obj, value):
                return obj.add(value)

            def kwargs_call(fn, a):
                return fn(a, k=1)

            def star_call(fn, args):
                return fn(*args)

            def dstar_lit(fn):
                return fn(**{"k": 1})

            def star_list(fn):
                return fn(*[2, 3])

            for func in (
                positional, method_call, kwargs_call, star_call,
                dstar_lit, star_list,
            ):
                assert cinderjit.force_compile(func) is True, func.__name__
                assert cinderjit.is_jit_compiled(func), func.__name__

            assert positional(lambda x: x + 1, 3) == 4
            assert method_call(Box(), 3) == 4
            assert kwargs_call(lambda a, k=0: a + k, 3) == 4
            assert star_call(lambda a, b: a + b, (2, 3)) == 5
            assert dstar_lit(lambda k=0: k + 3) == 4
            assert star_list(lambda a, b: a + b) == 5

            stats = _cinderx._get_trigger_stats()
            print(json.dumps(stats))
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        stats = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertGreaterEqual(stats["compiled_function_creations"], 6)
        self.assertGreaterEqual(stats["machine_code_entries"], 6)

    def test_canary_warm_call_specialization_stays_generic(self):
        # Warm compilation must see the interpreter's CALL_PY_* form and
        # still compile the unspecialized CALL: the execute surface has
        # no speculative callee guard, so a later different Python
        # function at the same site has to keep working.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import dis
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            def add1(x):
                return x + 1

            def add10(x):
                return x + 10

            def caller(fn, x):
                return fn(x)

            for _ in range(200):
                caller(add1, 3)
            plain = [i.opname for i in dis.get_instructions(caller)]
            adaptive = [
                i.opname for i in dis.get_instructions(caller, adaptive=True)
            ]
            specialized = [b for a, b in zip(plain, adaptive) if a != b]
            assert any(name.startswith("CALL_") for name in specialized), (
                specialized)

            assert jit.force_compile_warm(caller) is True
            assert cinderjit.is_jit_compiled(caller)
            assert caller(add1, 3) == 4
            assert caller(add10, 3) == 13
            print("warm call specialization stayed generic")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("warm call specialization stayed generic", proc.stdout)

    def test_canary_load_method_receiver_switch(self):
        # Plan-required shape: quicken and compile one LOAD_METHOD/self
        # form, then run the other (method hit vs attribute miss) at the
        # same site.  Natural deopt is MR-07; canary must keep answering.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            class Box:
                def add(self, value):
                    return value + 1

            class AttrBox:
                def __init__(self):
                    self.add = lambda value: value + 10

            def caller(obj, value):
                return obj.add(value)

            for _ in range(200):
                caller(Box(), 3)
            assert jit.force_compile_warm(caller) is True
            assert cinderjit.is_jit_compiled(caller)
            assert caller(Box(), 3) == 4
            assert caller(AttrBox(), 3) == 13

            def miss_first(obj, value):
                return obj.add(value)

            for _ in range(200):
                miss_first(AttrBox(), 3)
            assert jit.force_compile_warm(miss_first) is True
            assert miss_first(AttrBox(), 3) == 13
            assert miss_first(Box(), 3) == 4
            print("load_method receiver switch held")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("load_method receiver switch held", proc.stdout)

    def test_canary_classmethod_and_staticmethod_execute(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Callee:
                @classmethod
                def cm(cls, x):
                    return ("cm", cls.__name__, x)

                @staticmethod
                def sm(x, y):
                    return ("sm", x, y)

            def call_cm(x):
                return Callee.cm(x)

            def call_sm(x, y):
                return Callee.sm(x, y)

            def call_bound_cm(x):
                return Callee().cm(x)

            for func in (call_cm, call_sm, call_bound_cm):
                assert cinderjit.force_compile(func) is True, func.__name__
                assert cinderjit.is_jit_compiled(func), func.__name__

            assert call_cm(3) == ("cm", "Callee", 3)
            assert call_sm(2, 5) == ("sm", 2, 5)
            assert call_bound_cm(4) == ("cm", "Callee", 4)
            print("classmethod and staticmethod executed")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("classmethod and staticmethod executed", proc.stdout)

    def test_canary_call_function_churn(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def helper(x):
                return x + 1

            compiled = 0
            generations = 80
            for gen in range(generations):
                ns = {"helper": helper}
                exec(
                    "def churn_%d(x):\\n    return helper(x)\\n" % gen,
                    ns,
                )
                fn = ns.pop("churn_%d" % gen)
                assert cinderjit.force_compile(fn) is True, gen
                compiled += 1
                before = _cinderx._get_trigger_stats()["machine_code_entries"]
                assert fn(3) == 4, gen
                delta = (
                    _cinderx._get_trigger_stats()["machine_code_entries"]
                    - before
                )
                assert delta == 1, (gen, delta)
                del fn
                del ns
                gc.collect()
                for live in cinderjit.get_compiled_functions():
                    assert live.__qualname__, live

            assert compiled == generations
            leftover = [
                f.__qualname__
                for f in cinderjit.get_compiled_functions()
                if f.__qualname__.startswith("churn_")
            ]
            assert not leftover, leftover
            print("call function churn held")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("call function churn held", proc.stdout)

    def test_canary_call_thresholds_install(self):
        probe = textwrap.dedent(
            """
            import os
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            threshold = int(os.environ["PYTHONJITAUTO"])

            def add1(x):
                return x + 1

            def caller(x):
                return add1(x)

            for i in range(threshold + 4):
                assert caller(i) == i + 1
            assert cinderjit.is_jit_compiled(caller), (
                "threshold %s did not install" % threshold)
            assert caller(7) == 8
            print("threshold %s installed" % threshold)
            """
        )
        for threshold in (1, 4, 30):
            env = dict(os.environ)
            env["CINDERX_JIT_MODE"] = "canary"
            env["PYTHONJITAUTO"] = str(threshold)
            proc = subprocess.run(
                [sys.executable, "-c", probe],
                capture_output=True,
                text=True,
                env=env,
                timeout=120,
            )
            self.assertEqual(
                proc.returncode, 0, (threshold, proc.stderr[-800:])
            )
            self.assertIn("threshold %s installed" % threshold, proc.stdout)

    def test_canary_recursive_call_raises_recursion_error(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def rec(f, n):
                if n:
                    return f(f, n - 1)
                return 0

            assert cinderjit.force_compile(rec) is True
            assert rec(rec, 8) == 0

            old = sys.getrecursionlimit()
            sys.setrecursionlimit(20)
            try:
                rec(rec, 200)
                raise SystemExit("expected RecursionError")
            except RecursionError:
                pass
            finally:
                sys.setrecursionlimit(old)

            assert rec(rec, 4) == 0
            assert cinderjit.is_jit_compiled(rec), (
                "RecursionError withdrew the artifact; ROI deopt budget "
                "should not trip on a single bounded unwind")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_canary_deep_recursion_hits_c_stack_soft_limit(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def rec(f, n):
                if n:
                    return f(f, n - 1)
                return 0

            assert cinderjit.force_compile(rec) is True
            old = sys.getrecursionlimit()
            sys.setrecursionlimit(10 ** 6)
            try:
                rec(rec, 10 ** 6)
                raise SystemExit("expected RecursionError from C stack")
            except RecursionError:
                pass
            finally:
                sys.setrecursionlimit(old)
            assert rec(rec, 3) == 0
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_escaped_frame_stays_accessible_after_return(self):
        # MR-04 acceptance: a frame that escaped via an exception traceback
        # must stay safely accessible after the machine-code call returned
        # and after collection (the materialized _PyInterpreterFrame's
        # ownership handed over correctly).
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def raiser(x):
                none = None
                return x + none

            assert cinderjit.force_compile(raiser) is True
            frames = []
            for _ in range(3):
                try:
                    raiser(7)
                except TypeError as exc:
                    tb = exc.__traceback__
                    while tb.tb_next is not None:
                        tb = tb.tb_next
                    frames.append(tb.tb_frame)
            gc.collect()
            for frame in frames:
                assert frame.f_code.co_name == "raiser"
                assert frame.f_locals["x"] == 7
                assert frame.f_lineno > 0
            del frames
            gc.collect()
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_execute_surface_compiles_argument_shapes(self):
        # MR-06: defaults, keyword-only parameters and the variadic
        # collectors are bound by the generated vectorcall prologue.  A
        # body made of whitelisted opcodes is enough to execute them.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def kwonly(*, x):
                return x
            def varargs(*args):
                return args
            def varkw(**kwargs):
                return kwargs
            def mixed(a, *, b=2):
                return a + b
            def defaulted(a, b=3):
                return a + b

            for fn in (kwonly, varargs, varkw, mixed, defaulted):
                assert cinderjit.force_compile(fn) is True, fn.__name__
                assert cinderjit.is_jit_compiled(fn), fn.__name__

            before = entries()
            assert kwonly(x=7) == 7
            assert varargs(1, 2) == (1, 2)
            assert varkw(k=1) == {"k": 1}
            assert mixed(3) == 5
            assert mixed(3, b=4) == 7
            assert defaulted(1) == 4
            assert defaulted(1, 10) == 11
            assert defaulted(a=1, b=2) == 3
            assert entries() - before == 8, entries() - before
            print("argument shapes compiled")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("argument shapes compiled", proc.stdout)

    def test_canary_binding_typeerrors_match_interpreter(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def compiled(a, b):
                return a
            def interpreted(a, b):
                return a

            assert cinderjit.force_compile(compiled) is True

            def catch(fn, *args, **kwargs):
                try:
                    fn(*args, **kwargs)
                except TypeError as exc:
                    msg = str(exc)
                    if "()" in msg:
                        msg = msg.split("()", 1)[1]
                    return type(exc), msg
                raise SystemExit("expected TypeError")

            for args, kwargs in (
                ((1,), {}),
                ((1, 2, 3), {}),
                ((1,), {"z": 2}),
                ((), {"a": 1}),
            ):
                got = catch(compiled, *args, **kwargs)
                want = catch(interpreted, *args, **kwargs)
                assert got == want, (args, kwargs, got, want)
            print("binding TypeErrors match")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("binding TypeErrors match", proc.stdout)

    def test_canary_default_survives_defaults_rebind(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Boom:
                pass

            def rebind():
                victim.__defaults__ = ()

            def victim(x=Boom()):
                rebind()
                return x

            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(rebind) is False
            got = victim()
            assert type(got).__name__ == "Boom", type(got)
            print("default survived rebind")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("default survived rebind", proc.stdout)

    def test_canary_kwonly_default_survives_kwdefaults_clear(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Boom:
                pass

            def clear_kw():
                victim.__kwdefaults__.clear()

            def victim(*, x=Boom()):
                clear_kw()
                return x

            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(clear_kw) is False
            got = victim()
            assert type(got).__name__ == "Boom", type(got)
            print("kwonly default survived clear")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("kwonly default survived clear", proc.stdout)

    def test_canary_keyword_binding_snapshot_and_exceptions(self):
        # Binding can run arbitrary Python.  The invocation that already
        # entered GuardedEntry must keep its pinned artifact: tracing or a
        # __code__ swap in K.__eq__ must not retarget this call, and a
        # pending exception from __eq__ must not be replayed.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def traced(*, x):
                return x
            assert cinderjit.force_compile(traced) is True

            class TraceKey(str):
                __hash__ = str.__hash__
                def __eq__(self, other):
                    sys.settrace(lambda *a: None)
                    return str.__eq__(self, other)

            try:
                assert traced(**{TraceKey("x"): 7}) == 7
            finally:
                sys.settrace(None)

            calls = 0
            class BoomKey(str):
                __hash__ = str.__hash__
                def __eq__(self, other):
                    global calls
                    calls += 1
                    raise ValueError("boom")

            def boom(*, x):
                return x
            assert cinderjit.force_compile(boom) is True
            try:
                boom(**{BoomKey("x"): 1})
            except ValueError as exc:
                assert str(exc) == "boom"
            else:
                raise SystemExit("expected ValueError")
            assert calls == 1, calls

            calls = 0
            class FalseKey(str):
                __hash__ = str.__hash__
                def __eq__(self, other):
                    global calls
                    calls += 1
                    return False

            def missing(*, x):
                return x
            assert cinderjit.force_compile(missing) is True
            try:
                missing(**{FalseKey("wrong"): 1})
            except TypeError:
                pass
            else:
                raise SystemExit("expected TypeError")
            assert calls == 1, calls

            def old(*, x):
                return x + 1
            def new(*, x):
                return x + 100
            assert cinderjit.force_compile(old) is True

            class SwapKey(str):
                __hash__ = str.__hash__
                def __eq__(self, other):
                    old.__code__ = new.__code__
                    return str.__eq__(self, other)

            assert old(**{SwapKey("x"): 1}) == 2

            def variadic(*args, **kwargs):
                return (args, kwargs)
            assert cinderjit.force_compile(variadic) is True
            before = entries()
            assert variadic(args=1) == ((), {"args": 1})
            assert entries() - before == 1, entries() - before
            print("keyword binding snapshot ok")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("keyword binding snapshot ok", proc.stdout)

    def test_canary_load_global_builtin_without_guard(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def uses_abs(x):
                return abs(x)

            assert cinderjit.force_compile(uses_abs) is True
            assert cinderjit.is_jit_compiled(uses_abs)
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert uses_abs(-4) == 4
            assert uses_abs(5) == 5
            delta = _cinderx._get_trigger_stats()["machine_code_entries"] - before
            assert delta == 2, delta
            print("load_global builtin compiled")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("load_global builtin compiled", proc.stdout)

    def test_canary_tracing_falls_back_to_interpreter(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def target(a):
                return a + 1

            assert cinderjit.force_compile(target) is True
            assert cinderjit.is_jit_compiled(target)

            events = []
            def tracer(frame, event, arg):
                if frame.f_code.co_name == "target":
                    events.append(event)
                return tracer

            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            sys.settrace(tracer)
            try:
                assert target(3) == 4
            finally:
                sys.settrace(None)
            assert "call" in events, events
            assert "return" in events, events
            delta = _cinderx._get_trigger_stats()["machine_code_entries"] - before
            assert delta == 0, delta
            assert cinderjit.is_jit_compiled(target)
            assert target(3) == 4
            after = _cinderx._get_trigger_stats()["machine_code_entries"] - before
            assert after == 1, after
            print("tracing fell back")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("tracing fell back", proc.stdout)

    def test_compiled_artifact_is_never_shared_between_functions(self):
        # CPython 3.11 has no function-destroy notification, so a second
        # owner of one compiled artifact would keep it alive past the first
        # owner's death and leave that dead function as a borrowed pointer
        # in the registry -- reading it then segfaults.  Until the MR-05
        # lifecycle lands, a function whose (code, globals, builtins) is
        # already compiled for someone else is refused.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import types
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def twin(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            other = types.FunctionType(
                twin.__code__, twin.__globals__, "other")
            assert cinderjit.force_compile(twin) is True
            try:
                cinderjit.force_compile(other)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("a second owner was allowed to share")
            assert not cinderjit.is_jit_compiled(other)
            assert other(3, 5, 1) == twin(3, 5, 1)

            # The twin dies; the registry must stay readable and the
            # surviving function must keep running its machine code.
            del other
            gc.collect()
            for func in cinderjit.get_compiled_functions():
                assert func.__qualname__, func
            assert twin(3, 5, 1) == 15
            print("artifact ownership stayed exclusive")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("artifact ownership stayed exclusive", proc.stdout)

    def test_specialized_bytecode_deopts_organically_on_type_change(self):
        # Warm compilation plus Simplify install compact-long / float
        # guards.  Passing a different type must take the real deopt
        # restore and still answer correctly, remaining compiled.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import dis
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def arith(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                arith(3, 5, 1)
            specialized = [
                instr.opname
                for instr in dis.get_instructions(arith, adaptive=True)
                if instr.opname.endswith("_INT")
            ]
            assert specialized, "the interpreter never specialized the target"

            assert cinderjit.force_compile(arith) is True
            before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert arith(3, 5, 1) == 15
            assert arith(3.0, 5.0, 1.0) == 15.0
            assert cinderjit.is_jit_compiled(arith)
            after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert after > before, (before, after)
            print("organic deopt on type change")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("organic deopt on type change", proc.stdout)

    def test_canary_control_plane_is_restricted(self):
        # The full cinderjit method table is a control surface for
        # capabilities this port does not have yet: the batch and lazy
        # paths install machine code without passing the execute surface,
        # and the specialization and guard setters can re-open exactly the
        # speculation MR-04 excludes.  force_uncompile left this list when
        # MR-05 published it.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            # Capabilities later milestones own.
            withheld = [
                "lazy_compile", "precompile_all", "compile_all",
                "enable_specialized_opcodes",
                "disable_specialized_opcodes",
                "enable_emit_type_annotation_guards",
                "clear_runtime_stats",
            ]
            present = [name for name in withheld if hasattr(cinderjit, name)]
            assert not present, present

            # What this milestone's evidence needs, plus every API that
            # stops the JIT rather than extending it: withholding those
            # left the wrapper with stubs that silently did nothing.
            needed = [
                "force_compile", "is_jit_compiled", "is_attr_caches_enabled",
                "get_compiled_functions", "get_and_clear_runtime_stats",
                "is_enabled", "jit_suppress", "jit_unsuppress",
                "disable", "enable", "force_uncompile",
                "deopt_sites", "force_deopt",
                "_get_resident_compiled_functions",
            ]
            missing = [name for name in needed if not hasattr(cinderjit, name)]
            assert not missing, missing
            print("control plane restricted")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("control plane restricted", proc.stdout)

    def test_entry_rechecks_code_identity_and_runs_live_binding(self):
        # 3.11 has no function watchers, so a __code__ swap after
        # compilation must not keep running the old artifact.  Keyword
        # calls and defaults that appear after compilation are rebound by
        # the generated prologue and must still enter machine code.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def target(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(target) is True
            before = entries()
            assert target(3, 5, 1) == 15
            assert entries() - before == 1, "positional call must run compiled"

            before = entries()
            assert target(a=3, b=5, one=1) == 15
            assert entries() - before == 1, "keyword call must run compiled"

            before = entries()
            target.__defaults__ = (1,)
            assert target(3, 5) == 15
            assert entries() - before == 1, "live defaults must run compiled"
            target.__defaults__ = None

            def replacement(a, b, one):
                return "replaced"

            target.__code__ = replacement.__code__
            before = entries()
            assert target(3, 5, 1) == "replaced", "stale machine code ran"
            assert entries() - before == 0
            print("entry rechecked identity and ran live binding")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("entry rechecked identity and ran live binding", proc.stdout)

    def test_deopt_sites_are_stable_and_force_hits_the_real_restore(self):
        # Site ids are code + offset + kind + empty inline path.  Arming
        # one must enter the same restore as an organic guard miss, leave
        # the function compiled, and bump only the forced counter.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import dis
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def loop(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                loop(3, 5, 1)
            assert cinderjit.force_compile(loop) is True
            sites = cinderjit.deopt_sites(loop)
            assert sites, sites
            instruction_offsets = {
                instr.offset for instr in dis.get_instructions(loop)
            }
            assert all(s["bc_offset"] in instruction_offsets for s in sites), (
                sites, sorted(instruction_offsets)
            )
            ids = [s["id"] for s in sites]
            assert len(ids) == len(set(ids)), ids
            kinds = {s["kind"] for s in sites}
            assert all(s["inline_path"] == "" for s in sites)
            assert all(isinstance(s["forceable"], bool) for s in sites)
            assert "GuardFailure" in kinds, kinds

            cinderjit.force_uncompile(loop)
            assert cinderjit.force_compile(loop) is True
            again = [s["id"] for s in cinderjit.deopt_sites(loop)]
            assert again == ids, (ids, again)

            site = next(s for s in cinderjit.deopt_sites(loop)
                        if s["forceable"])
            before = _cinderx._get_trigger_stats()
            assert cinderjit.force_deopt(loop, site["id"], n=1) is True
            assert loop(3, 5, 1) == 15
            after = _cinderx._get_trigger_stats()
            assert after["forced_deopt_hits"] == before["forced_deopt_hits"] + 1
            assert after["organic_deopt_hits"] == before["organic_deopt_hits"]
            assert cinderjit.is_jit_compiled(loop)
            print("forced deopt used the real restore")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("forced deopt used the real restore", proc.stdout)

    def test_force_deopt_rejects_unhandled_exception_sites(self):
        # CheckExc sites exist on the execute surface and used to abort
        # with "unhandled exception without error set" when armed.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def loop(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                loop(3, 5, 1)
            assert cinderjit.force_compile(loop) is True
            sites = cinderjit.deopt_sites(loop)
            exc = [s for s in sites if s["kind"] == "UnhandledException"]
            assert exc, sites
            assert all(not s["forceable"] for s in exc), exc
            raised = False
            try:
                cinderjit.force_deopt(loop, exc[0]["id"], n=1)
            except ValueError as err:
                raised = True
                assert "forceable" in str(err), err
            assert raised
            assert loop(3, 5, 1) == 15
            print("unhandled exception site not forceable")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("unhandled exception site not forceable", proc.stdout)

    def test_repeated_forced_and_organic_deopts_preserve_ownership(self):
        # Alternate forced and organic restores while a user object is live
        # across every exit.  Each call must release exactly its own argument
        # reference after resuming; a duplicate steal or a missing decref is
        # exposed by the weakrefs and the exact destruction count.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "0"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            created = 0
            destroyed = 0

            class Probe:
                __slots__ = ("__weakref__",)

                def __init__(self):
                    global created
                    created += 1

                def __del__(self):
                    global destroyed
                    destroyed += 1

            def hot(obj, a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                if obj is None:
                    return -1
                return total

            warm = Probe()
            for _ in range(200):
                assert hot(warm, 3, 5, 1) == 15
            assert cinderjit.force_compile(hot) is True
            del warm
            gc.collect()
            created = destroyed = 0

            forceable = [s for s in cinderjit.deopt_sites(hot)
                         if s["forceable"]]
            assert forceable, cinderjit.deopt_sites(hot)
            site = max(forceable, key=lambda s: s["bc_offset"])
            refs = []
            before = _cinderx._get_trigger_stats()
            calls = 2000
            for index in range(calls):
                obj = Probe()
                refs.append(weakref.ref(obj))
                if index % 2 == 0:
                    assert cinderjit.force_deopt(
                        hot, site["id"], n=1) is True
                    assert hot(obj, 3, 5, 1) == 15
                else:
                    assert hot(obj, 3.0, 5.0, 1.0) == 15.0
                del obj
                if index % 100 == 99:
                    gc.collect()

            gc.collect()
            after = _cinderx._get_trigger_stats()
            assert created == calls, (created, calls)
            assert destroyed == calls, (destroyed, calls)
            assert all(ref() is None for ref in refs)
            assert (after["forced_deopt_hits"] ==
                    before["forced_deopt_hits"] + calls // 2), (before, after)
            assert (after["organic_deopt_hits"] >=
                    before["organic_deopt_hits"] + calls // 2), (before, after)
            assert cinderjit.is_jit_compiled(hot)
            print("repeated forced and organic ownership held")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "repeated forced and organic ownership held", proc.stdout
        )

    def test_deopt_sites_pins_artifact_across_reentrant_uncompile(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def loop(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                loop(3, 5, 1)
            assert cinderjit.force_compile(loop) is True

            finalized = []

            class Killer:
                def __init__(self):
                    self.cycle = self

                def __del__(self):
                    finalized.append(True)
                    cinderjit.force_uncompile(loop)

            killer = Killer()
            del killer
            old_threshold = gc.get_threshold()
            gc.set_threshold(1, 1, 1)
            try:
                sites = cinderjit.deopt_sites(loop)
            finally:
                gc.set_threshold(*old_threshold)
            assert finalized, "deopt_sites allocation did not run finalizer"
            assert sites, sites
            assert cinderjit.is_jit_compiled(loop) is False
            print("deopt_sites pinned artifact across uncompile")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "deopt_sites pinned artifact across uncompile", proc.stdout
        )

    def test_load_global_force_deopt_does_not_double_push_null(self):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def _g():
                return 7

            G = _g

            def read():
                return G()

            for _ in range(64):
                assert read() == 7
            assert cinderjit.force_compile(read) is True
            sites = [s for s in cinderjit.deopt_sites(read)
                     if s["kind"] == "GuardFailure"]
            assert sites, cinderjit.deopt_sites(read)
            cinderjit.force_deopt(read, sites[0]["id"], n=1)
            assert read() == 7
            print("load_global reexec ok")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("load_global reexec ok", proc.stdout)

    def test_auto_like_organic_deopt_on_type_change(self):
        # Acceptance 11: organic deopt must happen after the call threshold
        # installs machine code, with no force_compile in the process.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "32"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def arith(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(40):
                assert arith(3, 5, 1) == 15
            assert cinderjit.is_jit_compiled(arith), (
                "PYTHONJITAUTO=32 never installed")
            before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert arith(3.0, 5.0, 1.0) == 15.0
            after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert after > before, (before, after)
            assert cinderjit.is_jit_compiled(arith)
            print("auto-like organic deopt", before, after)
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("auto-like organic deopt", proc.stdout)

    def test_organic_deopt_restores_load_method_pair(self):
        # Acceptance 6: a LOAD_METHOD pair stays live on the stack while a
        # later GuardFailure restores.  obj.m(a + b) keeps the pair under
        # the BINARY_OP guard; a type change must still bind and call.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Box:
                def m(self, x):
                    return x

            def caller(obj, a, b):
                return obj.m(a + b)

            box = Box()
            for _ in range(200):
                assert caller(box, 3, 5) == 8
            assert cinderjit.force_compile(caller) is True
            before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert caller(box, 3.0, 5.0) == 8.0
            after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert after > before, (before, after)
            assert cinderjit.is_jit_compiled(caller)
            print("load_method pair restored")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("load_method pair restored", proc.stdout)

    def test_organic_deopt_on_load_global_keys_version(self):
        # Organic LOAD_GLOBAL miss: inserting a global key bumps dk_version
        # so the module-value guard fails and re-executes without a second
        # PUSH_NULL.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def _g():
                return 7

            G = _g

            def read():
                return G()

            for _ in range(64):
                assert read() == 7
            assert cinderjit.force_compile(read) is True
            before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            read.__globals__["_mr7_scratch"] = 1
            assert read() == 7
            after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert after > before, (before, after, cinderjit.deopt_sites(read))
            print("load_global organic miss")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("load_global organic miss", proc.stdout)

    def test_continue_site_does_not_reexecute_a_raising_call(self):
        # Acceptance 4 continue-type: CALL + CheckExc means the opcode
        # completed (the callee ran) and resume is the next instruction.
        # Re-executing CALL would bump the hit counter twice.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            hits = 0

            def inner():
                global hits
                hits += 1
                raise ValueError("boom")

            def outer():
                return inner()

            assert cinderjit.force_compile(outer) is True
            try:
                outer()
            except ValueError:
                pass
            else:
                raise SystemExit("ValueError did not propagate")
            assert hits == 1, hits
            print("continue site did not reexecute")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("continue site did not reexecute", proc.stdout)

    def test_dead_local_is_not_kept_alive_across_organic_deopt(self):
        # Acceptance 5: a local that is dead at the deopt site restores as
        # NULL and must not extend the object's lifetime.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            wr = []

            class Box:
                pass

            def make_probe():
                return Box()

            def capture(obj):
                wr.append(weakref.ref(obj))

            def arith(a, b, one):
                probe = make_probe()
                capture(probe)
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            for _ in range(200):
                wr.clear()
                assert arith(3, 5, 1) == 15
            gc.collect()
            assert wr[-1]() is None, "interpreter path leaked the probe"
            assert cinderjit.force_compile(arith) is True
            wr.clear()
            assert arith(3.0, 5.0, 1.0) == 15.0
            gc.collect()
            assert wr[-1]() is None, wr[-1]()
            print("dead local not extended")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("dead local not extended", proc.stdout)

    def test_organic_deopt_in_recursion_does_not_leak_the_depth_counter(self):
        # Acceptance 8/9: deopt from a recursive compiled frame must leave
        # prev_instr/stacktop usable and the recursion ledger balanced, so
        # a later deep call still succeeds instead of raising RecursionError
        # at a tiny depth.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def rec(n, a, b):
                if n:
                    return rec(n - 1, a, b)
                return a + b

            for _ in range(64):
                assert rec(8, 3, 5) == 8
            assert cinderjit.force_compile(rec) is True
            before = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert rec(8, 3.0, 5.0) == 8.0
            after = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            assert after > before, (before, after)
            old = sys.getrecursionlimit()
            sys.setrecursionlimit(200)
            try:
                assert rec(40, 1, 2) == 3
            finally:
                sys.setrecursionlimit(old)
            print("recursive deopt ledger held")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("recursive deopt ledger held", proc.stdout)

    def test_entry_survives_the_artifact_being_dropped_mid_call(self):
        # The body runs arbitrary Python through its operators, and that
        # code can drop the artifact's last reference -- clearing the
        # function's __dict__ is a documented way to uncompile -- which
        # would free the code buffer currently executing.  The entry pins
        # the artifact for the duration of the call.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            holder = None

            class Killer:
                def __add__(self, other):
                    # Drop the artifact's only strong reference while its
                    # machine code is on the stack.
                    holder.__dict__.clear()
                    gc.collect()
                    return 42

            def victim(a, b):
                return a + b

            holder = victim
            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(victim)
            assert victim(Killer(), object()) == 42
            gc.collect()
            # The function keeps working afterwards, interpreted now that
            # its artifact is gone.
            assert victim(1, 2) == 3
            print("entry survived the artifact being dropped")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("entry survived", proc.stdout)

    def test_entry_refuses_an_artifact_owned_by_another_function(self):
        # Assigning a compiled function's __code__ onto another function
        # would otherwise run an artifact anchored only by the donor: the
        # code identity matches, the globals match, and nothing else looked
        # at ownership.  If the donor then died, the borrower would be
        # executing freed machine code.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            # Same module globals on purpose: that is the case where every
            # other check the entry makes would pass.
            def borrower(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            def donor(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(borrower) is True
            assert cinderjit.force_compile(donor) is True

            borrower.__code__ = donor.__code__
            before = entries()
            assert borrower(3, 5, 1) == 15
            assert entries() - before == 0, "ran an artifact it does not own"
            assert not cinderjit.is_jit_compiled(borrower)
            # The donor keeps running its own artifact.
            before = entries()
            assert donor(3, 5, 1) == 15
            assert entries() - before == 1

            # And uncompiling the borrower retires the artifact its
            # association claims -- never the artifact that owns the code
            # the borrower's mutable __code__ happens to point at now.
            assert cinderjit.force_uncompile(borrower) is True
            assert cinderjit.is_jit_compiled(donor), (
                "uncompiling the borrower retired the donor's artifact")
            before = entries()
            assert donor(3, 5, 1) == 15
            assert entries() - before == 1, (
                "the donor lost its machine entry to the borrower's "
                "uncompile")
            print("borrowed artifact refused")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("borrowed artifact refused", proc.stdout)

    def test_jit_wrapper_reports_the_truth_on_a_restricted_build(self):
        # cinderx.jit imports the whole control plane in one statement, so a
        # capability-gated build made every name fall back to a no-op stub:
        # the wrapper reported the JIT as absent while machine code was
        # demonstrably executing, and tests gated on is_enabled() skipped.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert jit.is_enabled(), "wrapper reports the JIT as absent"
            assert jit.force_compile(hot) is True
            assert jit.is_jit_compiled(hot) is True
            assert any(f is hot for f in jit.get_compiled_functions())
            assert hot(3, 5, 1) == 15
            print("wrapper reports the truth")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("wrapper reports the truth", proc.stdout)

    def test_compiled_state_matches_what_the_entry_will_do(self):
        # "Is this compiled?" and "will this call run machine code?" have to
        # be the same question for function state.  Live defaults are
        # rebound by the prologue, so growing them after compilation must
        # keep both answers true.  A __code__ swap still clears both.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(hot) is True
            before = entries()
            assert hot(3, 5, 1) == 15
            assert entries() - before == 1
            assert cinderjit.is_jit_compiled(hot)
            assert any(f is hot for f in cinderjit.get_compiled_functions())

            for mutate, restore in (
                (lambda: setattr(hot, "__defaults__", (1,)),
                 lambda: setattr(hot, "__defaults__", None)),
                (lambda: setattr(hot, "__kwdefaults__", {"one": 1}),
                 lambda: setattr(hot, "__kwdefaults__", None)),
            ):
                mutate()
                before = entries()
                assert hot(3, 5, 1) == 15
                delta = entries() - before
                compiled = cinderjit.is_jit_compiled(hot)
                listed = any(f is hot
                             for f in cinderjit.get_compiled_functions())
                assert delta == 1, delta
                assert compiled, "live defaults cleared compiled state"
                assert listed, "live defaults dropped the installed listing"
                restore()
                before = entries()
                assert hot(3, 5, 1) == 15
                assert entries() - before == 1
                assert cinderjit.is_jit_compiled(hot)

            def replacement(a, b, one):
                return "replaced"
            hot.__code__ = replacement.__code__
            before = entries()
            assert hot(3, 5, 1) == "replaced"
            assert entries() - before == 0
            assert not cinderjit.is_jit_compiled(hot)
            print("state matches the entry")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("state matches the entry", proc.stdout)

    def test_function_dies_independently_of_its_pinned_artifact(self):
        # The artifact is an ordinary object in the function's __dict__, so
        # An external pin must not keep the function alive: it dies with
        # its last reference, the watch drains the logical registries, and
        # the machine code stays resident as long as the pin.  (Popped from
        # globals first: CodeRuntime holds the globals dict strongly.)
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace.pop("victim")
            del namespace
            assert cinderjit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15

            def deaths():
                stats = _cinderx._get_trigger_stats()
                return stats["function_destroyed_notifications"]

            pin = victim.__dict__["__cinderx_compiled_func__"]
            alive = weakref.ref(victim)
            before = deaths()
            del victim
            gc.collect()
            # Churn the allocator so a dangling registry entry would point
            # into poisoned memory rather than a stale-but-intact object.
            junk = [bytearray(400) for _ in range(5000)]

            assert alive() is None, (
                "an external artifact pin must not keep the function alive")
            assert deaths() == before + 1, (before, deaths())
            assert cinderjit.get_compiled_functions() == []
            # The pin owns the physical residency; the function's death
            # does not release it.
            assert cinderjit._get_resident_compiled_functions() == 1
            del junk

            del pin
            gc.collect()
            assert cinderjit._get_resident_compiled_functions() == 0
            assert cinderjit.get_compiled_functions() == []
            print("function died independently of its pinned artifact")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "function died independently of its pinned artifact", proc.stdout
        )

    def test_suppression_and_pause_actually_stop_the_jit(self):
        # A milestone may withhold what it cannot do; it may not withhold
        # what stops it doing something.  While the canary module hid
        # jit_suppress, disable and enable, the wrapper kept no-op stubs
        # for them: @jit_suppress returned the function unchanged without
        # setting the suppress flag, so a function marked "do not compile"
        # compiled and executed anyway, and pause() disabled nothing.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            @jit.jit_suppress
            def suppressed(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            try:
                cinderjit.force_compile(suppressed)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("suppression did not suppress")
            assert not cinderjit.is_jit_compiled(suppressed)
            before = entries()
            assert suppressed(3, 5, 1) == 15
            assert entries() - before == 0

            def paused(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(paused) is True
            with jit.pause(deopt_all=True):
                assert not cinderjit.is_jit_compiled(paused)
                before = entries()
                assert paused(3, 5, 1) == 15
                assert entries() - before == 0, "pause did not pause"

            # And un-pausing must put it back: a pause that cannot be
            # undone is a one-way door, which is how re-arming after the
            # re-attach loop rather than before it went unnoticed.
            assert cinderjit.is_jit_compiled(paused), "enable did not restore"
            before = entries()
            assert paused(3, 5, 1) == 15
            assert entries() - before == 1, "machine code did not come back"

            # A control API the build genuinely withholds must say so
            # rather than report success.
            try:
                jit.precompile_all()
            except RuntimeError:
                pass
            else:
                raise SystemExit("a withheld API returned success")
            print("suppression and pause hold")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("suppression and pause hold", proc.stdout)

    def test_one_artifact_per_code_object(self):
        # CodeExtra is the execute ledger and holds one triple per code
        # object.  A second function sharing the code under a DIFFERENT
        # namespace must be refused -- publishing it would overwrite the
        # first owner's ledger entry, leaving that owner registered but
        # never executed.  The existing sharing test only covers the
        # same-namespace twin.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import types
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            src = (
                "def tpl(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            g1 = {}
            exec(src, g1)
            f1 = g1.pop("tpl")
            code = f1.__code__
            g2 = {"__builtins__": __builtins__}
            f2 = types.FunctionType(code, g2, "f2")

            assert cinderjit.force_compile(f1) is True
            assert f1(3, 5, 1) == 15
            assert cinderjit.is_jit_compiled(f1) is True

            try:
                cinderjit.force_compile(f2)
            except RuntimeError as exc:
                print("second publication refused:", exc)
            else:
                raise SystemExit(
                    "a second artifact for the same code object was "
                    "published over the first owner's ledger")

            # The first owner's state must be untouched by the attempt.
            assert cinderjit.is_jit_compiled(f1) is True
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert f1(3, 5, 1) == 15
            after = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert after == before + 1, (before, after)
            assert f2(3, 5, 1) == 15
            print("first owner intact")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("second publication refused", proc.stdout)
        self.assertIn("first owner intact", proc.stdout)

    def test_forged_dict_anchor_does_not_transfer_ownership(self):
        # The ownership oracle must live entirely in C++.  The artifact
        # reference under __cinderx_compiled_func__ is ordinary writable
        # function state, so copying it onto a second function over the
        # same code must transfer nothing: the second publication stays
        # refused and the first owner's machine entry keeps counting.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import types
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            src = (
                "def tpl(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            g1 = {}
            exec(src, g1)
            f1 = g1.pop("tpl")
            f2 = types.FunctionType(
                f1.__code__, {"__builtins__": __builtins__}, "f2")

            assert cinderjit.force_compile(f1) is True
            assert f1(3, 5, 1) == 15

            artifact = f1.__dict__["__cinderx_compiled_func__"]
            f2.__dict__["__cinderx_compiled_func__"] = artifact

            try:
                cinderjit.force_compile(f2)
            except RuntimeError as exc:
                print("forged anchor refused:", exc)
            else:
                raise SystemExit(
                    "a forged __cinderx_compiled_func__ transferred "
                    "ownership and published a second artifact")

            assert cinderjit.is_jit_compiled(f1) is True
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert f1(3, 5, 1) == 15
            after = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert after == before + 1, (before, after)
            print("first owner intact")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("forged anchor refused", proc.stdout)
        self.assertIn("first owner intact", proc.stdout)

    def test_compiled_loop_yields_the_gil(self):
        # The eval breaker a compiled back edge observes is raised for a
        # GIL drop request too, and the anchored 3.11.6 evaluator services
        # it in eval_frame_handle_pending().  A back edge that only made
        # pending calls would hold the GIL for the whole loop, so the
        # sibling thread here would be scheduled only after the loop ends.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import threading
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            ns = {}
            exec(
                "def spin(limit, one):\\n"
                "    i = limit - limit\\n"
                "    while i < limit:\\n"
                "        i = i + one\\n"
                "    return i\\n",
                ns,
            )
            spin = ns.pop("spin")
            assert cinderjit.force_compile(spin) is True
            assert spin(100, 1) == 100

            spinner_running = threading.Event()
            spinner_done = threading.Event()
            sibling_ran = threading.Event()

            def spinner():
                spinner_running.set()
                spin(120_000_000, 1)
                spinner_done.set()

            def sibling():
                spinner_running.wait(30)
                sibling_ran.set()

            t1 = threading.Thread(target=spinner)
            t2 = threading.Thread(target=sibling)
            t1.start()
            spinner_running.wait(30)
            t2.start()
            # The sibling only needs one scheduling slot.  It must get it
            # while the compiled loop is still running, which is the
            # observable difference between servicing and ignoring the GIL
            # drop request.
            got_slot = sibling_ran.wait(20)
            still_spinning = not spinner_done.is_set()
            t1.join(120)
            t2.join(30)
            assert got_slot, "sibling thread never scheduled"
            assert still_spinning, (
                "sibling only ran after the compiled loop finished; the "
                "back edge is not servicing the GIL drop request")
            print("gil yielded during compiled loop")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1200:])
        self.assertIn("gil yielded during compiled loop", proc.stdout)

    def test_async_exception_lands_on_the_backedge(self):
        # PyThreadState_SetAsyncExc() raises the eval breaker; the anchored
        # evaluator delivers the exception at the next back edge.  A
        # compiled loop must not defer it to function return.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import ctypes
            import threading
            import time
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            ns = {}
            exec(
                "def spin(limit, one):\\n"
                "    i = limit - limit\\n"
                "    while i < limit:\\n"
                "        i = i + one\\n"
                "    return i\\n",
                ns,
            )
            spin = ns.pop("spin")
            assert cinderjit.force_compile(spin) is True
            assert spin(100, 1) == 100

            class Interrupted(Exception):
                pass

            outcome = {}
            tid_box = {}
            started = threading.Event()

            def victim():
                tid_box["tid"] = threading.get_ident()
                started.set()
                begin = time.monotonic()
                try:
                    spin(2_000_000_000, 1)
                    outcome["result"] = "ran to completion"
                except Interrupted:
                    outcome["result"] = "interrupted"
                outcome["elapsed"] = time.monotonic() - begin

            t = threading.Thread(target=victim)
            t.start()
            started.wait(30)
            time.sleep(0.3)
            hit = ctypes.pythonapi.PyThreadState_SetAsyncExc(
                ctypes.c_ulong(tid_box["tid"]),
                ctypes.py_object(Interrupted))
            assert hit == 1, hit
            t.join(60)
            assert not t.is_alive(), "compiled loop never delivered the exception"
            assert outcome.get("result") == "interrupted", outcome
            print("async exception delivered on the backedge")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1200:])
        self.assertIn("async exception delivered on the backedge", proc.stdout)

    def test_async_exception_on_c_call_stops_before_next_stmt(self):
        # CPython 3.11 CALL runs CHECK_EVAL_BREAKER after a C callable
        # returns.  ctypes callbacks swallow SetAsyncExc on the same
        # thread ("Exception ignored"), so arm it from a helper thread
        # and use time.sleep as the C CALL.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import ctypes
            import threading
            import time
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Interrupted(Exception):
                pass

            def drive(sleeper, box):
                sleeper(0.3)
                box.append(1)
                return box

            assert cinderjit.force_compile(drive) is True
            tid = threading.get_ident()
            started = threading.Event()

            def shooter():
                started.wait(30)
                time.sleep(0.05)
                ctypes.pythonapi.PyThreadState_SetAsyncExc(
                    ctypes.c_ulong(tid),
                    ctypes.py_object(Interrupted))

            t = threading.Thread(target=shooter)
            t.start()
            box = []
            started.set()
            try:
                drive(time.sleep, box)
            except Interrupted:
                pass
            else:
                raise SystemExit("expected Interrupted")
            t.join(30)
            assert box == [], box
            print("async exception delivered at c call")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("async exception delivered at c call", proc.stdout)

    def test_async_exception_on_star_call_to_python_stops_before_next_stmt(
        self,
    ):
        # CALL_FUNCTION_EX checks the eval breaker after a Python callee
        # returns.  Arm SetAsyncExc from a helper thread during sleep
        # inside that callee.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import ctypes
            import threading
            import time
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Interrupted(Exception):
                pass

            def callee(*x):
                x[0](0.3)

            def drive(callee, sleeper, box):
                callee(*[sleeper])
                box.append(1)
                return box

            assert cinderjit.force_compile(drive) is True
            tid = threading.get_ident()
            started = threading.Event()

            def shooter():
                started.wait(30)
                time.sleep(0.05)
                ctypes.pythonapi.PyThreadState_SetAsyncExc(
                    ctypes.c_ulong(tid),
                    ctypes.py_object(Interrupted))

            t = threading.Thread(target=shooter)
            t.start()
            box = []
            started.set()
            try:
                drive(callee, time.sleep, box)
            except Interrupted:
                pass
            else:
                raise SystemExit("expected Interrupted")
            t.join(30)
            assert box == [], box
            print("async exception delivered at star call")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("async exception delivered at star call", proc.stdout)

    def test_async_exception_position_matches_stock_at_the_backedge(self):
        # Where the async exception lands, oracled by stock CPython itself.
        # The back-edge polls are per edge and carry the frame state AT the
        # backward jump, matching where stock 3.11 runs its eval-breaker
        # check; the traceback must therefore agree with a stock arm of the
        # same interpreter running the same loop under the same injection
        # -- the full (tb_lasti, tb_lineno), not just the source line, and
        # not an offset the implementation itself selected.
        probe = textwrap.dedent(
            """
            import ctypes
            import sys
            import threading
            import time

            mode = sys.argv[1]
            if mode == "jit":
                import _cinderx, cinderx
                cinderx.init()
                _cinderx.install_frame_evaluator()
                import cinderjit

            ns = {}
            exec(
                "def spin(limit, one):\\n"
                "    i = limit - limit\\n"
                "    while i < limit:\\n"
                "        i = i + one\\n"
                "    return i\\n",
                ns,
            )
            spin = ns.pop("spin")
            if mode == "jit":
                assert cinderjit.force_compile(spin) is True
                assert spin(100, 1) == 100
                assert cinderjit.is_jit_compiled(spin)

            class Interrupted(Exception):
                pass

            outcome = {}
            tid_box = {}
            started = threading.Event()

            def victim():
                tid_box["tid"] = threading.get_ident()
                started.set()
                try:
                    spin(2_000_000_000, 1)
                    outcome["result"] = "ran to completion"
                except Interrupted as exc:
                    tb = exc.__traceback__
                    while tb.tb_next is not None:
                        tb = tb.tb_next
                    outcome["result"] = "interrupted"
                    outcome["lasti"] = tb.tb_lasti
                    outcome["lineno"] = tb.tb_lineno
                    outcome["code"] = tb.tb_frame.f_code.co_name

            t = threading.Thread(target=victim)
            t.start()
            started.wait(30)
            time.sleep(0.3)
            hit = ctypes.pythonapi.PyThreadState_SetAsyncExc(
                ctypes.c_ulong(tid_box["tid"]),
                ctypes.py_object(Interrupted))
            assert hit == 1, hit
            t.join(60)
            assert not t.is_alive()
            assert outcome.get("result") == "interrupted", outcome
            assert outcome.get("code") == "spin", outcome
            print("POSITION", outcome["lasti"], outcome["lineno"])
            """
        )

        def run_arm(mode):
            env = _capability_clean_env()
            if mode == "jit":
                env["CINDERX_JIT_MODE"] = "canary"
                env["PYTHONJITAUTO"] = "1000000"
            proc = subprocess.run(
                [sys.executable, "-c", probe, mode],
                capture_output=True,
                text=True,
                env=env,
                timeout=300,
            )
            self.assertEqual(proc.returncode, 0, (mode, proc.stderr[-1200:]))
            for line in proc.stdout.splitlines():
                if line.startswith("POSITION "):
                    _, lasti, lineno = line.split()
                    return (int(lasti), int(lineno))
            self.fail(f"{mode} arm produced no POSITION line: {proc.stdout!r}")

        # Repeated runs: the injection lands on whatever iteration is in
        # flight, so a stable answer across runs is part of the claim.
        stock_positions = {run_arm("stock") for _ in range(3)}
        jit_positions = {run_arm("jit") for _ in range(3)}
        self.assertEqual(len(stock_positions), 1, stock_positions)
        self.assertEqual(
            jit_positions,
            stock_positions,
            "the compiled loop attributes the async exception to a "
            "different bytecode position than stock CPython",
        )

    def test_disable_enable_round_trip_is_lossless(self):
        # disable(deopt_all=True) parks every compiled function; enable()
        # must put every one of them back, and the round trip must stay
        # lossless over repeated cycles -- the old re-optimization path
        # dropped the parked entry the moment a reattachment did not
        # complete, turning the first imperfect enable() into a one-way
        # door.  Failure injection inside the publication transaction is
        # native-test territory (EnableKeepsParkedFunctionsAcrossFailures);
        # this pins the Python-visible contract, including the tracing
        # interplay: publication does not refuse under an active tracer --
        # the guarded entry falls back per call instead -- so a traced
        # enable() is not a one-way door in either direction.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            assert cinderjit.force_compile(hot) is True
            assert cinderjit.is_jit_compiled(hot)

            def machine_entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            for cycle in range(3):
                cinderjit.disable(deopt_all=True)
                assert not cinderjit.is_jit_compiled(hot), cycle
                assert hot(3, 5, 1) == 15, cycle
                cinderjit.enable()
                assert cinderjit.is_jit_compiled(hot), (
                    "cycle %d dropped the parked entry" % cycle)
                before = machine_entries()
                assert hot(3, 5, 1) == 15, cycle
                assert machine_entries() > before, (
                    "cycle %d reattached without machine-code entry" % cycle)

            def tracer(frame, event, arg):
                return tracer

            cinderjit.disable(deopt_all=True)
            assert not cinderjit.is_jit_compiled(hot)
            sys.settrace(tracer)
            cinderjit.enable()
            assert hot(3, 5, 1) == 15
            sys.settrace(None)
            assert cinderjit.is_jit_compiled(hot), (
                "a traced enable() lost the parked entry")
            assert hot(3, 5, 1) == 15
            print("parked functions survived every round trip")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("parked functions survived every round trip", proc.stdout)

    def test_immortalize_config_is_refused_by_canary(self):
        # An immortal artifact skips the dictionary anchor and the owned-
        # function association: the registry would carry a borrowed pointer
        # with nothing behind it, and 3.11 has no function watcher to clean
        # it up.  The canary refuses the configuration outright instead of
        # silently ignoring it.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONJITIMMORTALIZECOMPILEDFUNCTIONS"] = "1"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            print("init unexpectedly succeeded")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn("init unexpectedly succeeded", proc.stdout)
        self.assertIn(
            "PYTHONJITIMMORTALIZECOMPILEDFUNCTIONS", proc.stderr
        )

    def test_force_uncompile_gates_on_state_not_on_the_call_predicate(self):
        # force_uncompile() removes retained compilation state; the call
        # predicate is_jit_compiled() is deliberately false while that
        # state still exists (parked, __code__ swapped, evaluator away).
        # Gated on the call predicate, uncompile was a no-op in each of
        # those states and the artifact revived on the way back.  Three
        # revival pins, one per state.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            def make(tag):
                ns = {}
                exec(
                    "def f_%s(a, b):\\n"
                    "    t = a - a\\n"
                    "    while t < b:\\n"
                    "        t = t + a\\n"
                    "    return t\\n"
                    "def g_%s(a, b):\\n"
                    "    t = a + a\\n"
                    "    while t < b:\\n"
                    "        t = t + a\\n"
                    "    return t\\n" % (tag, tag),
                    ns,
                )
                return ns["f_" + tag], ns["g_" + tag]

            # Pin 1: __code__ swap, uncompile, swap back -- no revival.
            f, g = make("swap")
            old_code = f.__code__
            assert cinderjit.force_compile(f) is True
            f.__code__ = g.__code__
            assert cinderjit.force_uncompile(f) is True, (
                "uncompile refused a function with retained state")
            f.__code__ = old_code
            assert not cinderjit.is_jit_compiled(f)
            before = entries()
            assert f(2, 5) == 6
            assert entries() == before, "the artifact revived after swap-back"

            # Pin 2: parked, uncompile, enable -- no revival, and the
            # function is recompilable afterwards.
            p, _ = make("park")
            assert cinderjit.force_compile(p) is True
            cinderjit.disable(deopt_all=True)
            assert cinderjit.force_uncompile(p) is True, (
                "uncompile refused a parked function")
            cinderjit.enable()
            assert not cinderjit.is_jit_compiled(p)
            before = entries()
            assert p(2, 5) == 6
            assert entries() == before, "the artifact revived after enable"
            assert cinderjit.force_compile(p) is True
            assert cinderjit.is_jit_compiled(p)

            # Pin 3: evaluator away, uncompile, evaluator back -- no
            # revival.
            e, _ = make("eval")
            assert cinderjit.force_compile(e) is True
            assert _cinderx.remove_frame_evaluator() is None
            assert cinderjit.force_uncompile(e) is True, (
                "uncompile refused while the evaluator was away")
            _cinderx.install_frame_evaluator()
            assert not cinderjit.is_jit_compiled(e)
            before = entries()
            assert e(2, 5) == 6
            assert entries() == before, (
                "the artifact revived after the evaluator returned")

            print("no revival in any state")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1500:])
        self.assertIn("no revival in any state", proc.stdout)

    def test_resident_count_is_physical_under_an_external_pin(self):
        # The resident count is a physical measurement of executable
        # memory, maintained on the buffer's real lifetime.  Registry
        # bookkeeping must not move it: force_uncompile() removes every
        # registry record, but an external reference still pins the
        # artifact and its machine code -- the count holds until the pin
        # drops, and a zero here while the buffer lives would be exactly
        # the false negative the measurement exists to prevent.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def resident():
                return cinderjit._get_resident_compiled_functions()

            ns = {}
            exec(
                "def held(a, b):\\n"
                "    t = a - a\\n"
                "    while t < b:\\n"
                "        t = t + a\\n"
                "    return t\\n",
                ns,
            )
            held = ns["held"]
            base = resident()
            assert cinderjit.force_compile(held) is True
            assert resident() == base + 1

            pin = held.__dict__["__cinderx_compiled_func__"]
            assert cinderjit.force_uncompile(held) is True
            assert not cinderjit.is_jit_compiled(held)
            assert resident() == base + 1, (
                "the count dropped while an external pin still holds the "
                "machine code")

            del pin
            assert resident() == base, (
                "the count did not drop when the last reference released "
                "the buffer")
            print("resident count tracked the buffer")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1200:])
        self.assertIn("resident count tracked the buffer", proc.stdout)

    def test_code_swap_is_an_identity_guard_not_an_invalidation(self):
        # The __code__ replacement contract, stated exactly: it is a CODE
        # IDENTITY GUARD at the entry, not a permanent invalidation of the
        # compilation.  While the function's code differs from the one the
        # artifact was compiled for, every call falls back to the
        # interpreter; the association survives on purpose, so swapping
        # the original code back re-satisfies the guard and the original
        # machine code resumes service without recompilation -- the same
        # designed behaviour that lets a parked function re-attach to its
        # own artifact.  Permanent retirement is force_uncompile()'s job,
        # and a takeover by a newly compiled successor severs the old
        # claim; neither happens implicitly on a swap.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            ns = {}
            exec(
                "def steadfast(a, b):\\n"
                "    t = a - a\\n"
                "    while t < b:\\n"
                "        t = t + a\\n"
                "    return t\\n"
                "def other(a, b):\\n"
                "    t = a + a\\n"
                "    while t < b:\\n"
                "        t = t + a\\n"
                "    return t\\n",
                ns,
            )
            f, g = ns["steadfast"], ns["other"]
            old_code = f.__code__
            assert cinderjit.force_compile(f) is True

            # Guard closed: mismatched identity falls back per call.
            f.__code__ = g.__code__
            assert not cinderjit.is_jit_compiled(f)
            before = entries()
            assert f(2, 5) == 6
            assert entries() == before

            # Guard re-satisfied: the original code resumes machine
            # service without a recompile.
            f.__code__ = old_code
            assert cinderjit.is_jit_compiled(f)
            before = entries()
            assert f(2, 5) == 6
            assert entries() == before + 1

            # Permanent retirement is explicit.
            assert cinderjit.force_uncompile(f) is True
            f.__code__ = g.__code__
            f.__code__ = old_code
            assert not cinderjit.is_jit_compiled(f)
            before = entries()
            assert f(2, 5) == 6
            assert entries() == before
            print("identity guard, not invalidation")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-1200:])
        self.assertIn("identity guard, not invalidation", proc.stdout)

    def test_instrumentation_support_config_is_refused_by_canary(self):
        # The flag patches sys.setprofile/settrace/monitoring to pause the
        # whole JIT while instrumentation is active.  This mode delivers
        # tracing correctness through the guarded entry and the RFC
        # 3.3.4.5 exemption (running frames finish natively) instead, and
        # the toggle's disable arm has no audited 3.11 story -- so the
        # canary refuses the configuration by name rather than running an
        # unaudited pause-the-world path alongside the audited one.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONJITSUPPORTINSTRUMENTATION"] = "1"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            print("init unexpectedly succeeded")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertNotIn("init unexpectedly succeeded", proc.stdout)
        self.assertIn("PYTHONJITSUPPORTINSTRUMENTATION", proc.stderr)

    def test_tracing_falls_back_to_interpreter(self):
        # MR-04 implements no instrumentation, so it fails closed: while a
        # legacy trace or profile function is registered, the guarded entry
        # must hand every call to the interpreter, which delivers the
        # call/line/return events the tracer is owed.  Machine code resumes
        # once tracing stops.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            assert cinderjit.force_compile(hot) is True

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            base = entries()
            assert hot(3, 5, 1) == 15
            assert entries() == base + 1, "machine entry before tracing"

            events = []

            def tracer(frame, event, arg):
                events.append(event)
                return tracer

            sys.settrace(tracer)
            base = entries()
            assert hot(3, 5, 1) == 15
            traced_delta = entries() - base
            sys.settrace(None)
            assert traced_delta == 0, (
                "machine code executed %d time(s) under sys.settrace" %
                traced_delta)
            assert "call" in events, events
            print("traced through the interpreter")

            base = entries()
            assert hot(3, 5, 1) == 15
            assert entries() == base + 1, "machine entry after tracing"
            print("machine entry restored")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("traced through the interpreter", proc.stdout)
        self.assertIn("machine entry restored", proc.stdout)

    def test_profile_enabled_inside_binary_op_follows_the_rfc_exemption(self):
        # RFC 3.3.4.5: a sys.setprofile from inside the running compiled
        # frame (here: synchronously inside __add__) does NOT interrupt
        # that frame -- it keeps running natively to its natural return
        # and its remaining events, including PyTrace_RETURN, are not
        # delivered.  Only new calls observe the activation: the guarded
        # entry falls back and the interpreted run is fully profiled.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            events = []
            def prof(frame, what, arg):
                events.append((frame.f_code.co_name, what))

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            class Num:
                def __init__(self, n):
                    self.n = n
                def __sub__(self, other):
                    return Num(self.n - other.n)
                def __add__(self, other):
                    n = self.n + other.n
                    if n == 6:
                        sys.setprofile(prof)
                    return Num(n)
                def __lt__(self, other):
                    return self.n < other.n

            assert cinderjit.force_compile(hot) is True
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            result = hot(Num(2), Num(10), Num(1))
            entered = _cinderx._get_trigger_stats()["machine_code_entries"] \\
                - before
            assert entered == 1, "the probe call must start in machine code"
            assert result.n == 20, result.n
            assert not [e for e in events if e[0] == "hot"], (
                "the running compiled frame must not deliver events "
                "(RFC 3.3.4.5 items 2-3): %r" % (events,)
            )

            # A new call under the active profiler: interpreted, and
            # fully profiled.
            events.clear()
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            follow = hot(Num(2), Num(10), Num(1))
            assert _cinderx._get_trigger_stats()["machine_code_entries"] \\
                == before, "a new call under profiling entered machine code"
            sys.setprofile(None)
            assert follow.n == 20, follow.n
            assert ("hot", "call") in events and ("hot", "return") in events, (
                "the interpreted follow-up call was not profiled: %r"
                % (events,)
            )
            print("mid-frame profile followed the RFC exemption")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("mid-frame profile followed the RFC exemption", proc.stdout)

    def test_profile_enabled_by_pending_call_follows_the_rfc_exemption(self):
        # The same transition through the other door: Py_AddPendingCall()
        # runs its callback on this thread at the compiled loop's back
        # edge (the eval-breaker service stays wired per RFC -- signals,
        # pending calls and GIL periodic tasks are delivered), and the
        # callback enables profiling.  RFC 3.3.4.5: the running compiled
        # frame still finishes natively with no events; only new calls
        # observe the activation.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import threading
            import _testcapi
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            events = []
            def prof(frame, what, arg):
                events.append((frame.f_code.co_name, what))

            armed = threading.Event()
            def enable_profile():
                sys.setprofile(prof)
                return 0

            def arm():
                armed.wait()
                # Give the main thread time to enter machine code, then
                # land the pending call while the loop is running.  The
                # machine-entry assertion below rejects the too-early
                # ordering, so a mistimed run fails instead of passing
                # without exercising the mid-frame transition.
                import time
                time.sleep(0.05)
                _testcapi._pending_threadfunc(enable_profile)

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            assert hot(1, 10, 1) == 10

            worker = threading.Thread(target=arm)
            worker.start()
            armed.set()
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            result = hot(1, 60_000_000, 1)
            worker.join()
            assert result == 60_000_000, result
            assert _cinderx._get_trigger_stats()["machine_code_entries"] \\
                - before == 1, (
                "the pending call landed before the loop entered machine"
                " code; the mid-frame transition was not exercised"
            )
            assert sys.getprofile() is prof, (
                "the pending call never ran on the loop thread")
            assert not [e for e in events if e[0] == "hot"], (
                "RFC 3.3.4.5 items 2-3: the running compiled frame must "
                "keep running natively and deliver no events: %r"
                % (events[:10],)
            )

            events.clear()
            before = _cinderx._get_trigger_stats()["machine_code_entries"]
            follow = hot(1, 10, 1)
            assert _cinderx._get_trigger_stats()["machine_code_entries"] \\
                == before, "a new call under profiling entered machine code"
            sys.setprofile(None)
            assert follow == 10, follow
            assert ("hot", "call") in events and ("hot", "return") in events, (
                "the interpreted follow-up call was not profiled: %r"
                % (events[:10],)
            )
            print("pending-call profile followed the RFC exemption")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "pending-call profile followed the RFC exemption", proc.stdout)

    def _run_transition_probe(self, body):
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        prelude = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            events = []
            def prof(frame, what, arg):
                events.append((frame.f_code.co_name, what))

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", prelude + textwrap.dedent(body)],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("transition followed the RFC exemption", proc.stdout)

    def test_profile_enabled_inside_for_iter_next_follows_the_exemption(self):
        # Activation from inside an iterator's __next__ mid-frame: RFC
        # 3.3.4.5 -- the running compiled frame finishes natively with no
        # events; the follow-up call is interpreted and fully profiled.
        self._run_transition_probe(
            """
            class It:
                def __init__(self, arm):
                    self.arm = arm
                def __iter__(self):
                    return self
                def __next__(self):
                    if self.arm:
                        sys.setprofile(prof)
                    raise StopIteration

            ns = {}
            exec(
                "def hot(it):\\n"
                "    for x in it:\\n"
                "        pass\\n"
                "    return 42\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            before = entries()
            result = hot(It(True))
            assert entries() - before == 1, "must start in machine code"
            assert result == 42, result
            assert not [e for e in events if e[0] == "hot"], events
            events.clear()
            before = entries()
            follow = hot(It(False))
            assert entries() == before, "new call entered machine code"
            sys.setprofile(None)
            assert follow == 42, follow
            assert ("hot", "call") in events, events
            assert ("hot", "return") in events, events
            print("transition followed the RFC exemption")
            """
        )

    def test_profile_enabled_inside_bool_follows_the_exemption(self):
        # Activation from inside __bool__ feeding a conditional jump:
        # same RFC exemption oracle.
        self._run_transition_probe(
            """
            class Flag:
                def __init__(self, arm):
                    self.arm = arm
                def __bool__(self):
                    if self.arm:
                        sys.setprofile(prof)
                    return False

            ns = {}
            exec(
                "def hot(flag):\\n"
                "    if flag:\\n"
                "        return 1\\n"
                "    return 2\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            before = entries()
            result = hot(Flag(True))
            assert entries() - before == 1, "must start in machine code"
            assert result == 2, result
            assert not [e for e in events if e[0] == "hot"], events
            events.clear()
            before = entries()
            follow = hot(Flag(False))
            assert entries() == before, "new call entered machine code"
            sys.setprofile(None)
            assert follow == 2, follow
            assert ("hot", "call") in events, events
            assert ("hot", "return") in events, events
            print("transition followed the RFC exemption")
            """
        )

    def test_foreign_code_extra_slot_below_ours_refuses_canary(self):
        # CPython 3.11's code_dealloc walks every registered co_extra slot
        # from zero up to ce_size, calling each freefunc whether or not
        # this code object ever populated the slot.  Capping our writes
        # protects the slots ABOVE ours; nothing can protect a foreign
        # slot BELOW ours, because publishing our ledger forces ce_size
        # past it.  If that foreign freefunc dies before the last code
        # object does -- a ctypes callback at interpreter shutdown -- the
        # walk calls freed memory.  The only sound MR-04 answer is to
        # refuse the executing mode up front when the order is wrong.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import ctypes

            FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

            @FREEFUNC
            def foreign_free(ptr):
                pass

            req = ctypes.pythonapi._PyEval_RequestCodeExtraIndex
            req.restype = ctypes.c_ssize_t
            req.argtypes = [FREEFUNC]
            index = req(foreign_free)
            assert index == 0, "the foreign user must win slot 0: %d" % index

            # The executing mode initializes with the module import, so
            # the refusal surfaces there; wrap the whole entry sequence.
            try:
                import _cinderx, cinderx
                cinderx.init()
            except RuntimeError as exc:
                assert "code-extra" in str(exc), exc
                print("canary refused the reversed registration order")
            else:
                raise AssertionError(
                    "canary initialized with a foreign code-extra slot"
                    " below its own; interpreter shutdown now walks a"
                    " freefunc this process does not control"
                )
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "canary refused the reversed registration order", proc.stdout)

    def test_canary_init_installs_the_frame_evaluator(self):
        # The canary is documented as the mode that "brings the compiler
        # up, installs entry points and executes them" -- an execution
        # mode, not a library a harness assembles.  Without the frame
        # evaluator, the interpreter's specialized CALL pushes the callee
        # frame inline and never consults the vectorcall entry, so a
        # function the control plane calls compiled runs interpreted on
        # the hottest path there is.  Initialization therefore installs
        # and verifies the evaluator itself; no separate
        # install_frame_evaluator() call appears in this probe on purpose.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            import cinderjit

            # The contract line: initialization itself installed the
            # evaluator, before anything compiled.  The lazy install in
            # the compile entries is a backstop, not the contract.
            assert _cinderx.is_frame_evaluator_installed(), (
                "canary initialization completed without the frame"
                " evaluator installed"
            )

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True

            def caller():
                return hot(3, 5, 1)

            # Let the interpreter specialize caller's CALL site.
            for _ in range(200):
                assert caller() == 15
            assert cinderjit.is_jit_compiled(hot)

            before = entries()
            rounds = 50
            for _ in range(rounds):
                assert caller() == 15
            delta = entries() - before
            assert delta == rounds, (
                "the specialized CALL site bypassed the compiled entry"
                " (%d of %d calls entered machine code); the evaluator"
                " the canary owes was not installed" % (delta, rounds)
            )
            print("canary installed the evaluator itself")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=180,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("canary installed the evaluator itself", proc.stdout)

    def test_evaluator_ownership_loss_fails_closed(self):
        # Initialization installs and verifies the evaluator, but the
        # entry point can change hands afterwards: remove_frame_evaluator
        # stays published for testing, and another PEP 523 client can take
        # the slot outright.  Without the evaluator, the interpreter's
        # specialized CALL pushes frames inline and never consults the
        # vectorcall entry -- so a predicate that keeps answering
        # "compiled" is lying about exactly the calls that matter.  The
        # installed-artifact predicate therefore re-checks ownership on
        # every query: lose the entry point and is_jit_compiled() turns
        # false while every call runs interpreted; reinstall and the
        # published artifact serves again without recompiling.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            ns = {}
            exec(
                "def hot(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            assert cinderjit.is_jit_compiled(hot)
            before = entries()
            assert hot(3, 5, 1) == 15
            assert entries() - before == 1

            _cinderx.remove_frame_evaluator()
            assert not _cinderx.is_frame_evaluator_installed()
            assert not cinderjit.is_jit_compiled(hot), (
                "the evaluator is gone and specialized calls bypass the"
                " entry, yet the control plane still reports compiled"
            )
            assert not cinderjit.is_enabled(), (
                "no call can reach machine code, yet the global state"
                " still says enabled"
            )
            before = entries()
            assert hot(3, 5, 1) == 15
            assert entries() - before == 0, (
                "machine code ran without the frame evaluator installed"
            )

            # enable() reclaims the entry point itself when stock holds it.
            cinderjit.enable()
            assert _cinderx.is_frame_evaluator_installed(), (
                "enable() reported success without taking the evaluator"
                " back"
            )
            assert cinderjit.is_enabled()
            assert cinderjit.is_jit_compiled(hot), (
                "the artifact never left; reinstalling the evaluator must"
                " restore the published entry without a recompile"
            )
            before = entries()
            assert hot(3, 5, 1) == 15
            assert entries() - before == 1
            print("evaluator ownership loss failed closed")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("evaluator ownership loss failed closed", proc.stdout)

    def test_mid_frame_pep523_takeover_does_not_capture_the_deopt(self):
        # Changing the PEP 523 evaluator affects future frame entries; a
        # frame already in flight keeps the interpreter that launched it.
        # The deopt continuation used to re-dispatch through
        # _PyEval_EvalFrame(), which reads interp->eval_frame at call
        # time -- so a third party that took the entry point from inside
        # the compiled frame's own BINARY_OP would retroactively receive
        # the reified mid-frame, entry-frame cleanup contract and all.
        # The continuation is now pinned to the anchored 3.11 evaluator:
        # the recorder below must never see the hot frame it was
        # installed under, and must see the fresh calls made afterwards.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _testinternalcapi
            import _cinderx, cinderx
            cinderx.init()
            import cinderjit

            record = []

            class Bomb:
                def __add__(self, other):
                    # The third party takes the entry point while hot()'s
                    # machine code is on the stack, then the operator
                    # raises, forcing hot() to deopt.
                    _testinternalcapi.set_eval_frame_record(record)
                    raise ValueError("mid-frame takeover")

            ns = {}
            exec("def hot(a, b):\\n    return a + b\\n", ns)
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            assert hot(1, 2) == 3
            assert cinderjit.is_jit_compiled(hot)

            try:
                hot(Bomb(), 1)
            except ValueError:
                pass
            else:
                raise AssertionError("the operator must raise")

            # The recorder appends co_name strings on each entry to the
            # recording evaluator.
            hot_sightings = [name for name in record if name == "hot"]
            assert not hot_sightings, (
                "the reified mid-frame was handed to the evaluator"
                " installed DURING its execution: %d sighting(s)"
                % len(hot_sightings)
            )

            # Future entries belong to the new owner.  Two provisos make
            # the demonstration precise on 3.11: a frame reaches
            # interp->eval_frame only when entered from C (inline
            # specialized CALL keeps pure-Python callees inside the loop
            # that pushed them), and a function still carrying the guarded
            # entry falls back through the interpreter CinderX vendors --
            # its vectorcall is CinderX property even while the evaluator
            # slot is not.  So the recorder's liveness for future entries
            # is shown with an unguarded function at a C boundary, and
            # hot itself is required to run correctly, interpreted.
            assert not cinderjit.is_jit_compiled(hot)
            ns = {}
            exec("def plain(a, b):\\n    return a + b\\n", ns)
            plain = ns.pop("plain")
            n = len(record)
            assert list(map(plain, (1,), (2,))) == [3]
            assert "plain" in record[n:], (
                "a fresh C-boundary entry after the takeover never"
                " reached the new evaluator"
            )
            assert hot(1, 2) == 3

            # While the third party holds the entry point, enable() must
            # refuse rather than usurp -- and must not claim capability.
            assert not cinderjit.is_enabled()
            try:
                cinderjit.enable()
            except RuntimeError:
                pass
            else:
                raise AssertionError(
                    "enable() reported success while another component"
                    " held the frame-evaluator entry point"
                )
            assert not cinderjit.is_enabled()

            # Hand the entry point back; enable() reclaims it, and the
            # artifact never left.
            _testinternalcapi.set_eval_frame_default()
            cinderjit.enable()
            assert _cinderx.is_frame_evaluator_installed()
            assert cinderjit.is_enabled()
            assert cinderjit.is_jit_compiled(hot)
            assert hot(1, 2) == 3
            print("mid-frame takeover stayed out of the deopt")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("mid-frame takeover stayed out of the deopt", proc.stdout)

    def test_profile_enabled_inside_del_follows_the_exemption(self):
        # Activation from a DECREF's __del__ -- the transition the
        # bytecode never shows: same RFC exemption oracle.
        self._run_transition_probe(
            """
            class Temp:
                def __init__(self, arm):
                    self.arm = arm
                def __del__(self):
                    if self.arm:
                        sys.setprofile(prof)

            class Num:
                def __init__(self, arm):
                    self.arm = arm
                def __add__(self, other):
                    return Temp(self.arm)

            ns = {}
            exec(
                "def hot(a, b):\\n"
                "    a + b\\n"
                "    return 42\\n",
                ns,
            )
            hot = ns.pop("hot")
            del ns

            assert cinderjit.force_compile(hot) is True
            before = entries()
            result = hot(Num(True), Num(True))
            assert entries() - before == 1, "must start in machine code"
            assert result == 42, result
            assert not [e for e in events if e[0] == "hot"], events
            events.clear()
            before = entries()
            follow = hot(Num(False), Num(False))
            assert entries() == before, "new call entered machine code"
            sys.setprofile(None)
            assert follow == 42, follow
            assert ("hot", "call") in events, events
            assert ("hot", "return") in events, events
            print("transition followed the RFC exemption")
            """
        )

    def test_stale_generation_teardown_does_not_clobber_successor(self):
        # Association survives a deopt, so a function that swaps its
        # __code__ while parked and then compiles again is claimed by two
        # generations at once unless the new installation severs the old
        # claim.  Left unsevered, the old artifact's delayed destruction
        # walks its member list and dismantles whatever is installed for
        # those functions TODAY -- the successor's registry entry and its
        # machine-code entry point -- because the teardown believed
        # membership still meant "currently installed by me".  Python can
        # hold the old artifact for arbitrarily long: it is an ordinary
        # object in the function's __dict__.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            body = (
                "def {name}(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            ns = {}
            exec(body.format(name="victim"), ns)
            exec(body.format(name="replacement"), ns)
            victim = ns.pop("victim")
            rep_code = ns.pop("replacement").__code__
            del ns

            assert cinderjit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15
            old = victim.__dict__["__cinderx_compiled_func__"]

            cinderjit.disable(True)
            victim.__code__ = rep_code
            cinderjit.enable()

            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(victim)
            before = entries()
            assert victim(3, 5, 1) == 15
            assert entries() - before == 1, "successor must run compiled"

            # The stale generation dies only now.  Its teardown must not
            # touch an installation it no longer owns.
            del old
            gc.collect()

            assert cinderjit.is_jit_compiled(victim), (
                "the stale generation's teardown removed the successor's"
                " registry entry"
            )
            before = entries()
            assert victim(3, 5, 1) == 15
            assert entries() - before == 1, (
                "the stale generation's teardown reset the successor's"
                " machine-code entry point"
            )
            print("stale teardown left the successor installed")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("stale teardown left the successor installed", proc.stdout)

    def test_reassociation_is_a_one_way_transfer(self):
        # Severing the old claim at re-association time has a visible
        # consequence: while the old artifact is still published for the
        # old code object, the function cannot walk back onto it -- the
        # one-artifact-per-code refusal now finds a published artifact
        # that no longer names the function.  That is the fail-closed
        # reading of "association ends with an explicit re-association":
        # the transfer is not reversible while the abandoned publisher
        # lives, and becomes an ordinary fresh compile once it dies.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            body = (
                "def {name}(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            ns = {}
            exec(body.format(name="victim"), ns)
            exec(body.format(name="replacement"), ns)
            victim = ns.pop("victim")
            old_code = victim.__code__
            rep_code = ns.pop("replacement").__code__
            del ns

            assert cinderjit.force_compile(victim) is True
            old = victim.__dict__["__cinderx_compiled_func__"]

            cinderjit.disable(True)
            victim.__code__ = rep_code
            cinderjit.enable()
            assert cinderjit.force_compile(victim) is True

            # Swap back while the abandoned publisher is still alive: the
            # old code object still has a published artifact, and the
            # function is no longer among its owners.
            cinderjit.disable(True)
            victim.__code__ = old_code
            cinderjit.enable()
            refused = False
            try:
                cinderjit.force_compile(victim)
            except RuntimeError as exc:
                refused = "CANNOT_SPECIALIZE" in str(exc)
            assert refused, "walking back onto an abandoned artifact"
            assert not cinderjit.is_jit_compiled(victim)
            before = entries()
            assert victim(3, 5, 1) == 15
            assert entries() - before == 0, "refused shape ran machine code"

            # Once the abandoned publisher dies the refusal dies with it.
            del old
            gc.collect()
            assert cinderjit.force_compile(victim) is True
            before = entries()
            assert victim(3, 5, 1) == 15
            assert entries() - before == 1
            print("re-association transferred one way")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("re-association transferred one way", proc.stdout)

    def test_stale_generation_outliving_its_function_stays_inert(self):
        # The death-order mirror of the clobber case: the function dies
        # first, the stale artifact second.  An unsevered claim would leave
        # the stale artifact holding a pointer to the dead function, and
        # its teardown would write that function's entry point after the
        # allocator reclaimed it -- the assertion with teeth runs in the
        # sanitizer arm, where that write is a hard report.  At this level
        # the contract is that both teardown orders exit cleanly and leave
        # the registry readable.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            body = (
                "def {name}(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            ns = {}
            exec(body.format(name="victim"), ns)
            exec(body.format(name="replacement"), ns)
            victim = ns.pop("victim")
            rep_code = ns.pop("replacement").__code__
            del ns

            assert cinderjit.force_compile(victim) is True
            old = victim.__dict__["__cinderx_compiled_func__"]

            cinderjit.disable(True)
            victim.__code__ = rep_code
            cinderjit.enable()
            assert cinderjit.force_compile(victim) is True

            del victim
            gc.collect()
            # Churn the allocator so a freed function object is reused.
            junk = [bytearray(400) for _ in range(5000)]
            del old
            gc.collect()
            del junk

            for fn in cinderjit.get_compiled_functions():
                assert fn.__qualname__
            print("stale generation went quietly")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("stale generation went quietly", proc.stdout)

    def test_successor_compile_survives_the_predecessors_reentrant_death(self):
        # With no external pin, installing the successor is itself what
        # frees the predecessor: the association overwrites the artifact
        # reference in the function's __dict__, and the predecessor's
        # teardown runs reentrantly inside the installation transaction.
        # The transaction must come out of that with the successor fully
        # installed -- registry entry, entry point, and a callable result.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            body = (
                "def {name}(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n"
            )
            ns = {}
            exec(body.format(name="victim"), ns)
            exec(body.format(name="replacement"), ns)
            victim = ns.pop("victim")
            rep_code = ns.pop("replacement").__code__
            del ns

            assert cinderjit.force_compile(victim) is True
            cinderjit.disable(True)
            victim.__code__ = rep_code
            cinderjit.enable()

            # No pin: the predecessor dies inside this call.
            assert cinderjit.force_compile(victim) is True
            assert cinderjit.is_jit_compiled(victim)
            before = entries()
            assert victim(3, 5, 1) == 15
            assert entries() - before == 1
            assert any(
                fn.__qualname__ == "victim"
                for fn in cinderjit.get_compiled_functions()
            )
            print("successor survived the reentrant teardown")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn(
            "successor survived the reentrant teardown", proc.stdout)

    def test_deopt_survives_cycle_only_function(self):
        # The state the paused-death case cannot reach: every external
        # reference dropped BEFORE the deopt, no collection run, so the
        # artifact <-> function cycle is the only owner when
        # disable(deopt_all=True) walks the registry.  A deopt keeps the
        # artifact's claim on the function -- membership is the ownership
        # oracle and survives parking -- so the walk must write the
        # interpreter entry and park a function whose only owner is the
        # cycle it is part of, without collapsing that cycle mid-walk;
        # under the borrowed registries of MR-05 the function may instead
        # already be gone before disable() looks.  Both worlds must exit
        # cleanly and leave the registry readable.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            ns = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                ns,
            )
            victim = ns.pop("victim")
            assert cinderjit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15
            del ns
            del victim  # no gc.collect() on purpose
            print("cycle-only state armed")
            cinderjit.disable(True)
            print("survived disable")
            cinderjit.enable()
            print("survived enable")
            for fn in cinderjit.get_compiled_functions():
                assert fn.__qualname__
            print("registry readable")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        for marker in (
            "cycle-only state armed",
            "survived disable",
            "survived enable",
            "registry readable",
        ):
            self.assertIn(marker, proc.stdout)

    def test_function_dying_while_paused_survives_re_enable(self):
        # disable(deopt_all=True) parks every compiled function in the
        # deopted set, which re-enabling walks again.  The entries are
        # borrowed, so what keeps that walk safe is the weak-reference death
        # watch: a function that dies while paused has to leave the set
        # before enable() reaches it.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit
            import cinderjit

            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace["victim"]
            del namespace
            assert jit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15
            alive = weakref.ref(victim)

            with jit.pause(deopt_all=True):
                del victim
                gc.collect()
                # Nothing holds it now, so it has to be gone -- and the
                # parked set has to have been told, or the walk below
                # dereferences freed memory.
                assert alive() is None
                junk = [bytearray(400) for _ in range(5000)]
                del junk
            # Re-enabling walks the parked set; nothing here may dangle.
            gc.collect()
            assert alive() is None
            assert not cinderjit.get_compiled_functions()
            print("survived re-enable")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("survived re-enable", proc.stdout)

    def test_resident_count_is_physical_not_logical(self):
        # The resident metric answers "is a code buffer still alive", so it
        # may not depend on whether the JIT happens to be paused, and it may
        # not drop a deopted-but-resident artifact.  Reporting zero while
        # machine code is still installed is the false negative it exists
        # to prevent.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(hot) is True
            running = cinderjit._get_resident_compiled_functions()
            assert isinstance(running, int), type(running)
            assert running >= 1, running

            cinderjit.disable()
            paused = cinderjit._get_resident_compiled_functions()
            assert isinstance(paused, int), type(paused)
            assert paused >= running, (paused, running)
            cinderjit.enable()
            print("resident count is physical")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("resident count is physical", proc.stdout)

    def test_cold_and_warm_entries_refuse_the_wrong_timing(self):
        # The plan names cold and warm compilation as separate entry
        # points because they compile different inputs.  Leaving the
        # distinction to whether the caller happened to warm the function
        # first makes it an accident; each entry refuses the other timing.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            from cinderx import jit

            def make(name):
                namespace = {}
                exec(
                    "def " + name + "(a, b, one):\\n"
                    "    total = a - a\\n"
                    "    i = total\\n"
                    "    while i < b:\\n"
                    "        total = total + a\\n"
                    "        i = i + one\\n"
                    "    return total\\n",
                    namespace,
                )
                return namespace[name]

            cold = make("cold")
            assert jit.force_compile_cold(cold) is True
            assert jit.is_jit_compiled(cold)

            warm = make("warm")
            try:
                jit.force_compile_warm(warm)
            except RuntimeError as exc:
                assert "not been specialized" in str(exc), exc
            else:
                raise SystemExit("warm entry accepted an unquickened function")
            for _ in range(200):
                warm(3, 5, 1)
            assert jit.force_compile_warm(warm) is True
            assert jit.is_jit_compiled(warm)

            already = make("already")
            for _ in range(200):
                already(3, 5, 1)
            try:
                jit.force_compile_cold(already)
            except RuntimeError as exc:
                assert "already run" in str(exc), exc
            else:
                raise SystemExit("cold entry accepted a quickened function")
            print("cold and warm entries hold their timings")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("cold and warm entries hold", proc.stdout)

    def test_ten_thousand_calls_leave_no_reference_drift(self):
        # MR-04 acceptance: after 10,000 machine-code calls the function, its
        # code object and the arguments must show zero refcount drift, on the
        # normal-return path and on the exception path alike.  Retained
        # entry frames show up here first: a frame that is never cleared and
        # popped keeps its arguments alive, so argument drift is the
        # sensitive detector for frame-chain residue.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        # No call threshold: organic scheduling and the ROI deopt backoff both
        # key off compile_after_n_calls, so leaving it unset keeps the two
        # functions below under force-compile control for the whole run and
        # lets the exception path stay in machine code past the backoff
        # budget (whose own behaviour is covered separately).
        env.pop("PYTHONJITAUTO", None)
        probe = textwrap.dedent(
            """
            import gc
            import json
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            CALLS = 10000

            def hot(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            def raiser(x, none):
                return x + none

            assert cinderjit.force_compile(hot) is True
            assert cinderjit.force_compile(raiser) is True

            class Arg(int):
                pass

            arg = Arg(3)
            payload = object()

            def burn_normal(n):
                for _ in range(n):
                    hot(arg, 5, 1)

            def burn_raising(n):
                for _ in range(n):
                    try:
                        raiser(payload, None)
                    except TypeError:
                        pass

            # Warm both paths first: the first calls legitimately publish
            # caches and stubs, so the baseline is taken after that settles.
            burn_normal(16)
            burn_raising(16)
            gc.collect()

            before = {
                "hot": sys.getrefcount(hot),
                "hot_code": sys.getrefcount(hot.__code__),
                "raiser": sys.getrefcount(raiser),
                "raiser_code": sys.getrefcount(raiser.__code__),
                "arg": sys.getrefcount(arg),
                "payload": sys.getrefcount(payload),
            }
            entries_before = _cinderx._get_trigger_stats()[
                "machine_code_entries"]

            burn_normal(CALLS)
            burn_raising(CALLS)
            gc.collect()

            after = {
                "hot": sys.getrefcount(hot),
                "hot_code": sys.getrefcount(hot.__code__),
                "raiser": sys.getrefcount(raiser),
                "raiser_code": sys.getrefcount(raiser.__code__),
                "arg": sys.getrefcount(arg),
                "payload": sys.getrefcount(payload),
            }
            drift = {k: after[k] - before[k] for k in before}
            assert all(v == 0 for v in drift.values()), drift

            # Every one of those calls must have entered machine code and
            # linked its own frame; a stuck counter would make the drift
            # assertion vacuous.
            entries = _cinderx._get_trigger_stats()["machine_code_entries"]
            assert entries - entries_before == 2 * CALLS, (
                entries - entries_before, 2 * CALLS)
            assert cinderjit.is_jit_compiled(hot)
            assert cinderjit.is_jit_compiled(raiser)
            print(json.dumps({"drift": drift, "entries": entries}))
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        self.assertEqual(set(report["drift"].values()), {0}, report)

    def test_repeated_deopts_demote_the_function_safely(self):
        # Machine code that keeps deopting is withdrawn by the shared ROI
        # backoff policy once its deopt budget is spent.  MR-04 is the first
        # point at which that transition is reachable on 3.11 -- nothing
        # executed before it, so nothing could deopt -- and the transition
        # unpatches an entry point that calls are still arriving at.  Results
        # must stay correct across it, the machine-code counter must stop
        # growing once the function is withdrawn, and nothing may drift.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        # The backoff only arms when a call threshold exists; a huge one keeps
        # organic compilation out of the way while leaving the policy live.
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import json
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def raiser(x, none):
                return x + none

            assert cinderjit.force_compile(raiser) is True
            payload = object()

            def burn(n):
                raised = 0
                for _ in range(n):
                    try:
                        raiser(payload, None)
                    except TypeError:
                        raised += 1
                return raised

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            # Spend the budget.  Every call must still raise TypeError, both
            # while compiled and after the withdrawal.
            assert burn(16) == 16
            assert cinderjit.is_jit_compiled(raiser)
            armed = entries()
            assert armed == 16, armed

            assert burn(2048) == 2048
            assert not cinderjit.is_jit_compiled(raiser), (
                "the deopt budget never withdrew the machine code")
            withdrawn = entries()

            gc.collect()
            before = (
                sys.getrefcount(raiser),
                sys.getrefcount(raiser.__code__),
                sys.getrefcount(payload),
            )
            assert burn(2048) == 2048
            gc.collect()
            after = (
                sys.getrefcount(raiser),
                sys.getrefcount(raiser.__code__),
                sys.getrefcount(payload),
            )
            assert before == after, (before, after)
            # Withdrawn means withdrawn: no further machine-code entries.
            assert entries() == withdrawn, (entries(), withdrawn)
            print(json.dumps({"armed": armed, "withdrawn": withdrawn}))
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        report = json.loads(proc.stdout.strip().splitlines()[-1])
        # The withdrawal happens at the configured budget, well before the
        # 2048 calls that follow: the counter must stop far below them.
        self.assertLess(report["withdrawn"], 2048, report)

    def test_code_extra_write_does_not_arm_foreign_slots(self):
        # Regression: CPython's _PyCode_SetExtra sizes a fresh co_extra to the
        # interpreter's total registered index count, and code_dealloc then
        # invokes EVERY registered freefunc below that size -- including for
        # slots holding NULL.  Attaching our counter to a code object must
        # therefore not enlarge its co_extra past our own index, or every code
        # object the runtime touches starts calling third-party freefuncs it
        # never stored anything for.  Lib/test/test_code.py registers such a
        # freefunc as a ctypes closure, which dies during shutdown; before the
        # fix that combination segfaulted at finalization.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1"
        # On 3.11 the only writer of a fresh code object's co_extra is
        # compilation, and compiled code is otherwise pinned for the life of
        # the process by the twin-dedup cache -- which would leave the
        # assertion below with nothing to observe.  Turning dedup off is what
        # lets the subject both be written and then die.
        env["CINDERX_AUTOJIT_CODE_DEDUP"] = "0"
        probe = textwrap.dedent(
            """
            import ctypes
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            # Claim two indices after cinderx, so both are above ours.
            FREEFUNC = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
            request = ctypes.pythonapi._PyEval_RequestCodeExtraIndex
            request.argtypes = (ctypes.c_void_p,)
            request.restype = ctypes.c_ssize_t

            freed = []
            low_cb = FREEFUNC(lambda ptr: freed.append("low"))
            low = request(ctypes.cast(low_cb, ctypes.c_void_p))
            high_cb = FREEFUNC(lambda ptr: freed.append("high"))
            high = request(ctypes.cast(high_cb, ctypes.c_void_p))
            assert 0 < low < high, (low, high)
            index = low

            # A code object the runtime attaches its extra data to, then
            # drops.  The body stays inside the execute surface so that it
            # actually compiles: compilation is what writes co_extra.
            namespace = {}
            exec(
                "def victim(a, b, one):\\n"
                "    total = a - a\\n"
                "    i = total\\n"
                "    while i < b:\\n"
                "        total = total + a\\n"
                "        i = i + one\\n"
                "    return total\\n",
                namespace,
            )
            victim = namespace["victim"]

            # Liveness probe: a sentinel smuggled into the code object's
            # constants dies exactly when the code object does, so the
            # assertion cannot pass merely because nothing was deallocated.
            class Sentinel:
                pass

            sentinel = Sentinel()
            victim.__code__ = victim.__code__.replace(
                co_consts=victim.__code__.co_consts + (sentinel,))
            code_alive = weakref.ref(sentinel)
            del sentinel

            for _ in range(64):
                victim(3, 5, 1)
            # If a surface change ever stopped this from compiling, nothing
            # would write co_extra and the assertion below would pass for
            # the wrong reason.
            assert cinderjit.is_jit_compiled(victim), (
                "the probe subject did not compile, so nothing wrote "
                "co_extra and the check below would be vacuous")
            del namespace["victim"]
            del victim
            gc.collect()
            assert code_alive() is None, (
                "the victim code object outlived the probe; the check below "
                "would be vacuous")

            assert freed == [], (
                "our extra-data write enlarged co_extra into foreign slots; "
                f"freefuncs above index {index} ran: {freed}")

            # Control, through the stock path: writing the LOW foreign index
            # with CPython's own setter sizes co_extra to the interpreter-wide
            # index count, so dying also invokes the HIGH freefunc for a slot
            # nothing was ever stored in.  This is the upstream behaviour the
            # capped writer exists to avoid -- and it proves the probe above
            # can actually observe a foreign freefunc firing.
            set_extra = ctypes.pythonapi._PyCode_SetExtra
            set_extra.argtypes = (
                ctypes.py_object, ctypes.c_ssize_t, ctypes.c_void_p)
            set_extra.restype = ctypes.c_int
            namespace = {}
            exec("def control():\\n    return 1\\n", namespace)
            control = namespace["control"]
            assert set_extra(control.__code__, low, 0x1234) == 0
            del namespace["control"]
            del control
            gc.collect()
            assert freed == ["low", "high"], freed

            print("foreign extra slots stayed disarmed")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("foreign extra slots stayed disarmed", proc.stdout)

    def test_auto_threshold_alone_does_not_execute(self):
        # MR-04 acceptance: the product auto-JIT stays unavailable -- a
        # bare PYTHONJITAUTO without the explicit canary mode must leave
        # every counter at zero and provide no cinderjit module.
        env = dict(os.environ)
        env.pop("CINDERX_JIT_MODE", None)
        env["PYTHONJITAUTO"] = "5"
        probe = textwrap.dedent(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            try:
                import cinderjit
            except ImportError:
                pass
            else:
                raise SystemExit("cinderjit must not exist without canary")

            def hot(a, b, one):
                i = a - a
                while i < b:
                    i = i + one
                return i

            for j in range(64):
                hot(j, 5, 1)
            stats = _cinderx._get_trigger_stats()
            assert all(v == 0 for v in stats.values()), stats
            print("auto-alone stays gated")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_force_uncompile_does_not_manufacture_a_death(self):
        # function_destroyed_notifications is the proof that death
        # notifications are delivered at all, so administrative
        # unpublication must not move it: force_uncompile of a live
        # function leaves the counter flat, and the real death afterwards
        # moves it exactly once.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            src = (
                "def undead(a, b):\\n"
                "    total = a - a\\n"
                "    while total < b:\\n"
                "        total = total + a\\n"
                "    return total\\n"
            )
            ns = {}
            exec(src, ns)
            fn = ns.pop("undead")  # keep it out of a cycle with its globals
            assert cinderjit.force_compile(fn) is True
            assert fn(2, 6) == 6

            def deaths():
                stats = _cinderx._get_trigger_stats()
                return stats["function_destroyed_notifications"]

            before = deaths()
            assert cinderjit.force_uncompile(fn) is True
            assert cinderjit.is_jit_compiled(fn) is False
            assert deaths() == before, (before, deaths())
            assert fn(2, 6) == 6  # still callable, through the interpreter
            assert deaths() == before, (before, deaths())
            del fn
            assert deaths() == before + 1, (before, deaths())
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_collector_run_callbacks_cannot_resurrect_the_doomed(self):
        # A cyclic collection clears the weak references of the doomed and
        # runs the externally rooted callbacks BEFORE anything is
        # untracked, so a user callback in that batch can query the JIT
        # while a condemned compiled function is still GC-tracked.  The
        # listing must not hand it out -- appending it would resurrect it
        # from the garbage set -- in either interleaving of the user's
        # callback with the JIT's own death watch, which is why the user
        # weak reference is armed before the JIT's in one arm and after it
        # in the other.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def make(name):
                src = (
                    "def %s(a, b):\\n"
                    "    total = a - a\\n"
                    "    while total < b:\\n"
                    "        total = total + a\\n"
                    "    return total\\n"
                ) % name
                ns = {}
                exec(src, ns)
                return ns.pop(name)

            def deaths():
                stats = _cinderx._get_trigger_stats()
                return stats["function_destroyed_notifications"]

            escaped = []
            listings = []

            def observe(_ref):
                listing = [
                    f.__qualname__ for f in cinderjit.get_compiled_functions()
                ]
                listings.append(listing)
                escaped.extend(
                    f
                    for f in cinderjit.get_compiled_functions()
                    if f.__qualname__.startswith("doomed_")
                )

            watches = []
            before = deaths()
            for i in range(8):
                name = "doomed_%d" % i
                if (i % 2) == 0:
                    # User weak reference armed before the JIT's own watch.
                    fn = make(name)
                    watches.append(weakref.ref(fn, observe))
                    assert cinderjit.force_compile(fn) is True
                else:
                    # And armed after it.
                    fn = make(name)
                    assert cinderjit.force_compile(fn) is True
                    watches.append(weakref.ref(fn, observe))
                assert fn(2, 6) == 6
                # A cycle only the collector can take apart:
                # func -> __dict__ -> list -> func.
                fn.__cycle__ = [fn]
                del fn
                gc.collect()
            assert len(listings) == 8, listings
            for listing in listings:
                assert not any(q.startswith("doomed_") for q in listing), (
                    "a condemned function was handed out of the listing "
                    "mid-collection: %r" % (listings,)
                )
            assert not escaped, escaped
            assert deaths() == before + 8, (before, deaths())
            assert all(w() is None for w in watches)
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])

    def test_death_callback_disable_enable_cannot_resurrect_the_dying(self):
        # CPython clears every weak reference of a dying function BEFORE it
        # runs any callback (newest first), so a user callback armed after
        # the JIT's watch runs while the JIT's own callback is pending.  A
        # disable(deopt_all=True) in that window used to hit the watch's
        # corpse-replacement path: the cleared watch was erased, a fresh
        # weak reference was hung on the refcount-zero function, the parked
        # set took the raw pointer, and enable() -- its death-pending check
        # blinded by the fresh watch -- resurrected the function with an
        # INCREF from zero.  Both death paths (last DECREF and cyclic
        # collection) must survive the full disable/enable round trip.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def make(name):
                src = (
                    "def %s(a, b):\\n"
                    "    total = a - a\\n"
                    "    while total < b:\\n"
                    "        total = total + a\\n"
                    "    return total\\n"
                ) % name
                ns = {}
                exec(src, ns)
                return ns.pop(name)

            def deaths():
                stats = _cinderx._get_trigger_stats()
                return stats["function_destroyed_notifications"]

            # A healthy sibling: the disable/enable round trip inside the
            # callback must park and re-publish it without loss.
            keeper = make("keeper")
            assert cinderjit.force_compile(keeper) is True
            assert keeper(2, 6) == 6

            def run_arm(name, cyclic):
                fn = make(name)
                assert cinderjit.force_compile(fn) is True
                assert fn(2, 6) == 6
                before = deaths()
                seen = {}

                def on_death(_ref):
                    # Recording the counter proves the ordering: the JIT's
                    # callback has not delivered this death yet.
                    seen["deaths_at_callback"] = deaths()
                    cinderjit.disable(deopt_all=True)
                    seen["listed_mid"] = [
                        f.__qualname__
                        for f in cinderjit.get_compiled_functions()
                    ]
                    cinderjit.enable()
                    seen["listed_after"] = [
                        f.__qualname__
                        for f in cinderjit.get_compiled_functions()
                    ]

                # Armed after force_compile's watch, so it runs first.
                w = weakref.ref(fn, on_death)
                if cyclic:
                    fn.__cycle__ = [fn]
                    del fn
                    gc.collect()
                else:
                    del fn
                assert "deaths_at_callback" in seen, (
                    "the user callback never ran (%s)" % name
                )
                assert seen["deaths_at_callback"] == before, (
                    "the JIT callback ran before the user callback: the "
                    "death-in-flight window was not exercised (%s)" % name
                )
                assert deaths() == before + 1, (name, before, deaths())
                assert w() is None, name
                assert name not in seen["listed_mid"], seen
                assert name not in seen["listed_after"], seen

            run_arm("dying_decref", False)
            run_arm("dying_cycle", True)

            # The sibling made both round trips; fresh work still compiles.
            assert cinderjit.is_jit_compiled(keeper)
            assert keeper(2, 6) == 6
            fresh = make("fresh_after")
            assert cinderjit.force_compile(fresh) is True
            assert fresh(2, 6) == 6
            print("survived death-window round trips")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("survived death-window round trips", proc.stdout)


    # ------------------------------------------------------------------
    # MR-08: the exception surface.  A raise inside machine code exits
    # through the UnhandledException deopt and the anchored evaluator's
    # exception_unwind runs the handler; these tests hold the whole
    # observable contract of that route -- results, tracebacks, frame
    # state, tracing and eval-breaker interplay.
    #
    # Dual-arm oracles compare against the JIT-off arm of the same build
    # (evaluator installed, canary off).  That arm is this project's
    # baseline: libtest_jitoff_diff pins it to stock over the full test
    # corpus, and it shares the vendored evaluator's CALL dispatch, so
    # positions agree by construction rather than by accident of pure
    # CPython's inlined-call fast path (structurally disabled under an
    # installed PEP 523 evaluator).

    def _dual_arm(self, probe, timeout=120):
        """Run `probe` in the interp (JIT-off) and jit (canary) arms and
        return the two stdout JOURNAL payloads."""

        def run_arm(mode):
            env = _capability_clean_env()
            if mode == "jit":
                env["CINDERX_JIT_MODE"] = "canary"
                env["PYTHONJITAUTO"] = "1000000"
            proc = subprocess.run(
                [sys.executable, "-c", probe, mode],
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
            )
            self.assertEqual(proc.returncode, 0, (mode, proc.stderr[-2000:]))
            for line in proc.stdout.splitlines():
                if line.startswith("JOURNAL "):
                    return json.loads(line[len("JOURNAL "):])
            self.fail(f"{mode} arm produced no JOURNAL line: {proc.stdout!r}")

        return run_arm("interp"), run_arm("jit")

    _DUAL_ARM_PREAMBLE = textwrap.dedent(
        """
        import json
        import sys
        import _cinderx, cinderx
        cinderx.init()
        _cinderx.install_frame_evaluator()
        mode = sys.argv[1]
        if mode == "jit":
            import cinderjit

        def compile_all(*fns):
            if mode == "jit":
                for fn in fns:
                    assert cinderjit.force_compile(fn) is True, fn
                    assert cinderjit.is_jit_compiled(fn), fn

        def still_compiled(*fns):
            if mode == "jit":
                for fn in fns:
                    assert cinderjit.is_jit_compiled(fn), (
                        "an exception must not uninstall", fn)

        def tb_entries(exc, names):
            out = []
            tb = exc.__traceback__
            while tb is not None:
                code = tb.tb_frame.f_code
                if code.co_name in names:
                    out.append([code.co_name, tb.tb_lasti, tb.tb_lineno])
                tb = tb.tb_next
            return out
        """
    )

    def test_try_except_executes_and_deopts_organically(self):
        # The non-raising path stays in machine code; the raising path
        # exits through an organic UnhandledException deopt, the anchored
        # evaluator runs the handler, and the artifact survives to serve
        # the next call.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def guard(a, b):
                try:
                    return a // b
                except ZeroDivisionError:
                    return -1

            assert guard(6, 2) == 3
            assert guard(6, 0) == -1
            assert cinderjit.force_compile(guard) is True
            assert cinderjit.is_jit_compiled(guard)

            def organic():
                return _cinderx._get_trigger_stats()["organic_deopt_hits"]

            entries = _cinderx._get_trigger_stats()["machine_code_entries"]
            base = organic()
            assert guard(6, 2) == 3
            assert organic() == base, "the happy path must not deopt"

            assert guard(6, 0) == -1
            after_first = organic()
            assert after_first > base, "the raise must deopt organically"
            assert cinderjit.is_jit_compiled(guard)

            assert guard(6, 0) == -1
            assert organic() > after_first, (
                "the deopt is per-activation; the next call re-enters "
                "machine code and deopts again")
            assert _cinderx._get_trigger_stats()["machine_code_entries"] > (
                entries), "the raising calls entered machine code"
            print("exception surface holds")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("exception surface holds", proc.stdout)

    def test_exception_tracebacks_match_the_interpreter_arm(self):
        # Full (co_name, tb_lasti, tb_lineno) chains, not just lines: a
        # raising BINARY_OP carries an inline cache, a raising callee
        # exits the caller at its CALL, and a raise statement re-executes
        # in the interpreter.  All three must attribute the exception to
        # the same bytecode unit as the JIT-off arm.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def div(a, b):
                return a // b

            def raiser(x):
                raise ValueError(x)

            def outer(f, x):
                return f(x) + 1

            def boomcb(x):
                raise KeyError(x)

            compile_all(div, raiser, outer)
            names = {"div", "raiser", "outer", "boomcb"}
            journal = {}
            for label, thunk in (
                ("binary_op", lambda: div(1, 0)),
                ("raise_stmt", lambda: raiser("zz")),
                ("callee", lambda: outer(boomcb, 3)),
            ):
                try:
                    thunk()
                except Exception as exc:
                    journal[label] = [
                        type(exc).__name__,
                        tb_entries(exc, names),
                    ]
                else:
                    raise SystemExit(label + " did not raise")
            still_compiled(div, raiser, outer)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_finally_and_with_blocks_match_the_interpreter_arm(self):
        # finally runs on both exits, __exit__ sees the exception triple,
        # suppression resumes after the with block, and a non-manager
        # raises stock's TypeError out of BEFORE_WITH.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def fin(a, b, log):
                try:
                    return a // b
                finally:
                    log.append("fin")

            class CM:
                def __init__(self, log, suppress):
                    self.log = log
                    self.suppress = suppress

                def __enter__(self):
                    self.log.append("enter")
                    return self

                def __exit__(self, t, v, tb):
                    self.log.append(
                        "exit-" + ("none" if t is None else t.__name__))
                    return self.suppress

            def use(cm, log, z):
                with cm:
                    log.append("body")
                    return 10 // z
                return -1

            compile_all(fin, use)
            journal = {}

            log = []
            journal["fin_return"] = [fin(9, 3, log), log]
            log = []
            try:
                fin(9, 0, log)
            except ZeroDivisionError:
                log.append("caught")
            journal["fin_raise"] = log

            log = []
            journal["with_return"] = [use(CM(log, False), log, 2), log]
            log = []
            journal["with_suppressed"] = [use(CM(log, True), log, 0), log]
            log = []
            try:
                use(CM(log, False), log, 0)
            except ZeroDivisionError:
                log.append("caught")
            journal["with_propagates"] = log

            log = []
            try:
                use(42, log, 1)
            except TypeError as exc:
                journal["not_a_manager"] = [str(exc), log]
            still_compiled(fin, use)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_exception_chaining_matches_the_interpreter_arm(self):
        # raise-from sets __cause__ and __suppress_context__; a raise
        # inside the handler (which runs interpreted after the deopt)
        # chains __context__ implicitly.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def chained(mk_cause):
                try:
                    raise ValueError("primary") from mk_cause("root")
                except ValueError:
                    raise RuntimeError("secondary")

            compile_all(chained)
            try:
                chained(KeyError)
            except RuntimeError as exc:
                ctx = exc.__context__
                journal = [
                    type(exc).__name__,
                    str(exc),
                    exc.__cause__ is None,
                    type(ctx).__name__,
                    str(ctx),
                    ctx.__suppress_context__,
                    type(ctx.__cause__).__name__,
                ]
            else:
                raise SystemExit("chained did not raise")
            still_compiled(chained)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_assert_and_bare_reraise_match_the_interpreter_arm(self):
        # assert exercises LOAD_ASSERTION_ERROR + RAISE_VARARGS; a bare
        # raise in the handler re-raises the original exception with its
        # traceback extended, not replaced.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def check(x):
                assert x > 0, "must be positive"
                return x

            def rethrow(a, b):
                try:
                    return a // b
                except ZeroDivisionError:
                    raise

            compile_all(check, rethrow)
            names = {"check", "rethrow"}
            journal = {"ok": check(3)}
            try:
                check(0)
            except AssertionError as exc:
                journal["assert"] = [str(exc), tb_entries(exc, names)]
            try:
                rethrow(5, 0)
            except ZeroDivisionError as exc:
                journal["reraise"] = [str(exc), tb_entries(exc, names)]
            still_compiled(check, rethrow)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_handler_frame_state_matches_the_interpreter_arm(self):
        # The deopt reifies locals before the evaluator dispatches to the
        # handler: a helper called from the handler must see the same
        # f_locals, f_lasti and f_lineno in the raising frame as the
        # JIT-off arm.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def snap(n, grab):
                a = n + 1
                b = a * 2
                try:
                    return a // (n - n)
                except ZeroDivisionError:
                    return grab()

            def grab_impl():
                fr = sys._getframe(1)
                return {
                    "locals": {
                        k: v
                        for k, v in fr.f_locals.items()
                        if k in ("a", "b", "n")
                    },
                    "lasti": fr.f_lasti,
                    "lineno": fr.f_lineno,
                }

            compile_all(snap)
            journal = snap(4, grab_impl)
            still_compiled(snap)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_exception_deopt_releases_operand_stack_references(self):
        # A value sitting on the operand stack when the raise happens (the
        # pending BUILD_TUPLE element) is owned by the reified frame and
        # released by the evaluator's unwind -- not leaked, not
        # double-freed.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import gc
            import weakref
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Probe:
                pass

            def build(p, z):
                return (p, 1 // z)

            assert cinderjit.force_compile(build) is True
            p = Probe()
            wr = weakref.ref(p)
            ok = build(p, 1)
            assert ok[0] is p
            del ok
            try:
                build(p, 0)
            except ZeroDivisionError:
                pass
            else:
                raise SystemExit("build did not raise")
            assert cinderjit.is_jit_compiled(build)
            del p
            gc.collect()
            assert wr() is None, "the exception deopt leaked a stack slot"
            print("operand stack released")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("operand stack released", proc.stdout)

    def test_getframe_from_a_callee_matches_the_interpreter_arm(self):
        # A callee inspecting the running machine-code frame must see what
        # stock shows: the exact f_locals content (arguments, rebound
        # values, locals assigned so far, deletions), the call line, and
        # the frame chain.  The executing mode writes every local store
        # through to the materialized frame for exactly this reason.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            import traceback

            def peek():
                fr = sys._getframe(1)
                return {
                    "name": fr.f_code.co_name,
                    "locals": {
                        k: v
                        for k, v in fr.f_locals.items()
                        if k not in ("cb",)
                    },
                    "cb_present": "cb" in fr.f_locals,
                    "back": fr.f_back.f_code.co_name,
                    "line_delta": fr.f_lineno - fr.f_code.co_firstlineno,
                    "stack_tail": [
                        entry.name for entry in traceback.extract_stack(fr)
                    ][-2:],
                }

            def caller(n, cb):
                n = n + 1
                m = n * 2
                snap = cb()
                del m
                snap2 = cb()
                return (snap, snap2, n)

            def driver():
                return caller(7, peek)

            compile_all(caller)
            journal = driver()
            still_compiled(caller)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_current_frames_from_another_thread_sees_the_running_loop(self):
        # sys._current_frames() from a second thread must observe the
        # machine-code frame mid-flight without tearing: right code
        # object, sane f_lasti, walkable chain.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import threading
            import time
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def spin(limit, one, stop):
                i = limit - limit
                while i < limit:
                    i = i + one
                    if stop():
                        return i
                return i

            assert cinderjit.force_compile(spin) is True
            assert spin(100, 1, bool) == 100

            done = threading.Event()
            tid_box = {}
            started = threading.Event()

            def victim():
                tid_box["tid"] = threading.get_ident()
                started.set()
                spin(2_000_000_000, 1, done.is_set)

            t = threading.Thread(target=victim)
            t.start()
            started.wait(30)
            observed = []
            try:
                for _ in range(400):
                    frames = sys._current_frames()
                    fr = frames.get(tid_box["tid"])
                    while fr is not None:
                        if fr.f_code.co_name == "spin":
                            observed.append(
                                (fr.f_lasti, dict(fr.f_locals)))
                            break
                        fr = fr.f_back
                    if observed:
                        break
                    time.sleep(0.005)
            finally:
                done.set()
            t.join(60)
            assert not t.is_alive(), "the loop never honored the stop flag"
            assert observed, "the running compiled loop was never observed"
            lasti, frame_locals = observed[0]
            assert isinstance(lasti, int)
            # Observable frame state, not just survival: the sampled frame
            # must show the same locals stock would -- the arguments and
            # the loop counter written through as it advances.
            assert frame_locals.get("limit") == 2_000_000_000, frame_locals
            assert frame_locals.get("one") == 1, frame_locals
            assert callable(frame_locals.get("stop")), frame_locals
            assert isinstance(frame_locals.get("i"), int), frame_locals
            print("current_frames observed the loop")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("current_frames observed the loop", proc.stdout)

    def test_settrace_mid_flight_matches_the_interpreter_arm(self):
        # sys.settrace installed by a callee while the caller runs in
        # machine code: stock 3.11 delivers no line/return events to the
        # already-running frame (its f_trace was never populated by a
        # 'call' event), and frame.f_trace stays None.  Under the RFC
        # 3.3.4.5 exemption the JIT arm lands in exactly that behavior --
        # neither missing events stock sends, nor inventing events stock
        # does not.  (No item-7 restoration applies here: the tracer is
        # not yet installed at the moment the call site delegates.)
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            events = []
            watched = ("workload", "probe_ftrace", "activate")

            def tracer(frame, event, arg):
                if frame.f_code.co_name in watched:
                    events.append([event, frame.f_code.co_name])
                return None

            def activate():
                sys.settrace(tracer)
                return 1

            def probe_ftrace():
                return sys._getframe(1).f_trace is None

            def workload(n, activate, probe):
                acc = n - n
                acc = acc + activate()
                acc = acc + n
                acc = acc * 2
                journal.append(["caller_f_trace_none", probe()])
                return acc

            journal = []
            compile_all(workload)
            result = workload(5, activate, probe_ftrace)
            sys.settrace(None)
            journal.append(["result", result])
            journal.append(["events", events])
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_settrace_before_the_call_traces_the_whole_frame(self):
        # With the tracer active before the call, the guarded entry falls
        # back and the whole frame runs interpreted: the full stock event
        # stream -- call, lines, the exception event in the handler, and
        # return -- must be identical across arms.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            events = []

            def tracer(frame, event, arg):
                if frame.f_code.co_name == "traced_target":
                    what = event
                    if event == "exception":
                        what = "exception-" + arg[0].__name__
                    events.append(
                        [what, frame.f_lineno - frame.f_code.co_firstlineno])
                return tracer

            def traced_target(n):
                try:
                    return n // (n - n)
                except ZeroDivisionError:
                    return -n

            compile_all(traced_target)
            sys.settrace(tracer)
            result = traced_target(6)
            sys.settrace(None)
            print("JOURNAL " + json.dumps([result, events]))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_delete_fast_unbound_matches_the_interpreter_arm(self):
        # Stock DELETE_FAST raises UnboundLocalError when the slot is
        # already empty; the compiled form must not silently clear an
        # unbound local (the uninitialized-local TNullptr reaching
        # definition makes that shape compilable).  Covers the always-
        # unbound form, the conditionally-bound form on both paths, and
        # an in-frame handler catching the error through this frame's
        # exception table.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def observe(fn, *args):
                try:
                    return ["value", fn(*args)]
                except Exception as exc:
                    return [type(exc).__name__, str(exc)]

            def f():
                del x
                return 1

            def g(flag):
                if flag:
                    x = 1
                del x
                return "deleted"

            def h():
                try:
                    del x
                except UnboundLocalError:
                    return "caught"
                return "not-raised"

            compile_all(f, g, h)
            journal = [
                observe(f),
                observe(g, True),
                observe(g, False),
                observe(h),
            ]
            still_compiled(f, g, h)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_fused_store_fast_writes_through_matches_interp(self):
        # The 3.11 superinstructions execute their SETLOCALs in opcode
        # order, each immediately visible in localsplus.  The oracle must
        # dodge two masks: a call-shaped observation deopts the frame and
        # reification rewrites every slot, and a type-guard deopt (mixed
        # warmup types) does the same -- so the fused stores run in a
        # CALL-FREE hot loop and another thread samples the running frame
        # via sys._current_frames() mid-loop.  The loop bound is sized
        # per arm (the sampled values do not depend on it, and only they
        # are journaled); the probe asserts the quickened code really
        # carries a fused store so the oracle cannot pass vacuously.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            import dis
            import threading

            def fused_loop(a, b, n):
                i = 0
                x = None
                y = None
                while i < n:
                    x = a
                    y = b
                    i = i + 1
                return i, x, y

            for _ in range(40):
                fused_loop(1, 2, 5)
            ops = [i.opname for i in
                   dis.get_instructions(fused_loop, adaptive=True)]
            fused_ops = [o for o in ops if o.startswith("STORE_FAST__")]
            assert fused_ops, (
                "quickening no longer produces a fused store; the "
                "oracle would be vacuous", ops)
            compile_all(fused_loop)
            for _ in range(3):
                fused_loop(1, 2, 5)

            main_tid = threading.get_ident()
            out = {}

            def sampler():
                for _ in range(200000):
                    fr = sys._current_frames().get(main_tid)
                    if fr is not None and fr.f_code.co_name == "fused_loop":
                        fl = fr.f_locals
                        if fl.get("i", 0) > 0:
                            out["x"] = fl.get("x")
                            out["y"] = fl.get("y")
                            return

            n = (1 << 29) if mode == "jit" else 5000000
            # The sampler can miss a short window; retry the whole run a
            # few times -- the sampled VALUES are timing-independent.
            for _ in range(5):
                out.clear()
                t = threading.Thread(target=sampler)
                t.start()
                result = fused_loop(7, 8, n)
                t.join(60)
                if "x" in out:
                    break
            journal = [out.get("x"), out.get("y"), result[1], result[2]]
            still_compiled(fused_loop)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_mid_flight_tracer_protocol_matches_the_interpreter_arm(self):
        # RFC 3.3.4.5 item 7, refined: after an ordinary mid-function
        # deopt with tracing active, f_trace_lines is made explicit and a
        # user-installed f_trace is preserved -- but an ABSENT f_trace is
        # never forged from the GLOBAL tracer (tstate->c_traceobj).  The
        # local tracer is only ever installed by a 'call' event's return
        # value, and a mid-flight frame never had that event, so stock
        # delivers nothing for it.  Four tracer shapes pin the protocol:
        # the classic self-returning global, a two-level global-only-
        # accepts-call tracer, a global returning None, and an explicit
        # frame-level f_trace planted before the deopt.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            journal = []

            def scenario(name, make_tracer, plant_local=None):
                # Fresh globals per scenario so each guarded load has its
                # own dk_version story; everything else stays in function
                # scope (a new module-level name after compile would fire
                # the first guarded load before the tracer exists).
                gname = "G_" + name
                kname = "K_" + name
                globals()[gname] = 5
                armed = {"on": False}
                events = []

                class Flip:
                    def __add__(self, other):
                        if armed["on"]:
                            if plant_local is not None:
                                sys._getframe(1).f_trace = plant_local(
                                    events)
                            sys.settrace(make_tracer(events))
                            globals()[kname] = 1
                        return 100

                src = (
                    "def workload(x):\\n"
                    "    a = " + gname + "\\n"
                    "    b = x + 1\\n"
                    "    c = " + gname + "\\n"
                    "    return a + b + c\\n"
                )
                exec(src, globals())
                wl = globals().pop("workload")
                f = Flip()
                for _ in range(40):
                    assert wl(f) == 110
                compile_all(wl)
                for _ in range(3):
                    assert wl(f) == 110
                armed["on"] = True
                result = wl(f)
                # A follow-up NEW call observes the activation at entry
                # and runs interpreted under the full protocol.
                follow = wl(f)
                sys.settrace(None)
                journal.append([name, result, follow, events])

            # 1. Classic self-returning global tracer: the running frame
            # has no local tracer, so it stays silent (stock parity).
            def classic(events):
                def tracer(frame, event, arg):
                    if frame.f_code.co_name == "workload":
                        events.append(["g", event])
                    return tracer
                return tracer

            scenario("classic", classic)

            # 2. Two-level protocol: the global tracer ONLY accepts
            # 'call' and returns a distinct local tracer.  A forged
            # f_trace would hand it a 'line' event and blow the
            # assertion.
            def two_level(events):
                def local(frame, event, arg):
                    if frame.f_code.co_name == "workload":
                        events.append(["l", event])
                    return local

                def tracer(frame, event, arg):
                    assert event == "call", (
                        "global tracer must only see call events", event)
                    if frame.f_code.co_name == "workload":
                        events.append(["g", event])
                    return local
                return tracer

            scenario("twolevel", two_level)

            # 3. Global tracer returning None: no local tracing at all.
            def none_returning(events):
                def tracer(frame, event, arg):
                    if frame.f_code.co_name == "workload":
                        events.append(["g", event])
                    return None
                return tracer

            scenario("noneret", none_returning)

            # 4. An f_trace the user planted on the frame object BEFORE
            # the deopt is preserved and receives the interpreted
            # remainder's events.
            def planted_local(events):
                def local(frame, event, arg):
                    if frame.f_code.co_name == "workload":
                        events.append(["planted", event])
                    return local
                return local

            scenario("planted", classic, plant_local=planted_local)

            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)
        # The jit arm must have taken the organic deopt path in every
        # scenario -- otherwise the mid-flight half of the protocol was
        # never exercised.  The interp arm has no machine code, so the
        # assertion lives here rather than inside the probe.

    def test_ordinary_deopt_with_tracing_is_organic_on_the_jit_arm(self):
        # Companion to the protocol test: prove the jit arm actually
        # deopts mid-frame on the guarded load (the dual-arm probe cannot
        # assert jit-only counters without breaking arm symmetry).
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import sys
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            G_probe = 5
            armed = {"on": False}

            def tracer(frame, event, arg):
                return tracer

            class Flip:
                def __add__(self, other):
                    if armed["on"]:
                        sys.settrace(tracer)
                        globals()["K_probe"] = 1
                    return 100

            def workload(x):
                a = G_probe
                b = x + 1
                c = G_probe
                return a + b + c

            def main():
                f = Flip()
                for _ in range(40):
                    assert workload(f) == 110
                assert cinderjit.force_compile(workload) is True
                for _ in range(3):
                    assert workload(f) == 110
                before = _cinderx._get_trigger_stats()[
                    "organic_deopt_hits"]
                armed["on"] = True
                assert workload(f) == 110
                after = _cinderx._get_trigger_stats()[
                    "organic_deopt_hits"]
                sys.settrace(None)
                assert after > before, (
                    "the guarded load never deopted; the mid-flight "
                    "protocol was not exercised", before, after)
                print("mid-flight deopt is organic")

            main()
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("mid-flight deopt is organic", proc.stdout)
    def test_signal_handler_exception_interrupts_the_compiled_loop(self):
        # A signal arriving in a tight compiled loop is delivered at the
        # back-edge poll; the handler's exception unwinds out of the
        # machine-code frame with a traceback naming the loop.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import signal
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Boom(Exception):
                pass

            def on_alarm(signum, frame):
                raise Boom()

            def spin(limit, one):
                i = limit - limit
                while i < limit:
                    i = i + one
                return i

            assert cinderjit.force_compile(spin) is True
            assert spin(100, 1) == 100

            signal.signal(signal.SIGALRM, on_alarm)
            signal.setitimer(signal.ITIMER_REAL, 0.2)
            try:
                spin(2_000_000_000, 1)
                raise SystemExit("the signal never interrupted the loop")
            except Boom as exc:
                tb = exc.__traceback__
                names = []
                while tb is not None:
                    names.append(tb.tb_frame.f_code.co_name)
                    tb = tb.tb_next
                assert "spin" in names, names
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
                signal.signal(signal.SIGALRM, signal.SIG_DFL)
            print("signal interrupted the compiled loop")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("signal interrupted the compiled loop", proc.stdout)

    def test_handler_only_opcodes_do_not_refuse_the_function(self):
        # The execute whitelist is scoped to normal-flow-reachable
        # instructions: an opcode that occurs solely inside an
        # exception-handler region runs in the interpreter after the
        # deopt regardless of what it is, so STORE_DEREF or a subscript
        # in a handler must not refuse a function whose normal path this
        # milestone fully supports.  A normal-flow subscript still
        # refuses.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def make_counter():
                count = 0

                def bump(a, b):
                    nonlocal count
                    try:
                        return a // b
                    except ZeroDivisionError:
                        count = count + 1
                        return -count

                return bump

            def pick(d, k, z):
                try:
                    return 10 // z
                except ZeroDivisionError:
                    return d[k]

            bump = make_counter()
            compile_all(bump, pick)
            journal = [
                bump(6, 2), bump(6, 0), bump(6, 0),
                pick({"k": "v"}, "k", 2), pick({"k": "v"}, "k", 0),
            ]
            if mode == "jit":
                def normal_flow_subscr(d, k):
                    return d[k]

                try:
                    cinderjit.force_compile(normal_flow_subscr)
                except RuntimeError as exc:
                    assert "CANNOT_SPECIALIZE" in str(exc), exc
                else:
                    raise SystemExit(
                        "a normal-flow subscript compiled; the reachability "
                        "scope leaks")
            still_compiled(bump, pick)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_except_star_shapes_stay_refused(self):
        # The MR-08 audit keeps except* refused: PREP_RERAISE_STAR has no
        # reference implementation.  The refusal must be stable across
        # attempts and the function must keep its interpreted behavior.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def star(fail):
                out = "ok"
                try:
                    if fail:
                        raise ValueError("x")
                except* ValueError:
                    out = "star"
                return out

            for attempt in range(2):
                try:
                    cinderjit.force_compile(star)
                except RuntimeError as exc:
                    assert "CANNOT_SPECIALIZE" in str(exc), (attempt, exc)
                else:
                    raise SystemExit("except* was compiled")
            assert not cinderjit.is_jit_compiled(star)
            assert star(True) == "star"
            assert star(False) == "ok"
            print("except* stays refused")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("except* stays refused", proc.stdout)

    def test_exception_injection_sweep_matches_the_interpreter(self):
        # The injection sweep moves the raising checkpoint across the
        # whole composed body -- loop iterations inside a with inside a
        # try/finally -- and every swept answer must equal the answer the
        # interpreter gave before any machine code existed.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Erratic:
                def __init__(self, fail_at):
                    self.fail_at = fail_at
                    self.count = 0

                def tick(self):
                    self.count = self.count + 1
                    if self.count == self.fail_at:
                        raise RuntimeError("inj-%d" % self.fail_at)
                    return 1

            class Guard:
                def __init__(self, log):
                    self.log = log

                def __enter__(self):
                    self.log.append("enter")
                    return self

                def __exit__(self, t, v, tb):
                    self.log.append(
                        "exit-" + ("none" if t is None else t.__name__))
                    return False

            def target(e, n, log):
                acc = n - n
                try:
                    with Guard(log):
                        i = acc
                        while i < n:
                            acc = acc + e.tick()
                            i = i + 1
                except RuntimeError:
                    log.append("caught")
                    return -acc
                finally:
                    log.append("fin")
                return acc

            def run_target(fail_at, n):
                log = []
                result = target(Erratic(fail_at), n, log)
                return (result, log)

            N = 6
            sweep = list(range(1, N + 2)) + [0]
            expected = {k: run_target(k, N) for k in sweep}
            assert cinderjit.force_compile(target) is True
            base = _cinderx._get_trigger_stats()["organic_deopt_hits"]
            for k in sweep:
                got = run_target(k, N)
                assert got == expected[k], (k, got, expected[k])
                assert cinderjit.is_jit_compiled(target), k
            assert _cinderx._get_trigger_stats()["organic_deopt_hits"] > (
                base), "the injected raises never deopted"
            print("injection sweep matched")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("injection sweep matched", proc.stdout)


    # ------------------------------------------------------------------
    # MR-09: pull-validated attribute caches.  3.11 has no dict/type
    # watchers, so every cache entry proves its own validity at use time
    # (captured tp_version_tag, keys versions, module dict version).
    # These tests hold mutation visibility, object-protocol ordering,
    # reentrancy and the observability contract with the caches on --
    # and the off arm equal to the on arm.

    def test_attr_cache_off_arm_matches_the_on_arm(self):
        # -X jit-attr-caches / PYTHONJITATTRCACHES=0 is the comparison
        # arm: identical answers with the caches off, and the tallies
        # prove the off arm really ran uncached.
        base = textwrap.dedent(
            """
            import json
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class P:
                def __init__(self, x):
                    self.x = x

                def ping(self):
                    return self.x * 2

            def work(p, v):
                p.x = v
                return p.ping() + p.x

            ps = [P(i) for i in range(4)]
            for _ in range(40):
                for p in ps:
                    work(p, p.x)
            assert cinderjit.force_compile(work) is True
            out = [work(p, i) for i, p in enumerate(ps)]
            P.ping = lambda self: -self.x
            out += [work(p, i + 10) for i, p in enumerate(ps)]
            stats = cinderjit.get_attr_cache_stats()
            total = sum(sum(cls.values()) for cls in stats.values())
            print("JOURNAL " + json.dumps(
                {"out": out, "cached": total, "enabled":
                 cinderjit.is_attr_caches_enabled()}))
            """
        )

        def run_arm(env_extra):
            env = dict(os.environ)
            env["CINDERX_JIT_MODE"] = "canary"
            env["PYTHONJITAUTO"] = "1000000"
            env.update(env_extra)
            proc = subprocess.run(
                [sys.executable, "-c", base],
                capture_output=True,
                text=True,
                env=env,
                timeout=120,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
            for line in proc.stdout.splitlines():
                if line.startswith("JOURNAL "):
                    return json.loads(line[len("JOURNAL "):])
            self.fail(f"no JOURNAL line: {proc.stdout!r}")

        on = run_arm({})
        off = run_arm({"PYTHONJITATTRCACHES": "0"})
        self.assertEqual(on["out"], off["out"])
        self.assertTrue(on["enabled"])
        self.assertFalse(off["enabled"])
        self.assertGreater(on["cached"], 0)
        self.assertEqual(off["cached"], 0, "the off arm walked a cache")

    def test_class_and_descriptor_mutation_matches_the_interpreter_arm(self):
        # Every mutation must be visible on the very next call: method
        # redefinition, a data descriptor (property) appearing over a
        # plain method, its removal, and a non-data descriptor being
        # shadowed -- with the caches retiring entries by pull.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class P:
                def __init__(self, x):
                    self.x = x

                def ping(self):
                    return ("method", self.x)

            def read(p):
                return p.ping()

            def read_attr(p):
                return p.x

            ps = [P(i) for i in range(3)]
            for _ in range(40):
                for p in ps:
                    read(p)
                    read_attr(p)
            compile_all(read, read_attr)
            journal = []
            journal.append([read(p) for p in ps])

            P.ping = lambda self: ("redefined", self.x)
            journal.append([read(p) for p in ps])

            # The property returns a callable so read()'s p.ping() call
            # goes through the descriptor and then invokes its result.
            P.ping = property(
                lambda self: (lambda: ("property", self.x)))
            journal.append([read(p) for p in ps])

            del P.ping
            def late_ping(self):
                return ("late", self.x)
            P.ping = late_ping
            journal.append([read(p) for p in ps])

            # Data descriptor over the instance attribute: the property
            # must win over any instance shadow.
            ps[0].__dict__["y"] = "shadow"
            P.y = property(lambda self: "descr")
            def read_y(p):
                return p.y
            compile_all(read_y)
            journal.append([read_y(p) for p in ps])
            del P.y
            journal.append([getattr(ps[0], "y", None),
                            getattr(ps[1], "y", None)])

            still_compiled(read, read_attr, read_y)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_instance_shadow_and_dict_migration_matches_interp(self):
        # Instance dict transitions: live inline values, a shadow added
        # and deleted, vars() materializing the dict mid-stream, and new
        # attributes growing the shared keys -- reads must track stock
        # through every transition.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class P:
                def __init__(self, x):
                    self.x = x

                def ping(self):
                    return self.x

            def read(p):
                return (p.ping(), p.x)

            ps = [P(i) for i in range(4)]
            for _ in range(40):
                for p in ps:
                    read(p)
            compile_all(read)
            journal = [[read(p) for p in ps]]

            ps[0].ping = lambda: "inst"
            journal.append(read(ps[0]))
            del ps[0].ping
            journal.append(read(ps[0]))

            # vars() materializes the managed dict; the cache must follow.
            vars(ps[1])["x"] = 77
            journal.append(read(ps[1]))
            ps[1].x = 78
            journal.append(read(ps[1]))

            # Grow the instance storage attribute by attribute.
            for i in range(24):
                setattr(ps[2], "extra_%d" % i, i)
            journal.append(read(ps[2]))
            journal.append(sorted(vars(ps[2]).items())[:3])

            still_compiled(read)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_module_attr_mutation_matches_the_interpreter_arm(self):
        # Module attribute loads and method calls are version-validated
        # against the module dict; set, delete and restore must all be
        # visible immediately.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            import types

            mod = types.ModuleType("mr9_mod")
            mod.value = 1
            mod.get = lambda: "get-1"

            def read(m):
                return (m.value, m.get())

            for _ in range(40):
                read(mod)
            compile_all(read)
            journal = [read(mod)]
            mod.value = 2
            journal.append(read(mod))
            mod.get = lambda: "get-2"
            journal.append(read(mod))
            del mod.value
            try:
                read(mod)
            except AttributeError as exc:
                journal.append(str(exc))
            mod.value = 3
            journal.append(read(mod))
            still_compiled(read)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_getattr_hook_and_ordering_match_the_interpreter_arm(self):
        # __getattr__ fires only when normal lookup fails; a non-data
        # descriptor loses to an instance shadow; both must survive the
        # cached path and its refills.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class NonData:
                def __get__(self, obj, objtype=None):
                    return "descr"

            class P:
                nd = NonData()

                def __init__(self, x):
                    self.x = x

                def __getattr__(self, name):
                    return ("getattr", name)

            def read_missing(p):
                return p.absent

            def read_nd(p):
                return p.nd

            ps = [P(i) for i in range(3)]
            for _ in range(40):
                for p in ps:
                    read_missing(p)
                    read_nd(p)
            compile_all(read_missing, read_nd)
            journal = [[read_missing(p) for p in ps],
                       [read_nd(p) for p in ps]]

            ps[0].absent = "present-now"
            journal.append(read_missing(ps[0]))
            del ps[0].absent
            journal.append(read_missing(ps[0]))

            ps[1].nd = "shadowed"
            journal.append([read_nd(p) for p in ps])

            still_compiled(read_missing, read_nd)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_reentrant_mutation_during_cached_access_is_safe(self):
        # A __del__ fired by a cached store, mutating the class mid-store,
        # must neither crash nor leave a stale entry serving the old
        # layout.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class P:
                def __init__(self):
                    self.slot = None

                def ping(self):
                    return "orig"

            class Bomb:
                def __del__(self):
                    P.ping = lambda self: "mutated-by-del"

            def store(p, v):
                p.slot = v
                return p.ping()

            ps = [P() for _ in range(4)]
            for _ in range(40):
                for p in ps:
                    store(p, object())
            assert cinderjit.force_compile(store) is True

            for p in ps:
                p.slot = Bomb()
            results = [store(p, None) for p in ps]
            assert results[0] in ("orig", "mutated-by-del"), results
            assert results[-1] == "mutated-by-del", results
            assert store(ps[0], 1) == "mutated-by-del"
            assert cinderjit.is_jit_compiled(store)
            print("reentrant mutation safe")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("reentrant mutation safe", proc.stdout)

    def test_descriptor_type_demotion_matches_the_interpreter_arm(self):
        # Deleting D.__set__ mutates the DESCRIPTOR's type, not the
        # receiver, so the receiver-version guard alone cannot see it:
        # the cache must pull-validate the descriptor type as well.
        # After demotion an instance shadow wins (non-data precedence)
        # and stores land in the instance dict instead of the vanished
        # tp_descr_set; a get-side deletion must likewise never reach a
        # NULL tp_descr_get.  Re-adding __set__ promotes it back.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class D:
                def __get__(self, obj, objtype=None):
                    return ("data", obj.tag)

                def __set__(self, obj, value):
                    obj.__dict__["log"] = ("descr-set", value)

            class C:
                x = D()

                def __init__(self, tag):
                    self.tag = tag

            def read(c):
                return c.x

            def write(c, v):
                c.x = v

            cs = [C(i) for i in range(3)]
            for c in cs:
                # Plant instance shadows while the descriptor is data:
                # they stay invisible until the demotion flips precedence.
                c.__dict__["x"] = ("shadow", c.tag)
            for _ in range(40):
                for c in cs:
                    read(c)
                    write(c, 1)
            compile_all(read, write)

            journal = []
            journal.append([read(c) for c in cs])
            journal.append([c.__dict__.get("log") for c in cs])

            del D.__set__
            # Demoted: the planted shadows win the very next load, and
            # tp_descr_set is NULL -- a stale kDataDescr dispatch would
            # call it.
            journal.append([read(c) for c in cs])
            for c in cs:
                write(c, 9)
            journal.append([c.__dict__["x"] for c in cs])
            journal.append([read(c) for c in cs])

            D.__set__ = lambda self, obj, value: obj.__dict__.__setitem__(
                "log", ("re-promoted-set", value))
            # Promoted back to a data descriptor: it wins over the
            # instance entries again, for loads and stores alike.
            journal.append([read(c) for c in cs])
            for c in cs:
                write(c, 11)
            journal.append([c.__dict__.get("log") for c in cs])

            class G:
                def __get__(self, obj, objtype=None):
                    return "get-side"

                def __set__(self, obj, value):
                    obj.__dict__["glog"] = value

            class H:
                y = G()

            def read_y(h):
                return h.y

            hs = [H() for _ in range(3)]
            for _ in range(40):
                for h in hs:
                    read_y(h)
            compile_all(read_y)
            journal.append([read_y(h) for h in hs])
            del G.__get__
            # tp_descr_get is NULL now; the descriptor object itself is
            # returned by the full lookup (set-only descriptors have no
            # instance shadow here).
            journal.append(
                [type(read_y(h)).__name__ for h in hs])

            still_compiled(read, write, read_y)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_raising_dict_key_eq_propagates_from_cached_loads(self):
        # PyDict_GetItem swallows exceptions; stock's GenericGetAttr uses
        # PyDict_GetItemWithError and PROPAGATES a raising __eq__ from a
        # hash-colliding non-unicode instance-dict key.  Every cached
        # load path must do the same instead of folding the error into
        # "attribute absent" and running descriptor/__getattr__ logic.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class K:
                def __init__(self, target):
                    self.target = target

                def __hash__(self):
                    return hash(self.target)

                def __eq__(self, other):
                    raise RuntimeError("eq boom")

            journal = []

            def observe(fn, *args):
                try:
                    return ["value", fn(*args)]
                except Exception as exc:
                    return [type(exc).__name__, str(exc)]

            # Managed receiver, non-data descriptor: the shadow peek runs
            # the dict lookup that trips K.__eq__.
            class P:
                def x(self):
                    return "method"

            def read(p):
                return p.x

            ps = [P() for _ in range(3)]
            for _ in range(40):
                for p in ps:
                    read(p)
            compile_all(read)
            for _ in range(3):
                # Post-compile calls FILL the per-site cache and then hit
                # it; without these the bomb below only meets the cold
                # slow path (PyObject_GetAttr), which was never broken.
                for p in ps:
                    read(p)
            bomb = P()
            bomb.__dict__[K("x")] = 1
            journal.append(observe(read, bomb))

            # Non-managed receiver (tp_dictoffset): the combined-dict
            # cached load runs the same lookup.
            class E(Exception):
                pass

            def read_attr(e):
                return e.attr

            es = []
            for _ in range(3):
                e = E()
                e.attr = "v"
                es.append(e)
            for _ in range(40):
                for e in es:
                    read_attr(e)
            compile_all(read_attr)
            for _ in range(3):
                for e in es:
                    read_attr(e)
            eb = E()
            eb.__dict__[K("attr")] = 1
            journal.append(observe(read_attr, eb))

            # The caches must still serve clean receivers afterwards.
            journal.append([read(p) is P.x.__get__(p, P) or "bound"
                            for p in ps][:1])
            journal.append([read_attr(e) for e in es])
            still_compiled(read, read_attr)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_dict_swapping_eq_keeps_the_found_value_alive(self):
        # A hash-colliding key whose __eq__ replaces obj.__dict__ leaves
        # the OLD dict alive only through the lookup's temporary
        # reference.  Stock strengthens the found value BEFORE dropping
        # the dict; the cached peek must use the same ordering or the
        # returned pointer dangles (the ASAN leg turns a regression here
        # into a hard failure).
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class Swapper:
                def __init__(self, target, holder):
                    self.target = target
                    self.holder = holder

                def __hash__(self):
                    return hash(self.target)

                def __eq__(self, other):
                    self.holder["obj"].__dict__ = {}
                    return other == self.target

            class P:
                def x(self):
                    return "method"

            def read(p):
                return p.x

            ps = [P() for _ in range(3)]
            for _ in range(40):
                for p in ps:
                    read(p)
            compile_all(read)
            for _ in range(3):
                # Fill the per-site cache post-compile so the swapper
                # exercises the cached hit path, not the cold slow path.
                for p in ps:
                    read(p)

            class Canary:
                deleted = False

                def __del__(self):
                    Canary.deleted = True

            holder = {}
            victim = P()
            holder["obj"] = victim
            # The dict holds the ONLY reference to the canary: after the
            # swap, the old dict lives solely through the lookup's
            # temporary, and dropping it before strengthening the found
            # value destroys the canary mid-lookup.  Stock's ordering
            # keeps it alive.
            victim.__dict__[Swapper("x", holder)] = Canary()
            got = read(victim)
            journal = [
                type(got).__name__,
                Canary.deleted,
                victim.__dict__ == {},
            ]
            del got
            journal.append(Canary.deleted)
            journal.append([callable(read(p)) for p in ps])
            still_compiled(read)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_descriptor_class_swap_matches_the_interpreter_arm(self):
        # d.__class__ = D2 swaps the descriptor's type without touching
        # the receiver OR D1's version: only a pointer guard on
        # Py_TYPE(descr) can see it.  After the swap to a get-only type
        # the planted instance shadows win loads and stores land in the
        # instance dict (a stale kDataDescr dispatch would call D2's NULL
        # tp_descr_set); swapping to another data-descriptor type must
        # re-route through its slots.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class D1:
                def __get__(self, obj, objtype=None):
                    return ("d1-get", obj.tag)

                def __set__(self, obj, value):
                    obj.__dict__["log"] = ("d1-set", value)

            class D2:
                def __get__(self, obj, objtype=None):
                    return ("d2-get", obj.tag)

            class D3:
                def __get__(self, obj, objtype=None):
                    return ("d3-get", obj.tag)

                def __set__(self, obj, value):
                    obj.__dict__["log"] = ("d3-set", value)

            class C:
                x = D1()

                def __init__(self, tag):
                    self.tag = tag

            def read(c):
                return c.x

            def write(c, v):
                c.x = v

            cs = [C(i) for i in range(3)]
            for c in cs:
                c.__dict__["x"] = ("shadow", c.tag)
            for _ in range(40):
                for c in cs:
                    read(c)
                    write(c, 1)
            compile_all(read, write)
            for _ in range(3):
                for c in cs:
                    read(c)
                    write(c, 2)

            journal = []
            journal.append([read(c) for c in cs])

            d = C.__dict__["x"]
            d.__class__ = D2
            # Demoted by instance mutation alone: shadows win, stores go
            # to the instance dict.
            journal.append([read(c) for c in cs])
            for c in cs:
                write(c, 9)
            journal.append([c.__dict__["x"] for c in cs])
            journal.append([read(c) for c in cs])

            d.__class__ = D3
            # Data descriptor again, through the NEW type's slots.
            journal.append([read(c) for c in cs])
            for c in cs:
                write(c, 11)
            journal.append([c.__dict__.get("log") for c in cs])

            still_compiled(read, write)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_attribute_error_from_eq_follows_the_hook_protocol(self):
        # Stock's slot_tp_getattr_hook runs the generic lookup
        # UNSUPPRESSED and clears any escaping AttributeError into
        # __getattr__ -- whether the colliding key's __eq__ raised on an
        # absent OR a present name; on receivers WITHOUT __getattr__ it
        # propagates like any error.  The cached paths must reproduce
        # all three outcomes.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class KAttr:
                def __init__(self, target):
                    self.target = target

                def __hash__(self):
                    return hash(self.target)

                def __eq__(self, other):
                    raise AttributeError("eq attr boom")

            journal = []

            def observe(fn, *args):
                try:
                    return ["value", fn(*args)]
                except Exception as exc:
                    return [type(exc).__name__, str(exc)]

            class H:
                def __getattr__(self, name):
                    return ["hooked", name]

                def x(self):
                    return "method"

            def read_missing(h):
                return h.missing

            def read_x(h):
                return h.x

            hs = [H() for _ in range(3)]
            for _ in range(40):
                for h in hs:
                    read_missing(h)
                    read_x(h)
            compile_all(read_missing, read_x)
            if mode == "jit":
                stats_before = cinderjit.get_attr_cache_stats()["load_attr"]
            for _ in range(3):
                for h in hs:
                    read_missing(h)
                    read_x(h)
            if mode == "jit":
                # The hook cache family must actually serve these calls:
                # a silently-disabled family (object_getattribute once
                # went uninitialized on 3.11 and every hook fill bailed)
                # would leave this test green-but-vacuous on the stock
                # slow path.
                stats_after = cinderjit.get_attr_cache_stats()["load_attr"]
                assert stats_after["fills"] > stats_before["fills"], (
                    "hook-receiver loads never filled the cache",
                    stats_before, stats_after)
                assert stats_after["hits"] > stats_before["hits"], (
                    "hook-receiver loads never hit the cache",
                    stats_before, stats_after)

            hb = H()
            hb.__dict__[KAttr("missing")] = 1
            # Absent name: suppressed and routed to __getattr__.
            journal.append(observe(read_missing, hb))

            hb2 = H()
            hb2.__dict__[KAttr("x")] = 1
            # Present name: 3.11's hook runs the generic part
            # unsuppressed and clears ANY escaping AttributeError into
            # __getattr__ -- the probe's error wins over the descriptor.
            got = observe(read_x, hb2)
            journal.append([got[0], callable(got[1])])

            class E(Exception):
                pass

            def read_attr(e):
                return e.attr

            es = []
            for _ in range(3):
                e = E()
                e.attr = "v"
                es.append(e)
            for _ in range(40):
                for e in es:
                    read_attr(e)
            compile_all(read_attr)
            for _ in range(3):
                for e in es:
                    read_attr(e)
            eb = E()
            eb.__dict__[KAttr("attr")] = 1
            # No hook: the AttributeError propagates.
            journal.append(observe(read_attr, eb))

            still_compiled(read_missing, read_x, read_attr)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_hook_unpublished_during_lookup_uses_the_pinned_hook(self):
        # Stock INCREFs the __getattr__ it will use BEFORE the generic
        # part runs: a colliding key's __eq__ that deletes P.__getattr__
        # mid-probe neither changes which hook answers THIS call nor
        # leaves the cached borrowed pointer dangling (the deleted
        # function's only other reference was the class dict).
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            class P:
                pass

            def old_hook(self, name):
                return ["old-hook", name]

            P.__getattr__ = old_hook

            class Killer:
                def __init__(self, target):
                    self.target = target

                def __hash__(self):
                    return hash(self.target)

                def __eq__(self, other):
                    del P.__getattr__
                    return False

            def read_missing(p):
                return p.missing

            ps = [P() for _ in range(3)]
            for _ in range(40):
                for p in ps:
                    read_missing(p)
            compile_all(read_missing)
            for _ in range(3):
                for p in ps:
                    read_missing(p)

            del old_hook
            pb = P()
            pb.__dict__[Killer("missing")] = 1
            result = read_missing(pb)
            journal = [result]
            # The hook is gone for FUTURE calls on both arms.
            try:
                read_missing(P())
                journal.append("no-raise")
            except AttributeError:
                journal.append("AttributeError")
            still_compiled(read_missing)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_audited_member_read_raises_the_event_on_every_access(self):
        # Stock member_get() raises the object.__getattr__ audit event
        # for PY_AUDIT_READ members (traceback.tb_frame) BEFORE
        # PyMember_GetOne; a cached hit would silently drop it.  Audited
        # member reads are therefore fail-closed -- never filled, every
        # access runs the stock path -- while a plain member on the SAME
        # receiver (tb_lasti) still fills and hits.
        probe = self._DUAL_ARM_PREAMBLE + textwrap.dedent(
            """
            def read_frame(tb):
                return tb.tb_frame

            def read_lasti(tb):
                return tb.tb_lasti

            def make_tb():
                try:
                    raise ValueError("boom")
                except ValueError as exc:
                    return exc.__traceback__

            tbs = [make_tb() for _ in range(3)]
            for _ in range(40):
                for tb in tbs:
                    read_frame(tb)
                    read_lasti(tb)
            compile_all(read_frame, read_lasti)

            if mode == "jit":
                b = cinderjit.get_attr_cache_stats()["load_attr"]
            for _ in range(3):
                for tb in tbs:
                    read_lasti(tb)
            if mode == "jit":
                # The un-audited member on the same receiver type caches:
                # the audit gate must be narrow, not a blanket member ban.
                a = cinderjit.get_attr_cache_stats()["load_attr"]
                assert a["hits"] > b["hits"], ("tb_lasti never cached", b, a)

                # The audited member never fills or hits.
                b2 = cinderjit.get_attr_cache_stats()["load_attr"]
                for _ in range(4):
                    read_frame(tbs[0])
                a2 = cinderjit.get_attr_cache_stats()["load_attr"]
                assert a2["fills"] == b2["fills"], ("tb_frame filled", b2, a2)
                assert a2["hits"] == b2["hits"], ("tb_frame hit", b2, a2)

            events = []

            def hook(event, args):
                if event == "object.__getattr__" and args[1] == "tb_frame":
                    events.append(args[1])

            sys.addaudithook(hook)
            results = [read_frame(tbs[0]) is tbs[0].tb_frame
                       for _ in range(4)]
            # Each compiled read fires one event; the `is` comparison's
            # own tb_frame read fires another -- eight total, every
            # access audited, none swallowed by a cache hit.
            journal = [len(events), results]
            still_compiled(read_frame, read_lasti)
            print("JOURNAL " + json.dumps(journal))
            """
        )
        interp, jit = self._dual_arm(probe)
        self.assertEqual(interp, jit)

    def test_type_receiver_method_stays_on_the_generic_path(self):
        # Type-receiver loads must stay correct under a metaclass
        # takeover.  The type caches are pull-validated for exactly this
        # (owner version + metaclass identity/version), and on 3.11 no IR
        # shape reaches them anyway -- guarded global/builtin loads carry
        # no object spec, so type(obj) never yields a static TType.  Both
        # facts are asserted: the metaclass descriptor wins on the very
        # next call, and the type-cache counters stay at zero.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class Meta(type):
                pass

            class T(metaclass=Meta):
                @classmethod
                def cm(cls):
                    return "class-cm"

            def call_tm(o):
                return type(o).cm()

            def main():
                t = T()
                for _ in range(40):
                    assert call_tm(t) == "class-cm"
                assert cinderjit.force_compile(call_tm) is True
                for _ in range(3):
                    assert call_tm(t) == "class-cm"

                # A metaclass data descriptor takes precedence over the
                # class-dict classmethod; the receiver class version does
                # not change, so only the generic path answers this
                # correctly.
                Meta.cm = property(
                    lambda cls: (lambda: "meta-descr"))
                assert call_tm(t) == "meta-descr", call_tm(t)

                stats = cinderjit.get_attr_cache_stats()["load_type_method"]
                assert stats["fills"] == 0 and stats["hits"] == 0, (
                    "the 3.11 fail-closed gate leaked a type-method "
                    "cache: %r" % (stats,))
                assert cinderjit.is_jit_compiled(call_tm)
                print("type receiver stayed generic")

            main()
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("type receiver stayed generic", proc.stdout)

    def test_mutation_hammer_stays_correct_over_100_rounds(self):
        # 100 rounds of class mutation, instance shadowing and deletion
        # interleaved with compiled access: values stay right, the
        # artifact stays installed, and the pull checks keep retiring
        # entries.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        env["PYTHONMALLOC"] = "debug"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class P:
                def __init__(self, x):
                    self.x = x

                def ping(self):
                    return ("gen", 0, self.x)

            def read(p):
                return p.ping()

            def write(p, v):
                p.x = v

            ps = [P(i) for i in range(4)]
            for _ in range(40):
                for p in ps:
                    read(p)
                    write(p, p.x)
            assert cinderjit.force_compile(read) is True
            assert cinderjit.force_compile(write) is True

            before = cinderjit.get_attr_cache_stats()
            for round_no in range(100):
                def make(round_no=round_no):
                    def ping(self):
                        return ("gen", round_no, self.x)
                    return ping
                P.ping = make()
                for i, p in enumerate(ps):
                    write(p, round_no * 10 + i)
                    assert read(p) == ("gen", round_no, round_no * 10 + i)
                shadowed = ps[round_no % 4]
                shadowed.ping = lambda r=round_no: ("shadow", r)
                assert read(shadowed) == ("shadow", round_no)
                del shadowed.ping
                assert read(shadowed)[0] == "gen"
            after = cinderjit.get_attr_cache_stats()
            assert cinderjit.is_jit_compiled(read)
            assert cinderjit.is_jit_compiled(write)
            inv = sum(
                after[c]["invalidations"] - before[c]["invalidations"]
                for c in after)
            assert inv >= 100, ("mutations did not retire entries", inv)
            print("mutation hammer survived")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("mutation hammer survived", proc.stdout)

    def test_attr_cache_stats_cover_every_cache_class(self):
        # The acceptance requires per-class fill/hit/miss/invalidation
        # reporting; drive one workload through each class and require
        # fills and hits to move where they must.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import math
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            class P:
                def __init__(self, x):
                    self.x = x

                def ping(self):
                    return self.x

                @staticmethod
                def make():
                    return "made"

            # Cold compiles keep LOAD_ATTR unspecialized, so the loads go
            # through the helper caches rather than the quickened guarded
            # fast path.
            ns = {"P": P, "math": math}
            exec(
                "def load(p):\\n"
                "    return p.x\\n"
                "def store(p, v):\\n"
                "    p.x = v\\n"
                "def method(p):\\n"
                "    return p.ping()\\n"
                "def type_method():\\n"
                "    return P.make()\\n"
                "def module_attr():\\n"
                "    return math.pi\\n"
                # A global receiver compiles as LOAD_ATTR + CALL (served
                # by the module *attr* cache); the module *method* cache
                # needs a LOAD_METHOD site, which an argument receiver
                # quickened to LOAD_METHOD_MODULE provides.
                "def module_method(m, v):\\n"
                "    return m.sqrt(v)\\n",
                ns,
            )
            fns = {k: ns[k] for k in (
                "load", "store", "method", "type_method",
                "module_attr", "module_method")}
            # The module caches need the receiver proven to be a module,
            # which the quickened LOAD_ATTR_MODULE / LOAD_METHOD_MODULE
            # guard provides -- warm only those two; the instance-facing
            # functions stay cold so their unspecialized loads drive the
            # helper caches.
            for _ in range(40):
                fns["module_attr"]()
                fns["module_method"](math, 16.0)
            for fn in fns.values():
                assert cinderjit.force_compile(fn) is True

            p = P(4)
            before = cinderjit.get_attr_cache_stats()
            for _ in range(32):
                assert fns["load"](p) == p.x
                fns["store"](p, 4)
                assert fns["method"](p) == 4
                assert fns["type_method"]() == "made"
                assert fns["module_attr"]() == math.pi
                assert fns["module_method"](math, 16.0) == 4.0
            after = cinderjit.get_attr_cache_stats()

            expectations = {
                "load_attr": True,
                "store_attr": True,
                "load_method": True,
                # The type-receiver caches ARE implemented and
                # pull-validated (owner version + metaclass identity and
                # version; see AttrCache311Test), but no 3.11 IR shape
                # hands them a statically-typed receiver: guarded global
                # and builtin loads carry no object spec, so the
                # type(obj) / isinstance / len specializations that would
                # produce a TType never fire.  Zero traffic is therefore
                # the honest expectation here, and it is a reachability
                # fact, not a correctness gate.
                "load_type_method": False,
                "load_type_attr": False,
                "load_module_attr": True,
                "load_module_method": True,
            }
            for cls, must_move in expectations.items():
                delta_hits = after[cls]["hits"] - before[cls]["hits"]
                delta_fills = after[cls]["fills"] - before[cls]["fills"]
                if must_move:
                    assert delta_hits > 0 or delta_fills > 0, (
                        cls, before[cls], after[cls])
                else:
                    assert sum(after[cls].values()) == 0, (cls, after[cls])
            # The type-receiver fixture still ran -- through the generic
            # method path, counted as misses.
            assert after["load_method"]["misses"] > (
                before["load_method"]["misses"])
            print("stats cover every class")
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", probe],
            capture_output=True,
            text=True,
            env=env,
            timeout=120,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        self.assertIn("stats cover every class", proc.stdout)


if __name__ == "__main__":
    unittest.main()
