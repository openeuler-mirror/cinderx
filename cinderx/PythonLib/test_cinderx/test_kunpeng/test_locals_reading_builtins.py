# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Builtins that read the calling frame's fast locals -- locals(), vars()
and dir() without an argument, eval() and exec() -- are not compiled: a
compiled frame does not keep its locals where PyEval_GetLocals reads them
(3.11 inline-linked frames and 3.14 lightweight frames alike), so such a
function stays in the interpreter and keeps answering as the interpreter
does."""

import unittest

import cinderx.jit
from cinderx.test_support import skip_unless_jit


@skip_unless_jit("Tests the JIT's compile refusal for locals-reading builtins")
class LocalsReadingBuiltinsTest(unittest.TestCase):
    def assert_stays_interpreted(self, func) -> None:
        self.assertFalse(cinderx.jit.is_jit_compiled(func))
        with self.assertRaises(RuntimeError):
            cinderx.jit.force_compile(func)
        self.assertFalse(cinderx.jit.is_jit_compiled(func))

    def assert_locals_reader(self, func, expected) -> None:
        self.assert_stays_interpreted(func)
        self.assertEqual(func(), expected)

    def test_vars_without_argument(self) -> None:
        def uses_vars():
            a = 1
            b = 2
            return vars()

        self.assert_locals_reader(uses_vars, {"a": 1, "b": 2})

    def test_dir_without_argument(self) -> None:
        def uses_dir():
            a = 1
            return dir()

        self.assert_locals_reader(uses_dir, ["a"])

    def test_locals_is_not_compiled(self) -> None:
        def uses_locals():
            a = 1
            return locals()

        self.assert_stays_interpreted(uses_locals)
        self.assertEqual(uses_locals(), {"a": 1})

    def test_other_functions_still_compile(self) -> None:
        def plain(x):
            return x + 1

        if not cinderx.jit.is_jit_compiled(plain):
            self.assertTrue(cinderx.jit.force_compile(plain))
        self.assertTrue(cinderx.jit.is_jit_compiled(plain))
        self.assertEqual(plain(1), 2)


if __name__ == "__main__":
    unittest.main()
