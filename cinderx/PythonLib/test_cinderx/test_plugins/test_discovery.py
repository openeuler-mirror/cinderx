# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import json
import importlib.util
import os
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from dataclasses import FrozenInstanceError
from types import SimpleNamespace
from unittest.mock import Mock, patch

import _cinderx_plugins_bootstrap as bootstrap_module
from importlib import metadata
from cinderx.plugins import DiscoveryIssueCode, discover
from cinderx.plugins import discovery as discovery_module
from cinderx.plugins.manifest import MAX_MANIFEST_BYTES, ManifestIssueCode


MANIFEST_PATH = "cinderx_plugin.json"


def valid_manifest(plugin_id: str) -> str:
    return json.dumps(
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
            "adapter": {
                "entry": "never_imported.adapter",
                "target": "never_imported_target",
            },
        },
        separators=(",", ":"),
    )


class FakeDistribution:
    def __init__(
        self,
        name: str,
        payload: str | None = None,
        *,
        read_error: Exception | None = None,
    ) -> None:
        self.metadata = {"Name": name}
        self._payload = payload
        self._read_error = read_error
        self.read_requests: list[str] = []

    def read_text(self, filename: str) -> str | None:
        self.read_requests.append(filename)
        if self._read_error is not None:
            raise self._read_error
        return self._payload


class PluginDiscoveryTests(unittest.TestCase):
    def test_enumerates_static_manifests_in_normalized_name_order(self) -> None:
        zeta = FakeDistribution("Zeta.Plugin", valid_manifest("zeta"))
        missing = FakeDistribution("middle", None)
        alpha = FakeDistribution("alpha_plugin", valid_manifest("alpha"))

        with patch.object(
            discovery_module.metadata,
            "distributions",
            return_value=(zeta, missing, alpha),
        ):
            results = discover()

        self.assertIsInstance(results, tuple)
        self.assertEqual(
            [result.distribution_name for result in results],
            ["alpha_plugin", "Zeta.Plugin"],
        )
        self.assertEqual(
            [result.normalized_distribution_name for result in results],
            ["alpha-plugin", "zeta-plugin"],
        )
        self.assertTrue(all(result.available for result in results))
        self.assertEqual(
            [result.manifest.id for result in results],  # type: ignore[union-attr]
            ["alpha", "zeta"],
        )
        self.assertEqual(alpha.read_requests, [MANIFEST_PATH])
        self.assertEqual(missing.read_requests, [MANIFEST_PATH])
        self.assertEqual(zeta.read_requests, [MANIFEST_PATH])
        self.assertNotIn("never_imported", sys.modules)
        self.assertNotIn("never_imported.adapter", sys.modules)
        self.assertNotIn("never_imported_target", sys.modules)
        with self.assertRaises(FrozenInstanceError):
            results[0].distribution_name = "changed"  # type: ignore[misc]

    def test_equal_normalized_names_have_a_deterministic_tie_break(self) -> None:
        underscore = FakeDistribution("demo_plugin", valid_manifest("underscore"))
        dotted = FakeDistribution("Demo.Plugin", valid_manifest("dotted"))

        first = discover(distributions=(underscore, dotted))
        second = discover(distributions=(dotted, underscore))

        self.assertEqual(first, second)
        self.assertEqual(
            [result.distribution_name for result in first],
            ["Demo.Plugin", "demo_plugin"],
        )

    def test_equal_distribution_identities_sort_by_manifest_content(self) -> None:
        first_payload = json.loads(valid_manifest("duplicate"))
        first_payload["provides"] = {"contracts": [{"variant": "first"}]}
        second_payload = json.loads(valid_manifest("duplicate"))
        second_payload["provides"] = {"contracts": [{"variant": "second"}]}
        first_distribution = FakeDistribution(
            "duplicate",
            json.dumps(first_payload, separators=(",", ":")),
        )
        second_distribution = FakeDistribution(
            "duplicate",
            json.dumps(second_payload, separators=(",", ":")),
        )

        forward = discover(distributions=(first_distribution, second_distribution))
        reverse = discover(distributions=(second_distribution, first_distribution))

        self.assertEqual(forward, reverse)

    def test_invalid_payload_type_isolated_between_valid_siblings(self) -> None:
        invalid = FakeDistribution("invalid", None)
        invalid._payload = 42  # type: ignore[assignment]

        results = discover(
            distributions=(
                FakeDistribution("alpha", valid_manifest("alpha")),
                invalid,
                FakeDistribution("zeta", valid_manifest("zeta")),
            )
        )

        self.assertEqual(
            [result.distribution_name for result in results],
            ["alpha", "invalid", "zeta"],
        )
        self.assertTrue(results[0].available)
        self.assertFalse(results[1].available)
        self.assertEqual(
            results[1].discovery_issue.code,  # type: ignore[union-attr]
            DiscoveryIssueCode.MANIFEST_INVALID_PAYLOAD,
        )
        self.assertTrue(results[2].available)

    def test_iterator_failure_keeps_prior_and_later_candidates(self) -> None:
        class RecoveringIterator:
            def __init__(self) -> None:
                self.index = 0

            def __iter__(self):
                return self

            def __next__(self):
                self.index += 1
                if self.index == 1:
                    return FakeDistribution("alpha", valid_manifest("alpha"))
                if self.index == 2:
                    raise OSError("transient metadata failure")
                if self.index == 3:
                    return FakeDistribution("zeta", valid_manifest("zeta"))
                raise StopIteration

        results = discover(distributions=RecoveringIterator())

        self.assertEqual(
            [result.distribution_name for result in results],
            ["alpha", "zeta"],
        )

    def test_bad_candidate_is_unavailable_without_truncating_siblings(self) -> None:
        candidates = (
            FakeDistribution("zeta-good", valid_manifest("zeta-good")),
            FakeDistribution("oversized", "{" + " " * MAX_MANIFEST_BYTES),
            FakeDistribution("malformed", '{"id":'),
            FakeDistribution(
                "unreadable",
                read_error=OSError("filesystem details must not escape"),
            ),
            FakeDistribution("alpha-good", valid_manifest("alpha-good")),
        )

        results = discover(distributions=candidates)

        self.assertEqual(
            [result.distribution_name for result in results],
            ["alpha-good", "malformed", "oversized", "unreadable", "zeta-good"],
        )
        by_name = {result.distribution_name: result for result in results}
        self.assertTrue(by_name["alpha-good"].available)
        self.assertTrue(by_name["zeta-good"].available)
        self.assertFalse(by_name["malformed"].available)
        malformed_validation = by_name["malformed"].validation
        assert malformed_validation is not None
        assert malformed_validation.manifest_rejection is not None
        self.assertEqual(
            malformed_validation.manifest_rejection.code,
            ManifestIssueCode.MALFORMED_JSON,
        )
        self.assertFalse(by_name["oversized"].available)
        oversized_validation = by_name["oversized"].validation
        assert oversized_validation is not None
        assert oversized_validation.manifest_rejection is not None
        self.assertEqual(
            oversized_validation.manifest_rejection.code,
            ManifestIssueCode.MANIFEST_TOO_LARGE,
        )
        self.assertFalse(by_name["unreadable"].available)
        self.assertIsNone(by_name["unreadable"].validation)
        self.assertEqual(
            by_name["unreadable"].discovery_issue.code,  # type: ignore[union-attr]
            DiscoveryIssueCode.MANIFEST_UNREADABLE,
        )
        self.assertNotIn(
            "filesystem details",
            by_name["unreadable"].discovery_issue.message,  # type: ignore[union-attr]
        )


class PluginDiscoveryBootstrapTests(unittest.TestCase):
    def setUp(self) -> None:
        bootstrap_module._discovery_started = False
        bootstrap_module._discovery_results = ()

    def test_candidate_schedules_discovery_once(self) -> None:
        candidate = FakeDistribution("candidate", valid_manifest("candidate"))
        discovered_candidates = []

        def discover_candidates(*, distributions):
            discovered_candidates.extend(distributions)
            return ("stable-result",)

        discover_mock = Mock(side_effect=discover_candidates)
        plugins_module = SimpleNamespace(discover=discover_mock)

        with patch.dict(os.environ, {}, clear=True), patch.object(
            bootstrap_module.metadata,
            "distributions",
            return_value=(candidate,),
        ) as distributions_mock, patch.object(
            bootstrap_module.importlib,
            "import_module",
            return_value=plugins_module,
        ) as import_mock:
            first = bootstrap_module.bootstrap()
            second = bootstrap_module.bootstrap()

        self.assertEqual(first, ("stable-result",))
        self.assertIs(first, second)
        distributions_mock.assert_called_once_with()
        import_mock.assert_called_once_with("cinderx.plugins")
        discover_mock.assert_called_once()
        self.assertEqual(len(discovered_candidates), 1)
        self.assertEqual(discovered_candidates[0].metadata["Name"], "candidate")
        self.assertEqual(
            discovered_candidates[0].read_text(MANIFEST_PATH),
            valid_manifest("candidate"),
        )
        self.assertEqual(candidate.read_requests, [MANIFEST_PATH])

    def test_no_candidate_does_not_import_cinderx(self) -> None:
        ordinary = FakeDistribution("ordinary", None)

        with patch.dict(os.environ, {}, clear=True), patch.object(
            bootstrap_module.metadata,
            "distributions",
            return_value=(ordinary,),
        ), patch.object(
            bootstrap_module.importlib,
            "import_module",
        ) as import_mock:
            self.assertEqual(bootstrap_module.bootstrap(), ())

        import_mock.assert_not_called()
        self.assertEqual(ordinary.read_requests, [MANIFEST_PATH])

    def test_disable_flags_skip_the_metadata_scan(self) -> None:
        for flag in (
            "CINDERX_PLUGIN_DISCOVERY_DISABLE",
            "CINDERX_DISABLE",
        ):
            with self.subTest(flag=flag):
                bootstrap_module._discovery_started = False
                bootstrap_module._discovery_results = ()
                with patch.dict(os.environ, {flag: "yes"}, clear=True), patch.object(
                    bootstrap_module.metadata,
                    "distributions",
                ) as distributions_mock, patch.object(
                    bootstrap_module.importlib,
                    "import_module",
                ) as import_mock:
                    self.assertEqual(bootstrap_module.bootstrap(), ())

                distributions_mock.assert_not_called()
                import_mock.assert_not_called()

    def test_preflight_bounds_standard_distribution_manifest_reads(self) -> None:
        class GuardedPathDistribution(metadata.PathDistribution):
            @property
            def files(self):
                raise AssertionError("manifest preflight parsed RECORD")

        self.assertEqual(
            bootstrap_module.MAX_MANIFEST_BYTES,
            MAX_MANIFEST_BYTES,
        )
        with tempfile.TemporaryDirectory() as tempdir:
            dist_info = Path(tempdir) / "oversized-1.0.dist-info"
            dist_info.mkdir()
            (dist_info / "METADATA").write_text(
                "Metadata-Version: 2.1\nName: oversized\nVersion: 1.0\n",
                encoding="utf-8",
            )
            (dist_info / "RECORD").write_text(
                "oversized-1.0.dist-info/METADATA,,\n"
                "oversized-1.0.dist-info/RECORD,,\n"
                "oversized-1.0.dist-info/cinderx_plugin.json,,\n",
                encoding="utf-8",
            )
            (dist_info / MANIFEST_PATH).write_bytes(
                b"x" * (MAX_MANIFEST_BYTES + 100)
            )
            distribution = GuardedPathDistribution(dist_info)

            payload = bootstrap_module._read_manifest(distribution)

        self.assertIsInstance(payload, bytes)
        self.assertEqual(len(payload), MAX_MANIFEST_BYTES + 1)

    def test_preflight_missing_manifest_does_not_parse_record(self) -> None:
        class GuardedPathDistribution(metadata.PathDistribution):
            @property
            def files(self):
                raise AssertionError("manifest preflight parsed RECORD")

        with tempfile.TemporaryDirectory() as tempdir:
            dist_info = Path(tempdir) / "ordinary-1.0.dist-info"
            dist_info.mkdir()
            (dist_info / "METADATA").write_text(
                "Metadata-Version: 2.1\nName: ordinary\nVersion: 1.0\n",
                encoding="utf-8",
            )
            (dist_info / "RECORD").write_text(
                "ordinary-1.0.dist-info/METADATA,,\n" * 100_000,
                encoding="utf-8",
            )
            distribution = GuardedPathDistribution(dist_info)

            payload = bootstrap_module._read_located_manifest(distribution)

        self.assertIsNone(payload)


class PluginBootstrapPackagingTests(unittest.TestCase):
    def test_build_ext_copies_the_plugin_bootstrap(self) -> None:
        if importlib.util.find_spec("setuptools") is None:
            self.skipTest("setuptools is not available in this interpreter")

        with patch("setuptools.setup"):
            setup_namespace = runpy.run_path(
                str(Path(__file__).parents[4] / "setup.py"),
                run_name="cinderx_setup_packaging_test",
            )
        from setuptools.dist import Distribution

        command = setup_namespace["BuildExt"](Distribution())
        with tempfile.TemporaryDirectory() as tempdir:
            command.build_lib = tempdir
            command._copy_required_pythonlib_file(
                "_cinderx_plugins_bootstrap.py"
            )
            copied = Path(tempdir) / "_cinderx_plugins_bootstrap.py"

            self.assertEqual(
                copied.read_bytes(),
                (
                    Path(__file__).parents[2]
                    / "_cinderx_plugins_bootstrap.py"
                ).read_bytes(),
            )


if __name__ == "__main__":
    unittest.main()
