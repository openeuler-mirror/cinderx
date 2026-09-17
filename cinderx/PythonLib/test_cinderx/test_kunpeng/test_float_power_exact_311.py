# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] in ((3, 11), (3, 14)), "CP311/CP314 power semantics")
class FloatPowerExact311Tests(unittest.TestCase):
    def test_runtime_power_bits_exceptions_and_hir(self):
        probe = textwrap.dedent("""
            import sys
            import struct
            import cinderx
            cinderx.init()
            if sys.version_info[:2] == (3, 11):
                import cinderjit as jit
            else:
                import cinderx.jit as jit
                jit.enable_specialized_opcodes()

            def oracle(x, exponent):
                return (x * 1.0) ** exponent
            jit.jit_suppress(oracle)

            def call_compiled(fn, value):
                if not jit.is_jit_compiled(fn):
                    assert jit.force_compile(fn)
                assert jit.is_jit_compiled(fn)
                if sys.version_info[:2] == (3, 11):
                    jit._jit311_reset_entry_ledger()
                try:
                    return fn(value)
                finally:
                    if sys.version_info[:2] == (3, 11):
                        ledger = jit._jit311_entry_ledger()
                        rows = [r for r in ledger['entries']
                                if r['qualname'] == fn.__qualname__ and
                                r['filename'] == fn.__code__.co_filename and
                                r['firstlineno'] == fn.__code__.co_firstlineno]
                        assert ledger['dropped'] == 0, ledger
                        assert len(rows) == 1 and rows[0]['entries'] == 1, ledger

            exponents = (0.5, 1.0, 1.5, 2.0, 3.0, -0.5, -1.0, -1.5, -2.0)
            values = (2.0, 3.0, 0.1, 1.0000000000000002,
                      float.fromhex('0x1.8aa959fd0b754p+8'),
                      1e-200, 1e200, 0.0, -0.0, -2.0,
                      float('inf'), float('-inf'), float('nan'))
            checks = 0
            for i, exponent in enumerate(exponents):
                scope = {}
                src = 'def exact_power_' + str(i) + '(x): return (x * 1.0) ** ' + repr(exponent)
                exec(compile(src, '/tmp/power-exact-regression.py', 'exec'), scope)
                fn = scope['exact_power_' + str(i)]
                jit.jit_suppress(fn)
                for _ in range(20): fn(2.0)
                jit.jit_unsuppress(fn)
                assert jit.force_compile(fn)
                assert jit.is_jit_compiled(fn)
                for value in values:
                    try:
                        expected = oracle(value, exponent)
                    except (OverflowError, ZeroDivisionError) as exc:
                        try: call_compiled(fn, value)
                        except type(exc): pass
                        else: raise AssertionError(('missing exception', value, exponent))
                    else:
                        actual = call_compiled(fn, value)
                        assert type(actual) is type(expected)
                        if isinstance(expected, complex):
                            assert struct.pack('!d', actual.real) == struct.pack('!d', expected.real)
                            assert struct.pack('!d', actual.imag) == struct.pack('!d', expected.imag)
                        else:
                            assert struct.pack('!d', actual) == struct.pack('!d', expected), (value, exponent, actual.hex(), expected.hex())
                    checks += 1
            def consumer_oracle(x, exponent):
                return ((x * 1.0) ** exponent) + 1.0
            jit.jit_suppress(consumer_oracle)
            consumer_checks = 0
            for i, exponent in enumerate((0.5, 1.5, -0.5, -1.5)):
                scope = {}
                name = 'power_consumer_' + str(i)
                src = ('def ' + name + '(x): return ((x * 1.0) ** ' +
                       repr(exponent) + ') + 1.0')
                exec(compile(src, '/tmp/power-consumer-regression.py', 'exec'), scope)
                fn = scope[name]
                jit.jit_suppress(fn)
                for _ in range(20): fn(4.0)
                jit.jit_unsuppress(fn)
                # Start every alternating input in compiled code: a negative
                # result may legitimately leave via a downstream float guard.
                for value in (-4.0, 4.0, -2.0, 2.0):
                    expected = consumer_oracle(value, exponent)
                    actual = call_compiled(fn, value)
                    assert type(actual) is type(expected), (value, exponent, actual)
                    if isinstance(expected, complex):
                        assert struct.pack('!d', actual.real) == struct.pack('!d', expected.real)
                        assert struct.pack('!d', actual.imag) == struct.pack('!d', expected.imag)
                    else:
                        assert struct.pack('!d', actual) == struct.pack('!d', expected)
                    consumer_checks += 1
            print('power consumer checks', consumer_checks)
            print('exact power checks', checks)
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO=("1000000" if sys.version_info[:2] == (3, 11)
                                  else "auto:1000000"),
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0",
                   PYTHONJITDUMPFINALHIR="1")
        env.pop("PYTHONJITLOGFILE", None)
        result = subprocess.run([sys.executable, "-c", probe], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-5000:])
        self.assertIn("exact power checks 117", result.stdout)
        self.assertIn("power consumer checks 16", result.stdout)
        expected_power = ("FloatBinaryOp<Power>" if sys.version_info[:2] == (3, 11)
                          else "DoubleBinaryOp<Power>")
        self.assertIn(expected_power, result.stderr)


if __name__ == "__main__":
    unittest.main()
