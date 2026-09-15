# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import subprocess
import sys
import textwrap
import unittest


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CP311 power semantics")
class FloatPowerExact311Tests(unittest.TestCase):
    def test_runtime_power_bits_exceptions_and_hir(self):
        probe = textwrap.dedent("""
            import math
            import cinderx
            cinderx.init()
            import cinderjit

            def oracle(x, exponent):
                return (x * 1.0) ** exponent
            cinderjit.jit_suppress(oracle)

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
                cinderjit.jit_suppress(fn)
                for _ in range(20): fn(2.0)
                cinderjit.jit_unsuppress(fn)
                assert cinderjit.force_compile(fn)
                assert cinderjit.is_jit_compiled(fn)
                for value in values:
                    try:
                        expected = oracle(value, exponent)
                    except (OverflowError, ZeroDivisionError) as exc:
                        try: fn(value)
                        except type(exc): pass
                        else: raise AssertionError(('missing exception', value, exponent))
                    else:
                        actual = fn(value)
                        assert type(actual) is type(expected)
                        if isinstance(expected, complex):
                            assert actual.real.hex() == expected.real.hex()
                            assert actual.imag.hex() == expected.imag.hex()
                        else:
                            assert actual.hex() == expected.hex(), (value, exponent, actual.hex(), expected.hex())
                    checks += 1
            print('exact power checks', checks)
        """)
        env = dict(os.environ)
        env.update(CINDERX_PLUGIN_ENABLE="1", CINDERX_EVAL_MODE="cinder",
                   CINDERX_JIT_MODE="execute", PYTHONJITAUTO="1000000",
                   PYTHONJITLIGHTWEIGHTFRAME="1", CINDERX_OSR_ENABLED="0",
                   PYTHONJITDUMPFINALHIR="1")
        env.pop("PYTHONJITLOGFILE", None)
        result = subprocess.run([sys.executable, "-c", probe], env=env,
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr[-5000:])
        self.assertIn("exact power checks 117", result.stdout)
        self.assertIn("FloatBinaryOp<Power>", result.stderr)


if __name__ == "__main__":
    unittest.main()
