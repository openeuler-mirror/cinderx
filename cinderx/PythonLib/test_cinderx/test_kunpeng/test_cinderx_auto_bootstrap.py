import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from cinderx.test_support import assert_python_child_ok, run_python_child


PYTHONLIB = Path(__file__).parents[2]
CHILD = Path(__file__).with_name("child_cases") / "auto_bootstrap.py"
FIXTURES = Path(__file__).with_name("fixtures") / "auto_bootstrap"


class CinderXAutoBootstrapTest(unittest.TestCase):
    def test_discovery_only_startup_stays_lightweight_without_candidate(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            env = os.environ.copy()
            env.pop("CINDERX_PLUGIN_ENABLE", None)
            env.pop("CINDERX_DISABLE", None)
            env["PYTHONPATH"] = f"{tempdir}{os.pathsep}{PYTHONLIB}"

            completed = run_python_child(
                CHILD,
                "discovery-only-zero-candidate",
                python_options=("-S",),
                env=env,
                timeout=30,
            )
            assert_python_child_ok(
                completed,
                context="zero-candidate plugin discovery bootstrap",
            )

    def test_manifest_candidate_runs_real_discovery_without_adapter_import(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            dist_info = temp / "candidate-1.0.dist-info"
            dist_info.mkdir()
            (dist_info / "METADATA").write_text(
                "Metadata-Version: 2.1\nName: candidate\nVersion: 1.0\n",
                encoding="utf-8",
            )
            (dist_info / "cinderx_plugin.json").write_text(
                json.dumps(
                    {
                        "id": "candidate",
                        "spi_version": "1",
                        "runtime_abi": {
                            "python_version": "3.14",
                            "soabi": "test",
                            "core_build_id": "test",
                            "cpu_caps": [],
                        },
                        "target_capabilities": [],
                        "provides": {},
                        "adapter": {
                            "entry": "never_imported.adapter",
                            "target": "never_imported_target",
                        },
                    }
                ),
                encoding="utf-8",
            )

            env = os.environ.copy()
            env.pop("CINDERX_PLUGIN_ENABLE", None)
            env.pop("CINDERX_DISABLE", None)
            env["PYTHONPATH"] = f"{temp}{os.pathsep}{PYTHONLIB}"

            completed = run_python_child(
                CHILD,
                "plugin-candidate",
                python_options=("-S",),
                env=env,
                timeout=30,
            )
            assert_python_child_ok(
                completed,
                context="manifest-candidate plugin discovery bootstrap",
            )

    def test_plugin_startup_uses_lightweight_bootstrap(self) -> None:
        with self.subTest("does not import the full cinderx package"):
            with tempfile.TemporaryDirectory() as tempdir:
                temp = Path(tempdir)
                shutil.copytree(FIXTURES / "lightweight", temp, dirs_exist_ok=True)

                env = os.environ.copy()
                env["CINDERX_PLUGIN_ENABLE"] = "1"
                env["PYTHONJITAUTO"] = "auto:2"
                env["PYTHONPATH"] = f"{temp}{os.pathsep}{PYTHONLIB}"

                completed = run_python_child(
                    CHILD,
                    "lightweight-bootstrap",
                    python_options=("-S",),
                    env=env,
                    timeout=30,
                )
                assert_python_child_ok(
                    completed,
                    context="lightweight CinderX auto bootstrap",
                )

    def test_jit_disabled_startup_does_not_import_cinderjit(self) -> None:
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            shutil.copytree(FIXTURES / "jit_disabled", temp, dirs_exist_ok=True)

            env = os.environ.copy()
            env["CINDERX_PLUGIN_ENABLE"] = "1"
            env["PYTHONJITDISABLE"] = "1"
            env["PYTHONPATH"] = f"{temp}{os.pathsep}{PYTHONLIB}"

            completed = run_python_child(
                CHILD,
                "jit-disabled",
                python_options=("-S",),
                env=env,
                timeout=30,
            )
            stderr = completed.stderr or ""
            assert_python_child_ok(
                completed,
                context="JIT-disabled CinderX auto bootstrap",
            )
            self.assertNotIn("Traceback", stderr)


if __name__ == "__main__":
    unittest.main()
