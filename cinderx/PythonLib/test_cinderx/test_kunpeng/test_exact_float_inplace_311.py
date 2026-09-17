# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import re
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CP311 inplace guards")
class ExactFloatInPlace311Tests(unittest.TestCase):
    def test_lowering_and_deopt_preserve_inplace_effects(self):
        probe = textwrap.dedent(r"""
            import dis
            import sys
            import cinderx
            cinderx.init()
            import cinderjit

            events = []
            control = [1.0, None, None]

            def index():
                events.append('index')
                return 0

            def right_value():
                events.append('rhs')
                if control[1] == 'raise':
                    raise ValueError('rhs error')
                if control[1] == 'clear':
                    control[2].clear()
                if control[1] == 'trace':
                    sys.settrace(lambda *args: None)
                if control[1] == 'profile':
                    sys.setprofile(lambda *args: None)
                return control[0]

            cinderjit.jit_suppress(index)
            cinderjit.jit_suppress(right_value)

            def step_add(items):
                items[index()] += right_value()
                return items[0]

            def step_sub(items):
                items[index()] -= right_value()
                return items[0]

            def oracle_add(items):
                items[index()] += right_value()
                return items[0]

            def oracle_sub(items):
                items[index()] -= right_value()
                return items[0]

            cinderjit.jit_suppress(oracle_add)
            cinderjit.jit_suppress(oracle_sub)

            class Left(float):
                def __iadd__(self, other):
                    events.append('iadd')
                    return 'inplace-add'
                def __isub__(self, other):
                    events.append('isub')
                    return 'inplace-sub'

            class Right(float):
                def __radd__(self, other):
                    events.append('radd')
                    return 'reflected-add'
                def __rsub__(self, other):
                    events.append('rsub')
                    return 'reflected-sub'

            def normalized(value):
                if isinstance(value, float):
                    return (type(value).__name__, value.hex())
                return (type(value).__name__, repr(value))

            def run(fn, left, right, mode=None):
                items = [left]
                control[:] = [right, mode, items]
                events.clear()
                try:
                    result = ('value', normalized(fn(items)))
                except Exception as exc:
                    result = ('exception', type(exc).__name__, str(exc))
                finally:
                    sys.settrace(None)
                    sys.setprofile(None)
                return result, list(map(normalized, items)), list(events)

            def reset_execution():
                cinderjit._jit311_reset_entry_ledger()
                cinderjit._jit311_reset_transition_ledger()

            def assert_execution(fn, calls, exit_kind=None):
                def is_target(row):
                    return (row['qualname'] == fn.__qualname__ and
                            row['filename'] == fn.__code__.co_filename and
                            row['firstlineno'] == fn.__code__.co_firstlineno)
                entries = cinderjit._jit311_entry_ledger()
                transitions = cinderjit._jit311_transition_ledger()
                assert entries['dropped'] == transitions['dropped'] == 0
                rows = [row for row in entries['entries'] if is_target(row)]
                assert len(rows) == 1 and rows[0]['entries'] == calls, entries
                exits = [row for row in transitions['rows'] if is_target(row)]
                if exit_kind is None:
                    assert not exits, exits
                else:
                    assert len(exits) == 1, exits
                    assert not exits[0]['forced'], exits
                    if exit_kind == 'instrumentation':
                        assert exits[0]['instrumentation'], exits
                    elif exit_kind == 'store_bounds':
                        assert exits[0]['deopt_reason'] == 'GuardFailure', exits
                        assert not exits[0]['instrumentation'], exits
                        store_offset = next(i.offset for i in dis.get_instructions(fn)
                                            if i.opname == 'STORE_SUBSCR')
                        assert exits[0]['resume_offset'] == store_offset, exits
                    else:
                        assert exits[0]['deopt_reason'] == 'UnhandledException', exits
                        assert not exits[0]['instrumentation'], exits

            checks = 0
            values = (0.0, -0.0, 1.0, -2.5, 1e300,
                      float('inf'), float('nan'))
            for fn, oracle, opcode in (
                (step_add, oracle_add, 'BINARY_OP_ADD_FLOAT'),
                (step_sub, oracle_sub, 'BINARY_OP_SUBTRACT_FLOAT'),
            ):
                cinderjit.jit_suppress(fn)
                for _ in range(30):
                    run(fn, 2.0, 1.0)
                assert any(i.opname == opcode for i in
                           dis.get_instructions(fn, adaptive=True))
                assert set(fn.__code__.co_freevars) == {'index', 'right_value'}
                cinderjit.jit_unsuppress(fn)
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)
                reset_execution()
                for left in values:
                    for right in values:
                        expected = run(oracle, left, right)
                        assert run(fn, left, right) == expected
                        checks += 1
                assert_execution(fn, len(values) ** 2)
                print(fn.__name__, '49 machine-code entries, zero deopts')
                for left, right, mode in (
                    (Left(2.0), 1.0, None),
                    (2.0, Right(1.0), None),
                    (2, 1.0, None),
                    (2.0, 1, None),
                    ('text', 1.0, None),
                    (2.0, 1.0, 'raise'),
                    (Left(2.0), 1.0, 'clear'),
                    (2.0, 1.0, 'trace'),
                    (2.0, 1.0, 'profile'),
                ):
                    for _ in range(30):
                        run(fn, 2.0, 1.0)
                    if not cinderjit.is_jit_compiled(fn):
                        assert cinderjit.force_compile(fn)
                    assert cinderjit.is_jit_compiled(fn)
                    expected = run(oracle, left, right, mode)
                    reset_execution()
                    actual = run(fn, left, right, mode)
                    assert actual == expected, (fn.__name__, actual, expected)
                    assert actual[2].count('index') == 1
                    assert actual[2].count('rhs') == 1
                    if mode in ('trace', 'profile'):
                        assert_execution(fn, 1, 'instrumentation')
                    elif mode == 'clear':
                        assert_execution(fn, 1, 'store_bounds')
                    elif actual[0][0] == 'exception':
                        assert_execution(fn, 1, 'exception')
                    else:
                        assert_execution(fn, 1)
                    checks += 1
            print('inplace checks', checks)
            print('entry and transition ledgers verified')
        """)
        # Loop variables and helper state must remain local after compilation.
        # The tested functions then load index/right_value from closure cells,
        # so adding a module-global key cannot silently deopt every case.
        probe = "def exercise():\n" + textwrap.indent(probe, "    ") + "\nexercise()\n"
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0",
                   PYTHONJITDUMPFINALHIR="1")
        env.pop("PYTHONJITLOGFILE", None)
        command = "exec(compile(" + repr(probe) + ", '/tmp/exact-float-inplace-probe.py', 'exec'))"
        result = subprocess.run([sys.executable, "-c", command], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        self.assertIn("inplace checks 116", result.stdout)
        self.assertIn("entry and transition ledgers verified", result.stdout)
        for name, op in (("step_add", "Add"), ("step_sub", "Subtract")):
            match = re.search(r"fun [^\n]*:" + r"[^\n]*\b" + name + r" \{(.*?)(?=\nJIT:|\Z)",
                              result.stderr, re.S)
            self.assertIsNotNone(match, result.stderr[-6000:])
            self.assertIn("DoubleBinaryOp<" + op + ">", match.group(1))
            self.assertIn("InPlaceOp<" + op + ">", match.group(1))
            self.assertIn("CondBranchCheckType", match.group(1))
            self.assertNotIn("FloatFastPath", match.group(1))


    def test_integer_seed_stays_in_jit_without_guard_deopt(self):
        probe = textwrap.dedent(r"""
            import cinderx
            cinderx.init()
            import cinderjit

            def accumulate_add(values, initial):
                result = initial
                for value in values:
                    result += value
                return result

            def accumulate_sub(values, initial):
                result = initial
                for value in values:
                    result -= value
                return result

            for fn in (accumulate_add, accumulate_sub):
                cinderjit.jit_suppress(fn)
                for _ in range(30):
                    fn([1.0, 2.0, 3.0], 0.0)
                cinderjit.jit_unsuppress(fn)
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)

            cinderjit._jit311_reset_entry_ledger()
            cinderjit._jit311_reset_transition_ledger()
            for fn, expected in ((accumulate_add, 6.0),
                                 (accumulate_sub, -6.0)):
                for _ in range(20):
                    assert fn([1.0, 2.0, 3.0], 0).hex() == expected.hex()
            entries = cinderjit._jit311_entry_ledger()
            transitions = cinderjit._jit311_transition_ledger()
            rows = {r['qualname']: r['entries'] for r in entries['entries']
                    if r['qualname'] in ('accumulate_add', 'accumulate_sub')}
            assert rows == {'accumulate_add': 20, 'accumulate_sub': 20}, entries
            assert entries['dropped'] == transitions['dropped'] == 0
            assert not [r for r in transitions['rows']
                        if r['qualname'] in rows], transitions

            for fn in (accumulate_add, accumulate_sub):
                for value in (0, 0.0, -0.0):
                    actual = fn([], value)
                    assert type(actual) is type(value)
                    if isinstance(value, float):
                        assert actual.hex() == value.hex()
                    else:
                        assert actual == value

            assert accumulate_add([1.0, 2, 3.0], 0) == 6.0
            assert accumulate_sub([1.0, 2, 3.0], 0) == -6.0
            # Alternate exact-float and mixed inputs on already-published code.
            for i in range(100):
                initial = 0.0 if i & 1 else 0
                assert accumulate_add([1.0], initial) == 1.0
                assert accumulate_sub([1.0], initial) == -1.0
            assert cinderjit.is_jit_compiled(accumulate_add)
            assert cinderjit.is_jit_compiled(accumulate_sub)
            print('integer seeds: add/sub 20 entries, zero deopts; '
                  'empty, mixed and alternating exact')
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0")
        env.pop("PYTHONJITLOGFILE", None)
        env.pop("PYTHONJITDUMPFINALHIR", None)
        command = "exec(compile(" + repr(probe) + ", '/tmp/inplace-integer-seed.py', 'exec'))"
        result = subprocess.run([sys.executable, "-c", command], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-6000:])
        self.assertIn("add/sub 20 entries, zero deopts", result.stdout)


if __name__ == "__main__":
    unittest.main()
