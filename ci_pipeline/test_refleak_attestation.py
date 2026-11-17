"""The measurement shim must not repair startup or omit failed isolation."""

from pathlib import Path
import runpy
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import patch

from ci_pipeline.jit311.refleak_verdict import check_attestation


SHIM = Path(__file__).parent / "scripts/refleak_attest_sitecustomize.py"


class RefleakAttestationTests(unittest.TestCase):
    def exercise(self, *, present=True, suppress_error=False, compiled=False,
                 cached_alias=False):
        counters = {"machine_code_entries": 1000, "compiled_function_creations": 500}

        def driver(*args):
            counters["machine_code_entries"] += 77
            counters["compiled_function_creations"] += 5
            return "original result"

        def cleanup():
            pass

        def target():
            pass

        suppressed = []

        def suppress(function):
            suppressed.append(function)
            if suppress_error:
                raise RuntimeError("isolation failed")

        def is_compiled(function):
            # Even an observing path which itself enters machine code must
            # not manufacture the entry delta recorded for the original -R.
            counters["machine_code_entries"] += 1000
            return compiled

        jit = SimpleNamespace(
            jit_suppress=suppress,
            is_jit_compiled=is_compiled,
        )
        native = SimpleNamespace(
            is_frame_evaluator_installed=lambda: present,
            _get_trigger_stats=lambda: dict(counters),
        )
        package = ModuleType("test")
        package.__path__ = []
        regrtest = ModuleType("test.libregrtest")
        regrtest.refleak = SimpleNamespace(dash_R=driver, dash_R_cleanup=cleanup)
        runner = ModuleType("test.libregrtest.runtest")
        runner.dash_R = driver
        modules = {
            "test": package,
            "test.libregrtest": regrtest,
            "test.libregrtest.runtest": runner,
            "cinderjit": jit if present else None,
            "_cinderx": native,
        }
        with tempfile.TemporaryDirectory() as directory:
            ledger = Path(directory) / "ledger"
            with patch.dict(sys.modules, modules), patch.dict(
                "os.environ", {"CINDERX_REFLEAK_LEDGER": str(ledger)}
            ), patch("atexit.register") as register:
                if suppress_error:
                    with self.assertRaisesRegex(RuntimeError, "isolation failed"):
                        runpy.run_path(str(SHIM))
                else:
                    runpy.run_path(str(SHIM))
                    if present:
                        self.assertIs(runner.dash_R, regrtest.refleak.dash_R)
                        call = runner.dash_R if cached_alias else regrtest.refleak.dash_R
                        self.assertEqual(
                            call(None, "test_subject", None),
                            "original result",
                        )
                register.assert_called_once()
                register.call_args.args[0]()
                fields = ledger.read_text(encoding="utf-8").split()
        self.assertEqual(len(fields), 6)
        self.assertNotIn(target, suppressed)
        return fields, suppressed, (driver, cleanup)

    def test_existing_product_startup_keeps_only_measurement_interpreted(self):
        fields, suppressed, functions = self.exercise()
        self.assertEqual(
            [function.__name__ for function in suppressed],
            ["_attest", "_isolate_measurement", "driver", "cleanup", "measured"],
        )
        self.assertTrue(all(function in suppressed for function in functions))
        self.assertEqual(fields[1:], ["True", "77", "5", "True", "test_subject"])

    def test_missing_product_startup_is_not_repaired(self):
        fields, suppressed, _ = self.exercise(present=False)
        self.assertEqual(suppressed, [])
        self.assertEqual(fields[1:], ["False", "0", "0", "False", "-"])

    def test_cached_runner_alias_preserves_result_and_delta(self):
        fields, _, _ = self.exercise(cached_alias=True)
        self.assertEqual(fields[1:], ["True", "77", "5", "True", "test_subject"])

    def test_isolation_exception_still_attests_failure(self):
        fields, _, _ = self.exercise(suppress_error=True)
        self.assertEqual(fields[-2], "False")

    def test_compiled_measurement_cannot_attest_success(self):
        fields, _, _ = self.exercise(compiled=True)
        self.assertEqual(fields[-2], "False")

    def test_all_original_processes_must_be_valid(self):
        good = "7 True 77 5 True test_subject\n"
        helper = "8 False 0 0 False -\n"
        self.assertTrue(check_attestation("test_subject", good + helper)[0])
        for bad in (
            "8 False 77 5 True test_subject\n",
            "8 True 0 5 True test_subject\n",
            "8 True 77 5 False test_subject\n",
            "8 True -1 5 True test_subject\n",
            "8 True nope 5 True test_subject\n",
            "8 True 77 5 True\n",
        ):
            with self.subTest(bad=bad):
                self.assertFalse(check_attestation("test_subject", good + bad)[0])

    def test_startup_or_other_module_cannot_supply_entry_proof(self):
        self.assertFalse(check_attestation("test_subject", "")[0])
        self.assertFalse(check_attestation(
            "test_subject", "7 True 77 5 True different_module\n"
        )[0])


if __name__ == "__main__":
    unittest.main()
