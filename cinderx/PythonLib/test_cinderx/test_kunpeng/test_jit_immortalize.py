# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

import os
import sys
import unittest

# Keep this in globals so the JIT specializes the concrete mortal constant.
from re._constants import BIGCHARSET

import cinderx
from cinderx.jit import jit_suppress
from cinderx.test_support import passUnless, run_in_subprocess, skip_if_ft


def _immortalize_leaf(value):
    return value


def _immortalize_caller(value):
    return _immortalize_leaf(value)


@skip_if_ft("Free threading doesn't support heap immortalization")
@passUnless(cinderx.is_initialized(), "Tests immortalization APIs in CinderX")
class JITImmortalizeTests(unittest.TestCase):
    @unittest.skipUnless(
        sys.version_info >= (3, 12), "Heap immortalization requires Python 3.12+"
    )
    @run_in_subprocess
    @jit_suppress
    def test_jit_incref_after_immortalize(self) -> None:
        from cinderx import jit

        def load_constant():
            return BIGCHARSET

        self.assertFalse(cinderx.is_immortal(BIGCHARSET))
        jit.force_compile(load_constant)
        self.assertTrue(jit.is_jit_compiled(load_constant))
        cinderx.immortalize_heap()
        self.assertTrue(cinderx.is_immortal(BIGCHARSET))
        before = sys.getrefcount(BIGCHARSET)
        for _ in range(10):
            value = load_constant()
            self.assertEqual(sys.getrefcount(BIGCHARSET), before)
            self.assertTrue(cinderx.is_immortal(value))
            del value
            self.assertEqual(sys.getrefcount(BIGCHARSET), before)
        self.assertTrue(jit.is_jit_compiled(load_constant))

    @unittest.skipUnless(
        sys.version_info >= (3, 12), "Heap immortalization requires Python 3.12+"
    )
    @run_in_subprocess
    @jit_suppress
    def test_jit_decref_after_immortalize(self) -> None:
        from cinderx import jit

        def use_constant(observe):
            value = BIGCHARSET
            observe(value)

        jit.force_compile(use_constant)
        self.assertTrue(jit.is_jit_compiled(use_constant))
        self.assertFalse(cinderx.is_immortal(BIGCHARSET))
        before = sys.getrefcount(BIGCHARSET)
        seen = []
        use_constant(lambda value: seen.append(value is BIGCHARSET))
        self.assertEqual(seen, [True])
        self.assertEqual(sys.getrefcount(BIGCHARSET), before)

        cinderx.immortalize_heap()
        self.assertTrue(cinderx.is_immortal(BIGCHARSET))
        before = sys.getrefcount(BIGCHARSET)

        def observe(value):
            self.assertIs(value, BIGCHARSET)
            # Check INCREF separately so a matching bad DECREF cannot hide it.
            self.assertEqual(sys.getrefcount(value), before)

        for _ in range(10):
            use_constant(observe)
            self.assertEqual(sys.getrefcount(BIGCHARSET), before)
        def raise_after_observing(value):
            observe(value)
            raise ValueError("immortal callback")

        with self.assertRaisesRegex(ValueError, "immortal callback"):
            use_constant(raise_after_observing)
        self.assertEqual(sys.getrefcount(BIGCHARSET), before)
        self.assertTrue(jit.is_jit_compiled(use_constant))

    @run_in_subprocess
    @jit_suppress
    def test_jit_mortal_after_immortalize(self) -> None:
        from cinderx import jit

        def identity(value):
            return value

        jit.force_compile(identity)
        self.assertTrue(jit.is_jit_compiled(identity))
        cinderx.immortalize_heap()
        destroyed = []

        class Mortal:
            def __del__(self):
                destroyed.append(True)

        value = Mortal()
        self.assertFalse(cinderx.is_immortal(value))
        before = sys.getrefcount(value)
        result = identity(value)
        self.assertIs(result, value)
        self.assertEqual(sys.getrefcount(value), before + 1)
        del result
        self.assertEqual(sys.getrefcount(value), before)
        del value
        self.assertEqual(destroyed, [True])

    @unittest.skipUnless(
        sys.version_info >= (3, 12), "Heap immortalization requires Python 3.12+"
    )
    @unittest.skipIf(
        os.environ.get("PYTHONJITLIGHTWEIGHTFRAME") == "0",
        "Inlining requires lightweight frames",
    )
    @run_in_subprocess
    @jit_suppress
    def test_jit_inline_frame_decref_after_immortalize(self) -> None:
        from cinderx import jit

        self.assertFalse(cinderx.is_immortal(_immortalize_leaf))
        # Aggregate suites can leave an explicit JIT list or disable inlining.
        # Establish callee eligibility inside this subprocess before compiling.
        jit.enable_hir_inliner()
        jit.append_jit_list(
            f"{_immortalize_leaf.__module__}:{_immortalize_leaf.__qualname__}"
        )
        jit.force_uncompile(_immortalize_caller)
        self.assertTrue(jit.force_compile(_immortalize_caller))
        self.assertTrue(jit.is_jit_compiled(_immortalize_caller))
        counts = jit.get_function_hir_opcode_counts(_immortalize_caller)
        self.assertGreater(counts.get("BeginInlinedFunction", 0), 0, counts)
        cinderx.immortalize_heap()
        self.assertTrue(cinderx.is_immortal(_immortalize_leaf))
        function_before = sys.getrefcount(_immortalize_leaf)
        code_before = sys.getrefcount(_immortalize_leaf.__code__)
        marker = []
        self.assertIs(_immortalize_caller(marker), marker)
        self.assertEqual(sys.getrefcount(_immortalize_leaf), function_before)
        self.assertEqual(sys.getrefcount(_immortalize_leaf.__code__), code_before)

    @unittest.skipUnless(
        sys.version_info >= (3, 12), "Heap immortalization requires Python 3.12+"
    )
    @run_in_subprocess
    @jit_suppress
    def test_immortalize_preserves_frozen_gc_links(self) -> None:
        import gc

        first = []
        second = [first]
        first.append(second)
        third = [first, second]
        gc.freeze()
        frozen_before = gc.get_freeze_count()
        cinderx.immortalize_heap()
        for value in (first, second, third):
            self.assertTrue(cinderx.is_immortal(value))
            self.assertTrue(gc.is_tracked(value))
        self.assertGreaterEqual(gc.get_freeze_count(), frozen_before)
