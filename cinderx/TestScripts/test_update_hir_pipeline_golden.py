#!/usr/bin/env python3

import subprocess
import unittest
from unittest.mock import patch

from cinderx.TestScripts import update_hir_pipeline_golden


class UpdateHIRPipelineGoldenTest(unittest.TestCase):
    @patch(
        "sys.argv",
        [
            "update_hir_pipeline_golden.py",
            "/tmp/runtime_tests",
            "--gtest_filter=HIRPipelineGolden.Example",
            "--gtest_repeat=2",
        ],
    )
    def test_parse_args_accepts_direct_gtest_options(self):
        args = update_hir_pipeline_golden.parse_args()

        self.assertEqual(args.runtime_tests, "/tmp/runtime_tests")
        self.assertEqual(
            args.gtest_args,
            ["--gtest_filter=HIRPipelineGolden.Example", "--gtest_repeat=2"],
        )

    @patch("sys.argv", ["update_hir_pipeline_golden.py", "/tmp/runtime_tests"])
    @patch.object(update_hir_pipeline_golden.os.path, "isfile", return_value=True)
    @patch.object(update_hir_pipeline_golden.os, "access", return_value=True)
    @patch.object(update_hir_pipeline_golden.subprocess, "run")
    def test_main_uses_default_gtest_filter(
        self,
        run: unittest.mock.Mock,
        _access: unittest.mock.Mock,
        _isfile: unittest.mock.Mock,
    ):
        run.return_value = subprocess.CompletedProcess(args=[], returncode=0)

        self.assertEqual(update_hir_pipeline_golden.main(), 0)

        command = run.call_args.args[0]
        env = run.call_args.kwargs["env"]
        self.assertEqual(
            command,
            ["/tmp/runtime_tests", "--gtest_filter=HIRPipelineGolden.*"],
        )
        self.assertEqual(env["UPDATE_HIR_PIPELINE_GOLDEN"], "1")


if __name__ == "__main__":
    unittest.main()
