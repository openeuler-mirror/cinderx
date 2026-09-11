import importlib.util
from pathlib import Path
import tempfile
import unittest



def _load_probe_module():
    script = (
        Path(__file__).resolve().parents[4]
        / "cinderx"
        / "TestScripts"
        / "pgo_dependency_probe.py"
    )
    spec = importlib.util.spec_from_file_location("pgo_dependency_probe", script)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = _load_probe_module()


class PgoDependencyProbeTests(unittest.TestCase):
    def test_snapshot_and_change_detection(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "module.gcda"
            ignored = root / "module.gcno"
            profile.write_bytes(b"first")
            ignored.write_bytes(b"ignored")
            before = probe.snapshot(root)
            self.assertEqual(set(before), {"module.gcda"})
            profile.write_bytes(b"second")
            raw = root / "child.profraw"
            raw.write_bytes(b"raw")
            after = probe.snapshot(root)
            self.assertEqual(
                probe.changed(before, after),
                ["child.profraw", "module.gcda"],
            )

    def test_pip_install_is_offline_and_does_not_resolve_dependencies(self) -> None:
        arguments = probe.pip_arguments(Path("sample.whl"), Path("target"))
        self.assertIn("--no-index", arguments)
        self.assertIn("--no-deps", arguments)
        self.assertEqual(arguments[-1], "sample.whl")


if __name__ == "__main__":
    unittest.main()
