# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Inline calls and the qualified JIT-off evaluator entry."""

from ._interp311_test_support import Interpreter311Case


class InlineCall311Tests(Interpreter311Case):
    probes = ("hook",)

    def test_evaluator_ownership(self):
        self.run_case("case_inline_hook.py", mode=None)


class OffEntry311Tests(Interpreter311Case):
    def test_jit_off_entry_qualification(self):
        for mode, flag in (("off", "0"), ("off", "1"), ("observe", "1"),
                           ("shadow", "1"), ("execute", "1")):
            with self.subTest(mode=mode, flag=flag):
                self.run_case("case_off_entry.py", "off", (mode, flag))
