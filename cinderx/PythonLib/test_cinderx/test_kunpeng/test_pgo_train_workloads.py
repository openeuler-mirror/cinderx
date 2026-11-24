import ast
import importlib.util
import logging
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock



def _load_training_module():
    script = (
        Path(__file__).resolve().parents[4]
        / "cinderx"
        / "TestScripts"
        / "pgo_train_workloads.py"
    )
    spec = importlib.util.spec_from_file_location("pgo_train_workloads", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


workloads = _load_training_module()


class PgoTrainingScenarioTests(unittest.TestCase):
    def small_counts(self) -> dict[str, int]:
        counts = {name: 3 for name in workloads.SCENARIOS}
        counts.update(
            {
                "record_processing": 20,
                "generator_pipeline": 5,
                "generator_cleanup": 5,
                "invalid_inputs": 20,
                "runtime_changes": 20,
                "asyncio_tasks": 2,
                "temporary_objects": 20,
                "cycle_cleanup": 130,
            }
        )
        return counts

    def test_all_twelve_scenarios_complete_with_small_inputs(self) -> None:
        counts = self.small_counts()
        workloads.validate_repetitions(counts)
        results = {
            name: workloads.run_scenario(name, counts[name])
            for name in workloads.SCENARIOS
        }
        self.assertEqual(len(results), 12)
        self.assertTrue(all(results.values()))

    def test_stdlib_workload_uses_a_scoped_clock(self) -> None:
        original_time = logging.time
        original_start = logging._startTime
        milliseconds = []

        def capture(record: logging.LogRecord) -> bool:
            milliseconds.append(record.msecs)
            return True

        logger = logging.getLogger("cinderx-pgo-workload")
        logger.addFilter(capture)
        self.addCleanup(logger.removeFilter, capture)

        for _ in range(2):
            workloads.train_stdlib_boundaries(129)
            self.assertIs(logging.time, original_time)
            self.assertEqual(logging._startTime, original_start)

        self.assertEqual(milliseconds, [125.0, 375.0, 625.0, 875.0, 125.0] * 2)

    def test_missing_extra_and_zero_counts_are_rejected(self) -> None:
        counts = self.small_counts()
        counts.pop("batch_records")
        counts["unexpected"] = 1
        counts["call_shapes"] = 0
        with self.assertRaisesRegex(ValueError, "missing=.*batch_records"):
            workloads.validate_repetitions(counts)

    def test_unknown_scenario_and_zero_work_are_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown training scenario"):
            workloads.run_scenario("missing", 1)
        with self.assertRaisesRegex(ValueError, "must be positive"):
            workloads.run_scenario("batch_records", 0)

    def test_extension_must_come_from_the_requested_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build_lib = root / "build-lib"
            build_lib.mkdir()
            extension = build_lib / "_cinderx.so"
            extension.touch()
            self.assertEqual(
                workloads.verify_extension_path(str(extension), str(build_lib)),
                extension.resolve(),
            )
            outside = root / "other" / "_cinderx.so"
            outside.parent.mkdir()
            outside.touch()
            with self.assertRaisesRegex(RuntimeError, "expected a file under"):
                workloads.verify_extension_path(str(outside), str(build_lib))

    def test_jit_configuration_must_be_set_before_import(self) -> None:
        environment: dict[str, str] = {}
        configured = workloads.configure_jit_environment(environment, {})
        self.assertEqual(configured["PYTHONJITAUTO"], "2")
        self.assertEqual(environment["PYTHONHASHSEED"], "0")
        with self.assertRaisesRegex(RuntimeError, "before the training configuration"):
            workloads.configure_jit_environment({}, {"_cinderx": object()})

    def test_trainer_rejects_other_python_versions(self) -> None:
        with mock.patch.object(workloads.sys, "version_info", (3, 14)):
            with self.assertRaisesRegex(RuntimeError, "require CPython 3.11"):
                workloads.initialize_jit("/tmp/build-lib")

    @mock.patch.object(workloads.subprocess, "run")
    def test_management_process_uses_isolated_children(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "ok\n", "")
        counts = {name: 1 for name in workloads.SCENARIOS}
        workloads.run_training("/tmp/build-lib", counts, timeout=17)
        self.assertEqual(run.call_count, len(workloads.training_plan()))
        for call in run.call_args_list:
            command = call.args[0]
            self.assertEqual(command[1], "-S")
            self.assertIn("--build-lib", command)
            # Children inherit the same per-scenario timeout so grandchild
            # startups follow one configurable budget.
            self.assertIn("--timeout", command)
            self.assertEqual(command[command.index("--timeout") + 1], "17")
            self.assertEqual(call.kwargs["timeout"], 17)

    def test_total_training_timeout_covers_every_scenario(self) -> None:
        self.assertEqual(
            workloads.total_training_timeout(30),
            len(workloads.training_plan()) * 30,
        )
        self.assertEqual(
            workloads.total_training_timeout(),
            len(workloads.training_plan()) * workloads.DEFAULT_SCENARIO_TIMEOUT,
        )
        with self.assertRaisesRegex(ValueError, "must be positive"):
            workloads.total_training_timeout(0)

    @mock.patch.object(workloads.subprocess, "run")
    def test_startup_imports_wrapper_launches_children_without_jit(
        self, run: mock.Mock
    ) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        counts = workloads.TRAINING_REPETITIONS
        with (
            mock.patch.dict(workloads.os.environ, {"CINDERX_JIT_MODE": "off"}),
            mock.patch.dict(workloads.sys.modules),
            mock.patch.object(workloads, "initialize_jit") as initialize_jit,
        ):
            workloads.sys.modules.pop("_cinderx", None)
            workloads._run_child(
                "startup_imports", counts["startup_imports"], "/tmp/build-lib", 45
            )
        initialize_jit.assert_not_called()
        self.assertEqual(run.call_count, 4)
        for call in run.call_args_list:
            self.assertEqual(call.args[0][1:3], ["-S", "-c"])
            self.assertEqual(call.kwargs["timeout"], 45)
            self.assertEqual(call.kwargs["env"]["CINDERX_JIT_MODE"], "execute")

    @mock.patch.object(workloads.subprocess, "run")
    def test_training_aggregates_child_timeouts_and_failures(self, run: mock.Mock) -> None:
        def finish(command, **kwargs):
            scenario = command[command.index("--scenario") + 1]
            if scenario == "startup_imports":
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            if scenario == "asyncio_tasks":
                raise subprocess.CalledProcessError(1, command, stderr="child failed")
            return subprocess.CompletedProcess(command, 0, "ok\n", "")

        run.side_effect = finish
        counts = {name: 1 for name in workloads.SCENARIOS}
        with self.assertRaisesRegex(RuntimeError, "PGO training failed") as error:
            workloads.run_training("/tmp/build-lib", counts, timeout=17)
        self.assertRegex(str(error.exception), r"startup_imports:[^\n]*timed out after 17")
        self.assertIn("asyncio_tasks: child failed", str(error.exception))
        self.assertEqual(run.call_count, len(workloads.training_plan()))

    def test_training_modes_preserve_execute_and_add_off_coverage(self):
        with mock.patch.dict(workloads.os.environ, {"CINDERX_PGO_INTERPRETER_TRAINING": "1"}):
            plan = workloads.training_plan()
            self.assertEqual(plan[:12], [("execute", n) for n in workloads.SCENARIOS])
            self.assertEqual(len(plan), 19)
        with mock.patch.dict(workloads.os.environ, {"CINDERX_PGO_INTERPRETER_TRAINING": "0"}):
            self.assertEqual(len(workloads.training_plan()), 12)
            self.assertEqual(workloads.total_training_timeout(17), 12 * 17)
        with mock.patch.dict(workloads.os.environ, {"CINDERX_PGO_INTERPRETER_TRAINING": "bad"}):
            with self.assertRaises(ValueError):
                workloads.training_plan()

    def test_training_scrubs_inherited_runtime_policy(self):
        environment = dict(
            ATTR_PATH_MODE="cinderx",
            CINDERX_JIT_DISABLE="1",
            CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS="60000",
            PYTHONJITDISABLE="1",
        )
        workloads.configure_jit_environment(environment, {})
        self.assertNotIn("ATTR_PATH_MODE", environment)
        self.assertNotIn("PYTHONJITDISABLE", environment)
        self.assertEqual(environment["CINDERX_JIT_MODE"], "execute")
        self.assertEqual(environment["CINDERX_JIT_STARTUP_GRACE_MS"], "0")
        self.assertEqual(environment["CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS"], "0")

    def test_generic_attribute_training_checksum(self):
        self.assertTrue(workloads.train_attribute_protocol(103))

    def test_training_module_has_no_third_party_imports(self) -> None:
        tree = ast.parse(Path(workloads.__file__).read_text(encoding="utf-8"))
        imported = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".")[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imported.add(node.module.split(".")[0])
        self.assertNotIn("pytest", imported)
        self.assertNotIn("pyperf", imported)
        self.assertNotIn("pyperformance", imported)
        self.assertNotIn("diffgate_rt", imported)


if __name__ == "__main__":
    unittest.main()
