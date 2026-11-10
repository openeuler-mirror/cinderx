# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Range fusion preserves iterator state, argument protocols, and fallback."""

import os
import re
import subprocess
import sys
import textwrap
import unittest


PROBE = """
import _cinderx, cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
assert cinderx.is_initialized() and cinderjit.is_enabled()
assert _cinderx.is_frame_evaluator_installed()

def one(stop):
    total = 0
    for i in range(stop):
        total += i
    return total

def two(start, stop):
    total = 0
    for i in range(start, stop):
        total += i
    return total

def three(start, stop, step):
    total = 0
    for i in range(start, stop, step):
        total += i
    return total

cases = (
    (one, (7,), ((-1,), (0,), (1,), (7,))),
    (two, (2, 7), ((3, 3), (3, 9), (10**30, 10**30 + 3))),
    (three, (2, 9, 2), ((1, 9, 2), (9, 1, -2), (1, 9, -1),
                       (10**30, 10**30 + 5, 2))),
)
for func, warm, inputs in cases:
    expected = sum(range(*warm))
    for _ in range(100):
        assert func(*warm) == expected
    assert cinderjit.force_compile(func), func.__name__
    before = _cinderx._get_trigger_stats()['machine_code_entries']
    value = func(*warm)
    assert value == expected and type(value) is int
    assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
    for args in inputs:
        expected = sum(range(*args))
        value = func(*args)
        assert value == expected and type(value) is int, (func.__name__, args, value)

try:
    three(0, 1, 0)
except ValueError as exc:
    assert str(exc) == 'range() arg 3 must not be zero'
else:
    raise AssertionError('zero step did not raise')

# The same compiled functions must retain C-long boundary, bool and
# __index__ semantics when the fused helper falls back to range/iter.
for start in (1 << 62, -(1 << 63)):
    assert three(start, start + 3, 1) == sum(range(start, start + 3, 1))
assert one(True) == 0 and one(False) == 0

events = []
class Index:
    def __init__(self, value, label):
        self.value, self.label = value, label
    def __index__(self):
        events.append(self.label)
        return self.value

for start in (1, 10**30):
    events.clear()
    assert three(Index(start, 'start'), Index(start + 3, 'stop'),
                 Index(1, 'step')) == 3 * start + 3
    assert events == ['start', 'stop', 'step'], events

def caught(stop):
    try:
        for value in range(stop):
            pass
    except ValueError:
        return 'caught'
    return 'ok'

for _ in range(100):
    assert caught(3) == 'ok'
assert cinderjit.force_compile(caught)
class RaisingIndex:
    def __index__(self):
        events.append('raise')
        raise ValueError('index')
events.clear()
assert caught(RaisingIndex()) == 'caught'
assert events == ['raise'], events

# A replaced builtin must fail the identity pin and execute the replacement.
calls = []
def replacement(stop):
    calls.append(stop)
    return [10, 20]
before = _cinderx._get_trigger_stats()['organic_deopt_hits']
globals()['range'] = replacement
try:
    assert one(7) == 30
    assert calls == [7], calls
    assert _cinderx._get_trigger_stats()['organic_deopt_hits'] > before
finally:
    globals().pop('range')

"""


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 range lowering")
class RangeLowering311Tests(unittest.TestCase):
    def test_range_fusion_and_primitive_index_store(self):
        env = {k: v for k, v in os.environ.items()
               if k != "PYTHONPATH" and not k.startswith(
                   ("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   CINDERX_JIT_STARTUP_GRACE_MS="0", SPIKE_RANGEITER="1",
                   PYTHONJITDUMPFINALHIR="1")
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(PROBE)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        sections = re.split(r"JIT: [^\n]* -- Optimized HIR for __main__:", proc.stderr)[1:]
        latest = {s.split(":", 1)[0]: s for s in sections}
        for name in ("one", "two", "three"):
            with self.subTest(function=name):
                # Check the intended fused path, not only an interpreter result.
                self.assertRegex(latest.get(name, ""), r"StoreField<index@\d+>")

    def test_shadow_range_fusion_passes_hir_validation(self):
        env = {k: v for k, v in os.environ.items()
               if k != "PYTHONPATH" and not k.startswith(
                   ("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE="shadow", PYTHONJITAUTO="20",
                   CINDERX_JIT_STARTUP_GRACE_MS="0", PYTHONJITDUMPFINALHIR="1")
        source = """
            import cinderx, _cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            assert cinderx.is_initialized()
            assert _cinderx.is_frame_evaluator_installed()
            def hot(n):
                total = 0
                for i in range(n):
                    total += i
                return total
            for _ in range(100):
                assert hot(7) == 21
            events = [e for e in _cinderx._get_observe_stats()['events']
                      if e['qualname'] == 'hot']
            assert len(events) == 1 and events[0]['result'] == 'compiled', events
        """
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(source)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(proc.stderr, r"StoreField<index@\d+>")


    def test_mid_loop_forced_deopt_preserves_remaining_range_and_locals(self):
        source = r"""
            import cinderx, _cinderx, dis, sys
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit
            cases = (
                ('one', 'stop', 'stop', (7,)),
                ('two', 'start, stop', 'start, stop', (1000, 1007)),
                ('three', 'start, stop, step', 'start, stop, step',
                 (1012, 1000, -2)),
            )
            for name, parameters, arguments, values in cases:
                namespace = {'__name__': 'range_deopt_resume'}
                exec('def ' + name + '(' + parameters +
                     ', events, items, inspect, inspect_at):\n'
                     '    for value in range(' + arguments + '):\n'
                     '        events.append(("before", value))\n'
                     '        items[0]\n'
                     '        if value == inspect_at:\n'
                     '            inspect()\n'
                     '        events.append(("after", value))\n'
                     '    return value\n', namespace)
                target = namespace[name]
                sequence = list(range(*values))
                expected = [(phase, value) for value in sequence
                            for phase in ('before', 'after')]
                inspect_at = sequence[2]
                observations = []
                def inspect_frame():
                    local = sys._getframe(1).f_locals
                    observations.append((local['value'], tuple(local['events'])))
                for _ in range(100):
                    events = []
                    assert target(*values, events, [0], inspect_frame,
                                  inspect_at) == sequence[-1]
                    assert events == expected
                assert cinderjit.force_compile(target)
                offset = next(i.offset for i in dis.get_instructions(target)
                              if i.opname == 'BINARY_SUBSCR')
                sites = [row for row in cinderjit.deopt_sites(target)
                         if row['forceable'] and row['bc_offset'] == offset]
                assert sites, cinderjit.deopt_sites(target)
                observations.clear()
                events = []
                before = _cinderx._get_trigger_stats()
                # The third visit is inside the loop, after two completed
                # iterations; FOR_ITER has already advanced its iterator.
                assert cinderjit.force_deopt(target, sites[0]['id'], n=3)
                assert target(*values, events, [0], inspect_frame,
                              inspect_at) == sequence[-1]
                after = _cinderx._get_trigger_stats()
                assert after['machine_code_entries'] > before['machine_code_entries']
                assert after['forced_deopt_hits'] == before['forced_deopt_hits'] + 1
                assert events == expected, (name, events, expected)
                assert observations == [(inspect_at, tuple(expected[:5]))], (
                    name, observations)
        """
        env = {k: v for k, v in os.environ.items()
               if k != "PYTHONPATH" and not k.startswith(
                   ("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))}
        env.update(CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   CINDERX_JIT_STARTUP_GRACE_MS="0", PYTHONJITDUMPFINALHIR="1")
        proc = subprocess.run([sys.executable, "-c", textwrap.dedent(source)],
                              env=env, capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        sections = re.split(r"JIT: [^\n]* -- Optimized HIR for ", proc.stderr)[1:]
        for name in ('one', 'two', 'three'):
            matching = [section for section in sections
                        if section.startswith('range_deopt_resume:' + name + ':')]
            self.assertTrue(matching, proc.stderr)
            self.assertRegex(matching[-1], r"StoreField<index@\d+>")


if __name__ == "__main__":
    unittest.main()
