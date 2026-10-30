# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import re
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CP311 execute admission")
class SubscriptProfitability311Tests(unittest.TestCase):
    def test_array_index_overflow_and_local_handler(self):
        source = textwrap.dedent("""
            from array import array
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            # Literal indices ensure the array fast path has type evidence.
            # Compile before executing: huge indices always raise.
            for index in (2**100, -(2**100), 2**63, -(2**63)-1):
                for operation in ('load', 'store'):
                    if operation == 'store':
                        body = f'a[{index}] = 1.0\\n    return x * 1.0'
                    else:
                        body = (f'for _ in range(1):\\n'
                                f'        x += a[{index}]\\n'
                                f'    return x + 1.0')
                    for local_handler in (False, True):
                        if local_handler:
                            body = ('try:\\n        ' +
                                    body.replace('\\n', '\\n    ') +
                                    '\\n    except IndexError:\\n'
                                    '        return "caught"')
                        ns = {}
                        exec('def f(a, x):\\n    ' + body, ns)
                        f = ns['f']
                        a = array('d', [3.0])
                        def outcome():
                            try:
                                return ('return', f(a, 0.0))
                            except Exception as exc:
                                return ('raise', type(exc), str(exc))
                        cinderjit.jit_suppress(f)
                        expected = outcome()
                        assert expected[1] == ('caught' if local_handler else IndexError)
                        cinderjit.jit_unsuppress(f)
                        assert cinderjit.force_compile(f)
                        assert cinderjit.is_jit_compiled(f)
                        before = _cinderx._get_trigger_stats()['machine_code_entries']
                        actual = outcome()
                        assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
                        assert actual == expected, (operation, index, expected, actual)
                        assert a.tolist() == [3.0]

            def generic_store(a, index, x):
                a[index] = 1.0
                return x * 1.0

            class Index:
                def __init__(self, value):
                    self.value = value
                def __index__(self):
                    calls.append(self.value)
                    return self.value

            calls = []
            indices = (0, -1, True, Index(-1), Index(2**100),
                       Index(-(2**100)), Index('invalid'), 0.0, object())
            def outcomes():
                results = []
                for index in indices:
                    calls.clear()
                    a = array('d', [3.0, 4.0])
                    try:
                        result = ('return', generic_store(a, index, 2.0))
                    except Exception as exc:
                        result = ('raise', type(exc), str(exc))
                    results.append((result, a.tolist(), list(calls)))
                return results
            cinderjit.jit_suppress(generic_store)
            expected = outcomes()
            cinderjit.jit_unsuppress(generic_store)
            assert cinderjit.force_compile(generic_store)
            assert cinderjit.is_jit_compiled(generic_store)
            before = _cinderx._get_trigger_stats()['machine_code_entries']
            assert outcomes() == expected
            assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="0", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", PYTHONJITGENERATOR="1",
                   CINDERX_OSR_ENABLED="0")
        env.pop("PYTHONJITLOGFILE", None)
        result = subprocess.run([sys.executable, "-c", source], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_array_compact_index_fast_paths(self):
        source = textwrap.dedent("""
            from array import array
            import _cinderx, cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def array_load(items):
                return items[1]

            def array_store(items, index, value):
                items[index] = value
                return items[index]

            items = array('d', [1.0, 2.0, 3.0])
            # The dynamic store learns the stuck array specialization;
            # the load has a literal integer index as type evidence.
            for _ in range(16):
                assert array_load(items) == items[1]
                assert array_store(items, 1, 2.5) == 2.5
            for fn in (array_load, array_store):
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)
            before = _cinderx._get_trigger_stats()['machine_code_entries']
            assert array_load(items) == 2.5
            assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
            before = _cinderx._get_trigger_stats()['machine_code_entries']
            assert array_store(items, 1, 3.5) == 3.5
            assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
            assert items.tolist() == [1.0, 3.5, 3.0]
        """)
        env = {
            key: value for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))
        }
        env.update(CINDERX_PLUGIN_ENABLE="0", CINDERX_JIT_MODE="execute",
                   PYTHONJITAUTO="1000000", PYTHONJITDUMPFINALHIR="1")
        result = subprocess.run([sys.executable, "-c", source], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        functions = dict(re.findall(
            r"fun __main__:(array_load|array_store) \{(.*?)\n\}",
            result.stderr, re.DOTALL,
        ))
        self.assertIn("LoadArrayItem", functions.get("array_load", ""))
        for instruction in ("IsCompactLong", "CompactLongUnbox", "StoreArrayItem"):
            self.assertIn(instruction, functions.get("array_store", ""))

    def test_array_store_bounds_replay_preserves_operands(self):
        source = textwrap.dedent("""
            from array import array
            import cinderx
            import _cinderx
            cinderx.init()
            _cinderx.install_frame_evaluator()
            import cinderjit

            def store(items, index, value):
                items[index] = value
                return items[index]

            def outcome(index):
                items = array('d', [1.0, 2.0, 3.0])
                try:
                    result = ('return', store(items, index, 1.5))
                except Exception as exc:
                    result = ('raise', type(exc), str(exc))
                return result, items.tolist()

            cinderjit.jit_suppress(store)
            expected = [outcome(index) for index in (3, -4)]
            cinderjit.jit_unsuppress(store)
            # Seed the exact-array/float evidence used by the inline store.
            for _ in range(16):
                assert store(array('d', [1.0, 2.0, 3.0]), 1, 2.5) == 2.5
            for index, oracle in zip((3, -4), expected):
                if not cinderjit.is_jit_compiled(store):
                    assert cinderjit.force_compile(store)
                assert cinderjit.is_jit_compiled(store)
                before = _cinderx._get_trigger_stats()['machine_code_entries']
                actual = outcome(index)
                assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
                assert actual == oracle, (index, oracle, actual)
                assert actual[1] == [1.0, 2.0, 3.0]
        """)
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_"))
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="0",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
        )
        result = subprocess.run(
            [sys.executable, "-c", source], env=env,
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_auto_entry_and_generic_semantics(self):
        source = textwrap.dedent("""
            import _cinderx, cinderx
            cinderx.init()
            import cinderjit

            def lookup(mapping, key):
                return mapping[key]

            def numeric(items, x):
                items[0] += x * 1.0
                return items[0]

            def warm_numeric(items, x, y):
                items[0] += x * y
                return items[0]

            def default_float(mapping, key):
                try:
                    return mapping[key]
                except KeyError:
                    return 0.0

            def incidental_float(items, timestamp):
                return items[0], timestamp * 1000.0

            def numeric_loop(items):
                total = 0.0
                for index in range(len(items)):
                    total += items[index]
                return total

            def handler_loop(items, timestamp):
                try:
                    return items[0], timestamp * 1000.0
                except IndexError:
                    for index in range(2):
                        timestamp += 1.0
                    return timestamp

            for _ in range(40):
                assert lookup({'x': 3}, 'x') == 3
                assert numeric([1.0], 2.0) == 3.0
                assert default_float({'x': 3}, 'x') == 3
                assert incidental_float([3], 1.0) == (3, 1000.0)
                assert numeric_loop([1.0, 2.0]) == 3.0
                assert handler_loop([3], 1.0) == (3, 1000.0)
            cases = (
                (lookup, ({'x': 3}, 'x'), 3),
                (numeric, ([1.0], 2.0), 3.0),
                (default_float, ({'x': 3}, 'x'), 3),
                (incidental_float, ([3], 1.0), (3, 1000.0)),
                (numeric_loop, ([1.0, 2.0],), 3.0),
                (handler_loop, ([3], 1.0), (3, 1000.0)),
            )
            for fn, args, expected in cases:
                assert cinderjit.is_jit_compiled(fn), fn
                before = _cinderx._get_trigger_stats()['machine_code_entries']
                assert fn(*args) == expected
                assert _cinderx._get_trigger_stats()['machine_code_entries'] > before
            cinderjit.jit_suppress(warm_numeric)
            for _ in range(40):
                assert warm_numeric([1.0], 2.0, 3.0) == 7.0
            cinderjit.jit_unsuppress(warm_numeric)
            assert cinderjit.force_compile(warm_numeric)
            assert cinderjit.is_jit_compiled(warm_numeric)
            class Add:
                def __iadd__(self, rhs):
                    return ('slow', rhs)
            assert numeric([Add()], 2.0) == ('slow', 2.0)
            assert default_float({}, 'missing') == 0.0
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="2",
                   PYTHONJITLIGHTWEIGHTFRAME="1", PYTHONJITGENERATOR="1",
                   CINDERX_OSR_ENABLED="0")
        result = subprocess.run([sys.executable, "-c", source], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
