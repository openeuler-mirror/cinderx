# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Keep method-call descriptor errors consistent with Python attribute lookup."""

from pathlib import Path
import re
import unittest

from cinderx.test_support import (
    assert_python_child_ok,
    run_python_child,
    subprocess_env,
)


CHILD_SCRIPT = (
    Path(__file__).with_name("child_cases") / "load_method_descriptor_fallback.py"
)


class LoadMethodDescriptorFallbackTests(unittest.TestCase):
    def run_case(self, case, *, data_descriptor=False):
        proc = run_python_child(
            CHILD_SCRIPT,
            case,
            "data" if data_descriptor else "non-data",
            env={
                **subprocess_env(),
                "CINDERX_JIT_MODE": "execute",
                "CINDERX_OSR_ENABLED": "0",
                "PYTHONJITDUMPFINALHIR": "1",
            },
            timeout=60,
        )
        output = assert_python_child_ok(proc, context=case)
        target_hir = re.search(
            r"Optimized HIR for __main__:call_hook:\n(.*?)(?=\nJIT:|\Z)",
            output,
            re.DOTALL,
        )
        self.assertIsNotNone(target_hir, output)
        self.assertIn("LoadMethodCached", target_hir.group(1), output)

    def test_unset_slot_falls_back_to_getattr(self):
        self.run_case("unset_slot_falls_back_to_getattr")

    def test_descriptor_attribute_error_uses_getattr(self):
        for data in (False, True):
            with self.subTest(data_descriptor=data):
                self.run_case(
                    "descriptor_attribute_error_uses_getattr", data_descriptor=data
                )

    def test_other_descriptor_exception_is_preserved(self):
        for data in (False, True):
            with self.subTest(data_descriptor=data):
                self.run_case(
                    "other_descriptor_exception_is_preserved", data_descriptor=data
                )

    def test_descriptor_without_getattr_keeps_attribute_error(self):
        self.run_case("descriptor_without_getattr_keeps_attribute_error")

    def test_successful_descriptor_does_not_call_getattr(self):
        for data in (False, True):
            with self.subTest(data_descriptor=data):
                self.run_case(
                    "successful_descriptor_does_not_call_getattr", data_descriptor=data
                )

    def test_instance_attribute_shadows_non_data_descriptor(self):
        self.run_case("instance_attribute_shadows_non_data_descriptor")

    def test_getattr_added_by_descriptor_applies_to_next_lookup(self):
        for data in (False, True):
            with self.subTest(data_descriptor=data):
                self.run_case(
                    "getattr_added_by_descriptor_applies_to_next_lookup",
                    data_descriptor=data,
                )

    def test_getattr_exception_is_preserved(self):
        self.run_case("getattr_exception_is_preserved")

    def test_custom_getattribute_is_respected(self):
        self.run_case("custom_getattribute_is_respected")


if __name__ == "__main__":
    unittest.main()
