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

CHILD = textwrap.dedent(
    """
    import json
    import _cinderx, cinderx
    cinderx.init()
    _cinderx.install_frame_evaluator()
    import cinderjit

    assert cinderjit.is_attr_caches_enabled() is False, (
        "3.11 attribute caches must default off until MR-09")

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

            def uses_call(a):
                return len(str(a))

            try:
                cinderjit.force_compile(uses_call)
            except RuntimeError as exc:
                assert "CANNOT_SPECIALIZE" in str(exc), exc
            else:
                raise SystemExit("execute surface failed to refuse CALL")
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

    def test_execute_surface_refuses_unaudited_argument_shapes(self):
        # Argument binding beyond exact positional arguments -- keyword-only
        # parameters, defaults, and the variadic collectors -- carries error
        # and fallback contracts that MR-06 owns.  A body made only of
        # whitelisted opcodes must not be enough to let those signatures
        # execute machine code.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def kwonly(*, x):
                return x
            def varargs(*args):
                return args
            def varkw(**kwargs):
                return kwargs
            def mixed(a, *, b=2):
                return a
            def defaulted(a, b=3):
                return a

            for fn in (kwonly, varargs, varkw, mixed, defaulted):
                try:
                    cinderjit.force_compile(fn)
                except RuntimeError as exc:
                    assert "CANNOT_SPECIALIZE" in str(exc), (fn, exc)
                else:
                    raise SystemExit(
                        f"execute surface accepted {fn.__name__}")
                assert not cinderjit.is_jit_compiled(fn), fn

            # The same body with plain positional parameters still compiles,
            # so the refusal is about the signature and not the body.
            def positional(a, b, one):
                return a
            assert cinderjit.force_compile(positional) is True
            assert cinderjit.is_jit_compiled(positional)
            print("argument shapes refused")
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
        self.assertIn("argument shapes refused", proc.stdout)

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

    def test_specialized_bytecode_does_not_become_a_speculative_guard(self):
        # Warm compilation reads the interpreter's quickened forms.  Turning
        # those into type guards is MR-07 work, so canary compiles the
        # unspecialized forms: a warm function must keep answering
        # correctly when the argument type changes, with no deopt.
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
            cinderjit.get_and_clear_runtime_stats()
            assert arith(3, 5, 1) == 15
            assert arith(3.0, 5.0, 1.0) == 15.0
            assert cinderjit.is_jit_compiled(arith)
            deopts = cinderjit.get_and_clear_runtime_stats().get("deopt", [])
            assert not deopts, deopts
            print("no speculative guard on the execute surface")
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
        self.assertIn("no speculative guard", proc.stdout)

    def test_canary_control_plane_is_restricted(self):
        # The full cinderjit method table is a control surface for
        # capabilities MR-04 does not have: the batch and lazy paths install
        # machine code without passing the execute surface, force_uncompile
        # belongs to MR-05, and the specialization and guard setters can
        # re-open exactly the speculation this milestone excludes.
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
                "force_uncompile", "enable_specialized_opcodes",
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
                "disable", "enable", "_get_resident_compiled_functions",
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

    def test_entry_rechecks_code_identity_and_call_form(self):
        # 3.11 has no function watchers, so nothing reports a __code__ or
        # defaults change after compilation, and the compiled body assumes
        # the exact positional call form the surface accepted.  The entry
        # re-checks both on every call and hands anything else back to the
        # interpreter.
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

            # Keyword form: the compiled body never bound keywords.
            before = entries()
            assert target(a=3, b=5, one=1) == 15
            assert entries() - before == 0, "keyword call must not run compiled"

            # Defaults appearing after compilation.
            before = entries()
            target.__defaults__ = (1,)
            assert target(3, 5) == 15
            assert entries() - before == 0, "defaults must not run compiled"
            target.__defaults__ = None

            # A different code object behind the same function.
            def replacement(a, b, one):
                return "replaced"

            target.__code__ = replacement.__code__
            before = entries()
            assert target(3, 5, 1) == "replaced", "stale machine code ran"
            assert entries() - before == 0
            print("entry rechecked identity and call form")
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
        self.assertIn("entry rechecked identity", proc.stdout)

    def test_artifacts_carry_no_speculative_guard(self):
        # Guards do not only come from quickened opcodes: Simplify installs
        # its own for `x ** 2`, for compact-long comparisons and for float
        # division, each with a deopt behind it.  The executing mode
        # therefore compiles without that pass, and the optimized artifact
        # is scanned so anything that still carries a guard is refused
        # rather than shipped.  What this test holds is the observable
        # consequence: shapes that would have been guarded now execute for
        # any argument type, with no deopt at all.
        env = dict(os.environ)
        env["CINDERX_JIT_MODE"] = "canary"
        env["PYTHONJITAUTO"] = "1000000"
        probe = textwrap.dedent(
            """
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def square(x):
                return x ** 2

            def loop(a, b, one):
                total = a - a
                i = total
                while i < b:
                    total = total + a
                    i = i + one
                return total

            assert cinderjit.force_compile(square) is True
            assert cinderjit.force_compile(loop) is True
            assert cinderjit.is_jit_compiled(square)
            assert cinderjit.is_jit_compiled(loop)

            def entries():
                return _cinderx._get_trigger_stats()["machine_code_entries"]

            cinderjit.get_and_clear_runtime_stats()
            before = entries()
            # Both argument types, on both shapes: a speculative guard would
            # deopt on whichever type it did not bake in.
            assert square(3.0) == 9.0
            assert square(3) == 9
            assert loop(3, 5, 1) == 15
            assert loop(3.0, 5.0, 1.0) == 15.0
            assert entries() - before == 4, entries() - before
            deopts = cinderjit.get_and_clear_runtime_stats().get("deopt", [])
            assert not deopts, deopts
            print("artifacts carry no speculative guard")
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
        self.assertIn("artifacts carry no speculative guard", proc.stdout)

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
        # be the same question.  Growing defaults after compilation sends
        # every call to the interpreter, so reporting the function as
        # compiled -- and counting it as installed -- would describe a state
        # the runtime is not in.
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
                assert delta == 0, delta
                assert not compiled, "reported compiled but ran interpreted"
                assert not listed, "listed as installed but ran interpreted"
                restore()
                # Restoring the state restores the answer, both ways.
                before = entries()
                assert hot(3, 5, 1) == 15
                assert entries() - before == 1
                assert cinderjit.is_jit_compiled(hot)
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

    def test_pinned_artifact_keeps_its_function_alive(self):
        # The artifact is an ordinary object in the function's __dict__, so
        # Python can hold it on its own.  With borrowed references the
        # function could then die while the registry still pointed at it,
        # and reading the registry dereferenced freed memory.  On 3.11 the
        # artifact owns its function instead; the cycle is collectable, so
        # dropping the pin still releases both.
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
            victim = namespace["victim"]
            del namespace
            assert cinderjit.force_compile(victim) is True
            assert victim(3, 5, 1) == 15

            pin = victim.__dict__["__cinderx_compiled_func__"]
            alive = weakref.ref(victim)
            del victim
            gc.collect()
            # Churn the allocator so a freed function object would be reused.
            junk = [bytearray(400) for _ in range(5000)]

            assert alive() is not None, "the artifact did not keep it alive"
            listed = cinderjit.get_compiled_functions()
            assert len(listed) == 1, listed
            assert listed[0].__qualname__ == "victim"
            assert listed[0](3, 5, 1) == 15
            del listed, junk

            del pin
            gc.collect()
            assert cinderjit.get_compiled_functions() == []
            assert alive() is None, "the cycle did not collect"
            print("pinned artifact kept its function alive")
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
        self.assertIn("pinned artifact kept its function alive", proc.stdout)

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
            env = dict(os.environ)
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

    def test_instrumentation_support_config_is_refused_by_canary(self):
        # The flag patches sys.setprofile/settrace/monitoring to pause the
        # whole JIT while instrumentation is active.  This mode delivers
        # tracing correctness through the bytecode-boundary polls and the
        # guarded entry instead, and the toggle's disable arm has no
        # audited 3.11 story -- so the canary refuses the configuration by
        # name rather than running an unaudited pause-the-world path
        # alongside the audited polling one.
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

    def test_profile_enabled_inside_binary_op_reaches_the_running_frame(self):
        # The entry check answers "is instrumentation active NOW"; the body
        # can change the answer.  BINARY_OP on the execute surface runs
        # arbitrary protocol code, and stock 3.11 reacts to a mid-frame
        # sys.setprofile immediately: the frame that is currently executing
        # delivers its remaining events, above all PyTrace_RETURN on exit.
        # A compiled frame must therefore leave machine code at the first
        # safe point after the transition and hand the rest of the frame to
        # the interpreter -- running natively to the end swallows the
        # return event and is an observable differential.  No threads and
        # no timing: the transition happens synchronously inside __add__.
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
            assert _cinderx._get_trigger_stats()["machine_code_entries"] \\
                - before == 1, "the probe call must start in machine code"
            sys.setprofile(None)
            assert result.n == 20, result.n

            returned = [e for e in events if e == ("hot", "return")]
            assert returned, (
                "sys.setprofile() from inside the compiled loop's BINARY_OP"
                " never produced PyTrace_RETURN for the running frame: %r"
                % (events,)
            )
            print("mid-frame profile reached the running frame")
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
        self.assertIn("mid-frame profile reached the running frame", proc.stdout)

    def test_profile_enabled_by_pending_call_reaches_the_running_frame(self):
        # The same transition through the other door: Py_AddPendingCall()
        # runs its callback on this thread at the compiled loop's back
        # edge -- the eval-breaker service this port wired to the vendored
        # eval_frame_handle_pending() -- and the callback enables
        # profiling via the C API surface (sys.setprofile is the same
        # call).  Stock updates the running frame's tracing state at that
        # instant and delivers PyTrace_RETURN when the frame exits; the
        # compiled frame must not run to completion unaware.
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
            sys.setprofile(None)
            assert result == 60_000_000, result
            assert _cinderx._get_trigger_stats()["machine_code_entries"] \\
                - before == 1, (
                "the pending call landed before the loop entered machine"
                " code; the mid-frame transition was not exercised"
            )

            returned = [e for e in events if e == ("hot", "return")]
            assert returned, (
                "the pending call enabled profiling on the running thread"
                " and the compiled frame still returned without"
                " PyTrace_RETURN: %r" % (events[:10],)
            )
            print("pending-call profile reached the running frame")
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
            "pending-call profile reached the running frame", proc.stdout)

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
        self.assertIn("transition reached the running frame", proc.stdout)

    def test_profile_enabled_inside_for_iter_next_reaches_the_frame(self):
        # The transition consumed by a terminator: FOR_ITER lowers to an
        # iterator-next whose consumer is the branch that ends the block,
        # so there is no later point in the SAME block to poll -- the
        # first resumable boundary after __next__ runs is the successor's
        # entry.  A poll keyed to "reentrant instruction, then a snapshot
        # in this block" misses it, and the frame returns natively with
        # the profiler never hearing from it.
        self._run_transition_probe(
            """
            class It:
                def __iter__(self):
                    return self
                def __next__(self):
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
            result = hot(It())
            assert entries() - before == 1, "must start in machine code"
            sys.setprofile(None)
            assert result == 42, result
            assert ("hot", "return") in events, events
            print("transition reached the running frame")
            """
        )

    def test_profile_enabled_inside_bool_reaches_the_frame(self):
        # The same terminator shape through truthiness: a conditional jump
        # asks the operand for __bool__, and the answer feeds the branch
        # directly.  The transition must be observed at the successor's
        # entry boundary.
        self._run_transition_probe(
            """
            class Flag:
                def __bool__(self):
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
            result = hot(Flag())
            assert entries() - before == 1, "must start in machine code"
            sys.setprofile(None)
            assert result == 2, result
            assert ("hot", "return") in events, events
            print("transition reached the running frame")
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

    def test_profile_enabled_inside_del_reaches_the_frame(self):
        # The transition the bytecode never shows: reference counting is
        # arbitrary execution too.  The poll after BINARY_OP runs with the
        # profiler still off; POP_TOP's DECREF then frees the operand and
        # its __del__ makes the switch.  The refcount instructions are
        # generated by a later pass than any bytecode-keyed scan can see,
        # which is why the poll has to be a boundary invariant rather than
        # an enumeration of reentrant instructions.
        self._run_transition_probe(
            """
            class Temp:
                def __del__(self):
                    sys.setprofile(prof)

            class Num:
                def __add__(self, other):
                    return Temp()

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
            result = hot(Num(), Num())
            assert entries() - before == 1, "must start in machine code"
            sys.setprofile(None)
            assert result == 42, result
            assert ("hot", "return") in events, events
            print("transition reached the running frame")
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
        # deopted set, which re-enabling walks again.  With borrowed
        # references and no function watcher to clear them, a function that
        # died while paused was dereferenced on the way back.
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
                junk = [bytearray(400) for _ in range(5000)]
                # The parked function must stay valid for as long as the
                # runtime can walk it again.
                assert alive() is not None
                del junk
            # Re-enabling walks the parked set; nothing here may dangle.
            gc.collect()
            assert alive() is None or alive().__qualname__ == "victim"
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


if __name__ == "__main__":
    unittest.main()
