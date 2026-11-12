# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Inline calls and the qualified JIT-off evaluator entry."""

from ._interp311_test_support import Interpreter311Case


class InlineCall311Tests(Interpreter311Case):
    probes = ("hook",)

    def test_evaluator_ownership(self):
        self.run_case("case_inline_hook.py", mode=None)
