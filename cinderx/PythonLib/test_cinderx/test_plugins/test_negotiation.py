# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import json
import sys
import unittest
from dataclasses import FrozenInstanceError, replace
from types import ModuleType
from unittest.mock import Mock, patch

from cinderx.plugins import (
    MAX_NEGOTIATION_DETAIL_CHARS,
    MAX_NEGOTIATION_DETAIL_VALUES,
    NegotiationIssue,
    NegotiationReason,
    PluginDiscoveryResult,
    RuntimeFingerprint,
    negotiate,
)
from cinderx.plugins.manifest import validate_manifest


def discovered_plugin(
    distribution_name: str,
    plugin_id: str,
    *,
    spi_version: str = "1",
    python_version: str = "3.14",
    soabi: str = "cpython-314-aarch64-linux-gnu",
    core_build_id: str = "cinderx-dev",
    cpu_caps: tuple[str, ...] = ("neon",),
    target_capabilities: tuple[str, ...] = ("plugin-manifest-v1",),
    adapter_entry: str | None = None,
    adapter_target: str | None = None,
) -> PluginDiscoveryResult:
    payload: dict[str, object] = {
        "id": plugin_id,
        "spi_version": spi_version,
        "runtime_abi": {
            "python_version": python_version,
            "soabi": soabi,
            "core_build_id": core_build_id,
            "cpu_caps": list(cpu_caps),
        },
        "target_capabilities": list(target_capabilities),
        "provides": {"policies": [], "seeds": []},
    }
    if adapter_entry is not None and adapter_target is not None:
        payload["adapter"] = {
            "entry": adapter_entry,
            "target": adapter_target,
        }
    validation = validate_manifest(
        json.dumps(payload, separators=(",", ":")),
        allow_unsupported_spi=True,
    )
    assert validation.accepted
    return PluginDiscoveryResult(
        distribution_name=distribution_name,
        normalized_distribution_name=distribution_name,
        distribution_version="1.0",
        validation=validation,
        discovery_issue=None,
    )


def compatible_runtime() -> RuntimeFingerprint:
    return RuntimeFingerprint(
        spi_version="1",
        python_version="3.14",
        soabi="cpython-314-aarch64-linux-gnu",
        core_build_id="cinderx-dev",
        cpu_caps=("neon", "sve"),
        target_capabilities=("plugin-manifest-v1", "plugin-diagnostics-v1"),
    )


class PluginNegotiationTests(unittest.TestCase):
    def test_reason_values_are_stable_diagnostic_identifiers(self) -> None:
        self.assertEqual(
            tuple(reason.value for reason in NegotiationReason),
            (
                "spi_mismatch",
                "py_mismatch",
                "soabi_mismatch",
                "build_mismatch",
                "cpu_insufficient",
                "capability_missing",
                "id_conflict",
            ),
        )

    def test_current_fingerprint_accepts_injected_native_capabilities(self) -> None:
        with patch(
            "cinderx.plugins.negotiation.sysconfig.get_config_var",
            return_value="test-soabi",
        ):
            fingerprint = RuntimeFingerprint.from_current(
                native_capabilities={
                    "core_build_id": "injected-build",
                    "cpu_caps": ("sve", "neon", "sve"),
                    "numa_node_count": 4,
                },
                target_capabilities=("zeta", "alpha", "zeta"),
            )

        self.assertEqual(
            fingerprint.python_version,
            f"{sys.version_info.major}.{sys.version_info.minor}",
        )
        self.assertEqual(fingerprint.soabi, "test-soabi")
        self.assertEqual(fingerprint.core_build_id, "injected-build")
        self.assertEqual(fingerprint.cpu_caps, ("neon", "sve"))
        self.assertEqual(fingerprint.target_capabilities, ("alpha", "zeta"))

    def test_current_fingerprint_uses_default_native_module_path(self) -> None:
        native_module = ModuleType("_cinderx")
        get_runtime_capabilities = Mock(
            return_value={
                "core_build_id": "native-build",
                "cpu_caps": ("asimd", "sve"),
            }
        )
        native_module.get_runtime_capabilities = (  # type: ignore[attr-defined]
            get_runtime_capabilities
        )

        with patch.dict(sys.modules, {"_cinderx": native_module}), patch(
            "cinderx.plugins.negotiation.sysconfig.get_config_var",
            return_value="native-soabi",
        ):
            fingerprint = RuntimeFingerprint.from_current()

        get_runtime_capabilities.assert_called_once_with()
        self.assertEqual(fingerprint.core_build_id, "native-build")
        self.assertEqual(fingerprint.cpu_caps, ("asimd", "sve"))
        self.assertEqual(fingerprint.soabi, "native-soabi")

    def test_compatible_plugin_is_available_and_results_are_immutable(self) -> None:
        discovery = discovered_plugin("example", "example")

        results = negotiate((discovery,), runtime=compatible_runtime())

        self.assertIsInstance(results, tuple)
        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].available)
        self.assertIs(results[0].discovery, discovery)
        self.assertEqual(results[0].reasons, ())
        with self.assertRaises(FrozenInstanceError):
            results[0].reasons = ()  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            results[0].discovery = discovery  # type: ignore[misc]

    def test_each_compatibility_axis_has_a_stable_reason(self) -> None:
        cases = (
            (
                "spi",
                discovered_plugin("example", "example"),
                replace(compatible_runtime(), spi_version="2"),
                NegotiationReason.SPI_MISMATCH,
            ),
            (
                "python",
                discovered_plugin("example", "example"),
                replace(compatible_runtime(), python_version="3.11"),
                NegotiationReason.PYTHON_VERSION_MISMATCH,
            ),
            (
                "soabi",
                discovered_plugin("example", "example"),
                replace(compatible_runtime(), soabi="cpython-314-x86_64-linux-gnu"),
                NegotiationReason.SOABI_MISMATCH,
            ),
            (
                "core_build_id",
                discovered_plugin("example", "example"),
                replace(compatible_runtime(), core_build_id="another-build"),
                NegotiationReason.CORE_BUILD_ID_MISMATCH,
            ),
            (
                "cpu",
                discovered_plugin(
                    "example",
                    "example",
                    cpu_caps=("neon", "sve2"),
                ),
                compatible_runtime(),
                NegotiationReason.CPU_CAPABILITY_MISSING,
            ),
            (
                "target",
                discovered_plugin(
                    "example",
                    "example",
                    target_capabilities=("plugin-manifest-v1", "column-layout-v1"),
                ),
                compatible_runtime(),
                NegotiationReason.TARGET_CAPABILITY_MISSING,
            ),
        )
        for name, discovery, runtime, expected_reason in cases:
            with self.subTest(name=name):
                result = negotiate((discovery,), runtime=runtime)[0]
                self.assertFalse(result.available)
                self.assertEqual(
                    tuple(issue.reason for issue in result.reasons),
                    (expected_reason,),
                )
                self.assertTrue(
                    all(len(issue.expected) <= 64 for issue in result.reasons)
                )
                self.assertTrue(
                    all(len(issue.actual) <= 64 for issue in result.reasons)
                )

    def test_negotiation_issue_bounds_expected_and_actual_details(self) -> None:
        issue = NegotiationIssue(
            NegotiationReason.CPU_CAPABILITY_MISSING,
            tuple(
                f"expected-{index:02d}-" + "x" * MAX_NEGOTIATION_DETAIL_CHARS
                for index in range(MAX_NEGOTIATION_DETAIL_VALUES + 1)
            ),
            tuple(
                f"actual-{index:02d}-" + "y" * MAX_NEGOTIATION_DETAIL_CHARS
                for index in range(MAX_NEGOTIATION_DETAIL_VALUES + 1)
            ),
        )

        self.assertEqual(len(issue.expected), MAX_NEGOTIATION_DETAIL_VALUES)
        self.assertEqual(len(issue.actual), MAX_NEGOTIATION_DETAIL_VALUES)
        self.assertTrue(
            all(
                len(value) == MAX_NEGOTIATION_DETAIL_CHARS
                for value in issue.expected
            )
        )
        self.assertTrue(
            all(
                len(value) == MAX_NEGOTIATION_DETAIL_CHARS
                for value in issue.actual
            )
        )

    def test_multiple_mismatches_retain_documented_decision_order(self) -> None:
        discovery = discovered_plugin(
            "example",
            "example",
            cpu_caps=("sve2",),
            target_capabilities=("column-layout-v1",),
        )
        runtime = RuntimeFingerprint(
            spi_version="2",
            python_version="3.11",
            soabi="cpython-311-x86_64-linux-gnu",
            core_build_id="another-build",
            cpu_caps=("avx2",),
            target_capabilities=("plugin-manifest-v1",),
        )

        result = negotiate((discovery,), runtime=runtime)[0]

        self.assertEqual(
            tuple(issue.reason for issue in result.reasons),
            (
                NegotiationReason.SPI_MISMATCH,
                NegotiationReason.PYTHON_VERSION_MISMATCH,
                NegotiationReason.SOABI_MISMATCH,
                NegotiationReason.CORE_BUILD_ID_MISMATCH,
                NegotiationReason.CPU_CAPABILITY_MISSING,
                NegotiationReason.TARGET_CAPABILITY_MISSING,
            ),
        )

    def test_first_compatible_duplicate_id_wins(self) -> None:
        first = discovered_plugin("alpha", "duplicate")
        second = discovered_plugin("zeta", "duplicate")

        results = negotiate((first, second), runtime=compatible_runtime())

        self.assertTrue(results[0].available)
        self.assertFalse(results[1].available)
        self.assertEqual(
            tuple(issue.reason for issue in results[1].reasons),
            (NegotiationReason.PLUGIN_ID_CONFLICT,),
        )

    def test_conflict_is_last_after_compatibility_issues(self) -> None:
        owner = discovered_plugin("alpha", "duplicate")
        conflicting = discovered_plugin(
            "zeta",
            "duplicate",
            python_version="3.11",
            core_build_id="wrong-build",
        )

        results = negotiate((owner, conflicting), runtime=compatible_runtime())

        self.assertTrue(results[0].available)
        self.assertEqual(
            tuple(issue.reason for issue in results[1].reasons),
            (
                NegotiationReason.PYTHON_VERSION_MISMATCH,
                NegotiationReason.CORE_BUILD_ID_MISMATCH,
                NegotiationReason.PLUGIN_ID_CONFLICT,
            ),
        )

    def test_incompatible_candidate_does_not_reserve_duplicate_id(self) -> None:
        incompatible = discovered_plugin(
            "alpha",
            "duplicate",
            core_build_id="wrong-build",
        )
        compatible = discovered_plugin("zeta", "duplicate")

        results = negotiate((incompatible, compatible), runtime=compatible_runtime())

        self.assertFalse(results[0].available)
        self.assertEqual(
            tuple(issue.reason for issue in results[0].reasons),
            (NegotiationReason.CORE_BUILD_ID_MISMATCH,),
        )
        self.assertTrue(results[1].available)
        self.assertEqual(results[1].reasons, ())

    def test_mismatch_isolated_to_one_plugin_and_siblings_continue(self) -> None:
        candidates = (
            discovered_plugin("alpha", "alpha"),
            discovered_plugin("middle", "middle", python_version="3.11"),
            discovered_plugin("zeta", "zeta"),
        )

        results = negotiate(candidates, runtime=compatible_runtime())

        self.assertEqual([result.available for result in results], [True, False, True])
        self.assertEqual(results[0].reasons, ())
        self.assertEqual(
            tuple(issue.reason for issue in results[1].reasons),
            (NegotiationReason.PYTHON_VERSION_MISMATCH,),
        )
        self.assertEqual(results[2].reasons, ())

    def test_unavailable_discovery_does_not_reserve_id_or_stop_siblings(
        self,
    ) -> None:
        unavailable = PluginDiscoveryResult(
            distribution_name="middle",
            normalized_distribution_name="middle",
            distribution_version="1.0",
            validation=validate_manifest('{"id":"shared"}'),
            discovery_issue=None,
        )
        candidates = (
            discovered_plugin("alpha", "alpha"),
            unavailable,
            discovered_plugin("zeta", "shared"),
        )

        results = negotiate(candidates, runtime=compatible_runtime())

        self.assertEqual([result.available for result in results], [True, False, True])
        self.assertEqual(results[0].reasons, ())
        self.assertEqual(results[1].reasons, ())
        self.assertEqual(results[2].reasons, ())

    def test_neon_manifest_matches_native_asimd_without_losing_diagnostics(
        self,
    ) -> None:
        runtime = replace(compatible_runtime(), cpu_caps=("asimd", "sve"))

        compatible = negotiate(
            (discovered_plugin("compatible", "compatible", cpu_caps=("neon",)),),
            runtime=runtime,
        )[0]
        mismatched = negotiate(
            (
                discovered_plugin(
                    "mismatched",
                    "mismatched",
                    cpu_caps=("neon", "sve2"),
                ),
            ),
            runtime=runtime,
        )[0]

        self.assertTrue(compatible.available)
        self.assertEqual(
            mismatched.reasons[0].reason,
            NegotiationReason.CPU_CAPABILITY_MISSING,
        )
        self.assertEqual(mismatched.reasons[0].expected, ("neon", "sve2"))
        self.assertEqual(mismatched.reasons[0].actual, ("asimd", "sve"))

    def test_alternate_manifest_spi_emits_spi_mismatch(self) -> None:
        discovery = discovered_plugin("alternate", "alternate", spi_version="2")

        result = negotiate((discovery,), runtime=compatible_runtime())[0]

        self.assertFalse(result.available)
        self.assertEqual(
            tuple(issue.reason for issue in result.reasons),
            (NegotiationReason.SPI_MISMATCH,),
        )

    def test_negotiation_never_imports_adapter_or_target(self) -> None:
        adapter_entry = "negotiation_test_never_imported.adapter"
        adapter_target = "negotiation_test_never_imported_target"
        discovery = discovered_plugin(
            "example",
            "example",
            adapter_entry=adapter_entry,
            adapter_target=adapter_target,
        )

        result = negotiate((discovery,), runtime=compatible_runtime())[0]

        self.assertTrue(result.available)
        self.assertNotIn("negotiation_test_never_imported", sys.modules)
        self.assertNotIn(adapter_entry, sys.modules)
        self.assertNotIn(adapter_target, sys.modules)


if __name__ == "__main__":
    unittest.main()
