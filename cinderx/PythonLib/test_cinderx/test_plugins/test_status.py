# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import builtins
import json
import threading
import unittest
from dataclasses import FrozenInstanceError
from unittest.mock import patch

from cinderx.plugins import (
    ArtifactIssue,
    ArtifactIssueCode,
    ArtifactVerificationResult,
    MAX_STATUS_DETAILS_PER_DIAGNOSTIC,
    MAX_STATUS_DIAGNOSTICS_PER_PLUGIN,
    MAX_STATUS_PLUGINS,
    MAX_STATUS_STRING_BYTES,
    NegotiationIssue,
    NegotiationReason,
    PluginDiscoveryResult,
    PluginNegotiationResult,
    PluginState,
    PluginStatus,
    PluginStatusReason,
    StageDiagnostic,
    StatusSnapshot,
    build_status_snapshot,
    install_status_snapshot,
    status,
    update_status,
)
from cinderx.plugins.manifest import validate_manifest
from cinderx.plugins.status import _materialize_diagnostic


def discovered_plugin(
    distribution_name: str,
    plugin_id: str | None = None,
) -> PluginDiscoveryResult:
    plugin_id = distribution_name if plugin_id is None else plugin_id
    validation = validate_manifest(
        json.dumps(
            {
                "id": plugin_id,
                "spi_version": "1",
                "runtime_abi": {
                    "python_version": "3.14",
                    "soabi": "cpython-314-aarch64-linux-gnu",
                    "core_build_id": "cinderx-dev",
                    "cpu_caps": ["neon"],
                },
                "target_capabilities": ["plugin-manifest-v1"],
                "provides": {"policies": [], "seeds": []},
            },
            separators=(",", ":"),
        ),
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


def negotiation(
    discovery: PluginDiscoveryResult,
    *reasons: NegotiationReason,
) -> PluginNegotiationResult:
    return PluginNegotiationResult(
        discovery,
        tuple(NegotiationIssue(reason) for reason in reasons),
    )


def artifact(
    distribution_name: str,
    issue_code: ArtifactIssueCode | None = None,
    *,
    version: str = "1.0",
) -> ArtifactVerificationResult:
    issue = (
        ArtifactIssue(
            issue_code,
            distribution_name,
            "sensitive/path/to/native.so",
            "bounded source message",
        )
        if issue_code is not None
        else None
    )
    return ArtifactVerificationResult(
        root_distribution_name=distribution_name,
        accepted=issue is None,
        issue=issue,
        closure=(distribution_name,),
        inspected_file_records=1,
        root_distribution_version=version,
    )


class PluginStatusTests(unittest.TestCase):
    def test_public_enums_have_exact_stable_values(self) -> None:
        self.assertEqual(
            tuple(state.value for state in PluginState),
            ("discovered", "available", "unavailable"),
        )
        self.assertEqual(
            tuple(reason.value for reason in PluginStatusReason),
            (
                "spi_mismatch",
                "py_mismatch",
                "soabi_mismatch",
                "build_mismatch",
                "cpu_insufficient",
                "capability_missing",
                "id_conflict",
                "schema_invalid",
                "native_in_closure",
            ),
        )

    def test_discovery_only_and_available_happy_path(self) -> None:
        discovery = discovered_plugin("Example.Plugin", "example")

        discovered = build_status_snapshot((discovery,))
        available = build_status_snapshot(
            (discovery,),
            negotiation_results=(negotiation(discovery),),
            artifact_results=(artifact("example-plugin"),),
        )

        self.assertEqual(discovered.plugins[0].state, PluginState.DISCOVERED)
        self.assertEqual(available.plugins[0].state, PluginState.AVAILABLE)
        self.assertEqual(available.plugins[0].plugin_id, "example")
        self.assertEqual(available.plugins[0].reasons, ())

    def test_each_public_reason_is_mapped(self) -> None:
        cases = tuple(
            (reason, PluginStatusReason(reason.value))
            for reason in NegotiationReason
        )
        for source_reason, public_reason in cases:
            with self.subTest(reason=source_reason.value):
                discovery = discovered_plugin("example")
                snapshot = build_status_snapshot(
                    (discovery,),
                    negotiation_results=(
                        negotiation(discovery, source_reason),
                    ),
                )
                self.assertEqual(
                    snapshot.plugins[0].reasons,
                    (public_reason,),
                )

        rejected = PluginDiscoveryResult(
            distribution_name="invalid",
            normalized_distribution_name="invalid",
            distribution_version="1.0",
            validation=validate_manifest('{"id":"invalid"}'),
            discovery_issue=None,
        )
        schema_snapshot = build_status_snapshot((rejected,))
        self.assertEqual(
            schema_snapshot.plugins[0].reasons,
            (PluginStatusReason.SCHEMA_INVALID,),
        )

        native_snapshot = build_status_snapshot(
            (discovered_plugin("native"),),
            artifact_results=(
                artifact("native", ArtifactIssueCode.NATIVE_IN_CLOSURE),
            ),
        )
        self.assertEqual(
            native_snapshot.plugins[0].reasons,
            (PluginStatusReason.NATIVE_IN_CLOSURE,),
        )

    def test_artifact_fail_closed_family_retains_lower_level_code_only(self) -> None:
        discovery = discovered_plugin("example")
        snapshot = build_status_snapshot(
            (discovery,),
            negotiation_results=(negotiation(discovery),),
            artifact_results=(
                artifact("example", ArtifactIssueCode.HASH_MISMATCH),
            ),
        )

        plugin = snapshot.plugins[0]
        self.assertEqual(
            plugin.reasons,
            (PluginStatusReason.NATIVE_IN_CLOSURE,),
        )
        artifact_diagnostic = next(
            detail for detail in plugin.diagnostics if detail.stage == "artifact"
        )
        self.assertEqual(
            artifact_diagnostic.code,
            ArtifactIssueCode.HASH_MISMATCH.value,
        )
        self.assertNotIn("sensitive/path", repr(artifact_diagnostic))

    def test_normalized_identity_correlation_sorting_and_reason_dedup(self) -> None:
        zeta = discovered_plugin("Zeta.Plugin", "zeta")
        alpha = discovered_plugin("alpha_plugin", "alpha")
        snapshot = build_status_snapshot(
            (zeta, alpha),
            negotiation_results=(
                negotiation(
                    zeta,
                    NegotiationReason.PLUGIN_ID_CONFLICT,
                    NegotiationReason.SPI_MISMATCH,
                    NegotiationReason.SPI_MISMATCH,
                ),
                negotiation(alpha),
            ),
            artifact_results=(
                artifact("zeta-plugin", ArtifactIssueCode.RECORD_INVALID),
                artifact("alpha.plugin"),
            ),
        )

        self.assertEqual(
            tuple(plugin.normalized_distribution_name for plugin in snapshot.plugins),
            ("alpha-plugin", "zeta-plugin"),
        )
        self.assertEqual(snapshot.plugins[0].state, PluginState.AVAILABLE)
        self.assertEqual(
            snapshot.plugins[1].reasons,
            (
                PluginStatusReason.SPI_MISMATCH,
                PluginStatusReason.PLUGIN_ID_CONFLICT,
                PluginStatusReason.NATIVE_IN_CLOSURE,
            ),
        )

    def test_unavailable_discovery_with_empty_negotiation_is_not_malformed(
        self,
    ) -> None:
        rejected = PluginDiscoveryResult(
            distribution_name="rejected",
            normalized_distribution_name="rejected",
            distribution_version="1.0",
            validation=validate_manifest("{}"),
            discovery_issue=None,
        )

        snapshot = build_status_snapshot(
            (rejected,),
            negotiation_results=(PluginNegotiationResult(rejected),),
        )

        self.assertEqual(
            snapshot.plugins[0].reasons,
            (PluginStatusReason.SCHEMA_INVALID,),
        )
        self.assertNotIn(
            "invalid_negotiation_result",
            tuple(item.code for item in snapshot.plugins[0].diagnostics),
        )

    def test_bounded_diagnostic_selection_is_input_order_independent(self) -> None:
        diagnostics = tuple(
            StageDiagnostic("status", f"code-{index:02d}")
            for index in range(MAX_STATUS_DIAGNOSTICS_PER_PLUGIN + 1)
        )
        forward = PluginStatus(
            "example",
            "example",
            "1",
            "example",
            PluginState.UNAVAILABLE,
            diagnostics=diagnostics,
        )
        reverse = PluginStatus(
            "example",
            "example",
            "1",
            "example",
            PluginState.UNAVAILABLE,
            diagnostics=tuple(reversed(diagnostics)),
        )

        self.assertEqual(forward.diagnostics, reverse.diagnostics)
        self.assertEqual(
            len(forward.diagnostics),
            MAX_STATUS_DIAGNOSTICS_PER_PLUGIN,
        )

    def test_stage_diagnostic_reduction_is_order_independent_past_cap(
        self,
    ) -> None:
        discovery = discovered_plugin("example")
        stage_results = tuple(
            PluginNegotiationResult(
                discovery,
                (
                    NegotiationIssue(
                        NegotiationReason.SPI_MISMATCH,
                        expected=(f"expected-{index:03d}",),
                    ),
                ),
            )
            for index in range(MAX_STATUS_DIAGNOSTICS_PER_PLUGIN + 8)
        )

        with patch(
            "cinderx.plugins.status._materialize_diagnostic",
            wraps=_materialize_diagnostic,
        ) as materialize:
            forward = build_status_snapshot(
                (discovery,), negotiation_results=stage_results
            )
        reverse = build_status_snapshot(
            (discovery,),
            negotiation_results=tuple(reversed(stage_results)),
        )

        self.assertEqual(
            forward.plugins[0].diagnostics,
            reverse.plugins[0].diagnostics,
        )
        self.assertEqual(
            len(forward.plugins[0].diagnostics),
            MAX_STATUS_DIAGNOSTICS_PER_PLUGIN,
        )
        self.assertLessEqual(
            materialize.call_count,
            MAX_STATUS_DIAGNOSTICS_PER_PLUGIN,
        )
        self.assertEqual(
            forward.plugins[0].reasons,
            (
                PluginStatusReason.SPI_MISMATCH,
                PluginStatusReason.SCHEMA_INVALID,
            ),
        )

    def test_stale_and_duplicate_negotiation_results_fail_closed(self) -> None:
        discovery = discovered_plugin("example")
        stale = PluginDiscoveryResult(
            distribution_name=discovery.distribution_name,
            normalized_distribution_name=discovery.normalized_distribution_name,
            distribution_version="0.9",
            validation=discovery.validation,
            discovery_issue=discovery.discovery_issue,
        )
        cases = {
            "stale": (negotiation(stale),),
            "duplicate": (negotiation(discovery), negotiation(discovery)),
        }

        for name, results in cases.items():
            with self.subTest(name=name):
                plugin = build_status_snapshot(
                    (discovery,), negotiation_results=results
                ).plugins[0]
                self.assertEqual(plugin.state, PluginState.UNAVAILABLE)
                self.assertEqual(
                    plugin.reasons,
                    (PluginStatusReason.SCHEMA_INVALID,),
                )
                self.assertIn(
                    "invalid_negotiation_result",
                    tuple(item.code for item in plugin.diagnostics),
                )

    def test_stale_and_duplicate_artifact_results_fail_closed(self) -> None:
        discovery = discovered_plugin("example")
        cases = {
            "stale-version": (artifact("example", version="0.9"),),
            "duplicate": (artifact("example"), artifact("example")),
        }

        for name, results in cases.items():
            with self.subTest(name=name):
                plugin = build_status_snapshot(
                    (discovery,),
                    negotiation_results=(negotiation(discovery),),
                    artifact_results=results,
                ).plugins[0]
                self.assertEqual(plugin.state, PluginState.UNAVAILABLE)
                self.assertEqual(
                    plugin.reasons,
                    (PluginStatusReason.SCHEMA_INVALID,),
                )
                self.assertIn(
                    "invalid_artifact_result",
                    tuple(item.code for item in plugin.diagnostics),
                )

    def test_nonbool_and_inconsistent_artifact_results_are_malformed(
        self,
    ) -> None:
        discovery = discovered_plugin("example")
        rejected = artifact("example", ArtifactIssueCode.HASH_MISMATCH)
        malformed = (
            ArtifactVerificationResult(
                "example",
                1,  # type: ignore[arg-type]
                None,
                ("example",),
                1,
                "1.0",
            ),
            ArtifactVerificationResult(
                "example",
                True,
                rejected.issue,
                ("example",),
                1,
                "1.0",
            ),
            ArtifactVerificationResult(
                "example",
                False,
                None,
                ("example",),
                1,
                "1.0",
            ),
        )

        for result in malformed:
            with self.subTest(accepted=result.accepted, issue=result.issue):
                plugin = build_status_snapshot(
                    (discovery,),
                    negotiation_results=(negotiation(discovery),),
                    artifact_results=(result,),
                ).plugins[0]
                self.assertEqual(plugin.state, PluginState.UNAVAILABLE)
                self.assertEqual(
                    plugin.reasons,
                    (PluginStatusReason.SCHEMA_INVALID,),
                )
                self.assertNotIn(
                    PluginStatusReason.NATIVE_IN_CLOSURE,
                    plugin.reasons,
                )
                self.assertIn(
                    "invalid_artifact_result",
                    tuple(item.code for item in plugin.diagnostics),
                )

    def test_long_identity_prefixes_do_not_cross_correlate(self) -> None:
        common = "x" * (MAX_STATUS_STRING_BYTES + 20)
        alpha = discovered_plugin(common + "alpha", "alpha")
        zeta = discovered_plugin(common + "zeta", "zeta")

        snapshot = build_status_snapshot(
            (alpha, zeta),
            negotiation_results=(
                negotiation(alpha, NegotiationReason.SPI_MISMATCH),
                negotiation(zeta),
            ),
            artifact_results=(artifact(common + "alpha"), artifact(common + "zeta")),
        )

        by_plugin_id = {plugin.plugin_id: plugin for plugin in snapshot.plugins}
        self.assertEqual(
            by_plugin_id["alpha"].reasons,
            (PluginStatusReason.SPI_MISMATCH,),
        )
        self.assertEqual(by_plugin_id["zeta"].reasons, ())

    def test_public_reason_iteration_is_bounded(self) -> None:
        yielded = 0

        def reasons():
            nonlocal yielded
            while True:
                yielded += 1
                yield PluginStatusReason.SCHEMA_INVALID

        plugin = PluginStatus(
            "example",
            "example",
            "1",
            "example",
            PluginState.UNAVAILABLE,
            reasons=reasons(),  # type: ignore[arg-type]
        )

        self.assertEqual(
            plugin.reasons,
            (PluginStatusReason.SCHEMA_INVALID,),
        )
        self.assertLessEqual(yielded, MAX_STATUS_PLUGINS * 4)

    def test_duplicate_discovery_identity_fails_closed_without_fanout(self) -> None:
        first = discovered_plugin("duplicate", "first")
        second = discovered_plugin("duplicate", "second")

        snapshot = build_status_snapshot(
            (first, second),
            negotiation_results=(
                negotiation(first, NegotiationReason.SPI_MISMATCH),
                negotiation(second),
            ),
        )

        self.assertEqual(len(snapshot.plugins), 2)
        self.assertTrue(
            all(
                plugin.reasons == (PluginStatusReason.SCHEMA_INVALID,)
                for plugin in snapshot.plugins
            )
        )
        self.assertTrue(
            all(
                tuple(item.code for item in plugin.diagnostics)
                == ("duplicate_status_identity",)
                for plugin in snapshot.plugins
            )
        )

    def test_malformed_input_is_isolated_from_valid_siblings(self) -> None:
        alpha = discovered_plugin("alpha")
        zeta = discovered_plugin("zeta")

        snapshot = build_status_snapshot(
            (zeta, object(), alpha)  # type: ignore[arg-type]
        )

        by_name = {plugin.distribution_name: plugin for plugin in snapshot.plugins}
        self.assertEqual(by_name["alpha"].state, PluginState.DISCOVERED)
        self.assertEqual(by_name["zeta"].state, PluginState.DISCOVERED)
        malformed = next(
            plugin
            for plugin in snapshot.plugins
            if plugin.distribution_name not in {"alpha", "zeta"}
        )
        self.assertEqual(malformed.state, PluginState.UNAVAILABLE)
        self.assertEqual(
            malformed.reasons,
            (PluginStatusReason.SCHEMA_INVALID,),
        )

    def test_plugins_diagnostics_details_and_strings_are_bounded(self) -> None:
        oversized = "\N{SNOWMAN}" * (MAX_STATUS_STRING_BYTES + 1)
        diagnostic = StageDiagnostic(
            stage=oversized,
            code=oversized,
            message=oversized,
            expected=tuple(
                oversized for _ in range(MAX_STATUS_DETAILS_PER_DIAGNOSTIC + 1)
            ),
            actual=tuple(
                oversized for _ in range(MAX_STATUS_DETAILS_PER_DIAGNOSTIC + 1)
            ),
        )
        plugin = PluginStatus(
            distribution_name=oversized,
            normalized_distribution_name=oversized,
            distribution_version=oversized,
            plugin_id=oversized,
            state=PluginState.UNAVAILABLE,
            reasons=(PluginStatusReason.SCHEMA_INVALID,),
            diagnostics=tuple(
                StageDiagnostic(
                    stage=diagnostic.stage,
                    code=f"{index}:{diagnostic.code}",
                    message=diagnostic.message,
                    expected=diagnostic.expected,
                    actual=diagnostic.actual,
                )
                for index in range(MAX_STATUS_DIAGNOSTICS_PER_PLUGIN + 1)
            ),
        )
        snapshot = StatusSnapshot(
            tuple(plugin for _ in range(MAX_STATUS_PLUGINS + 1))
        )

        self.assertEqual(len(snapshot.plugins), MAX_STATUS_PLUGINS)
        bounded = snapshot.plugins[0]
        self.assertEqual(
            len(bounded.diagnostics), MAX_STATUS_DIAGNOSTICS_PER_PLUGIN
        )
        bounded_diagnostic = bounded.diagnostics[0]
        self.assertEqual(
            len(bounded_diagnostic.expected),
            MAX_STATUS_DETAILS_PER_DIAGNOSTIC,
        )
        self.assertEqual(
            len(bounded_diagnostic.actual),
            MAX_STATUS_DETAILS_PER_DIAGNOSTIC,
        )
        for value in (
            bounded.distribution_name,
            bounded.normalized_distribution_name,
            bounded.distribution_version,
            bounded.plugin_id,
            bounded_diagnostic.stage,
            bounded_diagnostic.code,
            bounded_diagnostic.message,
            *bounded_diagnostic.expected,
            *bounded_diagnostic.actual,
        ):
            assert value is not None
            self.assertLessEqual(
                len(value.encode("utf-8")),
                MAX_STATUS_STRING_BYTES,
            )

    def test_snapshot_is_recursively_immutable(self) -> None:
        snapshot = build_status_snapshot((discovered_plugin("example"),))

        with self.assertRaises(FrozenInstanceError):
            snapshot.plugins = ()  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            snapshot.plugins[0].state = PluginState.AVAILABLE  # type: ignore[misc]

    def test_atomic_install_exposes_only_old_or_new_snapshot(self) -> None:
        old = StatusSnapshot(
            (
                PluginStatus(
                    "old", "old", "1", "old", PluginState.DISCOVERED
                ),
            )
        )
        new = StatusSnapshot(
            (
                PluginStatus(
                    "new", "new", "1", "new", PluginState.AVAILABLE
                ),
            )
        )
        prior = status()
        install_status_snapshot(old)
        observed: list[StatusSnapshot] = []
        barrier = threading.Barrier(2)

        def reader() -> None:
            barrier.wait()
            for _ in range(2_000):
                observed.append(status())

        thread = threading.Thread(target=reader)
        try:
            thread.start()
            barrier.wait()
            install_status_snapshot(new)
            thread.join()
        finally:
            install_status_snapshot(prior)

        self.assertTrue(observed)
        self.assertTrue(all(item is old or item is new for item in observed))

    def test_update_installs_and_status_query_performs_no_import_or_io(self) -> None:
        discovery = discovered_plugin("example")
        prior = status()
        try:
            snapshot = update_status(
                (discovery,),
                negotiation_results=(negotiation(discovery),),
            )
            with patch.object(
                builtins,
                "__import__",
                side_effect=AssertionError("status imported a module"),
            ), patch.object(
                builtins,
                "open",
                side_effect=AssertionError("status performed file I/O"),
            ):
                queried = status()
        finally:
            install_status_snapshot(prior)

        self.assertIs(queried, snapshot)
        self.assertEqual(queried.plugins[0].state, PluginState.AVAILABLE)


if __name__ == "__main__":
    unittest.main()
