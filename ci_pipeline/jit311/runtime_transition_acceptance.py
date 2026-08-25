"""Unified CPython 3.11 CinderX JIT runtime-transition acceptance runner."""

from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import sys

from ci_pipeline.jit311.execution_acceptance import ExecutionAcceptanceRunner, PASS_STATES
from ci_pipeline.jit311.runtime_transition_report import (
    compare_frame_positions,
    compare_native_recursion_boundary,
    compare_penetration,
    compare_recursion_boundary,
    judge_transitions,
)

ADAPTIVE_DEVIATIONS = {
    "test.test_dis.DisTests.test_super_instructions",
    "test.test_dis.DisWithFileTests.test_super_instructions",
}
FOOTPRINT_DEVIATION = "test.test_descr.ClassPropertiesAndMethods.test_slots"
EXPECTED_PUBLICATION_TYPES = {
    "builtins.CompiledFunction": 1,
    "builtins.builtin_function_or_method": 1,
    "builtins.dict": 1,
    "builtins.tuple": 1,
    "weakref.ReferenceType": 1,
}


def validate_approved_deviations(penetration: dict, repetition: dict) -> dict:
    """Require independent proof for every exact compatibility deviation."""
    differential = penetration.get("differential", {})
    differences = set(differential.get("differences", {}))
    approved_rows = {
        row["testcase"]: row
        for row in differential.get("approved_deviations", [])
    }
    fingerprints = differential.get("fingerprints", {})
    errors = []
    proofs = {}

    unregistered = differences - set(approved_rows)
    if unregistered:
        errors.append(f"unregistered differences: {sorted(unregistered)}")
    supported = ADAPTIVE_DEVIATIONS | {FOOTPRINT_DEVIATION}
    unsupported = differences - supported
    if unsupported:
        errors.append(f"unsupported approved differences: {sorted(unsupported)}")

    adaptive_present = differences & ADAPTIVE_DEVIATIONS
    if adaptive_present:
        if adaptive_present != ADAPTIVE_DEVIATIONS:
            errors.append("adaptive disassembly deviation testcase pair is incomplete")
        semantic = penetration.get("adaptive_semantic_probe", {})
        required_checks = {
            "control_load_quickens",
            "control_loop_quickens",
            "exceptions_equal",
            "jit_load_stays_generic",
            "jit_loop_stays_generic",
            "jit_machine_entry",
            "semantic_results_equal",
        }
        failed_checks = sorted(
            name
            for name in required_checks
            if semantic.get("checks", {}).get(name) is not True
        )
        if semantic.get("result") != "PASS" or failed_checks:
            errors.append(
                "adaptive semantic proof failed: "
                f"result={semantic.get('result')} checks={failed_checks}"
            )
        if int(semantic.get("machine_entries_delta", 0)) <= 0:
            errors.append("adaptive semantic proof has no machine entry")
        for testcase in sorted(adaptive_present):
            if fingerprints.get(testcase, {}).get("matched") is not True:
                errors.append(f"adaptive diagnostic fingerprint failed: {testcase}")
            approved = approved_rows.get(testcase, {})
            if approved.get("classification") != (
                "APPROVED_ADAPTIVE_DISASSEMBLY_DEVIATION"
            ) or approved.get("proof") != "P/adaptive-semantic-probe.json":
                errors.append(f"adaptive baseline metadata is not exact: {testcase}")
        proofs["adaptive_disassembly"] = {
            "testcases": sorted(adaptive_present),
            "semantic_result": semantic.get("result"),
            "checks": semantic.get("checks", {}),
            "machine_entries_delta": semantic.get("machine_entries_delta"),
        }

    if FOOTPRINT_DEVIATION in differences:
        approved = approved_rows.get(FOOTPRINT_DEVIATION, {})
        expected_numeric_spec = {
            "regex": (
                "^AssertionError: AssertionError: "
                "(?P<lhs>[0-9]+) != (?P<rhs>[0-9]+)$"
            ),
            "direction": "rhs_minus_lhs",
            "expected": 5,
        }
        if (
            approved.get("classification") != "APPROVED_STRESS_MODE_DEVIATION"
            or approved.get("reason")
            != "one-time JIT publication footprint; not repeated lookup leak"
            or approved.get("proof") != "R/footprint.json"
            or approved.get("fingerprint_numeric_delta") != expected_numeric_spec
        ):
            errors.append("test_slots baseline metadata is not exact")
        fingerprint = fingerprints.get(FOOTPRINT_DEVIATION, {})
        diagnostic_numeric_delta = fingerprint.get("numeric_delta") or {}
        if (
            fingerprint.get("matched") is not True
            or diagnostic_numeric_delta.get("matched") is not True
            or diagnostic_numeric_delta.get("direction") != "rhs_minus_lhs"
            or diagnostic_numeric_delta.get("delta") != 5
        ):
            errors.append("test_slots actual diagnostic delta is not exact +5")
        footprint = repetition.get("footprint", {})
        delta = footprint.get("delta", {})
        strict_checks = footprint.get("strict_checks", {})
        footprint_errors = []
        if footprint.get("result") != "PASS":
            footprint_errors.append("probe result is not PASS")
        if footprint.get("classification") != "APPROVED_STRESS_MODE_DEVIATION":
            footprint_errors.append("classification is not final approved stress mode")
        if footprint.get("shape") != (
            "test.test_descr.ClassPropertiesAndMethods.test_slots:G.__eq__"
        ):
            footprint_errors.append("probe shape is not exact test_slots G.__eq__")
        if footprint.get("strict_plateau") is not True or not strict_checks or not all(
            value is True for value in strict_checks.values()
        ):
            footprint_errors.append("10/100/1000 or post-GC plateau is not strict")
        if delta.get("first_publication_gc_objects") != 5:
            footprint_errors.append("first publication GC delta is not exactly +5")
        if delta.get("first_publication_object_types") != EXPECTED_PUBLICATION_TYPES:
            footprint_errors.append("first publication type histogram is not exact")
        for field in (
            "steady_10_to_1000_gc_objects",
            "steady_resident_code_buffers",
            "steady_compiled_function_creations",
        ):
            if delta.get(field) != 0:
                footprint_errors.append(f"{field} is not zero")
        if delta.get("steady_10_to_1000_object_types") != {}:
            footprint_errors.append("steady object-type histogram changed")
        if fingerprints.get(FOOTPRINT_DEVIATION, {}).get("matched") is not True:
            footprint_errors.append("test_slots diagnostic fingerprint failed")
        errors.extend(f"footprint proof: {item}" for item in footprint_errors)
        proofs["one_time_publication_footprint"] = {
            "testcase": FOOTPRINT_DEVIATION,
            "classification": footprint.get("classification"),
            "shape": footprint.get("shape"),
            "strict_checks": strict_checks,
            "delta": delta,
            "diagnostic_numeric_delta": diagnostic_numeric_delta,
            "errors": footprint_errors,
        }

    return {
        "result": "PASS" if not errors else "FAIL",
        "actual_deviations": sorted(differences),
        "proofs": proofs,
        "errors": errors,
    }


def published_no_reentry_proof(aggressive: dict) -> dict:
    rows = []
    errors = []
    for module, row in sorted(aggressive.get("modules", {}).items()):
        if row.get("status") != "PUBLISHED_NO_REENTRY":
            continue
        publications = row.get("publication_events", [])
        post_frames = row.get("post_publication_interpreted_frames")
        evidence_complete = row.get("post_publication_evidence_complete") is True
        row_errors = []
        if not publications:
            row_errors.append("no own installed publication event")
        if not evidence_complete:
            row_errors.append("post-publication frame evidence missing")
        if post_frames != 0:
            row_errors.append(
                f"post-publication interpreted frames is {post_frames}, expected 0"
            )
        if row.get("own_code_entries") != 0:
            row_errors.append("own-code machine entry is not zero")
        errors.extend(f"{module}: {error}" for error in row_errors)
        rows.append(
            {
                "module": module,
                "functions": [event.get("qualname") for event in publications],
                "publication_events": publications,
                "publication_call_counts": [
                    event.get("count") for event in publications
                ],
                "machine_entries": row.get("own_code_entries"),
                "post_publication_interpreted_frames": post_frames,
                "evidence_complete": evidence_complete,
                "result": "PASS" if not row_errors else "FAIL",
                "errors": row_errors,
            }
        )
    if aggressive.get("result") != "PASS":
        errors.append("formal JIT-ALL coverage is not PASS")
    return {
        "result": "PASS" if not errors else "FAIL",
        "modules": rows,
        "count": len(rows),
        "errors": errors,
    }


class RuntimeTransitionAcceptanceRunner:
    def __init__(
        self,
        *,
        wheel: Path,
        source: Path,
        output: Path,
        lanes: set[str],
        jobs: int,
        timeout: int,
    ) -> None:
        self.base = ExecutionAcceptanceRunner(
            wheel=wheel,
            source=source,
            output=output,
            lanes=set(),
            jobs=jobs,
            timeout=timeout,
        )
        self.lanes = lanes
        self.results: dict[str, dict] = {}

    @property
    def output(self) -> Path:
        return self.base.output

    def _arm_command(
        self,
        *,
        out: Path,
        threshold: int | None = None,
        jit_all: bool = False,
        startup: Path | None = None,
        journal: Path | None = None,
    ) -> list[str]:
        targets = [
            line.strip()
            for line in (
                self.base.stage / "ci_pipeline/jit311/data/frozen_stdlib_modules.txt"
            )
            .read_text()
            .splitlines()
            if line.strip() and not line.startswith("#")
        ]
        command = [
            str(self.base.python),
            str(self.base.stage / "ci_pipeline/libtest_diff_311.py"),
            "run",
            "--python",
            str(self.base.python),
            "--jobs",
            str(self.base.jobs),
            "--timeout",
            str(self.base.timeout),
            "--out",
            str(out),
        ]
        if startup is not None:
            command += [
                "--pythonpath-prepend",
                str(startup),
                "--pythonpath-prepend",
                str(self.base.stage),
                "--env",
                f"AUTOCOMPILE_PENETRATION_JOURNAL={journal}",
                "--env",
                "CINDERX_JIT_MODE=canary",
                "--env",
                "PYTHONJITGENERATOR=1",
            ]
            command += (
                ["--env", "PYTHONJITALL=1"]
                if jit_all
                else ["--env", f"PYTHONJITAUTO={threshold}"]
            )
        command += ["--tests", *targets]
        return command

    def _jit_all_env(self) -> dict[str, str]:
        env = self.base._base_env()
        env.update(
            CINDERX_JIT_MODE="canary",
            PYTHONJITALL="1",
            PYTHONJITGENERATOR="1",
            PYTHONPATH=str(self.base.stage),
        )
        return env

    def _classify_penetration(
        self,
        name: str,
        journal: Path,
        test_result: Path,
        out: Path,
        *,
        stock_result: Path | None = None,
        jit_all_contract: bool = False,
    ) -> int:
        command = [
            str(self.base.python),
            "-m",
            "ci_pipeline.jit311.autocompile_penetration",
            "--journal",
            str(journal),
            "--targets",
            str(self.base.stage / "ci_pipeline/jit311/data/frozen_stdlib_modules.txt"),
            "--test-result",
            str(test_result),
            "--out",
            str(out),
        ]
        if stock_result is not None:
            command.extend(["--stock-result", str(stock_result)])
        if jit_all_contract:
            command.append("--jit-all-contract")
        return self.base._run(
            name,
            command,
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )

    def run_autocompile_coverage(self) -> dict:
        """The autocompile case proper: the whole-program arm.

        The execution smoke, the threshold-50 stdlib arm and the adaptive
        specialization proof all have their own formal cases (EXECUTION_SMOKE,
        STDLIB_AUTOJIT_REGRESSION, SPECIALIZATION_CONFORMANCE); this case
        proves only what nothing else owns -- the scheduler configuration
        contract, the whole-program JITALL arm with its three-state
        classification and publication-reentry proof, and the stock semantic
        oracle it is compared against.
        """
        directory = self.output / "AUTOCOMPILE_COVERAGE"
        directory.mkdir()
        stock = directory / "p0-stock"
        aggressive = directory / "p2-jit-all"
        aggressive_journal = directory / "p2-journal"
        aggressive_journal.mkdir()
        aggressive_startup = directory / "p2-startup"
        aggressive_startup.mkdir()
        (aggressive_startup / "sitecustomize.py").write_text(
            "from ci_pipeline.jit311 import autocompile_penetration\n"
        )

        rc_config_ut = self.base._run(
            "11-autocompile-config-ut",
            [
                str(self.base.python),
                "-m",
                "pytest",
                "-q",
                str(
                    self.base.stage
                    / "test_cinderx/test_kunpeng/test_jitall_scheduler_config.py"
                ),
            ],
            env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
        )
        rc0 = self.base._run("20-autocompile-stock", self._arm_command(out=stock))
        rc2 = self.base._run(
            "22-autocompile-jit-all",
            self._arm_command(
                out=aggressive,
                jit_all=True,
                startup=aggressive_startup,
                journal=aggressive_journal,
            ),
        )
        aggressive_coverage = directory / "p2-coverage.json"
        rc_aggressive_coverage = self._classify_penetration(
            "24-autocompile-coverage",
            aggressive_journal,
            aggressive / "result.json",
            aggressive_coverage,
            stock_result=stock / "result.json",
            jit_all_contract=True,
        )
        differential = compare_penetration(
            stock / "result.json",
            aggressive / "result.json",
            self.base.stage
            / "ci_pipeline/jit311/data/runtime_transition_compatibility_deviations.json",
        )
        (directory / "p0-vs-p2.json").write_text(
            json.dumps(differential, indent=2, sort_keys=True) + "\n"
        )
        # The adaptive disassembly pair in the frozen deviation register is
        # approved only against this probe's semantic-equivalence checks
        # under the whole-program environment; the execution domain runs
        # the same module for a different question (dis classification
        # under the threshold scheduler), so this is evidence, not a
        # duplicate.
        semantic_probe_path = directory / "adaptive-semantic-probe.json"
        rc_semantic_probe = self.base._run(
            "25-autocompile-adaptive-probe",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.specialization_disassembly_probe",
                "--out",
                str(semantic_probe_path),
            ],
            env=self._jit_all_env(),
        )
        semantic_probe = json.loads(semantic_probe_path.read_text())
        aggressive_report = json.loads(aggressive_coverage.read_text())
        stock_result = json.loads((stock / "result.json").read_text())
        stock_modules_ok = all(
            value == "pass" for value in stock_result.get("modules", {}).values()
        )
        good = (
            rc_config_ut == 0
            and rc0 == 0
            and rc2 == 0
            and rc_aggressive_coverage == 0
            and rc_semantic_probe == 0
            and stock_modules_ok
            and aggressive_report["result"] == "PASS"
            and differential["result"] in PASS_STATES
            and semantic_probe["result"] == "PASS"
        )
        result = {
            "result": (
                "PASS_WITH_APPROVED_DEVIATIONS"
                if good and differential["differences"]
                else "PASS" if good else "FAIL"
            ),
            "stock_result": stock_result,
            "aggressive_coverage": aggressive_report,
            "differential": differential,
            "adaptive_semantic_probe": semantic_probe,
            "commands": {
                "config_ut": rc_config_ut,
                "p0": rc0,
                "p2": rc2,
                "p2_coverage": rc_aggressive_coverage,
            },
        }
        result["published_no_reentry_proof"] = published_no_reentry_proof(
            aggressive_report
        )
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        return result

    def run_state_transitions(self) -> dict:
        directory = self.output / "STATE_TRANSITION"
        directory.mkdir()
        def dual_probe(number, tag, module, stock_out, jit_out):
            rc_probe_stock = self.base._run(
                f"{number}-{tag}-stock",
                [
                    str(self.base.python),
                    "-m",
                    module,
                    "--mode",
                    "stock",
                    "--out",
                    str(stock_out),
                ],
                env={**self.base._base_env(), "PYTHONPATH": str(self.base.stage)},
            )
            rc_probe_jit = self.base._run(
                f"{number}-{tag}-jit",
                [
                    str(self.base.python),
                    "-m",
                    module,
                    "--mode",
                    "jit",
                    "--out",
                    str(jit_out),
                ],
                env=self.base._product_env(threshold="1"),
            )
            return rc_probe_stock, rc_probe_jit

        stock = directory / "stock.json"
        jit = directory / "jit.json"
        rc_stock, rc_jit = dual_probe(
            "30", "transition", "ci_pipeline.jit311.state_transition_probe", stock, jit
        )
        tracing_regression_path = directory / "tracing-regression.json"
        rc_tracing_regression = self.base._run(
            "32-transition-aggressive-tracing-regression",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.tracing_conformance_probe",
                "--out",
                str(tracing_regression_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        running_stock = directory / "frame-position-stock.json"
        running_jit = directory / "frame-position-jit.json"
        error_stock = directory / "error-position-stock.json"
        error_jit = directory / "error-position-jit.json"
        rc_running_stock, rc_running_jit = dual_probe(
            "33",
            "frame-running",
            "ci_pipeline.jit311.frame_position_probe",
            running_stock,
            running_jit,
        )
        rc_error_stock, rc_error_jit = dual_probe(
            "35",
            "frame-error",
            "ci_pipeline.jit311.error_position_probe",
            error_stock,
            error_jit,
        )
        rc_inspect = self.base._run(
            "37-frame-test-inspect",
            [str(self.base.python), "-m", "test", "-j1", "test_inspect"],
            env=self.base._product_env(threshold="1"),
        )
        recursion_stock = directory / "recursion-boundary-stock.json"
        recursion_jit = directory / "recursion-boundary-jit.json"
        rc_recursion_stock, rc_recursion_jit = dual_probe(
            "38",
            "recursion",
            "ci_pipeline.jit311.recursion_boundary_probe",
            recursion_stock,
            recursion_jit,
        )
        native_recursion_stock = directory / "native-recursion-boundary-stock.json"
        native_recursion_jit = directory / "native-recursion-boundary-jit.json"
        rc_native_recursion_stock, rc_native_recursion_jit = dual_probe(
            "39",
            "native-recursion",
            "ci_pipeline.jit311.native_recursion_boundary_probe",
            native_recursion_stock,
            native_recursion_jit,
        )
        if stock.is_file() and jit.is_file():
            result = judge_transitions(
                stock,
                jit,
                self.base.stage / "ci_pipeline/jit311/data/runtime_transition_manifest.toml",
            )
        else:
            result = {
                "result": "FAIL",
                "transitions": [],
                "errors": ["stock or JIT transition worker produced no report"],
            }
        result["worker_returncodes"] = {"stock": rc_stock, "jit": rc_jit}
        tracing_regression = (
            json.loads(tracing_regression_path.read_text())
            if tracing_regression_path.is_file()
            else {"result": "FAIL"}
        )
        result["aggressive_tracing_regression"] = tracing_regression
        result["worker_returncodes"]["tracing_regression"] = rc_tracing_regression
        positions = (
            compare_frame_positions(
                running_stock,
                running_jit,
                error_stock,
                error_jit,
            )
            if all(
                path.is_file()
                for path in (running_stock, running_jit, error_stock, error_jit)
            )
            else {"result": "FAIL", "errors": ["position probe report missing"]}
        )
        result["frame_positions"] = positions
        recursion = (
            compare_recursion_boundary(recursion_stock, recursion_jit)
            if recursion_stock.is_file() and recursion_jit.is_file()
            else {"result": "FAIL", "errors": ["recursion probe report missing"]}
        )
        result["recursion_boundary"] = recursion
        native_recursion = (
            compare_native_recursion_boundary(
                native_recursion_stock, native_recursion_jit
            )
            if native_recursion_stock.is_file() and native_recursion_jit.is_file()
            else {
                "result": "FAIL",
                "errors": ["native recursion probe report missing"],
                "product_fix_required": True,
            }
        )
        result["native_recursion_boundary"] = native_recursion
        result["worker_returncodes"].update(
            {
                "frame_stock": rc_running_stock,
                "frame_jit": rc_running_jit,
                "error_stock": rc_error_stock,
                "error_jit": rc_error_jit,
                "test_inspect": rc_inspect,
                "recursion_stock": rc_recursion_stock,
                "recursion_jit": rc_recursion_jit,
                "native_recursion_stock": rc_native_recursion_stock,
                "native_recursion_jit": rc_native_recursion_jit,
            }
        )
        if (
            rc_stock != 0
            or rc_jit != 0
            or rc_tracing_regression != 0
            or rc_running_stock != 0
            or rc_running_jit != 0
            or rc_error_stock != 0
            or rc_error_jit != 0
            or rc_inspect != 0
            or rc_recursion_stock != 0
            or rc_recursion_jit != 0
            or rc_native_recursion_stock != 0
            or rc_native_recursion_jit != 0
            or tracing_regression.get("result") != "PASS"
            or positions.get("result") != "PASS"
            or recursion.get("result") != "PASS"
            or native_recursion.get("result") != "PASS"
        ):
            result["result"] = "FAIL"
        (directory / "result.json").write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n"
        )
        return result

    def run_transition_recovery(self) -> dict:
        directory = self.output / "TRANSITION_RECOVERY"
        directory.mkdir()
        repetition_path = directory / "repetition.json"
        footprint_path = directory / "footprint.json"
        code_swap_paths = {
            "default": directory / "code-swap-default.json",
            "zero": directory / "code-swap-budget-0.json",
            "large": directory / "code-swap-budget-65535.json",
            "force": directory / "code-swap-force-compile.json",
        }
        rc_repetition = self.base._run(
            "40-recovery-repetition",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.transition_recovery",
                "--cycles",
                "100",
                "--out",
                str(repetition_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        rc_footprint = self.base._run(
            "50-recovery-footprint",
            [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.recovery_footprint_probe",
                "--out",
                str(footprint_path),
            ],
            env=self.base._product_env(threshold="1"),
        )
        code_swap_returncodes = {}
        for name, path in code_swap_paths.items():
            command = [
                str(self.base.python),
                "-m",
                "ci_pipeline.jit311.code_swap_transition_probe",
                "--cycles",
                "100",
            ]
            if name == "force":
                command.append("--force-compile")
            command.extend(["--out", str(path)])
            env = self.base._product_env(threshold="1")
            if name == "zero":
                env["PYTHONJITFRESHATTACHBUDGET"] = "0"
            elif name == "large":
                env["PYTHONJITFRESHATTACHBUDGET"] = "65535"
            code_swap_returncodes[name] = self.base._run(
                f"45-recovery-code-swap-{name}", command, env=env
            )
        repetition = (
            json.loads(repetition_path.read_text())
            if repetition_path.is_file()
            else {"result": "FAIL"}
        )
        footprint = (
            json.loads(footprint_path.read_text())
            if footprint_path.is_file()
            else {"result": "FAIL"}
        )
        code_swap = {
            name: (
                json.loads(path.read_text()) if path.is_file() else {"result": "FAIL"}
            )
            for name, path in code_swap_paths.items()
        }
        good = (
            rc_repetition == 0
            and rc_footprint == 0
            and repetition["result"] == "PASS"
            and footprint["result"] == "PASS"
            and all(value == 0 for value in code_swap_returncodes.values())
            and all(arm.get("result") == "PASS" for arm in code_swap.values())
        )
        result = {
            "result": "PASS" if good else "FAIL",
            "repetition": repetition,
            "footprint": footprint,
            "code_swap": code_swap,
            "returncodes": {
                "repetition": rc_repetition,
                "footprint": rc_footprint,
                "code_swap": code_swap_returncodes,
            },
        }
        return result

    def finalize(self, provenance: dict) -> str:
        blockers: list[dict] = []
        penetration = self.results.get("autocompile_coverage", {})
        aggressive = penetration.get("aggressive_coverage", {})
        transitions = self.results.get("state_transition", {})
        repetition = self.results.get("transition_recovery", {})

        def add_blocker(
            *,
            ident: str,
            severity: str,
            classification: str,
            cluster: str,
            summary: str,
            reproducer: str,
            stock: str,
            jit: str,
            machine_proof: str,
            transition_proof: str,
            root_cause: str,
            changed_files: list[str],
            fix_summary: str,
            regression_tests: list[str],
            evidence: str,
            **extra,
        ) -> None:
            blockers.append(
                {
                    "id": ident,
                    "severity": severity,
                    "classification": classification,
                    "cluster": cluster,
                    "summary": summary,
                    "minimal_reproducer": reproducer,
                    "stock_observable": stock,
                    "jit_observable": jit,
                    "machine_entry_proof": machine_proof,
                    "transition_proof": transition_proof,
                    "root_cause_layer": root_cause,
                    "changed_files": changed_files,
                    "fix_summary": fix_summary,
                    "regression_tests": regression_tests,
                    "evidence": evidence,
                    **extra,
                }
            )

        gaps = sorted(
            module
            for module, row in aggressive.get("modules", {}).items()
            if row.get("status") == "COVERAGE_GAP"
        )
        coverage_counts = aggressive.get("counts", {})
        observed_thresholds = sorted(
            {
                row.get("scheduler_threshold")
                for row in aggressive.get("modules", {}).values()
                if row.get("scheduler_threshold") is not None
            }
        )
        classified = sum(
            int(coverage_counts.get(state, 0))
            for state in (
                "OWN_CODE_JIT",
                "PUBLISHED_NO_REENTRY",
                "EXPECTED_SAFE_REFUSAL",
            )
        )
        coverage_errors = []
        if aggressive.get("classified_modules") != 72 or classified != 72:
            coverage_errors.append(
                "three-state classified population is not exactly 72"
            )
        if gaps or int(coverage_counts.get("COVERAGE_GAP", 0)) != 0:
            coverage_errors.append(f"COVERAGE_GAP remains: {gaps}")
        if aggressive.get("unknown_refusals"):
            coverage_errors.append("unknown scheduler refusal remains")
        totals = aggressive.get("totals", {})
        if int(totals.get("ledger_dropped", 0)) != 0:
            coverage_errors.append("entry ledger dropped evidence")
        if int(totals.get("events_dropped", 0)) != 0:
            coverage_errors.append("scheduler event ledger dropped evidence")
        if observed_thresholds != [0]:
            coverage_errors.append(
                f"JIT-ALL scheduler threshold is not exactly [0]: {observed_thresholds}"
            )
        if penetration.get("commands", {}).get("config_ut") != 0:
            coverage_errors.append("scheduler configuration unit tests failed")
        no_reentry_proof = penetration.get("published_no_reentry_proof", {})
        if no_reentry_proof.get("result") != "PASS":
            coverage_errors.append(
                "PUBLISHED_NO_REENTRY post-publication proof is not PASS"
            )
        if coverage_errors:
            add_blocker(
                ident="AUTOCOMPILE_CONFIG",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="scheduler/configuration",
                summary="; ".join(coverage_errors),
                reproducer="Run the autocompile jit-all arm with only PYTHONJITALL=1 and inspect _get_observe_stats()['threshold'].",
                stock="Stock arm completes 72 requested modules without JIT scheduling.",
                jit=f"Three-state counts are {json.dumps(coverage_counts, sort_keys=True)}; scheduler thresholds are {observed_thresholds}.",
                machine_proof=f"P2 own-code machine entry is {totals.get('actual_own_code_machine_entry_modules', 0)}/72; all other modules require a typed non-entry class.",
                transition_proof="P2 requires threshold=0 and one of OWN_CODE_JIT, PUBLISHED_NO_REENTRY or EXPECTED_SAFE_REFUSAL for every target.",
                root_cause="JIT-ALL scheduler/configuration, classification, or evidence-ledger contract.",
                changed_files=[
                    "ci_pipeline/jit311/runtime_transition_acceptance.py",
                    "ci_pipeline/jit311/autocompile_penetration.py",
                ],
                fix_summary="The final autocompile classification fails closed on any unclassified module, unknown refusal, or dropped evidence.",
                regression_tests=[
                    "ci_pipeline.test_runtime_transition_report.RuntimeTransitionReportTest.test_autocompile_jit_all_and_diagnostic_uses_threshold_one",
                    "ci_pipeline.test_runtime_transition_report.RuntimeTransitionReportTest.test_penetration_classifier_uses_package_ownership",
                ],
                evidence="AUTOCOMPILE_COVERAGE/p2-coverage.json",
                modules=gaps,
                errors=coverage_errors,
            )
        unexpected = penetration.get("differential", {}).get("unexpected", {})
        other_unexpected = sorted(unexpected)
        if other_unexpected:
            add_blocker(
                ident="AUTOCOMPILE_COVERAGE_UNEXPECTED",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="penetration/differential",
                summary="Unexpected Stock vs JIT-ALL correctness differences remain.",
                reproducer="Run the autocompile stock and jit-all arms and compare normalized per-test outcomes.",
                stock="See P/p0-stock/result.json.",
                jit="See P/p2-jit-all/result.json.",
                machine_proof="P/p2-coverage.json records target machine entries.",
                transition_proof="Not applicable; differential is testcase-level.",
                root_cause="Unclassified product behavior difference.",
                changed_files=[],
                fix_summary="No automatic baseline was added.",
                regression_tests=other_unexpected,
                evidence="P/p0-vs-p2.json",
                testcases=other_unexpected,
            )
        deviation_proof = validate_approved_deviations(penetration, repetition)
        if deviation_proof["result"] != "PASS":
            add_blocker(
                ident="APPROVED_DEVIATION_PROOF",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="compatibility/deviation-proof",
                summary="An exact compatibility deviation lacks its required independent proof.",
                reproducer="Run the autocompile and recovery cases and validate the exact testcase fingerprints against the semantic and footprint probes.",
                stock="Stock outcomes are recorded in P/p0-stock/result.json.",
                jit="Only the three reviewed exact testcase outcomes may differ under JIT-ALL.",
                machine_proof="Adaptive and footprint probes each require their own machine-entry evidence.",
                transition_proof="The footprint proof requires exact +5 publication and a zero-growth plateau.",
                root_cause="Deviation fingerprint or independent semantic/mechanism proof.",
                changed_files=[
                    "ci_pipeline/jit311/data/runtime_transition_compatibility_deviations.json",
                    "ci_pipeline/jit311/runtime_transition_acceptance.py",
                ],
                fix_summary="The final judge rejects a baseline-only approval.",
                regression_tests=[
                    "RuntimeTransitionReportTest.test_final_deviation_proof_requires_semantics_and_exact_footprint"
                ],
                evidence="P/p0-vs-p2.json, P/adaptive-semantic-probe.json, R/footprint.json",
                errors=deviation_proof["errors"],
            )
        position_result = transitions.get("frame_positions", {})
        if position_result and position_result.get("result") != "PASS":
            add_blocker(
                ident="FRAME_POSITION_TRANSITION",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="frame/position",
                summary="Running-frame or traceback position matrix differs from Stock.",
                reproducer="Run frame_position_probe and error_position_probe in Stock/JIT arms.",
                stock="Exact f_lasti, line, column and traceback rows are recorded in T/*-stock.json.",
                jit="One or more exact rows differ.",
                machine_proof="Every JIT matrix row requires its own entry-ledger proof.",
                transition_proof="Every error row requires a typed deopt ledger row.",
                root_cause="JIT frame cursor or error resume mapping.",
                changed_files=[
                    "cinderx/Jit/hir/insert_update_prev_instr.cpp",
                    "cinderx/Jit/deopt.cpp",
                    "cinderx/Interpreter/3.11/ceval_wrapper.c",
                ],
                fix_summary="Precise boundary publication and opcode-family error resume were implemented, but the matrix is still red.",
                regression_tests=[
                    "InsertUpdatePrevInstrTest.PythonVisibleBoundariesPublishPrecisePositions",
                    "Exception311Test.PropagatedCallStopsAtTheLastCacheUnit",
                ],
                evidence="STATE_TRANSITION/frame-position-*.json, STATE_TRANSITION/error-position-*.json",
            )
        recursion_result = transitions.get("recursion_boundary", {})
        if recursion_result and recursion_result.get("result") != "PASS":
            add_blocker(
                ident="RECURSION_BOUNDARY_TRANSITION",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="recursion/entry-accounting",
                summary="Stock/JIT recursion frame cardinality or accounting differs.",
                reproducer="Run recursion_boundary_probe in Stock and JIT arms plus transition T10.",
                stock="R1-R6 and T10 provide the exact Stock traceback-frame oracle.",
                jit="Every target frame, exception, cursor and post-error accounting must match Stock.",
                machine_proof="Every JIT recursion row requires an entry-ledger row for its target.",
                transition_proof="T10 and R1-R6 compare full target-frame lists and balanced recursion state.",
                root_cause="Recursion frame lifecycle or recursion-slot ownership.",
                changed_files=[
                    "cinderx/Jit/jit_rt.cpp",
                    "cinderx/Interpreter/3.11/interpreter.c",
                ],
                fix_summary="The final gate does not permit a recursion-cardinality deviation.",
                regression_tests=[
                    "JITLifecycle311Test.BindFailureAtRecursionLimitMatchesStock",
                    "recursion_boundary_probe:R1-R6",
                    "T10",
                ],
                evidence="STATE_TRANSITION/recursion-boundary-*.json, STATE_TRANSITION/result.json",
                errors=recursion_result.get("errors", []),
            )
        native_recursion_result = transitions.get("native_recursion_boundary", {})
        if native_recursion_result.get("result") != "PASS":
            add_blocker(
                ident="NATIVE_RECURSION_BOUNDARY_TRANSITION",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="recursion/native-c-api",
                summary="Direct native Py_EnterRecursiveCall behavior differs from Stock.",
                reproducer="Run native_recursion_boundary_probe in Stock and JIT arms.",
                stock="The last admitted Python frame calls the native recursion probe.",
                jit="The compiled target must return the same rc, exception and accounting.",
                machine_proof="The JIT arm requires a native_recursive entry-ledger row.",
                transition_proof="Before/after remaining, headroom, boundary and JIT ownership are recorded.",
                root_cause="Logical JIT recursion boundary versus CPython native recursion API.",
                changed_files=[
                    "cinderx/Jit/jit_rt.cpp",
                    "cinderx/Interpreter/3.11/interpreter.c",
                ],
                fix_summary="CPython recovery headroom is no longer held across user code.",
                regression_tests=[
                    "native_recursion_boundary_probe",
                    "JITLifecycle311Test.BindFailureAtRecursionLimitMatchesStock",
                ],
                evidence="STATE_TRANSITION/native-recursion-boundary-*.json",
                errors=native_recursion_result.get("errors", []),
            )
        transition_failures = [
            row
            for row in transitions.get("transitions", [])
            if row.get("result") == "FAIL"
        ]
        if transition_failures:
            failed_ids = [row["id"] for row in transition_failures]
            add_blocker(
                ident="STATE_TRANSITION_CORRECTNESS",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="runtime/transition",
                summary="One or more state transitions fail the Stock semantic/recovery contract.",
                reproducer="Run the state-transition matrix.",
                stock="See T/stock.json.",
                jit="See T/jit.json.",
                machine_proof="Each failed transition retains pre-JIT entry proof.",
                transition_proof="See transition_rows in T/result.json.",
                root_cause="Unclassified transition runtime layer.",
                changed_files=[
                    "ci_pipeline/jit311/state_transition_probe.py",
                ],
                fix_summary="No transition failure is converted to a deviation.",
                regression_tests=failed_ids,
                evidence="T/stock.json, T/jit.json, T/result.json",
                transitions=failed_ids,
            )
        failed_code_swap = {
            name: arm
            for name, arm in repetition.get("code_swap", {}).items()
            if arm.get("result") != "PASS"
        }
        if failed_code_swap:
            add_blocker(
                ident="CODE_SWAP_POLICY",
                severity="FAIL",
                classification="PRODUCT_BUG",
                cluster="scheduler/code-identity-policy",
                summary="Code-swap diagnostics lack semantics, no-stale-entry or typed-policy proof.",
                reproducer="Run default, budget=0, budget=65535 and force-compile code-swap arms.",
                stock="Python semantics remain the oracle on every cycle.",
                jit="At least one arm fails its explicit policy contract.",
                machine_proof="Each cycle records current-code and stale-old-code entry deltas.",
                transition_proof="Each cycle records scheduler slot, artifact, verdict and attach state.",
                root_cause="Scheduler churn policy or compiler/runtime capability.",
                changed_files=[
                    "ci_pipeline/jit311/code_swap_transition_probe.py",
                    "cinderx/Jit/pyjit.cpp",
                ],
                fix_summary="No forced automatic re-JIT was introduced.",
                regression_tests=["code_swap_transition_probe:default/zero/large/force"],
                evidence="TRANSITION_RECOVERY/code-swap-*.json",
                arms=sorted(failed_code_swap),
            )
        states = [result.get("result") for result in self.results.values()]
        if any(state in {"FAIL", "REVIEW_REQUIRED"} for state in states):
            severity = "FAIL" if "FAIL" in states else "REVIEW_REQUIRED"
            add_blocker(
                ident="CASE_RESULT",
                severity=severity,
                classification="PRODUCT_BUG" if severity == "FAIL" else "REVIEW_REQUIRED",
                cluster="acceptance/lane-result",
                summary=f"One or more runtime-transition cases did not pass: {states}",
                reproducer="Inspect P/result.json, T/result.json and R probe outputs.",
                stock="Stock or control observations are recorded by each lane.",
                jit="At least one lane-level contract is not green.",
                machine_proof="See the failing lane's entry ledgers.",
                transition_proof="See the failing lane's typed result rows.",
                root_cause="Lane-specific acceptance contract.",
                changed_files=[],
                fix_summary="No lane failure is converted to an approved deviation.",
                regression_tests=[],
                evidence="P/result.json, T/result.json, R/*.json",
            )
        blocker_severities = {item["severity"] for item in blockers}
        if "FAIL" in states or "FAIL" in blocker_severities:
            final = "FAIL"
        elif "REVIEW_REQUIRED" in states or "REVIEW_REQUIRED" in blocker_severities:
            final = "REVIEW_REQUIRED"
        elif (
            penetration.get("differential", {}).get("differences")
            and deviation_proof["result"] == "PASS"
        ):
            final = "PASS_WITH_APPROVED_DEVIATIONS"
        else:
            final = "PASS"
        freeze_hardening = {
            "FH-1": native_recursion_result.get("result", "NOT_RUN"),
            "FH-2": no_reentry_proof.get("result", "NOT_RUN"),
            "FH-3": deviation_proof.get("result", "NOT_RUN"),
        }
        transition_frozen = (
            final in PASS_STATES
            and not blockers
            and all(result == "PASS" for result in freeze_hardening.values())
        )
        payload = {
            "final": final,
            "provenance": provenance,
            "penetration": self.results.get("autocompile_coverage", {}),
            "transitions": self.results.get("state_transition", {}),
            "repetition": repetition.get("repetition", {}),
            "footprint": repetition.get("footprint", {}),
            "code_swap_policy": repetition.get("code_swap", {}),
            "approved_deviation_proof": deviation_proof,
            "freeze_hardening": freeze_hardening,
            "domain": "runtime_transition",
            "freeze_state": "FROZEN" if transition_frozen else "NOT_FROZEN",
            "blockers": blockers,
            "commands": self.base.command_results,
        }
        (self.output / "runtime_transition_result.json").write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n"
        )
        return final

    def run(self) -> str:
        provenance = self.base.preflight()
        if "autocompile_coverage" in self.lanes:
            self.results["autocompile_coverage"] = self.run_autocompile_coverage()
        if "state_transition" in self.lanes:
            self.results["state_transition"] = self.run_state_transitions()
        if "transition_recovery" in self.lanes:
            self.results["transition_recovery"] = self.run_transition_recovery()
        return self.finalize(provenance)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", type=Path, default=None)
    parser.add_argument("--source", type=Path, default=None)
    parser.add_argument("--out", type=Path)
    parser.add_argument(
        "--case",
        type=lambda value: value.replace("-", "_").lower(),
        choices=("autocompile_coverage", "state_transition", "transition_recovery"),
        action="append",
    )
    parser.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 8))
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args(argv)
    if args.wheel is None:
        candidates = sorted(Path("/wheels").glob("cinderx-*cp311*.whl"))
        if not candidates:
            print("no wheel: pass --wheel or mount it at /wheels", file=sys.stderr)
            return 2
        args.wheel = candidates[-1]
    if args.source is None:
        args.source = Path(__file__).resolve().parents[2]
    output = args.out or Path.cwd() / f"cp311-runtime-transition-{datetime.now():%Y%m%d-%H%M%S}"
    runner = RuntimeTransitionAcceptanceRunner(
        wheel=args.wheel,
        source=args.source,
        output=output,
        lanes=set(args.case or ("autocompile_coverage", "state_transition", "transition_recovery")),
        jobs=args.jobs,
        timeout=args.timeout,
    )
    try:
        final = runner.run()
    except Exception as exc:
        print(
            f"runtime-transition runner failed before final judgment: {type(exc).__name__}: {exc}",
            file=sys.stderr,
        )
        return 1
    if final in PASS_STATES:
        return 0
    return 2 if final == "REVIEW_REQUIRED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
