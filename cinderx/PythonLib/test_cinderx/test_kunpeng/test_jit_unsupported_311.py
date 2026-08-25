# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Machine-code execution is capability-gated off on CPython 3.11.

The JIT source set is compiled and linked, but jit::initialize() returns
before initializing anything, so the runtime never becomes usable.  These
tests pin the gate: compilation requests are refused, nothing is ever
compiled, and refusing leaves the program running normally.
"""

import sys
import unittest


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 build is pinned to 3.11.6",
)
class JitUnsupported311Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        import cinderx

        cinderx.init()
        import cinderx.jit

        cls._jit = cinderx.jit

    def test_jit_reports_itself_as_disabled(self) -> None:
        self.assertFalse(self._jit.is_enabled())

    def test_force_compile_is_refused(self) -> None:
        def target(a, b):
            return a * b

        # Refusal is reported, never a silent success.
        result = self._jit.force_compile(target)
        self.assertFalse(result)
        self.assertFalse(self._jit.is_jit_compiled(target))

        # The refusal does not disturb the function.
        self.assertEqual(target(6, 7), 42)

    def test_nothing_is_compiled_after_running_hot_code(self) -> None:
        def hot(value):
            total = 0
            for _ in range(1000):
                total += value
            return total

        for _ in range(100):
            hot(2)

        self.assertEqual(hot(2), 2000)
        self.assertFalse(self._jit.is_jit_compiled(hot))


if __name__ == "__main__":
    unittest.main()
