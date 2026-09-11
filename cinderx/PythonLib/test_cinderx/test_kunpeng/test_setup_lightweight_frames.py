import ast
from pathlib import Path
import unittest


def _load_setup_function(name: str):
    setup_py = Path(__file__).resolve().parents[4] / "setup.py"
    setup_ast = ast.parse(setup_py.read_text(), filename=str(setup_py))
    for node in setup_ast.body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            module = ast.Module(body=[node], type_ignores=[])
            ast.fix_missing_locations(module)
            namespace: dict[str, object] = {}
            exec(compile(module, str(setup_py), "exec"), namespace)
            return namespace[name]
    raise RuntimeError(f"{name} not found in {setup_py}")


should_enable_lightweight_frames = _load_setup_function(
    "should_enable_lightweight_frames"
)


class LightweightFramesDefaultTests(unittest.TestCase):
    def test_oss_311_builds_runtime_selectable_support(self) -> None:
        for machine in ("aarch64", "x86_64"):
            with self.subTest(machine=machine):
                self.assertTrue(
                    should_enable_lightweight_frames(
                        py_version="3.11",
                        meta_python=False,
                        machine=machine,
                    )
                )

    def test_oss_314_aarch64_default_on(self) -> None:
        self.assertTrue(
            should_enable_lightweight_frames(
                py_version="3.14",
                meta_python=False,
                machine="aarch64",
            )
        )

    def test_oss_314_arm64_default_on(self) -> None:
        self.assertTrue(
            should_enable_lightweight_frames(
                py_version="3.14",
                meta_python=False,
                machine="arm64",
            )
        )

    def test_oss_314_x86_64_default_off(self) -> None:
        self.assertFalse(
            should_enable_lightweight_frames(
                py_version="3.14",
                meta_python=False,
                machine="x86_64",
            )
        )

    def test_meta_312_keeps_existing_default(self) -> None:
        self.assertTrue(
            should_enable_lightweight_frames(
                py_version="3.12",
                meta_python=True,
                machine="x86_64",
            )
        )


if __name__ == "__main__":
    unittest.main()
