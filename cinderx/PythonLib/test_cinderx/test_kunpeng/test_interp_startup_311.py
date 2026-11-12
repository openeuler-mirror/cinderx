# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Wheel startup configuration and main interpreter ownership."""

from ._interp311_test_support import Interpreter311Case


class InterpreterStartup311Tests(Interpreter311Case):
    def test_startup(self):
        self.run_case("case_startup.py", "off")

    def test_default_policy(self):
        self.run_case("case_default_policy.py", "off")

    def test_first_import_order(self):
        self.run_case("case_first_import_order.py", "off")

    def test_subinterpreter_ownership(self):
        self.run_case("case_subinterpreter_owner.py")
