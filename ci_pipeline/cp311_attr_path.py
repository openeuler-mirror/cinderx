"""Install and run the focused CPython 3.11 attribute-path acceptance suite."""

from hashlib import file_digest
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest


REPO = Path(__file__).resolve().parent.parent
KUNPENG = "test_cinderx.test_kunpeng"


def wheel_path(variable):
    value = os.environ.get(variable)
    if not value:
        raise SystemExit(f"{variable} must point to a local wheel")
    path = Path(value).resolve()
    if not path.is_file() or path.suffix != ".whl":
        raise SystemExit(f"{variable} is not a wheel file: {path}")
    with path.open("rb") as stream:
        print(f"{variable}: {path} sha256={file_digest(stream, 'sha256').hexdigest()}", flush=True)
    return str(path)


def setup(run_dir):
    cinderx_wheel = wheel_path("CINDERX_TEST_WHEEL")
    wheelhouse = os.environ.get("CINDERX_PIP_WHEELHOUSE")
    pip_args = ["--disable-pip-version-check"]
    if wheelhouse:
        pip_args += ["--no-index", "--find-links", wheelhouse]
    elif os.environ.get("CINDERX_PIP_OFFLINE") == "1":
        raise SystemExit("CINDERX_PIP_OFFLINE=1 requires CINDERX_PIP_WHEELHOUSE")

    venv = run_dir / "venv"
    subprocess.run([sys.executable, "-I", "-m", "venv", str(venv)], check=True)
    subprocess.run(
        [str(venv / "bin/python"), "-I", "-m", "pip", "install",
         *pip_args, cinderx_wheel],
        check=True,
    )
    # Expose only checkout tests. The installed wheel supplies all runtime files.
    tests = run_dir / "tests"
    tests.mkdir()
    (tests / "test_cinderx").symlink_to(REPO / "cinderx/PythonLib/test_cinderx", target_is_directory=True)


def test(run_dir):
    venv = (run_dir / "venv").resolve()
    if Path(sys.prefix).resolve() != venv:
        raise SystemExit(f"Expected the suite venv at {venv}, got {sys.prefix}")
    for module in ("_cinderx", "_cinderx_auto"):
        spec = importlib.util.find_spec(module)
        if spec is None or spec.origin is None or not Path(spec.origin).resolve().is_relative_to(venv):
            raise SystemExit(f"{module} must come from the suite venv")
        print(f"{module}: {spec.origin}", flush=True)
    if "_cinderx" in sys.modules:
        raise SystemExit("CinderX was already loaded in the OFF test driver")

    tests = str(run_dir / "tests")
    for variable in ("PYTHONHOME", "PYTHONOPTIMIZE", "TORCH_MODULE_ATTR_FASTPATH"):
        os.environ.pop(variable, None)
    os.environ["PYTHONPATH"] = tests
    sys.path.insert(0, tests)
    from test_cinderx.test_kunpeng._interp311_test_support import Interpreter311Case

    class TorchRegistry311Tests(Interpreter311Case):
        def test_same_name_field(self):
            fixture = "case_torch_registry.py"
            stock = self.run_case(fixture, "off", ("stock",), output=True)
            disabled = self.run_case(
                fixture, args=("registry-off",), output=True, ATTR_PATH_TORCH_REGISTRY="0"
            )
            enabled = self.run_case(fixture, args=("registry-on",), output=True)
            self.assertEqual(stock["trace"], disabled["trace"])
            self.assertEqual(stock["trace"], enabled["trace"])
            print("SC05-S1 " + json.dumps([stock, disabled, enabled]), flush=True)

    loader = unittest.TestLoader()
    suite = loader.loadTestsFromNames([
        f"{KUNPENG}.test_interp_startup_311.InterpreterStartup311Tests.test_startup",
        f"{KUNPENG}.test_attr_path_interpreter_311",
    ])
    suite.addTests(loader.loadTestsFromTestCase(TorchRegistry311Tests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    failed, errors, skipped = len(result.failures), len(result.errors), len(result.skipped)
    passed = max(0, result.testsRun - failed - errors - skipped)
    print(f"Tests: {passed} passed, {failed} failed, {errors} error, {skipped} skipped", flush=True)
    return 0 if result.wasSuccessful() and not skipped else 1


if __name__ == "__main__":
    if sys.version_info[:3] != (3, 11, 6):
        raise SystemExit(f"cp311_attr_path requires CPython 3.11.6, got {sys.version}")
    stage, directory = sys.argv[1:]
    run_dir = Path(directory).resolve()
    if stage == "setup":
        setup(run_dir)
    elif stage == "test":
        raise SystemExit(test(run_dir))
    else:
        raise SystemExit(f"Unknown cp311_attr_path stage: {stage}")
