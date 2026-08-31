# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

import _cinderx


class RuntimeCapabilitiesTest(unittest.TestCase):
    def test_query_has_stable_bounded_shape(self) -> None:
        capabilities = _cinderx.get_runtime_capabilities()

        self.assertEqual(
            tuple(capabilities),
            ("core_build_id", "cpu_caps", "numa_node_count"),
        )
        self.assertIsInstance(capabilities["core_build_id"], str)
        self.assertTrue(capabilities["core_build_id"])
        self.assertLessEqual(len(capabilities["core_build_id"]), 128)

        cpu_caps = capabilities["cpu_caps"]
        self.assertIsInstance(cpu_caps, tuple)
        self.assertEqual(cpu_caps, tuple(sorted(set(cpu_caps))))
        self.assertLessEqual(len(cpu_caps), 16)
        self.assertTrue(all(isinstance(cap, str) and cap for cap in cpu_caps))

        node_count = capabilities["numa_node_count"]
        if node_count is not None:
            self.assertIs(type(node_count), int)
            self.assertGreater(node_count, 0)
            self.assertLessEqual(node_count, 65536)
        self.assertEqual(_cinderx.get_runtime_capabilities(), capabilities)


if __name__ == "__main__":
    unittest.main()
