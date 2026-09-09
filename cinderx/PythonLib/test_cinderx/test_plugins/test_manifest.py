# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import json
import sys
import unittest
from dataclasses import FrozenInstanceError
from types import MappingProxyType
from typing import Any
from unittest.mock import patch

from cinderx.plugins import manifest as manifest_module
from cinderx.plugins.manifest import (
    MAX_ENTRIES_PER_SECTION,
    MAX_IDENTIFIER_BYTES,
    MAX_MANIFEST_BYTES,
    MAX_PROVIDE_ENTRIES,
    MAX_TARGET_CAPABILITIES,
    ManifestIssueCode,
    validate_manifest,
)


def valid_manifest() -> dict[str, Any]:
    return {
        "id": "cinderx-plugin-example",
        "spi_version": "1",
        "runtime_abi": {
            "python_version": "3.14",
            "soabi": "cpython-314-aarch64-linux-gnu",
            "core_build_id": "cinderx-dev",
            "cpu_caps": ["neon"],
        },
        "target_capabilities": ["plugin-manifest-v1"],
        "provides": {
            "contracts": [{"name": "column-layout"}],
            "policies": [{"enabled": True}],
            "seeds": ["seeds/profile.json"],
            "diagnostics": {"counters": {"enabled": False}},
            "pass": {"enabled": True, "config": {"level": 2}},
        },
    }


def encode(manifest: dict[str, Any]) -> str:
    return json.dumps(manifest, separators=(",", ":"))


class PluginManifestHappyPathTests(unittest.TestCase):
    def test_accepts_pure_declaration(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {"policies": [], "seeds": []}

        result = validate_manifest(encode(payload))

        self.assertTrue(result.accepted)
        self.assertIsNone(result.manifest_rejection)
        self.assertEqual(result.entry_rejections, ())
        self.assertIsNotNone(result.manifest)
        manifest = result.manifest
        assert manifest is not None
        self.assertEqual(manifest.id, "cinderx-plugin-example")
        self.assertEqual(manifest.spi_version, "1")
        self.assertEqual(manifest.provides.contracts, ())
        self.assertEqual(manifest.provides.policies, ())
        self.assertEqual(manifest.provides.seeds, ())
        self.assertIsNone(manifest.provides.diagnostics)
        self.assertIsNone(manifest.provides.pass_config)
        self.assertIsNone(manifest.adapter)

    def test_accepts_bytes_and_optional_adapter_without_importing_it(self) -> None:
        payload = valid_manifest()
        payload["adapter"] = {
            "entry": "never_imported.adapter",
            "target": "never_imported_target",
        }

        result = validate_manifest(encode(payload).encode("utf-8"))

        self.assertTrue(result.accepted)
        manifest = result.manifest
        assert manifest is not None
        self.assertIsNotNone(manifest.adapter)
        assert manifest.adapter is not None
        self.assertEqual(manifest.adapter.entry, "never_imported.adapter")
        self.assertEqual(manifest.adapter.target, "never_imported_target")
        self.assertNotIn("never_imported", sys.modules)
        self.assertNotIn("never_imported.adapter", sys.modules)
        self.assertNotIn("never_imported_target", sys.modules)

    def test_result_and_payloads_are_recursively_immutable(self) -> None:
        payload = valid_manifest()
        payload["provides"]["contracts"] = [
            {"nested": {"items": [1, {"value": "kept"}]}}
        ]

        result = validate_manifest(encode(payload))

        manifest = result.manifest
        assert manifest is not None
        contract = manifest.provides.contracts[0]
        self.assertIsInstance(contract, MappingProxyType)
        nested = contract["nested"]
        self.assertIsInstance(nested, MappingProxyType)
        self.assertIsInstance(nested["items"], tuple)
        with self.assertRaises(TypeError):
            contract["new"] = "value"  # type: ignore[index]
        with self.assertRaises(TypeError):
            nested["new"] = "value"  # type: ignore[index]
        with self.assertRaises(FrozenInstanceError):
            manifest.id = "changed"  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            result.manifest = None  # type: ignore[misc]


class PluginManifestBudgetTests(unittest.TestCase):
    def test_budget_constants_are_explicit(self) -> None:
        self.assertEqual(MAX_MANIFEST_BYTES, 64 * 1024)
        self.assertEqual(MAX_PROVIDE_ENTRIES, 256)
        self.assertEqual(MAX_ENTRIES_PER_SECTION, 128)
        self.assertEqual(MAX_TARGET_CAPABILITIES, 64)
        self.assertEqual(MAX_IDENTIFIER_BYTES, 128)

    def test_manifest_at_exact_byte_budget_is_accepted(self) -> None:
        raw = encode(valid_manifest())
        padded = raw + " " * (MAX_MANIFEST_BYTES - len(raw.encode("utf-8")))

        self.assertEqual(len(padded.encode("utf-8")), MAX_MANIFEST_BYTES)
        self.assertTrue(validate_manifest(padded).accepted)

    def test_manifest_over_byte_budget_is_rejected_before_parsing(self) -> None:
        result = validate_manifest(b"{" + b" " * MAX_MANIFEST_BYTES)

        self.assertFalse(result.accepted)
        self.assertEqual(
            result.manifest_rejection.code,
            ManifestIssueCode.MANIFEST_TOO_LARGE,
        )
        self.assertEqual(result.manifest_rejection.path, "$")

    def test_exact_section_and_total_entry_budgets_are_accepted(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": [{} for _ in range(MAX_ENTRIES_PER_SECTION)],
            "policies": [{} for _ in range(MAX_ENTRIES_PER_SECTION)],
        }

        result = validate_manifest(encode(payload))

        self.assertTrue(result.accepted)
        manifest = result.manifest
        assert manifest is not None
        self.assertEqual(len(manifest.provides.contracts), 128)
        self.assertEqual(len(manifest.provides.policies), 128)

    def test_section_over_entry_budget_is_rejected(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": [{} for _ in range(MAX_ENTRIES_PER_SECTION + 1)]
        }

        result = validate_manifest(encode(payload))

        self.assertFalse(result.accepted)
        self.assertEqual(
            result.manifest_rejection.code,
            ManifestIssueCode.TOO_MANY_ENTRIES,
        )
        self.assertEqual(result.manifest_rejection.path, "$.provides.contracts")

    def test_total_entry_budget_is_rejected(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": [{} for _ in range(MAX_ENTRIES_PER_SECTION)],
            "policies": [{} for _ in range(MAX_ENTRIES_PER_SECTION)],
            "seeds": ["one-too-many.json"],
        }

        result = validate_manifest(encode(payload))

        self.assertFalse(result.accepted)
        self.assertEqual(
            result.manifest_rejection.code,
            ManifestIssueCode.TOO_MANY_ENTRIES,
        )
        self.assertEqual(result.manifest_rejection.path, "$.provides")

    def test_exact_identifier_and_capability_budgets_are_accepted(self) -> None:
        payload = valid_manifest()
        payload["id"] = "x" * MAX_IDENTIFIER_BYTES
        payload["target_capabilities"] = [
            f"capability-{index}" for index in range(MAX_TARGET_CAPABILITIES)
        ]

        self.assertTrue(validate_manifest(encode(payload)).accepted)

    def test_over_identifier_or_capability_budget_is_rejected(self) -> None:
        cases = {
            "identifier": ("id", "x" * (MAX_IDENTIFIER_BYTES + 1)),
            "capabilities": (
                "target_capabilities",
                [f"capability-{index}" for index in range(MAX_TARGET_CAPABILITIES + 1)],
            ),
        }
        for name, (field, value) in cases.items():
            with self.subTest(name=name):
                payload = valid_manifest()
                payload[field] = value
                result = validate_manifest(encode(payload))
                self.assertFalse(result.accepted)


class PluginManifestFailureTests(unittest.TestCase):
    def assert_rejected(
        self,
        payload: str | bytes,
        code: ManifestIssueCode,
        path: str,
    ) -> None:
        result = validate_manifest(payload)
        self.assertFalse(result.accepted)
        self.assertIsNone(result.manifest)
        self.assertEqual(result.entry_rejections, ())
        self.assertIsNotNone(result.manifest_rejection)
        assert result.manifest_rejection is not None
        self.assertEqual(result.manifest_rejection.code, code)
        self.assertEqual(result.manifest_rejection.path, path)

    def test_malformed_json_is_rejected(self) -> None:
        self.assert_rejected(
            '{"id":',
            ManifestIssueCode.MALFORMED_JSON,
            "$",
        )

    def test_duplicate_json_field_is_rejected(self) -> None:
        self.assert_rejected(
            '{"id":"one","id":"two"}',
            ManifestIssueCode.DUPLICATE_FIELD,
            "$",
        )

    def test_non_standard_json_constant_is_rejected(self) -> None:
        self.assert_rejected(
            "NaN",
            ManifestIssueCode.MALFORMED_JSON,
            "$",
        )

    def test_overflowing_json_number_is_rejected(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {"contracts": [{"weight": 0.0}]}
        encoded = encode(payload)

        for value in ("1e400", "-1e400"):
            with self.subTest(value=value):
                self.assert_rejected(
                    encoded.replace("0.0", value),
                    ManifestIssueCode.MALFORMED_JSON,
                    "$",
                )

    def test_oversized_json_integer_returns_a_rejection(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {"contracts": [{"value": 0}]}
        encoded = encode(payload)
        oversized_integer = "9" * 5000

        self.assertLess(len(encoded) + len(oversized_integer), MAX_MANIFEST_BYTES)
        self.assert_rejected(
            encoded.replace('"value":0', f'"value":{oversized_integer}'),
            ManifestIssueCode.MALFORMED_JSON,
            "$",
        )

    def test_unknown_and_missing_top_level_fields_are_rejected(self) -> None:
        unknown = valid_manifest()
        unknown["schema_version"] = 1
        missing = valid_manifest()
        del missing["id"]

        self.assert_rejected(
            encode(unknown),
            ManifestIssueCode.SCHEMA_INVALID,
            "$.schema_version",
        )
        self.assert_rejected(
            encode(missing),
            ManifestIssueCode.SCHEMA_INVALID,
            "$.id",
        )

    def test_spi_version_is_the_v1_schema_version(self) -> None:
        for invalid in (1, "", "2", "1.0"):
            with self.subTest(invalid=invalid):
                payload = valid_manifest()
                payload["spi_version"] = invalid
                self.assert_rejected(
                    encode(payload),
                    ManifestIssueCode.SCHEMA_INVALID,
                    "$.spi_version",
                )

    def test_runtime_abi_has_an_exact_schema(self) -> None:
        cases = {
            "not_object": ([], "$.runtime_abi"),
            "missing": (
                {
                    "python_version": "3.14",
                    "soabi": "cpython-314-aarch64-linux-gnu",
                    "cpu_caps": [],
                },
                "$.runtime_abi.core_build_id",
            ),
            "unknown": (
                {
                    "python_version": "3.14",
                    "soabi": "cpython-314-aarch64-linux-gnu",
                    "core_build_id": "dev",
                    "cpu_caps": [],
                    "architecture": "aarch64",
                },
                "$.runtime_abi.architecture",
            ),
            "bad_scalar": (
                {
                    "python_version": 314,
                    "soabi": "cpython-314-aarch64-linux-gnu",
                    "core_build_id": "dev",
                    "cpu_caps": [],
                },
                "$.runtime_abi.python_version",
            ),
            "bad_cpu_caps": (
                {
                    "python_version": "3.14",
                    "soabi": "cpython-314-aarch64-linux-gnu",
                    "core_build_id": "dev",
                    "cpu_caps": [1],
                },
                "$.runtime_abi.cpu_caps[0]",
            ),
        }
        for name, (runtime_abi, path) in cases.items():
            with self.subTest(name=name):
                payload = valid_manifest()
                payload["runtime_abi"] = runtime_abi
                self.assert_rejected(
                    encode(payload),
                    ManifestIssueCode.SCHEMA_INVALID,
                    path,
                )

    def test_adapter_has_an_exact_schema(self) -> None:
        cases = {
            "not_object": ([], "$.adapter"),
            "missing": ({"entry": "adapter.module"}, "$.adapter.target"),
            "unknown": (
                {
                    "entry": "adapter.module",
                    "target": "framework",
                    "priority": 1,
                },
                "$.adapter.priority",
            ),
            "empty": ({"entry": "", "target": "framework"}, "$.adapter.entry"),
        }
        for name, (adapter, path) in cases.items():
            with self.subTest(name=name):
                payload = valid_manifest()
                payload["adapter"] = adapter
                self.assert_rejected(
                    encode(payload),
                    ManifestIssueCode.SCHEMA_INVALID,
                    path,
                )

    def test_target_capabilities_must_be_bounded_strings(self) -> None:
        cases = {
            "not_list": ("plugin-manifest-v1", "$.target_capabilities"),
            "not_string": ([1], "$.target_capabilities[0]"),
            "empty": ([""], "$.target_capabilities[0]"),
        }
        for name, (capabilities, path) in cases.items():
            with self.subTest(name=name):
                payload = valid_manifest()
                payload["target_capabilities"] = capabilities
                self.assert_rejected(
                    encode(payload),
                    ManifestIssueCode.SCHEMA_INVALID,
                    path,
                )

    def test_provides_has_an_exact_field_whitelist(self) -> None:
        payload = valid_manifest()
        payload["provides"]["entry_points"] = ["must.not.load"]

        self.assert_rejected(
            encode(payload),
            ManifestIssueCode.SCHEMA_INVALID,
            "$.provides.entry_points",
        )


class PluginManifestEntryIsolationTests(unittest.TestCase):
    def test_bad_entries_are_excluded_while_valid_siblings_survive(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": [{"valid": {"items": [1, 2]}}, "invalid"],
            "policies": [None, {"valid": True}],
            "seeds": [
                "seeds/valid.json",
                "../escape.json",
                "/absolute.json",
                "",
                "windows\\path.json",
            ],
            "diagnostics": [],
            "pass": "invalid",
        }

        result = validate_manifest(encode(payload))

        self.assertTrue(result.accepted)
        self.assertIsNone(result.manifest_rejection)
        manifest = result.manifest
        assert manifest is not None
        self.assertEqual(len(manifest.provides.contracts), 1)
        self.assertEqual(len(manifest.provides.policies), 1)
        self.assertEqual(manifest.provides.seeds, ("seeds/valid.json",))
        self.assertIsNone(manifest.provides.diagnostics)
        self.assertIsNone(manifest.provides.pass_config)
        self.assertEqual(
            tuple(issue.path for issue in result.entry_rejections),
            (
                "$.provides.contracts[1]",
                "$.provides.policies[0]",
                "$.provides.seeds[1]",
                "$.provides.seeds[2]",
                "$.provides.seeds[3]",
                "$.provides.seeds[4]",
                "$.provides.diagnostics",
                "$.provides.pass",
            ),
        )
        self.assertTrue(
            all(
                issue.code is ManifestIssueCode.ENTRY_INVALID
                for issue in result.entry_rejections
            )
        )

    def test_invalid_list_section_is_an_entry_rejection_not_manifest_failure(
        self,
    ) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": {},
            "policies": [{"valid": True}],
            "seeds": "seeds/not-a-list.json",
        }

        result = validate_manifest(encode(payload))

        self.assertTrue(result.accepted)
        manifest = result.manifest
        assert manifest is not None
        self.assertEqual(manifest.provides.contracts, ())
        self.assertEqual(len(manifest.provides.policies), 1)
        self.assertEqual(manifest.provides.seeds, ())
        self.assertEqual(
            tuple(issue.path for issue in result.entry_rejections),
            ("$.provides.contracts", "$.provides.seeds"),
        )

    def test_freeze_recursion_is_isolated_to_each_provider_entry(self) -> None:
        payload = valid_manifest()
        payload["provides"] = {
            "contracts": [{"too_deep": True}, {"valid": True}],
            "policies": [{"valid": True}],
            "diagnostics": {"too_deep": True},
        }
        real_freeze = manifest_module._freeze_json

        def freeze_or_recurse(value: Any) -> object:
            if isinstance(value, dict) and value.get("too_deep") is True:
                raise RecursionError
            return real_freeze(value)

        with patch.object(
            manifest_module,
            "_freeze_json",
            side_effect=freeze_or_recurse,
        ):
            result = validate_manifest(encode(payload))

        self.assertTrue(result.accepted)
        manifest = result.manifest
        assert manifest is not None
        self.assertEqual(len(manifest.provides.contracts), 1)
        self.assertEqual(len(manifest.provides.policies), 1)
        self.assertIsNone(manifest.provides.diagnostics)
        self.assertEqual(
            tuple(issue.path for issue in result.entry_rejections),
            ("$.provides.contracts[0]", "$.provides.diagnostics"),
        )


if __name__ == "__main__":
    unittest.main()
