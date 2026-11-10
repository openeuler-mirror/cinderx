# Copyright (c) Meta Platforms, Inc. and affiliates.
"""CPython 3.11 numeric speculation: float and int fast paths, per-site fuses."""

import os
import sys
import unittest
from pathlib import Path

from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "speculation_feedback_311.py"


@unittest.skipUnless(sys.version_info[:2] == (3, 11), "CPython 3.11 feedback")
class SpeculationFeedback311Tests(unittest.TestCase):
    def _run(self, case, **extra):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("CINDERX_", "PYTHONJIT", "SPIKE_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_PLUGIN_ENABLE="0",
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            CINDERX_JIT_STARTUP_GRACE_MS="0",
        )
        env.update(extra)
        proc = run_python_child(HELPER, case, env=env, timeout=300)
        self.assertEqual(
            proc.returncode, 0, proc.stdout[-2000:] + proc.stderr[-4000:]
        )
        self.assertIn(f"CASE_OK {case}", proc.stdout)
        return proc

    def test_forced_deopts_do_not_trigger_numeric_feedback(self):
        self._run("forced", CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET="1")

    def test_float_speculation_matches_interpreter(self):
        self._run("semantics")

    def test_failing_float_guard_site_recompiles_to_diamond(self):
        self._run("fuse")

    def test_fuse_retires_by_code_artifact(self):
        self._run("fuse_by_code", PYTHONJITAUTO="4")

    def test_float_site_under_exception_edge_stays_generic(self):
        self._run("exc_edge", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_slow_arm_keeps_float_operand_identity(self):
        self._run("identity")

    def test_fuse_state_follows_code_lifetime_under_batch_compile(self):
        for workers in (1, 8):
            with self.subTest(workers=workers):
                self._run(
                    "lifetime",
                    PYTHONJITMULTITHREADEDCOMPILETEST="1",
                    PYTHONJITBATCHCOMPILEWORKERS=str(workers),
                )

    def test_int_operations_match_interpreter(self):
        self._run("int_semantics")

    def test_failing_int_site_recompiles_generic(self):
        self._run("int_fuse")

    def test_deopt_rebuilds_unboxed_int_values(self):
        self._run("int_deopt_values")

    def test_wide_int_operands_stay_generic(self):
        self._run("int_wide_operands")

    def test_slow_arm_keeps_int_operand_identity(self):
        self._run("int_slow_arm_identity")

    def test_int64_operations_match_interpreter(self):
        self._run("int64_semantics")

    def test_mixed_int_float_operations_match_interpreter(self):
        self._run("mixed_semantics")

    def test_fused_float_site_dispatches_ints_without_deopt(self):
        self._run("mixed_fuse")

    def test_array_typecode_speculation(self):
        self._run("array_typecode")

    def test_array_record_reads_only_array_type(self):
        self._run("array_record_identity", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_array_record_ignores_leftover_function_version(self):
        self._run("array_record_leftover_version")

    def test_fused_inplace_int64_site_stays_generic(self):
        self._run("int64_inplace_fuse", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_int64_bounds_hold_for_negative_bitwise_operands(self):
        self._run("int64_bitwise_bounds")

    def test_slow_arm_operand_is_the_frame_local(self):
        self._run("slow_arm_frame_identity", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_slow_arm_operand_identity_survives_later_calls(self):
        self._run("slow_arm_successor_identity", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_compact_compare_and_inplace_sites_fuse(self):
        self._run("compact_compare_inplace_fuse", PYTHONJITLIGHTWEIGHTFRAME="0")

    def test_int64_boundaries_on_fresh_code(self):
        self._run("int64_boundaries_unfused")


if __name__ == "__main__":
    unittest.main()
