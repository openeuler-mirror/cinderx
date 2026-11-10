# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Loop unboxing preserves object identity and local binding after deoptimization."""

import os
import re
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 loop unboxing")
class LoopPhiIdentity311Tests(unittest.TestCase):
    def test_zero_iteration_constant_identity_after_forced_deopt(self):
        source = r"""
            import cinderx, _cinderx, dis, sys
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit
            def noop():
                pass
            for constant in ('1000', '1000.25'):
                namespace = {}
                exec('def target(n, cb, items):\n'
                     '    x = y = ' + constant + '\n'
                     '    for i in range(n):\n'
                     '        x += 1\n'
                     '    z = x * 2\n'
                     '    items[0]\n'
                     '    cb()\n'
                     '    return x * 3, y, z\n', namespace)
                target = namespace['target']
                for _ in range(100):
                    target(4, noop, [0])
                assert cinderjit.force_compile(target)
                offset = next(i.offset for i in dis.get_instructions(target)
                              if i.opname == 'BINARY_SUBSCR')
                sites = [s for s in cinderjit.deopt_sites(target)
                         if s['forceable'] and s['bc_offset'] == offset]
                assert sites, cinderjit.deopt_sites(target)
                before = _cinderx._get_trigger_stats()['forced_deopt_hits']
                assert cinderjit.force_deopt(target, sites[0]['id'], n=1)
                captured = []
                def inspect_frame():
                    values = sys._getframe(1).f_locals
                    captured.append(values['x'] is values['y'])
                target(0, inspect_frame, [0])
                assert _cinderx._get_trigger_stats()['forced_deopt_hits'] == before + 1
                assert captured == [True], (constant, captured)
        """
        env = {k: v for k, v in os.environ.items()
               if k != "PYTHONPATH" and not k.startswith(
                   ("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   CINDERX_JIT_STARTUP_GRACE_MS="0")
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(source)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_unbound_integer_and_float_locals_after_forced_deopt(self):
        source = r"""
            import cinderx, _cinderx, dis, sys
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def inspect_frame():
                values = sys._getframe(1).f_locals
                return ('value' in values, values.get('value'))

            for scale, expected_type in (('1000', int), ('1000.5', float)):
                namespace = {'__name__': 'loop_phi_binding'}
                name = 'target_' + expected_type.__name__
                # End the previous value before allocating its replacement.
                # Otherwise its live box and the Phi overlap in STORE_FAST's
                # FrameState, and identity preservation retains the boxed web.
                exec('def ' + name + '(n, cb, items):\n'
                     '    for i in range(n):\n'
                     '        value = None\n'
                     '        value = i * ' + scale + '\n'
                     '    items[0]\n'
                     '    return cb()\n', namespace)
                target = namespace[name]
                for _ in range(100):
                    target(3, inspect_frame, [0])
                assert cinderjit.force_compile(target)
                offset = next(i.offset for i in dis.get_instructions(target)
                              if i.opname == 'BINARY_SUBSCR')
                sites = [s for s in cinderjit.deopt_sites(target)
                         if s['forceable'] and s['bc_offset'] == offset]
                assert sites, cinderjit.deopt_sites(target)
                for n in (0, 3):
                    assert cinderjit.is_jit_compiled(target)
                    before = _cinderx._get_trigger_stats()
                    assert cinderjit.force_deopt(target, sites[0]['id'], n=1)
                    present, value = target(n, inspect_frame, [0])
                    after = _cinderx._get_trigger_stats()
                    assert after['machine_code_entries'] > before['machine_code_entries']
                    assert after['forced_deopt_hits'] == before['forced_deopt_hits'] + 1
                    assert present == (n > 0), (scale, n, present, value)
                    if present:
                        assert type(value) is expected_type, (scale, value)
                        assert value == 2 * expected_type(scale), (scale, value)
        """
        env = {k: v for k, v in os.environ.items()
               if k != "PYTHONPATH" and not k.startswith(
                   ("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   CINDERX_JIT_STARTUP_GRACE_MS="0", PYTHONJITDUMPFINALHIR="1")
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(source)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        # A bound flag in the optimized target proves this exercises the
        # primitive Phi metadata consumed by deopt, including the unbound edge.
        sections = re.split(r"JIT: [^\n]* -- Optimized HIR for ", proc.stderr)[1:]
        for name in ("target_int", "target_float"):
            matching = [section for section in sections
                        if section.startswith("loop_phi_binding:" + name + ":")]
            self.assertTrue(matching, proc.stderr)
            self.assertRegex(matching[-1], r"Locals<\d+>[^\n]*\?v\d+")
