# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Attribute fallback and specialization regressions."""

from ._interp311_test_support import Interpreter311Case


class AttrPathInterpreter311Tests(Interpreter311Case):
    probes = ("key",)

    def test_attribute_semantics(self):
        stock = self.run_case("semantic_worker.py", "off", output=True)
        optimized = self.run_case("semantic_worker.py", output=True)
        self.assertEqual(stock, optimized)

    def test_specialization(self):
        stock = self.run_case("case_specga.py", "off", output=True)
        optimized = self.run_case("case_specga.py", output=True)
        self.assertEqual(stock, optimized)
        without_split_hint = self.run_case("case_specga.py", output=True, ATTR_PATH_SPLIT_HINT="0")
        self.assertEqual(stock, without_split_hint)

    def test_instance_dictionary_errors(self):
        self.run_case("case_dict_attributeerror.py")

    def test_type_reentrancy(self):
        self.run_case("case_type_reentrancy.py")

    def test_target_reentrancy(self):
        for cache in ("0", "1"):
            with self.subTest(cache=cache):
                self.run_case("case_target_reentrancy.py", ATTR_PATH_MRO_CACHE=cache)

    def test_mro_cache(self):
        self.run_case("case_mro_cache.py")
