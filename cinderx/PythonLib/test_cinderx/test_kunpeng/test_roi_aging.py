"""Natural-Guard subprocess integration tests for pre-freeze ROI aging.

These are correctness tests, not performance samples. Each child uses a fresh
process, production auto:2, fixed work and explicit short wall-clock intervals.
Exact T-1/T/T+1 arithmetic and timestamp-overflow cases belong to the C++ helper
tests; the integration cases below require conservative measured time brackets.
No child force-compiles, force-deoptimizes, clears policy counts or unfreezes.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import types
import unittest

from cinderx.test_support import run_python_child


INTERVAL_MS = 1000
PERIOD_NS = INTERVAL_MS * 1_000_000


def _numeric_guard_sum(value):
    total = 0
    for _ in range(8):
        total += value
    return total


def _run_values(values):
    results = [_numeric_guard_sum(value) for value in values]
    for value, result in zip(values, results):
        expected_type = float if type(value) is float else int
        if type(result) is not expected_type or result != 8 * int(value):
            raise AssertionError("independent numeric/type oracle failed")
    return len(results)


def _guard_burst(count):
    return _run_values([float(index % 64 + 1) for index in range(count)])


def _wait_at_least(seconds):
    start = time.monotonic_ns()
    deadline = start + int(seconds * 1e9)
    while time.monotonic_ns() < deadline:
        time.sleep(min(0.05, max(0, (deadline - time.monotonic_ns()) / 1e9)))
    return time.monotonic_ns() - start


def _child(case_name):
    import cinderx

    cinderx.init()
    from cinderx import jit
    import cinderjit

    if not jit.is_enabled():
        raise AssertionError("AutoJIT must be active for natural-Guard test")
    # Suppress all drivers before first target call; the measured target is the
    # sole exception. Suppression is not a target state/control operation.
    for function in list(globals().values()):
        if (isinstance(function, types.FunctionType)
                and function.__module__ == __name__
                and function is not _numeric_guard_sum):
            jit.jit_suppress(function)
    cinderjit._clear_autojit_gate_stats()  # observations only, once
    _run_values([index % 64 + 1 for index in range(4096)])
    if not jit.is_jit_compiled(_numeric_guard_sum):
        raise AssertionError("fixed natural warmup did not produce compiled target")
    result = {"case": case_name, "warmup_calls": 4096,
              "interval_environment": os.environ["CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS"],
              "initial_compiled": True, "semantic_checks_pass": True,
              "target_frozen_bit": None, "machine_entry_count": None}
    before = cinderjit._autojit_gate_stats()
    for field in ("roi_aging_events", "roi_aging_count_reduced"):
        if field not in before:
            raise AssertionError("candidate observational aging API missing: " + field)
    if case_name in ("after_wait", "below_period", "above_period"):
        first = time.monotonic_ns()
        _guard_burst(24)
        result["early_span_ns"] = time.monotonic_ns() - first
        result["after_early_compiled"] = jit.is_jit_compiled(_numeric_guard_sum)
        seconds = {"after_wait": 2.1, "below_period": 0.75, "above_period": 1.1}[case_name]
        result["actual_wait_ns"] = _wait_at_least(seconds)
        _guard_burst(8)
        result["complete_guard_span_ns"] = time.monotonic_ns() - first
        result["after_late_compiled"] = jit.is_jit_compiled(_numeric_guard_sum)
        _run_values([index % 64 + 1 for index in range(4096)])
    elif case_name in ("budget_31", "budget_32", "frozen_no_resume"):
        count = 31 if case_name == "budget_31" else 32
        first = time.monotonic_ns()
        _guard_burst(count)
        result["dense_span_ns"] = time.monotonic_ns() - first
        result["after_dense_compiled"] = jit.is_jit_compiled(_numeric_guard_sum)
        if case_name == "frozen_no_resume":
            result["actual_wait_ns"] = _wait_at_least(2.1)
            _run_values([index % 64 + 1 for index in range(8192)])
    elif case_name == "stable":
        _run_values([index % 64 + 1 for index in range(16384)])
    else:
        raise ValueError("unknown child case")
    result["final_compiled"] = jit.is_jit_compiled(_numeric_guard_sum)
    after = cinderjit._autojit_gate_stats()
    result["gate_delta_process_wide"] = {key: after[key] - value for key, value in before.items()}
    print(json.dumps(result, sort_keys=True))


def _environment(interval_ms):
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    # Test configuration is explicit and isolated from surrounding test jobs.
    for name in list(env):
        if name.startswith("CINDERX_AUTOJIT_") or name.startswith("PYTHONJIT"):
            env.pop(name)
    for name in ("CINDERX_DISABLE", "CINDERX_JIT_DISABLE"):
        env.pop(name, None)
    env["CINDERX_PLUGIN_ENABLE"] = "1"
    env["PYTHONJITAUTO"] = "auto:2"
    env["CINDERX_AUTOJIT_ROI_BACKOFF"] = "1"
    env["CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET"] = "32"
    env["CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS"] = "1"
    env["CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS"] = str(interval_ms)
    return env


@unittest.skipIf(
    sys.version_info[:3] == (3, 14, 0),
    "CPython3.14.0 direct calls bypass the AutoJIT classification gate",
)
class RoiAgingNaturalGuardTests(unittest.TestCase):
    def run_case(self, name, interval_ms=INTERVAL_MS):
        with tempfile.TemporaryDirectory() as directory:
            completed = run_python_child(
                Path(__file__).resolve(), "--child", name,
                cwd=directory, env=_environment(interval_ms), timeout=30,
            )
        self.assertEqual(completed.returncode, 0,
                         f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}")
        data = json.loads(completed.stdout.splitlines()[-1])
        self.assertTrue(data["semantic_checks_pass"])
        self.assertTrue(data["initial_compiled"])
        return data

    def test_old_history_decays_after_real_short_interval(self):
        data = self.run_case("after_wait")
        self.assertTrue(data["after_early_compiled"])
        self.assertGreaterEqual(data["actual_wait_ns"], 2 * PERIOD_NS)
        self.assertTrue(data["after_late_compiled"])
        self.assertTrue(data["final_compiled"])
        self.assertGreaterEqual(data["gate_delta_process_wide"]["roi_aging_events"], 1)
        self.assertGreaterEqual(data["gate_delta_process_wide"]["roi_aging_count_reduced"], 18)

    def test_zero_interval_preserves_accumulated_budget(self):
        data = self.run_case("after_wait", interval_ms=0)
        self.assertTrue(data["after_early_compiled"])
        self.assertFalse(data["after_late_compiled"])
        self.assertFalse(data["final_compiled"])
        self.assertEqual(data["gate_delta_process_wide"]["roi_aging_events"], 0)
        self.assertEqual(data["gate_delta_process_wide"]["roi_aging_count_reduced"], 0)

    def test_stable_input_has_no_aging_or_freeze(self):
        data = self.run_case("stable")
        self.assertTrue(data["final_compiled"])
        self.assertEqual(data["gate_delta_process_wide"]["roi_aging_events"], 0)
        self.assertEqual(data["gate_delta_process_wide"]["roi_frozen"], 0)

    def test_natural_budget_boundary_without_elapsed_period(self):
        for count in (31, 32):
            with self.subTest(count=count):
                data = self.run_case("budget_" + str(count))
                self.assertLess(data["dense_span_ns"], PERIOD_NS,
                                "dense integration bracket invalid: period elapsed")
                self.assertEqual(data["after_dense_compiled"], count == 31)
                self.assertEqual(data["final_compiled"], count == 31)

    def test_below_period_does_not_forgive_history(self):
        data = self.run_case("below_period")
        self.assertLess(data["complete_guard_span_ns"], PERIOD_NS,
                        "scheduler overshoot invalidated below-period bracket")
        self.assertFalse(data["after_late_compiled"])
        self.assertEqual(data["gate_delta_process_wide"]["roi_aging_events"], 0)

    def test_above_period_forgives_old_history(self):
        data = self.run_case("above_period")
        self.assertGreaterEqual(data["actual_wait_ns"], PERIOD_NS)
        self.assertTrue(data["after_late_compiled"])
        self.assertGreaterEqual(data["gate_delta_process_wide"]["roi_aging_events"], 1)

    def test_elapsed_time_does_not_revive_already_frozen_target(self):
        data = self.run_case("frozen_no_resume")
        self.assertLess(data["dense_span_ns"], PERIOD_NS,
                        "dense integration bracket invalid: period elapsed")
        self.assertFalse(data["after_dense_compiled"])
        self.assertFalse(data["final_compiled"])
        self.assertEqual(data["gate_delta_process_wide"]["roi_aging_events"], 0)
        self.assertEqual(data["gate_delta_process_wide"]["roi_recompile"], 0)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--child":
        _child(sys.argv[2])
    else:
        unittest.main()
