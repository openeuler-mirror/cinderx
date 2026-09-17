# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import platform
import re
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11)
    and platform.machine().lower() in {"aarch64", "arm64"},
    "CP311 AArch64 temporary box sinking",
)
class FloatBoxSink311Tests(unittest.TestCase):
    def test_slow_exception_is_caught_in_compiled_frame(self):
        probe = textwrap.dedent(r"""
            import cinderx
            cinderx.init()
            import cinderjit
            import _cinderx

            captured = []
            class Raising:
                def __iadd__(self, rhs):
                    captured.append(rhs)
                    raise ValueError('inplace failed')
                __isub__ = __iadd__

            def caught_add(items, x, y):
                try:
                    items[0] += x * y
                except ValueError as exc:
                    return str(exc), items[0], captured[-1]
                return items[0]

            def caught_sub(items, x, y):
                try:
                    items[0] -= x * y
                except ValueError as exc:
                    return str(exc), items[0], captured[-1]
                return items[0]

            for fn, expected in ((caught_add, 7.0), (caught_sub, -5.0)):
                cinderjit.jit_suppress(fn)
                for _ in range(40):
                    assert fn([1.0], 2.0, 3.0) == expected
                cinderjit.jit_unsuppress(fn)
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)
                obj = Raising()
                items = [obj]
                before = _cinderx._get_trigger_stats()['organic_deopt_hits']
                captured.clear()
                result = fn(items, 3.0, 4.0)
                after = _cinderx._get_trigger_stats()['organic_deopt_hits']
                assert after > before, 'exception must take the compiled restore path'
                assert result[0] == 'inplace failed'
                assert result[1] is obj and items[0] is obj
                assert len(captured) == 1 and result[2] is captured[0]
                assert type(result[2]) is float and result[2].hex() == (12.0).hex()
                assert cinderjit.is_jit_compiled(fn)
            print('same-frame add/sub handlers and single slow side effect passed')
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0",
                   PYTHONJITDUMPFINALHIR="1")
        env.pop("PYTHONJITLOGFILE", None)
        command = "exec(compile(" + repr(probe) + ", '/tmp/box-sink-handlers.py', 'exec'))"
        result = subprocess.run([sys.executable, "-c", command], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-7000:])
        self.assertIn("same-frame add/sub handlers", result.stdout)
        for name, op in (("caught_add", "Add"), ("caught_sub", "Subtract")):
            match = re.search(r"fun [^\n]*:" + name + r" \{(.*?)(?=\nJIT:|\Z)",
                              result.stderr, re.S)
            self.assertIsNotNone(match, result.stderr[-7000:])
            blocks = re.split(r"(?m)^  bb ", match.group(1))
            slow = [b for b in blocks if "InPlaceOp<" + op + ">" in b]
            self.assertEqual(len(slow), 1, match.group(1))
            self.assertEqual(slow[0].count("= PrimitiveBox<CDouble>"), 1,
                             match.group(1))

    def test_temporary_sink_identity_and_forced_restore(self):
        probe = textwrap.dedent(r"""
            import dis
            import gc
            import sys
            import weakref
            import cinderx
            cinderx.init()
            import cinderjit
            import _cinderx

            def temporary(items, x, y):
                items[0] += x * y
                return items[0]

            def named(items, x, y):
                product = x * y
                items[0] += product
                return items[0]

            def temporary_sub(items, x, y):
                items[0] -= x * y
                return items[0]

            def named_sub(items, x, y):
                product = x * y
                items[0] -= product
                return items[0]

            for fn in (temporary, named, temporary_sub, named_sub):
                cinderjit.jit_suppress(fn)
                for _ in range(30):
                    assert fn([1.0], 2.0, 3.0) == (
                        -5.0 if fn in (temporary_sub, named_sub) else 7.0)
                cinderjit.jit_unsuppress(fn)
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)

            captured = []
            class Capture:
                def __init__(self, expect_named=False, fail=False):
                    self.expect_named = expect_named
                    self.fail = fail
                def __iadd__(self, rhs):
                    if self.expect_named:
                        assert sys._getframe(1).f_locals['product'] is rhs
                    captured.append(rhs)
                    if self.fail:
                        raise ValueError('slow failure')
                    return rhs
                __isub__ = __iadd__

            for fn in (temporary, named, temporary_sub, named_sub):
                for left, x, y in ((1.0, 2.0, 3.0), (-0.0, -0.0, 1.0),
                                   (2.0, 1e-200, 1e-100), (3.0, 1e100, 1e100)):
                    expected = (left - x * y if fn in (temporary_sub, named_sub)
                                else left + x * y)
                    assert fn([left], x, y).hex() == expected.hex()
                obj = Capture(fn in (named, named_sub))
                ref = weakref.ref(obj)
                items = [obj]
                captured.clear()
                result = fn(items, 3.0, 4.0)
                assert result is items[0] is captured[0]
                assert result.hex() == (12.0).hex()
                del obj
                gc.collect()
                assert ref() is None

                obj = Capture(fn in (named, named_sub), fail=True)
                items = [obj]
                captured.clear()
                try:
                    fn(items, 3.0, 4.0)
                except ValueError as exc:
                    assert str(exc) == 'slow failure'
                else:
                    raise AssertionError('missing slow exception')
                assert items[0] is obj
                assert len(captured) == 1 and captured[0].hex() == (12.0).hex()

                # Instrumentation sites before the operation are not forceable.
                # Exercise supported restoration before the store, preserving
                # the returned/captured object's identity. HIR checks below
                # separately verify the pre-operation unboxed stack state.
                offset = next(i.offset for i in dis.get_instructions(fn)
                              if i.opname == 'STORE_SUBSCR')
                sites = [s for s in cinderjit.deopt_sites(fn)
                         if s['forceable'] and s['bc_offset'] == offset]
                assert sites, cinderjit.deopt_sites(fn)
                before = _cinderx._get_trigger_stats()['forced_deopt_hits']
                assert cinderjit.force_deopt(fn, sites[0]['id'], n=1)
                captured.clear()
                result = fn([Capture(fn in (named, named_sub))], 3.0, 4.0)
                assert len(captured) == 1 and result is captured[0]
                assert result.hex() == (12.0).hex()
                assert _cinderx._get_trigger_stats()['forced_deopt_hits'] == before + 1
            print('temporary/named identity, lifetime, exceptions and forced store restore passed')
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0",
                   PYTHONJITDUMPFINALHIR="1")
        env.pop("PYTHONJITLOGFILE", None)
        command = "exec(compile(" + repr(probe) + ", '/tmp/float-box-sink-probe.py', 'exec'))"
        result = subprocess.run([sys.executable, "-c", command], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-7000:])
        self.assertIn("identity, lifetime, exceptions and forced store restore passed", result.stdout)
        for name, expected_boxes in (("temporary", 1), ("named", 0),
                                     ("temporary_sub", 1), ("named_sub", 0)):
            match = re.search(r"fun [^\n]*:" + name + r" \{(.*?)(?=\nJIT:|\Z)",
                              result.stderr, re.S)
            self.assertIsNotNone(match, result.stderr[-7000:])
            blocks = re.split(r"(?m)^  bb ", match.group(1))
            op = "Subtract" if name.endswith("_sub") else "Add"
            slow = [b for b in blocks if "InPlaceOp<" + op + ">" in b]
            self.assertEqual(len(slow), 1, match.group(1))
            self.assertEqual(slow[0].count("= PrimitiveBox<CDouble>"), expected_boxes,
                             match.group(1))
            if name.startswith("temporary"):
                product = re.search(r"(v\d+):CDouble = DoubleBinaryOp<Multiply>",
                                    match.group(1))
                self.assertIsNotNone(product, match.group(1))
                stacks = re.findall(r"Stack<\d+> ([^\n]*)", match.group(1))
                self.assertTrue(any(product.group(1) in stack.split() for stack in stacks),
                                match.group(1))


if __name__ == "__main__":
    unittest.main()
