import json
from pathlib import Path
import tempfile
import tomllib
import unittest

from ci_pipeline.jit311.autocompile_penetration import classify
from ci_pipeline.jit311.runtime_transition_report import (
    compare_frame_positions,
    compare_native_recursion_boundary,
    compare_penetration,
    compare_recursion_boundary,
    judge_transitions,
)
from ci_pipeline.jit311.runtime_transition_acceptance import RuntimeTransitionAcceptanceRunner, validate_approved_deviations


ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "ci_pipeline/jit311/data"


class RuntimeTransitionReportTest(unittest.TestCase):
    def test_native_recursion_comparison_requires_stock_c_api_semantics(self):
        state = {
            "recursion_remaining": 0,
            "recursion_headroom": 0,
            "boundary_active": False,
            "jit_entries": 0,
        }
        native = {
            "entered": False,
            "return_code": -1,
            "error_occurred": True,
            "exception_type": "RecursionError",
            "exception_message": "maximum recursion depth exceeded",
            "before": state,
            "after": state,
        }
        stock = {
            "result": "PASS",
            "outer_before": {**state, "recursion_remaining": 52},
            "native_helper_executed": True,
            "native": native,
            "call_error": None,
            "outer_after": {**state, "recursion_remaining": 52},
            "post_error_normal_call": 10,
        }
        jit = {
            **stock,
            "machine_entry_proven": True,
            "entry_ledger_dropped": 0,
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stock_path = directory / "stock.json"
            jit_path = directory / "jit.json"
            stock_path.write_text(json.dumps(stock))
            jit_path.write_text(json.dumps(jit))
            report = compare_native_recursion_boundary(stock_path, jit_path)
            jit["native_helper_executed"] = False
            jit["native"] = None
            jit["call_error"] = {
                "type": "RecursionError",
                "message": "maximum recursion depth exceeded",
                "frames": [],
            }
            jit_path.write_text(json.dumps(jit))
            wrong = compare_native_recursion_boundary(stock_path, jit_path)
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")
        self.assertTrue(wrong["product_fix_required"])

    def test_final_deviation_proof_requires_semantics_and_exact_footprint(self):
        adaptive = {
            "test.test_dis.DisTests.test_super_instructions",
            "test.test_dis.DisWithFileTests.test_super_instructions",
        }
        slots = "test.test_descr.ClassPropertiesAndMethods.test_slots"
        testcases = adaptive | {slots}
        numeric_spec = {
            "regex": (
                "^AssertionError: AssertionError: "
                "(?P<lhs>[0-9]+) != (?P<rhs>[0-9]+)$"
            ),
            "direction": "rhs_minus_lhs",
            "expected": 5,
        }
        penetration = {
            "differential": {
                "differences": {testcase: {} for testcase in testcases},
                "approved_deviations": [
                    {
                        "testcase": testcase,
                        "classification": (
                            "APPROVED_STRESS_MODE_DEVIATION"
                            if testcase == slots
                            else "APPROVED_ADAPTIVE_DISASSEMBLY_DEVIATION"
                        ),
                        "reason": (
                            "one-time JIT publication footprint; not repeated lookup leak"
                            if testcase == slots
                            else "adaptive"
                        ),
                        "proof": (
                            "R/footprint.json"
                            if testcase == slots
                            else "P/adaptive-semantic-probe.json"
                        ),
                        **(
                            {"fingerprint_numeric_delta": numeric_spec}
                            if testcase == slots
                            else {}
                        ),
                    }
                    for testcase in testcases
                ],
                "fingerprints": {
                    testcase: {
                        "matched": True,
                        **(
                            {
                                "numeric_delta": {
                                    "lhs": 19457,
                                    "rhs": 19462,
                                    "direction": "rhs_minus_lhs",
                                    "delta": 5,
                                    "expected": 5,
                                    "matched": True,
                                }
                            }
                            if testcase == slots
                            else {}
                        ),
                    }
                    for testcase in testcases
                },
            },
            "adaptive_semantic_probe": {
                "result": "PASS",
                "machine_entries_delta": 11,
                "checks": {
                    "control_load_quickens": True,
                    "control_loop_quickens": True,
                    "exceptions_equal": True,
                    "jit_load_stays_generic": True,
                    "jit_loop_stays_generic": True,
                    "jit_machine_entry": True,
                    "semantic_results_equal": True,
                },
            },
        }
        footprint = {
            "result": "PASS",
            "classification": "APPROVED_STRESS_MODE_DEVIATION",
            "shape": (
                "test.test_descr.ClassPropertiesAndMethods.test_slots:G.__eq__"
            ),
            "strict_plateau": True,
            "strict_checks": {"steady": True, "post_gc": True},
            "delta": {
                "first_publication_gc_objects": 5,
                "first_publication_object_types": {
                    "builtins.CompiledFunction": 1,
                    "builtins.builtin_function_or_method": 1,
                    "builtins.dict": 1,
                    "builtins.tuple": 1,
                    "weakref.ReferenceType": 1,
                },
                "steady_10_to_1000_gc_objects": 0,
                "steady_10_to_1000_object_types": {},
                "steady_resident_code_buffers": 0,
                "steady_compiled_function_creations": 0,
            },
        }
        report = validate_approved_deviations(
            penetration, {"footprint": footprint}
        )
        footprint["delta"]["steady_resident_code_buffers"] = 1
        wrong = validate_approved_deviations(
            penetration, {"footprint": footprint}
        )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")
        self.assertTrue(any("resident" in item for item in wrong["errors"]))

    def test_penetration_deviation_supports_exact_diagnostic_regex(self):
        testcase = "test.test_descr.ClassPropertiesAndMethods.test_slots"
        numeric_spec = {
            "regex": (
                "^AssertionError: AssertionError: "
                "(?P<lhs>[0-9]+) != (?P<rhs>[0-9]+)$"
            ),
            "direction": "rhs_minus_lhs",
            "expected": 5,
        }
        stock = {
            "modules": {"test_descr": "pass"},
            "cases": {testcase: "pass"},
            "diagnostics": {},
        }
        aggressive = {
            "modules": {"test_descr": "fail"},
            "cases": {testcase: "failure"},
            "diagnostics": {
                testcase: "AssertionError: AssertionError: 19457 != 19462"
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            left, right = directory / "left.json", directory / "right.json"
            deviations = directory / "deviations.json"
            left.write_text(json.dumps(stock))
            right.write_text(json.dumps(aggressive))
            deviations.write_text(
                json.dumps(
                    {
                        "deviations": [
                            {
                                "testcase": testcase,
                                "stock": "pass",
                                "aggressive": "failure",
                                "fingerprint_regex": [
                                    numeric_spec["regex"]
                                ],
                                "fingerprint_numeric_delta": numeric_spec,
                            }
                        ]
                    }
                )
            )
            report = compare_penetration(left, right, deviations)
            aggressive["diagnostics"][testcase] = (
                "AssertionError: AssertionError: 19457 != 19461"
            )
            right.write_text(json.dumps(aggressive))
            wrong = compare_penetration(left, right, deviations)
        self.assertEqual(report["result"], "PASS_WITH_APPROVED_DEVIATIONS")
        self.assertEqual(
            report["fingerprints"][testcase]["numeric_delta"]["delta"], 5
        )
        self.assertEqual(wrong["result"], "FAIL")
        self.assertIn("numeric", str(wrong["unexpected"][testcase]))

    def test_recursion_comparison_requires_exact_frames_and_balanced_state(self):
        state = {
            "recursion_remaining": 52,
            "recursion_headroom": 0,
            "boundary_active": False,
            "jit_entries": 0,
        }
        frame = {
            "function": "recursive",
            "tb_lasti": 52,
            "f_lasti": 52,
            "line": 12,
            "position": [12, 12, 36, 62],
        }
        stock_rows = []
        jit_rows = []
        for index in range(1, 7):
            ident = f"R{index}"
            error = (
                None
                if ident == "R6"
                else {
                    "type": "TypeError" if ident == "R4" else "RecursionError",
                    "message": (
                        "required_argument() missing 1 required positional argument: 'value'"
                        if ident == "R4"
                        else "maximum recursion depth exceeded"
                    ),
                }
            )
            target_frames = [] if ident in {"R4", "R6"} else [frame]
            common = {
                "id": ident,
                "before": dict(state),
                "after": dict(state),
                "error": error,
                "target_frames": target_frames,
                "post_error_recovery": 4,
            }
            stock_rows.append({**common, "pre": {"machine_entry_proven": False}})
            jit_rows.append({**common, "pre": {"machine_entry_proven": True}})
        stock = {
            "result": "PASS",
            "rows": stock_rows,
            "entry_ledger_dropped": 0,
        }
        jit = {
            "result": "PASS",
            "rows": jit_rows,
            "entry_ledger_dropped": 0,
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stock_path = directory / "stock.json"
            jit_path = directory / "jit.json"
            stock_path.write_text(json.dumps(stock))
            jit_path.write_text(json.dumps(jit))
            report = compare_recursion_boundary(stock_path, jit_path)
            jit_rows[0]["target_frames"] = []
            jit_rows[1]["after"] = {**state, "jit_entries": 1}
            jit_path.write_text(json.dumps(jit))
            wrong = compare_recursion_boundary(stock_path, jit_path)
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")
        self.assertTrue(any("cardinality" in item for item in wrong["errors"]))
        self.assertTrue(any("ownership leaked" in item for item in wrong["errors"]))

    def test_frame_position_comparison_requires_exact_lasti_and_traceback(self):
        observation = {
            "function": "target",
            "f_lasti": 8,
            "f_lineno": 3,
            "co_position": [3, 3, 4, 10],
            "inspect_position": [3, 3, 4, 10],
            "stack_position": [3, 3, 4, 10],
        }
        frame = {
            "function": "target",
            "tb_lasti": 8,
            "f_lasti": 8,
            "tb_lineno": 3,
            "position": [3, 3, 4, 10],
            "opcode": "CALL",
            "opcode_offset": 4,
            "inline_cache_span": 4,
        }
        running_stock = {
            "result": "PASS",
            "rows": [{"case": "CALL", "observation": observation}],
            "entry_ledger_dropped": 0,
        }
        running_jit = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "observation": observation,
                    "machine_entry_proven": True,
                }
            ],
            "entry_ledger_dropped": 0,
        }
        error_stock = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "target_frame": frame,
                    "traceback_frames": [frame],
                }
            ],
            "entry_ledger_dropped": 0,
        }
        error_jit = {
            "result": "PASS",
            "rows": [
                {
                    "case": "CALL",
                    "target_frame": frame,
                    "traceback_frames": [frame],
                    "machine_entry_proven": True,
                    "transitions": [{"deopt_reason": "UnhandledException"}],
                    "transition_ledger_dropped": 0,
                }
            ],
            "entry_ledger_dropped": 0,
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            paths = []
            for index, document in enumerate(
                (running_stock, running_jit, error_stock, error_jit)
            ):
                path = directory / f"{index}.json"
                path.write_text(json.dumps(document))
                paths.append(path)
            report = compare_frame_positions(*paths)
            error_jit["rows"][0]["target_frame"] = {**frame, "tb_lasti": 6}
            paths[-1].write_text(json.dumps(error_jit))
            wrong = compare_frame_positions(*paths)
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")
        self.assertTrue(any("traceback position" in item for item in wrong["errors"]))

    def test_autocompile_jit_all_and_diagnostic_uses_threshold_one(self):
        runner = RuntimeTransitionAcceptanceRunner(
            wheel=Path("wheel.whl"),
            source=ROOT,
            output=Path("out"),
            lanes={"autocompile_coverage"},
            jobs=16,
            timeout=1200,
        )
        runner.base.stage = ROOT
        jit_all = runner._arm_command(
            out=Path("p2"),
            jit_all=True,
            startup=Path("startup"),
            journal=Path("journal"),
        )
        diagnostic = runner._arm_command(
            out=Path("diag"),
            threshold=1,
            startup=Path("startup"),
            journal=Path("journal"),
        )
        self.assertIn("PYTHONJITALL=1", jit_all)
        self.assertFalse(any(item.startswith("PYTHONJITAUTO=") for item in jit_all))
        self.assertIn("PYTHONJITAUTO=1", diagnostic)
        self.assertNotIn("PYTHONJITALL=1", diagnostic)

    def test_penetration_deviation_requires_fingerprint(self):
        testcase = "test.test_dis.DisTests.test_super_instructions"
        stock = {
            "modules": {"test_dis": "pass"},
            "cases": {testcase: "pass"},
            "diagnostics": {},
        }
        aggressive = {
            "modules": {"test_dis": "fail"},
            "cases": {testcase: "failure"},
            "diagnostics": {
                testcase: "AssertionError RESUME_QUICK LOAD_FAST__LOAD_FAST STORE_FAST__STORE_FAST"
            },
        }
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            left, right = directory / "left.json", directory / "right.json"
            deviations = directory / "deviations.json"
            left.write_text(json.dumps(stock))
            right.write_text(json.dumps(aggressive))
            deviations.write_text(
                json.dumps(
                    {
                        "deviations": [
                            {
                                "testcase": testcase,
                                "stock": "pass",
                                "aggressive": "failure",
                                "fingerprint": [
                                    "AssertionError",
                                    "RESUME_QUICK",
                                    "LOAD_FAST__LOAD_FAST",
                                    "STORE_FAST__STORE_FAST",
                                ],
                            }
                        ]
                    }
                )
            )
            report = compare_penetration(left, right, deviations)
            aggressive["diagnostics"][testcase] = "AssertionError unrelated"
            right.write_text(json.dumps(aggressive))
            wrong = compare_penetration(left, right, deviations)
        self.assertEqual(report["result"], "PASS_WITH_APPROVED_DEVIATIONS")
        self.assertNotIn(testcase, report["unexpected"])
        self.assertEqual(wrong["result"], "FAIL")
        self.assertIn(testcase, wrong["unexpected"])

    def test_transition_judge_requires_stock_and_runtime_reason(self):
        with (DATA / "runtime_transition_manifest.toml").open("rb") as stream:
            specs = tomllib.load(stream)["transition"]
        stock_rows = []
        jit_rows = []
        for spec in specs:
            semantic = {"value": spec["id"]}
            stock_rows.append({"id": spec["id"], "semantic": semantic})
            required = spec["requires_transition_reason"]
            recovery = (
                {
                    "policy": spec["recovery"],
                    "interpreter_resume": True,
                    "semantic_correct": True,
                    "stale_machine_entry": False,
                }
                if spec["recovery"]
                in ("interpreter-resume", "interpreter-after-backoff")
                else {"reentered": True}
            )
            jit_rows.append(
                {
                    "id": spec["id"],
                    "semantic": semantic,
                    "pre": {"compiled": True, "machine_entry_proven": True},
                    "transition": {
                        "transition_ledger_dropped": 0,
                        "transition_rows": (
                            [{"deopt_reason": required[0]}] if required else []
                        ),
                    },
                    "recovery": recovery,
                    "result": "PASS",
                }
            )
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            stock_path, jit_path = directory / "stock.json", directory / "jit.json"
            stock_path.write_text(json.dumps({"transitions": stock_rows}))
            jit_path.write_text(json.dumps({"transitions": jit_rows}))
            report = judge_transitions(
                stock_path, jit_path, DATA / "runtime_transition_manifest.toml"
            )
            jit_rows[0]["transition"]["transition_rows"] = []
            jit_path.write_text(json.dumps({"transitions": jit_rows}))
            wrong = judge_transitions(
                stock_path, jit_path, DATA / "runtime_transition_manifest.toml"
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(wrong["result"], "FAIL")

    def test_penetration_classifier_requires_all_72_own_code_rows(self):
        targets = [
            line.strip()
            for line in (DATA / "frozen_stdlib_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {
                                "machine_code_entries": 1,
                                "compiled_function_creations": 1,
                                "organic_deopt_hits": 0,
                                "forced_deopt_hits": 0,
                            },
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": f"/usr/lib/python3.11/test/{target}.py",
                                "spec_origin": f"/usr/lib/python3.11/test/{target}.py",
                                "package_roots": [],
                            },
                            "entry_ledger": [
                                {
                                    "filename": f"/usr/lib/python3.11/test/{target}.py",
                                    "qualname": "witness",
                                    "firstlineno": 1,
                                    "entries": 1,
                                }
                            ],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["counts"]["OWN_CODE_JIT"], 72)

    def test_penetration_classifier_uses_package_ownership(self):
        targets = [
            line.strip()
            for line in (DATA / "frozen_stdlib_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                package = f"/usr/lib/python3.11/test/{target}"
                is_package = target == "test_dataclasses"
                filename = (
                    f"{package}/case.py"
                    if is_package
                    else f"/usr/lib/python3.11/test/{target}.py"
                )
                origin = f"{package}/__init__.py" if is_package else filename
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": 1},
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": origin,
                                "spec_origin": origin,
                                "package_roots": [package] if is_package else [],
                            },
                            "entry_ledger": [{"filename": filename, "entries": 1}],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
            )
        self.assertEqual(report["result"], "PASS")
        row = report["modules"]["test_dataclasses"]
        self.assertTrue(row["machine_entry_proven"])
        self.assertEqual(row["own_code_entries"], 1)

    def test_jitall_classifier_accepts_three_fail_closed_states(self):
        targets = [
            line.strip()
            for line in (DATA / "frozen_stdlib_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {target: "pass" for target in targets}
            for index, target in enumerate(targets):
                filename = f"/usr/lib/python3.11/test/{target}.py"
                event = {
                    "filename": filename,
                    "qualname": "witness",
                    "count": 1,
                    "post_publication_interpreted_frames": 0,
                }
                entries = []
                if index < 24:
                    entries = [{"filename": filename, "entries": 1}]
                    events = [{**event, "result": "installed"}]
                elif index < 48:
                    events = [{**event, "result": "installed"}]
                else:
                    events = [{**event, "result": "REFUSE_SHAPE_EXECUTE_SURFACE"}]
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": int(bool(entries))},
                            "observe": {
                                "threshold": 0,
                                "threshold_source": "shared-jit-config",
                                "events": events,
                                "events_dropped": 0,
                            },
                            "ownership": {
                                "module_file": filename,
                                "spec_origin": filename,
                                "package_roots": [],
                            },
                            "entry_ledger": entries,
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            stock_path = directory / "stock.json"
            result_path.write_text(json.dumps({"modules": modules}))
            stock_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
                stock_result_path=stock_path,
                jit_all_contract=True,
            )
            changed_path = journal / "24.json"
            changed = json.loads(changed_path.read_text())
            changed["observe"]["events"][0][
                "post_publication_interpreted_frames"
            ] = 1
            changed_path.write_text(json.dumps(changed))
            wrong = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
                stock_result_path=stock_path,
                jit_all_contract=True,
            )
        self.assertEqual(report["result"], "PASS")
        self.assertEqual(report["classified_modules"], 72)
        self.assertEqual(report["counts"]["OWN_CODE_JIT"], 24)
        self.assertEqual(report["counts"]["PUBLISHED_NO_REENTRY"], 24)
        self.assertEqual(report["counts"]["EXPECTED_SAFE_REFUSAL"], 24)
        self.assertEqual(report["counts"].get("COVERAGE_GAP", 0), 0)
        self.assertEqual(wrong["result"], "FAIL")
        self.assertEqual(wrong["modules"][targets[24]]["status"], "COVERAGE_GAP")

    def test_jitall_coverage_defers_own_code_semantics_to_exact_differential(self):
        targets = [
            line.strip()
            for line in (DATA / "frozen_stdlib_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            stock_modules = {target: "pass" for target in targets}
            jit_modules = dict(stock_modules)
            jit_modules[targets[0]] = "fail"
            for index, target in enumerate(targets):
                filename = f"/usr/lib/python3.11/test/{target}.py"
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": 1},
                            "observe": {
                                "threshold": 0,
                                "threshold_source": "shared-jit-config",
                                "events": [
                                    {
                                        "filename": filename,
                                        "qualname": "witness",
                                        "result": "installed",
                                    }
                                ],
                                "events_dropped": 0,
                            },
                            "ownership": {
                                "module_file": filename,
                                "spec_origin": filename,
                                "package_roots": [],
                            },
                            "entry_ledger": [
                                {"filename": filename, "entries": 1}
                            ],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            stock_path = directory / "stock.json"
            result_path.write_text(json.dumps({"modules": jit_modules}))
            stock_path.write_text(json.dumps({"modules": stock_modules}))
            report = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
                stock_result_path=stock_path,
                jit_all_contract=True,
            )
        self.assertEqual(report["result"], "PASS")
        self.assertFalse(report["modules"][targets[0]]["semantic_matches_stock"])
        self.assertEqual(report["modules"][targets[0]]["status"], "OWN_CODE_JIT")

    def test_penetration_classifier_rejects_unowned_rows(self):
        targets = [
            line.strip()
            for line in (DATA / "frozen_stdlib_modules.txt").read_text().splitlines()
            if line.strip() and not line.startswith("#")
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            journal = directory / "journal"
            journal.mkdir()
            modules = {}
            for index, target in enumerate(targets):
                modules[target] = "pass"
                filename = f"/usr/lib/python3.11/test/{target}.py"
                (journal / f"{index}.json").write_text(
                    json.dumps(
                        {
                            "target_module": "test." + target,
                            "trigger": {"machine_code_entries": 1},
                            "observe": {"events": [], "events_dropped": 0},
                            "ownership": {
                                "module_file": filename,
                                "spec_origin": filename,
                                "package_roots": [],
                            },
                            "entry_ledger": [
                                {
                                    "filename": "/usr/lib/python3.11/test/support/__init__.py",
                                    "entries": 1,
                                }
                            ],
                            "entry_ledger_dropped": 0,
                        }
                    )
                )
            result_path = directory / "result.json"
            result_path.write_text(json.dumps({"modules": modules}))
            report = classify(
                journal,
                DATA / "frozen_stdlib_modules.txt",
                result_path,
            )
        self.assertEqual(report["counts"]["A2_COVERAGE_GAP"], 72)
        self.assertEqual(report["counts"].get("OWN_CODE_JIT", 0), 0)


if __name__ == "__main__":
    unittest.main()
