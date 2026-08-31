# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import json
import sys
import threading
import time
import unittest
from dataclasses import FrozenInstanceError
from types import MappingProxyType
from typing import Any

from cinderx.plugins.manifest import MAX_IDENTIFIER_BYTES, PluginManifest, validate_manifest
from cinderx.plugins.registry import (
    PluginRegistry,
    RegistryEntry,
    RegistryRejectionReason,
)


def manifest_for(
    namespace: str,
    provides: dict[str, Any],
    *,
    adapter: dict[str, str] | None = None,
) -> PluginManifest:
    payload: dict[str, Any] = {
        "id": namespace,
        "spi_version": "1",
        "runtime_abi": {
            "python_version": "3.14",
            "soabi": "cpython-314-aarch64-linux-gnu",
            "core_build_id": "cinderx-dev",
            "cpu_caps": ["neon"],
        },
        "target_capabilities": ["plugin-manifest-v1"],
        "provides": provides,
    }
    if adapter is not None:
        payload["adapter"] = adapter
    validated = validate_manifest(json.dumps(payload, separators=(",", ":")))
    if validated.manifest is None:
        raise AssertionError(validated.issues)
    return validated.manifest


class PluginRegistryPublicationTests(unittest.TestCase):
    def test_publishes_all_four_logical_entry_families(self) -> None:
        registry = PluginRegistry()
        manifest = manifest_for(
            "acme.analytics",
            {
                "contracts": [
                    {"id": "column-layout", "shape": {"columns": ["x", "y"]}}
                ],
                "policies": [
                    {
                        "id": "vector-policy",
                        "depends_on": ["column-layout"],
                        "enabled": True,
                    }
                ],
                "seeds": ["profiles/base.json", "profiles/wide.json"],
                "diagnostics": {"counters": {"enabled": True}},
                "pass": {"pipeline": ["vectorize", "lower"]},
            },
        )

        result = registry.publish(manifest)

        self.assertTrue(result.published)
        self.assertEqual(result.rejections, ())
        self.assertEqual(result.snapshot.generation, 1)
        namespace = result.snapshot.for_namespace("acme.analytics")
        self.assertIsNotNone(namespace)
        assert namespace is not None
        self.assertEqual(namespace.namespace, "acme.analytics")
        self.assertEqual(namespace.generation, 1)

        contract = namespace.contracts["column-layout"]
        policy = namespace.admission_inputs.policies["vector-policy"]
        seeds = namespace.admission_inputs.seeds
        diagnostics = namespace.admission_inputs.diagnostics
        pass_config = namespace.pass_configuration
        for entry in (contract, policy, *seeds, diagnostics, pass_config):
            self.assertIsInstance(entry, RegistryEntry)
            assert entry is not None
            self.assertEqual(entry.namespace, "acme.analytics")
            self.assertEqual(entry.generation, 1)
        self.assertEqual(policy.depends_on, ("column-layout",))
        self.assertEqual([entry.id for entry in seeds], [
            "profiles/base.json",
            "profiles/wide.json",
        ])
        assert diagnostics is not None
        assert pass_config is not None
        self.assertEqual(diagnostics.id, "diagnostics")
        self.assertEqual(pass_config.id, "pass")

    def test_namespace_collision_fails_closed_and_ids_are_namespace_local(self) -> None:
        registry = PluginRegistry()
        first = registry.publish(
            manifest_for("plugin.one", {"contracts": [{"id": "shared", "v": 1}]})
        )
        second = registry.publish(
            manifest_for("plugin.two", {"contracts": [{"id": "shared", "v": 2}]})
        )

        self.assertTrue(first.published)
        self.assertTrue(second.published)
        one = second.snapshot.for_namespace("plugin.one")
        two = second.snapshot.for_namespace("plugin.two")
        assert one is not None and two is not None
        self.assertEqual(one.contracts["shared"].value["v"], 1)
        self.assertEqual(two.contracts["shared"].value["v"], 2)
        self.assertNotIn("only-two", one.contracts)

        before_collision = registry.snapshot
        collision = registry.publish(
            manifest_for(
                "plugin.one",
                {"contracts": [{"id": "replacement", "v": "must-not-win"}]},
            )
        )

        self.assertFalse(collision.published)
        self.assertIs(collision.snapshot, before_collision)
        self.assertIs(registry.snapshot, before_collision)
        self.assertEqual(
            [rejection.reason for rejection in collision.rejections],
            [RegistryRejectionReason.DUPLICATE_NAMESPACE],
        )
        one = registry.snapshot.for_namespace("plugin.one")
        assert one is not None
        self.assertEqual(tuple(one.contracts), ("shared",))

    def test_invalid_entries_are_isolated_and_evidence_is_ordered(self) -> None:
        registry = PluginRegistry()
        manifest = manifest_for(
            "plugin.invalid",
            {
                "contracts": [
                    {"id": "valid", "payload": {"kept": True}},
                    {"payload": "missing-id"},
                    {"id": "x" * (MAX_IDENTIFIER_BYTES + 1)},
                ],
                "seeds": ["profiles/kept.json"],
            },
        )

        result = registry.publish(manifest)

        self.assertTrue(result.published)
        namespace = result.snapshot.for_namespace("plugin.invalid")
        assert namespace is not None
        self.assertEqual(tuple(namespace.contracts), ("valid",))
        self.assertEqual(
            [(item.path, item.reason) for item in result.rejections],
            [
                (
                    "$.provides.contracts[1]",
                    RegistryRejectionReason.INVALID_ENTRY,
                ),
                (
                    "$.provides.contracts[2]",
                    RegistryRejectionReason.INVALID_ENTRY,
                ),
            ],
        )

    def test_missing_and_rejected_dependencies_reject_transitively(self) -> None:
        registry = PluginRegistry()
        manifest = manifest_for(
            "plugin.dependencies",
            {
                "contracts": [
                    {"id": "bad", "depends_on": "not-a-list"},
                    {"id": "missing-root", "depends_on": ["absent"]},
                    {"id": "missing-child", "depends_on": ["missing-root"]},
                    {"id": "bad-child", "depends_on": ["bad"]},
                    {"id": "bad-grandchild", "depends_on": ["bad-child"]},
                    {"id": "independent", "value": "visible"},
                ]
            },
        )

        result = registry.publish(manifest)

        self.assertTrue(result.published)
        namespace = result.snapshot.for_namespace("plugin.dependencies")
        assert namespace is not None
        self.assertEqual(tuple(namespace.contracts), ("independent",))
        self.assertEqual(
            [(item.entry_id, item.reason, item.dependency) for item in result.rejections],
            [
                ("bad", RegistryRejectionReason.INVALID_ENTRY, None),
                (
                    "missing-root",
                    RegistryRejectionReason.MISSING_DEPENDENCY,
                    "absent",
                ),
                (
                    "missing-child",
                    RegistryRejectionReason.REJECTED_DEPENDENCY,
                    "missing-root",
                ),
                (
                    "bad-child",
                    RegistryRejectionReason.REJECTED_DEPENDENCY,
                    "bad",
                ),
                (
                    "bad-grandchild",
                    RegistryRejectionReason.REJECTED_DEPENDENCY,
                    "bad-child",
                ),
            ],
        )

    def test_dependency_cycle_rolls_back_the_plugin_transaction(self) -> None:
        registry = PluginRegistry()
        initial = registry.publish(
            manifest_for("plugin.stable", {"contracts": [{"id": "stable"}]})
        ).snapshot
        cyclic = manifest_for(
            "plugin.cyclic",
            {
                "contracts": [
                    {"id": "a", "depends_on": ["b"]},
                    {"id": "b", "depends_on": ["c"]},
                    {"id": "c", "depends_on": ["a"]},
                ],
                "seeds": ["must/not/publish.json"],
            },
        )

        result = registry.publish(cyclic)

        self.assertFalse(result.published)
        self.assertIs(result.snapshot, initial)
        self.assertIs(registry.snapshot, initial)
        self.assertIsNone(initial.for_namespace("plugin.cyclic"))
        self.assertEqual(initial.generation, 1)
        self.assertEqual(len(result.rejections), 1)
        self.assertEqual(
            result.rejections[0].reason,
            RegistryRejectionReason.DEPENDENCY_CYCLE,
        )
        self.assertEqual(result.rejections[0].cycle, ("a", "b", "c", "a"))

    def test_duplicate_ids_are_all_rejected_without_last_writer_wins(self) -> None:
        registry = PluginRegistry()
        manifest = manifest_for(
            "plugin.duplicates",
            {
                "contracts": [
                    {"id": "duplicate", "source": "contract"},
                    {"id": "dependent", "depends_on": ["duplicate"]},
                    {"id": "unique"},
                ],
                "policies": [{"id": "duplicate", "source": "policy"}],
            },
        )

        result = registry.publish(manifest)

        self.assertTrue(result.published)
        namespace = result.snapshot.for_namespace("plugin.duplicates")
        assert namespace is not None
        self.assertEqual(tuple(namespace.contracts), ("unique",))
        self.assertEqual(namespace.admission_inputs.policies, {})
        self.assertEqual(
            [(item.entry_id, item.reason) for item in result.rejections],
            [
                ("duplicate", RegistryRejectionReason.DUPLICATE_ENTRY_ID),
                ("dependent", RegistryRejectionReason.REJECTED_DEPENDENCY),
                ("duplicate", RegistryRejectionReason.DUPLICATE_ENTRY_ID),
            ],
        )


class PluginRegistrySnapshotTests(unittest.TestCase):
    def test_snapshot_and_values_are_recursively_immutable(self) -> None:
        registry = PluginRegistry()
        result = registry.publish(
            manifest_for(
                "plugin.frozen",
                {
                    "contracts": [
                        {"id": "nested", "data": {"items": [1, {"v": 2}]}}
                    ],
                    "diagnostics": {"groups": [{"name": "compile"}]},
                    "pass": {"steps": [{"name": "simplify"}]},
                },
            )
        )
        snapshot = result.snapshot
        namespace = snapshot.for_namespace("plugin.frozen")
        assert namespace is not None
        entry = namespace.contracts["nested"]

        self.assertIsInstance(snapshot.namespaces, MappingProxyType)
        self.assertIsInstance(namespace.contracts, MappingProxyType)
        self.assertIsInstance(entry.value, MappingProxyType)
        self.assertIsInstance(entry.value["data"], MappingProxyType)
        self.assertIsInstance(entry.value["data"]["items"], tuple)
        with self.assertRaises(TypeError):
            snapshot.namespaces["other"] = namespace  # type: ignore[index]
        with self.assertRaises(TypeError):
            entry.value["new"] = True  # type: ignore[index]
        with self.assertRaises(TypeError):
            entry.value["data"]["items"][1]["v"] = 3  # type: ignore[index]
        with self.assertRaises(FrozenInstanceError):
            entry.generation = 99  # type: ignore[misc]
        with self.assertRaises(FrozenInstanceError):
            result.published = False  # type: ignore[misc]

    def test_readers_observe_only_complete_old_or_new_snapshots(self) -> None:
        registry = PluginRegistry()
        old_snapshot = registry.snapshot
        started = threading.Event()
        finished = threading.Event()
        failures: list[str] = []

        def writer() -> None:
            started.set()
            for index in range(40):
                name = f"plugin.threaded.{index:02d}"
                result = registry.publish(
                    manifest_for(
                        name,
                        {
                            "contracts": [{"id": "contract", "index": index}],
                            "policies": [
                                {"id": "policy", "depends_on": ["contract"]}
                            ],
                            "seeds": [f"profiles/{index}.json"],
                            "diagnostics": {"index": index},
                            "pass": {"index": index},
                        },
                    )
                )
                if not result.published:
                    failures.append(f"writer rejection: {result.rejections!r}")
                time.sleep(0)
            finished.set()

        def reader() -> None:
            started.wait()
            while not finished.is_set():
                snapshot = registry.snapshot
                if snapshot.generation != len(snapshot.namespaces):
                    failures.append(
                        f"partial namespace map at generation {snapshot.generation}"
                    )
                    return
                for name, namespace in snapshot.namespaces.items():
                    complete = (
                        namespace.namespace == name
                        and tuple(namespace.contracts) == ("contract",)
                        and tuple(namespace.admission_inputs.policies) == ("policy",)
                        and len(namespace.admission_inputs.seeds) == 1
                        and namespace.admission_inputs.diagnostics is not None
                        and namespace.pass_configuration is not None
                    )
                    if not complete:
                        failures.append(
                            f"partial buckets for {name} at generation "
                            f"{snapshot.generation}"
                        )
                        return
                time.sleep(0)

        readers = [threading.Thread(target=reader) for _ in range(4)]
        writer_thread = threading.Thread(target=writer)
        for thread in readers:
            thread.start()
        writer_thread.start()
        writer_thread.join(timeout=10)
        for thread in readers:
            thread.join(timeout=10)

        self.assertFalse(writer_thread.is_alive())
        self.assertTrue(all(not thread.is_alive() for thread in readers))
        self.assertEqual(failures, [])
        self.assertEqual(old_snapshot.generation, 0)
        self.assertEqual(old_snapshot.namespaces, {})
        self.assertEqual(registry.snapshot.generation, 40)
        self.assertEqual(len(registry.snapshot.namespaces), 40)

    def test_publish_does_not_import_the_declared_adapter(self) -> None:
        registry = PluginRegistry()
        adapter_module = "registry_test_adapter_must_never_import"
        manifest = manifest_for(
            "plugin.declarative",
            {"contracts": [{"id": "declaration"}]},
            adapter={"entry": adapter_module, "target": "unused_target"},
        )

        self.assertNotIn(adapter_module, sys.modules)
        result = registry.publish(manifest)

        self.assertTrue(result.published)
        self.assertNotIn(adapter_module, sys.modules)
        self.assertNotIn("unused_target", sys.modules)


if __name__ == "__main__":
    unittest.main()
