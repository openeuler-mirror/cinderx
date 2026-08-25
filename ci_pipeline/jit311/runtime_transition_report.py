"""Autocompile penetration differential, transition proof judge, and Markdown report."""

from __future__ import annotations

import json
from pathlib import Path
import re
import tomllib

from ci_pipeline.libtest_diff_311 import diff_results_symmetric, load


PASS_STATES = {"PASS", "PASS_WITH_APPROVED_DEVIATIONS"}


def compare_native_recursion_boundary(stock_path: Path, jit_path: Path) -> dict:
    stock = json.loads(stock_path.read_text())
    jit = json.loads(jit_path.read_text())
    errors = []
    if stock.get("native_helper_executed") != jit.get("native_helper_executed"):
        errors.append("native helper execution differs from Stock")
    if stock.get("call_error") != jit.get("call_error"):
        errors.append("exposed exception/traceback differs from Stock")
    semantic_keys = (
        "entered",
        "return_code",
        "error_occurred",
        "exception_type",
        "exception_message",
    )
    if stock.get("native") is not None and jit.get("native") is not None:
        for key in semantic_keys:
            if stock.get("native", {}).get(key) != jit.get("native", {}).get(key):
                errors.append(f"native {key} differs from Stock")
        for phase in ("before", "after"):
            for key in ("recursion_remaining", "recursion_headroom"):
                if stock.get("native", {}).get(phase, {}).get(key) != jit.get(
                    "native", {}
                ).get(phase, {}).get(key):
                    errors.append(f"native {phase} {key} differs from Stock")
    for label, document in (("Stock", stock), ("JIT", jit)):
        native = document.get("native", {})
        if native is not None:
            for key in ("recursion_remaining", "recursion_headroom"):
                if native.get("before", {}).get(key) != native.get(
                    "after", {}
                ).get(key):
                    errors.append(f"{label} native {key} drift")
            for key in ("boundary_active", "jit_entries"):
                if native.get("before", {}).get(key) != native.get(
                    "after", {}
                ).get(key):
                    errors.append(f"{label} native {key} changed")
        if document.get("outer_before", {}).get(
            "recursion_remaining"
        ) != document.get("outer_after", {}).get("recursion_remaining"):
            errors.append(f"{label} outer recursion_remaining drift")
        if document.get("post_error_normal_call") != 10:
            errors.append(f"{label} post-error normal call failed")
        if document.get("result") != "PASS":
            errors.append(f"{label} probe self-check failed")
    if jit.get("outer_after", {}).get("recursion_headroom") != 0:
        errors.append("JIT recursion_headroom leaked")
    if jit.get("outer_after", {}).get("boundary_active") is not False:
        errors.append("JIT boundary flag leaked")
    if jit.get("outer_after", {}).get("jit_entries") != 0:
        errors.append("JIT entry ownership leaked")
    if not jit.get("machine_entry_proven"):
        errors.append("JIT recursive target has no machine-entry proof")
    if jit.get("entry_ledger_dropped", 0):
        errors.append("JIT entry ledger dropped evidence")
    return {
        "result": "PASS" if not errors else "FAIL",
        "stock": stock,
        "jit": jit,
        "errors": errors,
        "product_fix_required": bool(errors),
    }


def compare_penetration(
    stock_path: Path,
    aggressive_path: Path,
    deviation_path: Path,
) -> dict:
    stock = load(str(stock_path))
    aggressive = load(str(aggressive_path))
    document = json.loads(deviation_path.read_text())
    deviations = document.get("deviations", [])
    allowed = {
        item["testcase"]: {
            "stock": item["stock"],
            "execute": item["aggressive"],
        }
        for item in deviations
    }
    report = diff_results_symmetric(stock, aggressive, allowed)
    fingerprints = {}
    for item in deviations:
        testcase = item["testcase"]
        if report["differences"].get(testcase) != allowed[testcase]:
            continue
        diagnostic = aggressive.get("diagnostics", {}).get(testcase, "")
        missing = [
            part for part in item.get("fingerprint", []) if part not in diagnostic
        ]
        unmatched_regex = [
            pattern
            for pattern in item.get("fingerprint_regex", [])
            if re.fullmatch(pattern, diagnostic) is None
        ]
        numeric_delta = None
        numeric_error = None
        numeric_spec = item.get("fingerprint_numeric_delta")
        if numeric_spec is not None:
            numeric_match = re.fullmatch(numeric_spec["regex"], diagnostic)
            if numeric_match is None:
                numeric_error = "numeric diagnostic regex did not match"
            else:
                lhs = int(numeric_match.group("lhs"))
                rhs = int(numeric_match.group("rhs"))
                direction = numeric_spec.get("direction")
                if direction != "rhs_minus_lhs":
                    numeric_error = f"unsupported numeric delta direction: {direction}"
                    delta = None
                else:
                    delta = rhs - lhs
                    if delta != int(numeric_spec["expected"]):
                        numeric_error = (
                            f"numeric diagnostic delta is {delta}, expected "
                            f"{numeric_spec['expected']}"
                        )
                numeric_delta = {
                    "lhs": lhs,
                    "rhs": rhs,
                    "direction": direction,
                    "delta": delta,
                    "expected": int(numeric_spec["expected"]),
                    "matched": numeric_error is None,
                }
        fingerprints[testcase] = {
            "matched": not missing and not unmatched_regex and numeric_error is None,
            "missing": missing,
            "unmatched_regex": unmatched_regex,
            "numeric_delta": numeric_delta,
            "numeric_error": numeric_error,
        }
        if missing or unmatched_regex or numeric_error is not None:
            report["unexpected"][testcase] = {
                **allowed[testcase],
                "diagnostic_fingerprint_missing": missing,
                "diagnostic_regex_unmatched": unmatched_regex,
                "diagnostic_numeric_delta_error": numeric_error,
            }
    concrete = {
        key for key in report["differences"] if key.startswith("test.test_dis.")
    }
    if (
        report["differences"].get("<module> test_dis")
        == {"stock": "pass", "execute": "fail"}
        and concrete
        and concrete <= set(allowed)
    ):
        report["unexpected"].pop("<module> test_dis", None)
        report["derived_module_summary"] = report["differences"].pop(
            "<module> test_dis"
        )
    derived_modules = {}
    for key in list(report["differences"]):
        if not key.startswith("<module> "):
            continue
        module = key.removeprefix("<module> ")
        if any(
            case.startswith(f"test.{module}.")
            for case in report["differences"]
            if not case.startswith("<module> ")
        ):
            derived_modules[key] = report["differences"].pop(key)
            report["unexpected"].pop(key, None)
    report["derived_module_summaries"] = derived_modules
    report["fingerprints"] = fingerprints
    report["approved_deviations"] = deviations
    report["result"] = (
        "FAIL"
        if report["unexpected"]
        else (
            "REVIEW_REQUIRED"
            if report["stale_baseline"]
            else "PASS_WITH_APPROVED_DEVIATIONS" if report["differences"] else "PASS"
        )
    )
    return report


def compare_frame_positions(
    running_stock_path: Path,
    running_jit_path: Path,
    error_stock_path: Path,
    error_jit_path: Path,
) -> dict:
    running_stock = json.loads(running_stock_path.read_text())
    running_jit = json.loads(running_jit_path.read_text())
    error_stock = json.loads(error_stock_path.read_text())
    error_jit = json.loads(error_jit_path.read_text())

    errors = []
    running_rows = []
    stock_by_case = {row["case"]: row for row in running_stock.get("rows", [])}
    jit_by_case = {row["case"]: row for row in running_jit.get("rows", [])}
    for case in sorted(set(stock_by_case) | set(jit_by_case)):
        stock = stock_by_case.get(case)
        jit = jit_by_case.get(case)
        row_errors = []
        if stock is None or jit is None:
            row_errors.append("missing Stock or JIT row")
        else:
            if stock.get("observation") != jit.get("observation"):
                row_errors.append("running frame observation differs from Stock")
            if not jit.get("machine_entry_proven"):
                row_errors.append("no target machine-entry proof")
        if row_errors:
            errors.extend(f"running {case}: {item}" for item in row_errors)
        running_rows.append(
            {
                "case": case,
                "stock": stock.get("observation") if stock else None,
                "jit": jit.get("observation") if jit else None,
                "machine_entry_proven": bool(jit and jit.get("machine_entry_proven")),
                "errors": row_errors,
                "result": "PASS" if not row_errors else "FAIL",
            }
        )

    error_rows = []
    stock_by_case = {row["case"]: row for row in error_stock.get("rows", [])}
    jit_by_case = {row["case"]: row for row in error_jit.get("rows", [])}
    for case in sorted(set(stock_by_case) | set(jit_by_case)):
        stock = stock_by_case.get(case)
        jit = jit_by_case.get(case)
        row_errors = []
        if stock is None or jit is None:
            row_errors.append("missing Stock or JIT row")
        else:
            if stock.get("target_frame") != jit.get("target_frame"):
                row_errors.append("target traceback position differs from Stock")
            if stock.get("traceback_frames") != jit.get("traceback_frames"):
                row_errors.append("full traceback frames differ from Stock")
            if not jit.get("machine_entry_proven"):
                row_errors.append("no target machine-entry proof")
            if not jit.get("transitions"):
                row_errors.append("no typed deopt transition proof")
            if jit.get("transition_ledger_dropped"):
                row_errors.append("transition ledger dropped evidence")
        if row_errors:
            errors.extend(f"error {case}: {item}" for item in row_errors)
        error_rows.append(
            {
                "case": case,
                "stock": stock.get("target_frame") if stock else None,
                "jit": jit.get("target_frame") if jit else None,
                "traceback_frames_match": bool(
                    stock
                    and jit
                    and stock.get("traceback_frames") == jit.get("traceback_frames")
                ),
                "machine_entry_proven": bool(jit and jit.get("machine_entry_proven")),
                "transitions": jit.get("transitions", []) if jit else [],
                "errors": row_errors,
                "result": "PASS" if not row_errors else "FAIL",
            }
        )

    for label, document in (
        ("running Stock", running_stock),
        ("running JIT", running_jit),
        ("error Stock", error_stock),
        ("error JIT", error_jit),
    ):
        if document.get("result") != "PASS":
            errors.append(f"{label} probe self-check failed")
        if document.get("entry_ledger_dropped", 0):
            errors.append(f"{label} entry ledger dropped evidence")

    return {
        "result": "PASS" if not errors else "FAIL",
        "running": running_rows,
        "error": error_rows,
        "errors": errors,
        "unreachable": error_jit.get("unreachable", {}),
    }


def compare_recursion_boundary(stock_path: Path, jit_path: Path) -> dict:
    stock = json.loads(stock_path.read_text())
    jit = json.loads(jit_path.read_text())
    stock_rows = {row["id"]: row for row in stock.get("rows", [])}
    jit_rows = {row["id"]: row for row in jit.get("rows", [])}
    rows = []
    errors = []
    for ident in sorted(set(stock_rows) | set(jit_rows)):
        left = stock_rows.get(ident)
        right = jit_rows.get(ident)
        row_errors = []
        if left is None or right is None:
            row_errors.append("missing Stock or JIT row")
        else:
            left_error = left.get("error") or {}
            right_error = right.get("error") or {}
            for key in ("type", "message"):
                if left_error.get(key) != right_error.get(key):
                    row_errors.append(f"exception {key} differs from Stock")
            if left.get("target_frames") != right.get("target_frames"):
                row_errors.append("traceback frame cardinality/position differs")
            if left.get("post_error_recovery") != right.get(
                "post_error_recovery"
            ):
                row_errors.append("post-error recovery differs from Stock")
            for side, document in (("Stock", left), ("JIT", right)):
                if document["before"]["recursion_remaining"] != document[
                    "after"
                ]["recursion_remaining"]:
                    row_errors.append(f"{side} recursion_remaining drift")
            if not right.get("pre", {}).get("machine_entry_proven"):
                row_errors.append("JIT machine-entry proof missing")
            if right["after"].get("recursion_headroom") != 0:
                row_errors.append("JIT recursion headroom leaked")
            if right["after"].get("boundary_active") is not False:
                row_errors.append("JIT recursion boundary flag leaked")
            if right["after"].get("jit_entries") != 0:
                row_errors.append("JIT recursion entry ownership leaked")
        errors.extend(f"{ident}: {error}" for error in row_errors)
        rows.append(
            {
                "id": ident,
                "stock": left,
                "jit": right,
                "errors": row_errors,
                "result": "PASS" if not row_errors else "FAIL",
            }
        )
    if stock.get("result") != "PASS" or jit.get("result") != "PASS":
        errors.append("Stock or JIT recursion probe self-check failed")
    if jit.get("entry_ledger_dropped", 0):
        errors.append("JIT recursion probe dropped entry evidence")
    return {
        "result": "PASS" if not errors and len(rows) == 6 else "FAIL",
        "rows": rows,
        "errors": errors,
    }


def judge_transitions(
    stock_path: Path,
    jit_path: Path,
    manifest_path: Path,
) -> dict:
    stock = json.loads(stock_path.read_text())
    jit = json.loads(jit_path.read_text())
    with manifest_path.open("rb") as stream:
        manifest = tomllib.load(stream)
    specs = {item["id"]: item for item in manifest["transition"]}
    stock_rows = {item["id"]: item for item in stock.get("transitions", [])}
    jit_rows = {item["id"]: item for item in jit.get("transitions", [])}
    rows = []
    for ident in sorted(specs):
        spec = specs[ident]
        stock_row = stock_rows.get(ident)
        jit_row = jit_rows.get(ident)
        errors = []
        if stock_row is None or jit_row is None:
            errors.append("missing stock or JIT row")
        else:
            if stock_row.get("semantic") != jit_row.get("semantic"):
                errors.append("semantic result differs from Stock")
            pre = jit_row.get("pre", {})
            if not pre.get("compiled") or not pre.get("machine_entry_proven"):
                errors.append("pre-state lacks compiled machine-entry proof")
            transition = jit_row.get("transition", {})
            if transition.get("transition_ledger_dropped", 0) != 0:
                errors.append("transition ledger dropped evidence")
            reasons = {
                row.get("deopt_reason") for row in transition.get("transition_rows", [])
            }
            required = set(spec.get("requires_transition_reason", []))
            if required and not (required & reasons):
                errors.append(
                    f"required deopt reason absent: expected one of {sorted(required)}, got {sorted(str(r) for r in reasons)}"
                )
            recovery = jit_row.get("recovery", {})
            recovery_policy = spec["recovery"]
            if recovery_policy in (
                "interpreter-resume",
                "interpreter-after-backoff",
                "policy-deferred",
            ):
                if recovery.get("policy") != recovery_policy:
                    errors.append("explicit recovery policy is missing or mismatched")
                if recovery.get("semantic_correct") is not True:
                    errors.append("policy recovery lacks semantic proof")
                if recovery.get("stale_machine_entry") is not False:
                    errors.append("policy recovery lacks no-stale-entry proof")
            elif not (
                recovery.get("reentered")
                or recovery.get("machine_entry_proven")
                or recovery.get("compiled")
            ):
                errors.append("recovery lacks JIT re-entry/recompile proof")
            if jit_row.get("result") != "PASS":
                errors.append("probe self-check failed")
        rows.append(
            {
                "id": ident,
                "name": spec["name"],
                "pre_jit": bool(
                    jit_row and jit_row.get("pre", {}).get("machine_entry_proven")
                ),
                "trigger": spec["probe"],
                "recovery_policy": spec["recovery"],
                "transition_proof": jit_row.get("transition", {}) if jit_row else None,
                "stock_match": not errors
                or "semantic result differs from Stock" not in errors,
                "recovery": jit_row.get("recovery", {}) if jit_row else None,
                "errors": errors,
                "result": "PASS" if not errors else "FAIL",
            }
        )
    return {
        "result": (
            "PASS"
            if len(rows) == 10 and all(row["result"] == "PASS" for row in rows)
            else "FAIL"
        ),
        "transitions": rows,
        "unknown_transitions": sorted((set(stock_rows) | set(jit_rows)) - set(specs)),
    }


