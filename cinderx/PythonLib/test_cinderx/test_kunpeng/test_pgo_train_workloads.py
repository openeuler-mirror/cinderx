import ast
import importlib.util
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
        self.assertEqual(run.call_count, 12)
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
            len(workloads.SCENARIOS) * 30,
        )
        self.assertEqual(
            workloads.total_training_timeout(),
            len(workloads.SCENARIOS) * workloads.DEFAULT_SCENARIO_TIMEOUT,
        )
        with self.assertRaisesRegex(ValueError, "must be positive"):
            workloads.total_training_timeout(0)

    @mock.patch.object(workloads.subprocess, "run")
    def test_startup_imports_uses_configurable_timeout(self, run: mock.Mock) -> None:
        run.return_value = subprocess.CompletedProcess([], 0, "", "")
        workloads.run_scenario(
            "startup_imports", 2, "/tmp/build-lib", scenario_timeout=45
        )
        self.assertEqual(run.call_count, 2)
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["timeout"], 45)

    @mock.patch.object(workloads.subprocess, "run")
    def test_child_failure_prevents_training_success(self, run: mock.Mock) -> None:
        run.side_effect = subprocess.CalledProcessError(1, ["python"])
        counts = {name: 1 for name in workloads.SCENARIOS}
        with self.assertRaisesRegex(RuntimeError, "PGO training failed"):
            workloads.run_training("/tmp/build-lib", counts)

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
