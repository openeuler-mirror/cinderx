"""Selftests for the quickened-counter artifact judgement.

The judgement decides whether a regrtest -R "memory blocks" line may be
dismissed.  Getting it wrong in the permissive direction hides a real
leak, so the cases below pin both directions.
"""

import unittest

from ci_pipeline.jit311.quickened_artifact import (
    classify,
    decide,
    only_block_artifact_failure,
)


def block_failure_log(module="test_listcomps", extra=""):
    return (
        f"{module} leaked [8, 8, 8] memory blocks, sum=24\n"
        f"{module} failed (reference leak)\n"
        f"{extra}"
        "\n== Tests result: FAILURE ==\n\n"
        "1 test failed:\n"
        f"    {module}\n\n"
        "Total test files: success=0 failed=1\n"
        "Result: FAILURE\n"
    )


class DecideTests(unittest.TestCase):
    """The arithmetic judgement, table-driven.

    Each row: (name, reported, blocks_series, quick_series,
    expect_artifact, expect_in_why).
    """

    CASES = [
        ("flat_blocks_matching_drift_is_artifact",
         [8, 8, 8, 8], [1000, 1000, 1000, 1000, 1000],
         [-10, -18, -26, -34, -42], True, "artifact"),
        ("growing_raw_blocks_is_real",
         [8, 8, 8], [1000, 1008, 1016, 1024], [-10, -18, -26, -34],
         False, "real leak"),
        ("counter_without_drift_cannot_excuse",
         [8, 8, 8], [1000, 1000, 1000, 1000], [5, 5, 5, 5],
         False, "real leak"),
        ("wrong_size_drift_is_unexplained",
         [8, 8, 8], [1000, 1000, 1000, 1000], [-1, -2, -3, -4],
         False, "unexplained"),
        # Blocks climb while the JIT is still compiling, then settle:
        # only the settled tail may be judged.
        ("early_compilation_growth_is_ignored",
         [8, 8, 8, 8], [1000, 1300, 1600, 1600, 1600, 1600, 1600],
         [-10, -18, -26, -34, -42, -50, -58], True, "artifact"),
        ("shrinking_blocks_are_acceptable",
         [3, 3, 3], [1000, 999, 998, 997], [-3, -6, -9, -12],
         True, "artifact"),
    ]

    def test_decide_matrix(self):
        for name, reported, blocks, quick, expect, why_part in self.CASES:
            with self.subTest(case=name):
                artifact, why = decide("test_x", reported, blocks, quick)
                self.assertEqual(artifact, expect, why)
                self.assertIn(why_part, why)


class ClassifyFailClosedTests(unittest.TestCase):
    """The verifier runs under CINDERX_JIT_MODE=execute, so the fault it is
    hunting can kill it.  A crashed verifier prints no "memory blocks"
    line; reading that silence as proof would let the gate pass exactly
    the runtime failure it exists to catch.  Every absence must fail.
    """

    GOOD_COUNTS = {"blocks": [100, 100, 100, 100], "quick": [-8, -16, -24, -32]}
    BLOCK_LINE = "test_listcomps leaked [8, 8, 8] memory blocks, sum=24\n"
    RESULT_FAIL = "== Tests result: FAILURE ==\n"
    RESULT_OK = "== Tests result: SUCCESS ==\n"

    def _classify(self, text, returncode, counts):
        def fake_run(python, module, warmups, reps):
            return text, returncode, counts
        return classify("py", "test_listcomps", 30, 8, run=fake_run)

    def test_segfaulted_verifier_is_not_an_artifact(self):
        # Killed by SIGSEGV: partial output, no block line, no counts.
        artifact, why = self._classify("beginning 38 repetitions\n", -11, None)
        self.assertFalse(artifact, why)
        self.assertIn("signal 11", why)

    def test_nonzero_exit_without_a_block_line_is_not_an_artifact(self):
        artifact, why = self._classify(
            "some failure\n" + self.RESULT_FAIL, 2, None
        )
        self.assertFalse(artifact, why)

    def test_run_that_never_reached_a_result_is_not_an_artifact(self):
        # Process exited 0 but produced no regrtest summary at all.
        artifact, why = self._classify("beginning 38 repetitions\n", 0, None)
        self.assertFalse(artifact, why)
        self.assertIn("never reached a regrtest result", why)

    def test_hook_producing_no_counts_is_not_an_artifact(self):
        artifact, why = self._classify(block_failure_log(), 2, None)
        self.assertFalse(artifact, why)
        self.assertIn("no counts", why)

    def test_non_reproducing_figure_is_unverified_not_excused(self):
        artifact, why = self._classify(self.RESULT_OK, 0, self.GOOD_COUNTS)
        self.assertFalse(artifact, why)
        self.assertIn("unverified", why)

    def test_reference_leak_is_never_excused(self):
        artifact, why = self._classify(
            "test_listcomps leaked [13, 13, 13] references, sum=39\n"
            + self.BLOCK_LINE + self.RESULT_FAIL,
            2,
            self.GOOD_COUNTS,
        )
        self.assertFalse(artifact, why)
        self.assertIn("reference leak", why)

    def test_file_descriptor_leak_is_never_excused(self):
        artifact, why = self._classify(
            "test_listcomps leaked [1, 1, 1] file descriptors, sum=3\n"
            + self.BLOCK_LINE + self.RESULT_FAIL,
            2,
            self.GOOD_COUNTS,
        )
        self.assertFalse(artifact, why)
        self.assertIn("file-descriptor leak", why)

    def test_a_healthy_run_with_matching_arithmetic_is_an_artifact(self):
        # The one path that may pass, so the tests above are not vacuous.
        artifact, why = self._classify(
            block_failure_log(), 2, self.GOOD_COUNTS
        )
        self.assertTrue(artifact, why)
        self.assertIn("artifact", why)


class OriginalFailureShapeTests(unittest.TestCase):
    def test_pure_block_artifact_failure_is_accepted(self):
        artifact, why = only_block_artifact_failure(
            "test_listcomps", block_failure_log(), 2
        )
        self.assertTrue(artifact, why)

    def test_block_artifact_plus_ordinary_failure_is_rejected(self):
        artifact, why = only_block_artifact_failure(
            "test_listcomps",
            block_failure_log(
                extra=(
                    "FAIL: test_real_failure (test_listcomps.Tests.test_real_failure)\n"
                    "Traceback (most recent call last):\n"
                    "AssertionError: real failure\n"
                )
            ),
            2,
        )
        self.assertFalse(artifact, why)
        self.assertIn("ordinary failure", why)

    def test_block_artifact_with_ambiguous_failure_reason_is_rejected(self):
        text = block_failure_log().replace(
            "test_listcomps failed (reference leak)", "test_listcomps failed"
        )
        artifact, why = only_block_artifact_failure("test_listcomps", text, 2)
        self.assertFalse(artifact, why)
        self.assertIn("failure reasons", why)
