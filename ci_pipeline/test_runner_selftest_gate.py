"""Keep runner regression tests in every required PR/Daily pipeline."""

import unittest

from ci_pipeline import run_gate


class TestRunnerSelftestGate(unittest.TestCase):
    def test_required_pipelines_include_runner_tempfiles(self):
        suite = run_gate.load_suite("runner_selftests")
        self.assertTrue(suite["fail_fast"])
        jobs = run_gate.suite_enabled_jobs(suite)
        self.assertEqual(len(jobs), 3)
        self.assertEqual(
            [job["command"] for job in jobs],
            [
                '"$CINDERX_TEST_PYTHON" -m unittest -v ci_pipeline.test_runner_selftest_gate',
                '"$CINDERX_TEST_PYTHON" -m unittest -v ci_pipeline.test_runner_tempfiles',
                '"$CINDERX_TEST_PYTHON" -m unittest -v ci_pipeline.test_ref314_contract',
            ],
        )
        for version in ((3, 11), (3, 14)):
            for pipeline in ("pr", "daily", "daily311"):
                with self.subTest(version=version, pipeline=pipeline):
                    self.assertEqual(
                        run_gate.pipeline_for_python(pipeline, version)[0],
                        ("runner_selftests", False),
                    )


if __name__ == "__main__":
    unittest.main()
