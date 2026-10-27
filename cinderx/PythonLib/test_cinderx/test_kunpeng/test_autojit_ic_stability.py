import json
import os
from pathlib import Path
import sys
import sysconfig
import tempfile
import unittest

from cinderx.test_support import run_python_child


HELPER = Path(__file__).with_name("child_cases") / "autojit_ic_stability.py"
SUPPORTED = (
    sys.version_info[:2] == (3, 14)
    and sys.version_info[:3] != (3, 14, 0)
    and not sysconfig.get_config_var("Py_GIL_DISABLED")
)


@unittest.skipUnless(SUPPORTED, "IC admission gate requires CPython 3.14 with GIL")
class AutoJitICStabilityTests(unittest.TestCase):
    def run_case(self, name, *, mode="auto:64", stability="1"):
        env = os.environ.copy()
        for key in list(env):
            if key.startswith("CINDERX_AUTOJIT_") or key in (
                "PYTHONPATH", "PYTHONJITALL", "PYTHONJITDISABLE",
                "PYTHONJITLISTFILE", "CINDERX_DISABLE", "CINDERX_JIT_DISABLE",
            ):
                env.pop(key, None)
        env.update(
            CINDERX_PLUGIN_ENABLE="1",
            PYTHONJITAUTO=mode,
            PYTHONJITSPECIALIZEDOPCODES="1",
            CINDERX_AUTOJIT_LOWROI_WARM_CALLS="0",
            CINDERX_AUTOJIT_ROI_BACKOFF="0",
        )
        if stability is not None:
            env["CINDERX_AUTOJIT_IC_STABILITY"] = stability
        with tempfile.TemporaryDirectory() as temporary:
            env["CINDERX_AUTOJIT_COMPILE_EVENTS_FILE"] = str(
                Path(temporary) / "events.jsonl"
            )
            completed = run_python_child(
                HELPER, name, cwd=temporary, env=env, timeout=120
            )
            self.assertEqual(
                completed.returncode, 0,
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
            )
            payloads = [
                json.loads(line.removeprefix("IC_STABILITY_RESULT="))
                for line in completed.stdout.splitlines()
                if line.startswith("IC_STABILITY_RESULT=")
            ]
            self.assertEqual(len(payloads), 1, completed.stdout)
            result = payloads[0]
            self.assertEqual(result["case"], name)
            self.assertTrue(result["correct"])
            return result

    def test_ready_numeric_feedback_reuses_warmup_at_original_threshold(self):
        result = self.run_case("mature")
        event = result["event"]
        self.assertEqual(event["ic_stability_reason"], "stable")
        self.assertEqual(event["ic_stability_samples"], 3)
        self.assertEqual(event["ic_stability_first_calls"], 56)
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_ready_sites"], event["ic_stability_sites"])
        self.assertGreater(event["ic_stability_sites"], 0)

    def test_unvisited_attribute_is_skipped_while_known_subset_matures(self):
        result = self.run_case("cold_branch")
        event = result["event"]
        self.assertEqual(event["ic_stability_reason"], "stable")
        self.assertEqual(event["ic_stability_first_calls"], 56)
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["calls"], 64)
        self.assertGreater(event["ic_stability_known_sites"], 0)
        self.assertGreater(event["ic_stability_skipped_sites"], 0)
        self.assertEqual(
            event["ic_stability_known_sites"]
            + event["ic_stability_skipped_sites"],
            event["ic_stability_sites"],
        )
        self.assertEqual(result["descriptor_reads"], 1)
        self.assertEqual(result["cold_exception"], "AttributeError")

    def test_inplace_only_feedback_uses_natural_stability_admission(self):
        result = self.run_case("inplace_numeric")
        event = result["event"]
        self.assertEqual(event["shape"]["loop_score"], 0)
        self.assertEqual(event["ic_stability_reason"], "stable")
        self.assertEqual(event["ic_stability_samples"], 3)
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["effective_limit"], 64)
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_sites"], 3)
        self.assertEqual(event["ic_stability_ready_sites"], 3)
        self.assertEqual(event["ic_stability_skipped_sites"], 0)
        self.assertEqual(
            result["operations"], [["add", 2], ["subtract", 2], ["multiply", 2]]
        )
        self.assertEqual(result["failed_operations"], [["add", 3], ["subtract", 3]])

    def test_natural_late_stable_waits_past_original_admission(self):
        result = self.run_case("natural_late_stable")
        event = result["event"]
        self.assertEqual(event["shape"]["loop_score"], 0)
        self.assertEqual(event["effective_limit"], 64)
        self.assertEqual(event["ic_stability_first_calls"], 56)
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "stable")
        self.assertEqual(event["calls"], 72)
        self.assertEqual(result["first_compiled_invocation"], event["calls"] + 1)
        self.assertEqual(event["ic_stability_sites"], 2)
        self.assertGreater(event["ic_stability_known_sites"], 0)
        self.assertGreater(event["ic_stability_samples"], 3)

    def test_natural_changing_feedback_releases_at_128_call_cap(self):
        result = self.run_case("natural_wait_cap")
        event = result["event"]
        self.assertEqual(event["shape"]["loop_score"], 0)
        self.assertEqual(event["effective_limit"], 64)
        self.assertEqual(event["ic_stability_first_calls"], 56)
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "cap")
        self.assertEqual(event["calls"] - event["ic_stability_admission_calls"], 128)
        self.assertEqual(event["calls"], 192)
        self.assertEqual(result["first_compiled_invocation"], event["calls"] + 1)
        self.assertEqual(event["ic_stability_sites"], 32)
        self.assertGreater(event["ic_stability_known_sites"], 0)
        self.assertLessEqual(event["ic_stability_known_sites"], 32)
        self.assertGreater(event["ic_stability_samples"], 3)

    def test_all_unknown_sites_preserve_original_threshold(self):
        result = self.run_case("all_unknown")
        event = result["event"]
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "no_known_sites")
        self.assertEqual(event["ic_stability_known_sites"], 0)
        self.assertGreater(event["ic_stability_skipped_sites"], 0)
        self.assertEqual(
            event["ic_stability_skipped_sites"], event["ic_stability_sites"]
        )
        self.assertEqual(result["descriptor_reads"], 1)
        self.assertEqual(result["cold_exception"], "AttributeError")

    def test_environment_off_preserves_original_threshold(self):
        result = self.run_case("mature", stability="0")
        self.assertEqual(result["event"]["calls"], 64)
        self.assertEqual(result["event"]["ic_stability_reason"], "disabled")
        self.assertEqual(result["event"]["ic_stability_samples"], 0)

    def test_default_configuration_enables_stability(self):
        result = self.run_case("mature", stability=None)
        self.assertEqual(result["event"]["calls"], 64)
        self.assertEqual(result["event"]["ic_stability_reason"], "stable")
        self.assertEqual(result["event"]["ic_stability_samples"], 3)
        self.assertEqual(result["event"]["ic_stability_first_calls"], 56)

    def test_loop_work_keeps_low_original_threshold(self):
        event = self.run_case("low_threshold", mode="auto:2")["event"]
        self.assertEqual(event["ic_stability_reason"], "loop_work")
        self.assertEqual(event["calls"], 2)
        self.assertEqual(event["effective_limit"], 2)
        self.assertEqual(event["ic_stability_admission_calls"], 2)
        self.assertEqual(event["ic_stability_samples"], 0)
        self.assertGreater(event["shape"]["loop_score"], 0)

    def test_loop_work_late_change_keeps_threshold_and_current_values(self):
        event = self.run_case("late_warmup_change")["event"]
        self.assertEqual(event["ic_stability_reason"], "loop_work")
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["calls"], 64)
        self.assertGreater(event["shape"]["loop_score"], 0)

    def test_loop_work_newly_visited_path_keeps_threshold_and_values(self):
        event = self.run_case("unknown_then_known")["event"]
        self.assertEqual(event["ic_stability_reason"], "loop_work")
        self.assertEqual(event["ic_stability_admission_calls"], 64)
        self.assertEqual(event["calls"], 64)
        self.assertGreater(event["shape"]["loop_score"], 0)

    def test_loop_work_does_not_override_environment_off(self):
        event = self.run_case("low_threshold", mode="auto:2", stability="0")["event"]
        self.assertEqual(event["calls"], 2)
        self.assertEqual(event["ic_stability_reason"], "disabled")

    def test_loop_work_does_not_override_numeric_mode(self):
        event = self.run_case("low_threshold", mode="2")["event"]
        self.assertEqual(event["calls"], 2)
        self.assertEqual(event["ic_stability_reason"], "non_auto")

    def test_loop_work_does_not_override_explicit_jit_list(self):
        result = self.run_case("explicit_loop", mode="auto:2")
        event = result["event"]
        self.assertEqual(event["calls"], 2)
        self.assertEqual(event["ic_stability_reason"], "explicit")
        self.assertEqual(event["ic_stability_samples"], 0)
        self.assertIsNotNone(result["first_compiled_invocation"])

    def test_explicit_jit_list_does_not_enable_stability_observation(self):
        event = self.run_case("explicit_jit_list")["event"]
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "explicit")
        self.assertEqual(event["ic_stability_samples"], 0)

    def test_numeric_autojit_mode_does_not_enable_stability(self):
        result = self.run_case("mature", mode="64")
        self.assertEqual(result["event"]["calls"], 64)
        self.assertEqual(result["event"]["ic_stability_reason"], "non_auto")

    def test_manual_threshold_preserves_non_classifying_mode(self):
        result = self.run_case("manual")
        self.assertEqual(result["event"]["calls"], 64)
        self.assertEqual(result["event"]["ic_stability_reason"], "non_auto")

    def test_no_supported_sites_preserves_original_threshold(self):
        result = self.run_case("no_sites")
        self.assertEqual(result["event"]["calls"], 64)
        self.assertEqual(result["event"]["ic_stability_reason"], "no_sites")

    def test_attribute_phase_changes_keep_values_exceptions_and_effects(self):
        result = self.run_case("attribute_phases")
        self.assertEqual(result["event"]["ic_stability_reason"], "stable")
        self.assertEqual(result["phase_results"], [8, 21, 31, 42])
        self.assertEqual(result["property_reads"], 2)
        self.assertEqual(result["exception"], "LookupError")

    def test_subscript_errors_and_type_changes_preserve_semantics(self):
        result = self.run_case("subscript")
        self.assertEqual(result["event"]["ic_stability_reason"], "stable")
        self.assertEqual(result["values"], [11, 23, 37])
        self.assertEqual(result["errors"], ["IndexError", "KeyError", "TypeError"])

    def test_instance_value_type_churn_does_not_delay_original_threshold(self):
        result = self.run_case("instance_value_type_churn")
        event = result["event"]
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "no_known_sites")
        self.assertEqual(event["ic_stability_known_sites"], 0)
        self.assertEqual(event["ic_stability_sites"], 1)
        self.assertEqual(event["ic_stability_skipped_sites"], 1)
        self.assertEqual(result["values"], [7, 11])

    def test_consumed_method_feedback_still_uses_stability_gate(self):
        event = self.run_case("method_with_values")["event"]
        self.assertEqual(event["calls"], 64)
        self.assertEqual(event["ic_stability_reason"], "stable")
        self.assertEqual(event["ic_stability_known_sites"], 1)
        self.assertEqual(event["ic_stability_skipped_sites"], 0)

    def test_instance_value_generic_load_and_transitions_preserve_semantics(self):
        result = self.run_case("instance_value_semantics")
        for key in ("event", "method_event"):
            event = result[key]
            self.assertEqual(event["ic_stability_reason"], "no_known_sites")
            self.assertEqual(event["calls"], 64)
            self.assertEqual(event["ic_stability_known_sites"], 0)
            self.assertEqual(event["ic_stability_skipped_sites"], 1)
            self.assertEqual(event["ic_stability_sites"], 1)
        self.assertEqual(result["values"], [7, 11, 31, 19, 23, 29, 37])
        self.assertEqual(result["getattr_reads"], 1)
        self.assertEqual(result["property_reads"], 2)
        self.assertEqual(result["exception"], "LookupError")
        self.assertEqual(result["method_values"], [3, 6, 10])
        self.assertEqual(result["method_getattr_reads"], 1)
        self.assertEqual(result["deopt_count"], 1)


if __name__ == "__main__":
    unittest.main()
