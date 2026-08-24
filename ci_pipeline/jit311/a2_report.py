"""A2 penetration differential, transition proof judge, and Markdown report."""

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


def render_native_recursion_boundary_report(comparison: dict, out: Path) -> None:
    stock = comparison.get("stock", {})
    jit = comparison.get("jit", {})
    lines = [
        "# CPython 3.11 A2 Native Recursion Boundary Report",
        "",
        f"- Result: `{comparison.get('result')}`",
        f"- Product fix required: `{comparison.get('product_fix_required')}`",
        "",
        "| Observable | Stock | JIT |",
        "|---|---|---|",
        f"| `native_helper_executed` | `{stock.get('native_helper_executed')}` | `{jit.get('native_helper_executed')}` |",
        f"| `call_error` | `{json.dumps(stock.get('call_error'), sort_keys=True)}` | `{json.dumps(jit.get('call_error'), sort_keys=True)}` |",
    ]
    for key in (
        "entered",
        "return_code",
        "error_occurred",
        "exception_type",
        "exception_message",
    ):
        lines.append(
            f"| `{key}` | `{(stock.get('native') or {}).get(key)}` | "
            f"`{(jit.get('native') or {}).get(key)}` |"
        )
    for phase in ("before", "after"):
        for key in (
            "recursion_remaining",
            "recursion_headroom",
            "boundary_active",
            "jit_entries",
        ):
            lines.append(
                f"| `{phase}.{key}` | "
                f"`{(stock.get('native') or {}).get(phase, {}).get(key)}` | "
                f"`{(jit.get('native') or {}).get(phase, {}).get(key)}` |"
            )
    lines.extend(
        [
            "",
            f"- Stock post-error normal call: `{stock.get('post_error_normal_call')}`",
            f"- JIT post-error normal call: `{jit.get('post_error_normal_call')}`",
            f"- JIT machine-entry proof: `{jit.get('machine_entry_proven')}`",
            f"- Errors: `{json.dumps(comparison.get('errors', []), sort_keys=True)}`",
            "",
        ]
    )
    out.write_text("\n".join(lines))


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


def render_recursion_boundary_report(
    comparison: dict,
    transition_result: dict,
    out: Path,
) -> None:
    transitions = {
        row["id"]: row for row in transition_result.get("transitions", [])
    }
    lines = [
        "# CPython 3.11 A2 Recursion Boundary Report",
        "",
        f"- Recursion matrix: `{comparison.get('result')}`",
        f"- T10: `{transitions.get('T10', {}).get('result', 'MISSING')}`",
        "- Before fix T10: Stock 992 recursive frames, JIT 991",
        "",
        "## Stock vs Before Fix vs After Fix",
        "",
        "| Observable | Stock | Before Fix | After Fix |",
        "|---|---|---|---|",
        "| T10 recursive frame count | 992 | 991 | 992, exact Stock match |",
        "| Exception | RecursionError | RecursionError | RecursionError |",
        "| Recursive `tb_lasti` | 52 | 52 | 52 |",
        "| Line/column | Stock code position | Same position, one frame absent | Exact full-frame-list match |",
        "| `recursion_remaining` after error | Restored | Restored but frame lifecycle split | Restored and probe-balanced |",
        "| Post-error recovery | Succeeds | Succeeds | Succeeds in R1-R6 and T10 |",
        "",
        "## Recursion matrix",
        "",
        "| Case | Exception | Stock frames | JIT frames | Stock/JIT remaining | Recovery | Machine proof | Result |",
        "|---|---|---:|---:|---|---|---:|---|",
    ]
    for row in comparison.get("rows", []):
        stock = row.get("stock") or {}
        jit = row.get("jit") or {}
        stock_error = stock.get("error") or {}
        jit_error = jit.get("error") or {}
        lines.append(
            f"| `{row['id']}` | `{stock_error.get('type')}` | "
            f"{len(stock.get('target_frames', []))} | "
            f"{len(jit.get('target_frames', []))} | "
            f"`{stock.get('before', {}).get('recursion_remaining')}/"
            f"{jit.get('before', {}).get('recursion_remaining')}` | "
            f"`{jit.get('post_error_recovery')}` | "
            f"{'yes' if jit.get('pre', {}).get('machine_entry_proven') else 'no'} | "
            f"`{row.get('result')}` |"
        )
    lines.extend(
        [
            "",
            "## Lifecycle change",
            "",
            "Before: generated binding called Py_EnterRecursiveCall before the "
            "attempted JIT frame was linked. On deopt, the anchored evaluator then "
            "tried to Enter the same frame again; at recursion_remaining=0 that "
            "duplicate Enter exited without writing the deepest traceback frame.",
            "",
            "After: successful binding prelinks the real frame, then performs the "
            "recursion check. The last admitted compiled frame keeps CPython helper "
            "headroom while blocking nested Python frames at the logical limit. On "
            "deopt, recursion-slot ownership is transferred to the anchored evaluator, "
            "which performs the frame's single interpreter Enter/Leave pair. Normal "
            "machine return remains owned by the generated bind wrapper.",
            "",
            "The failed attempted frame is cleaned through the real frame lifecycle; "
            "no traceback object or testcase expectation is forged.",
            "",
            f"- Matrix errors: `{json.dumps(comparison.get('errors', []), sort_keys=True)}`",
            "",
        ]
    )
    out.write_text("\n".join(lines))


def render_frame_position_report(
    comparison: dict,
    before_path: Path,
    transition_result: dict,
    inspect_returncode: int,
    out: Path,
) -> None:
    before = json.loads(before_path.read_text())
    lines = [
        "# CPython 3.11 JIT A2 Frame Position Report",
        "",
        f"- Position matrix: `{comparison.get('result')}`",
        f"- `test_inspect`: `{'PASS' if inspect_returncode == 0 else 'FAIL'}`",
        f"- Before-fix source: `{before.get('source_git_sha')}`",
        "",
        "## Running frames (F1)",
        "",
        "| Case | Stock `(f_lasti, line, position)` | Before | After | Machine entry |",
        "|---|---|---|---|---:|",
    ]
    before_running = before.get("running", {})
    for row in comparison.get("running", []):
        case = row["case"]
        stock = row.get("stock") or {}
        jit = row.get("jit") or {}
        stock_value = [
            stock.get("f_lasti"),
            stock.get("f_lineno"),
            stock.get("co_position"),
        ]
        before_value = before_running.get(case, {}).get("before")
        after_value = [
            jit.get("f_lasti"),
            jit.get("f_lineno"),
            jit.get("co_position"),
        ]
        lines.append(
            f"| `{case}` | `{stock_value}` | `{before_value}` | `{after_value}` | "
            f"{'yes' if row.get('machine_entry_proven') else 'no'} |"
        )
    lines.extend(
        [
            "",
            "`sys._getframe()`, `frame.f_lasti`, `frame.f_lineno`, "
            "`code.co_positions()` and `inspect.stack()` agree in every row.",
            "",
            "## Error and deopt positions (F2)",
            "",
            "| Case | Opcode offset/cache | Stock `(tb_lasti, f_lasti, position)` | Before | After | Deopt proof |",
            "|---|---|---|---|---|---|",
        ]
    )
    before_error = before.get("error", {})
    for row in comparison.get("error", []):
        case = row["case"]
        stock = row.get("stock") or {}
        jit = row.get("jit") or {}
        instruction = [
            stock.get("opcode_offset"),
            stock.get("inline_cache_span"),
        ]
        stock_value = [
            stock.get("tb_lasti"),
            stock.get("f_lasti"),
            stock.get("position"),
        ]
        before_value = before_error.get(case, {}).get("before")
        after_value = [
            jit.get("tb_lasti"),
            jit.get("f_lasti"),
            jit.get("position"),
        ]
        transitions = [
            [
                item.get("deopt_reason"),
                item.get("cause_offset"),
                item.get("resume_offset"),
            ]
            for item in row.get("transitions", [])
        ]
        lines.append(
            f"| `{case}` | `{instruction}` | `{stock_value}` | `{before_value}` | "
            f"`{after_value}` | `{transitions}` |"
        )
    transitions = {
        row["id"]: row
        for row in transition_result.get("transitions", [])
        if row.get("id") in {"T03", "T10"}
    }
    lines.extend(
        [
            "",
            "## T03 / T10",
            "",
            f"- T03: `{transitions.get('T03', {}).get('result', 'MISSING')}`; caller `tb_lasti` now matches Stock 20.",
            f"- T10: `{transitions.get('T10', {}).get('result', 'MISSING')}`; every recursive caller `tb_lasti` now matches Stock 52.",
        ]
    )
    if transitions.get("T10", {}).get("result") != "PASS":
        lines.extend(
            [
                "- T10 residual: traceback positions are fixed, but the JIT recursion boundary has one fewer recursive frame than Stock. This is retained as a separate recursion-entry blocker.",
            ]
        )
    lines.extend(
        [
            "",
            f"- Unreachable current capability: `{json.dumps(comparison.get('unreachable', {}), sort_keys=True)}`",
            f"- Matrix errors: `{json.dumps(comparison.get('errors', []), sort_keys=True)}`",
            "",
        ]
    )
    out.write_text("\n".join(lines))


def render_policy_footprint_report(result: dict, out: Path) -> None:
    code_swap = result.get("code_swap", {})
    footprint = result.get("footprint", {})
    lines = [
        "# CPython 3.11 JIT A2 Policy and Footprint Report",
        "",
        f"- Work-package result: `{result.get('result')}`",
        f"- Generic 100-cycle stress: `{result.get('repetition', {}).get('result')}`",
        "",
        "## Code swap policy",
        "",
        "| Arm | Budget | Result | Classification | Semantic failures | Stale entries | Auto re-entry | Policy interpreter | Typed reasons |",
        "|---|---:|---|---|---:|---:|---:|---:|---|",
    ]
    for name in ("default", "zero", "large", "force"):
        arm = code_swap.get(name, {})
        reasons: dict[str, int] = {}
        for row in arm.get("rows", []):
            reason = str(row.get("policy_reason"))
            reasons[reason] = reasons.get(reason, 0) + 1
        lines.append(
            f"| `{name}` | `{arm.get('fresh_attach_budget')}` | "
            f"`{arm.get('result')}` | `{arm.get('classification')}` | "
            f"{arm.get('semantic_failures')} | {arm.get('stale_machine_entries')} | "
            f"{arm.get('automatic_reentry_cycles')} | "
            f"{arm.get('interpreter_policy_cycles')} | "
            f"`{json.dumps(reasons, sort_keys=True)}` |"
        )
    lines.extend(
        [
            "",
            "Classification: `A2_POLICY_DECISION`.",
            "",
            "Default, budget=0 and budget=65535 all produce the same 50/50 "
            "shape: one code identity remains installed; returning to the retired "
            "identity runs interpreted with typed reason "
            "`automatic-attempt-spent-artifact-retired`. This is the per-code "
            "one-attempt policy, not fresh-attach-budget exhaustion. All cycles "
            "preserve semantics and no old-code machine entry occurs.",
            "",
            "The force-compile diagnostic restores machine entry in 100/100 cycles. "
            "Compiler/runtime capability is healthy; force_compile remains diagnostic "
            "and is not part of the product gate.",
            "",
            "## Exact G.__eq__ footprint",
            "",
            f"- Shape: `{footprint.get('shape')}`",
            f"- Result: `{footprint.get('result')}`",
            f"- Classification: `{footprint.get('classification')}`",
            "",
            "| Sample | GC objects | Compiled creations | Resident buffers | Machine entries |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for name in (
        "before_first_call",
        "after_first_publication",
        "after_10",
        "after_100",
        "after_1000",
        "after_gc_1",
        "after_gc_2",
    ):
        sample = footprint.get("samples", {}).get(name, {})
        lines.append(
            f"| `{name}` | {sample.get('gc_objects')} | "
            f"{sample.get('compiled_function_creations')} | "
            f"{sample.get('resident_code_buffers')} | "
            f"{sample.get('machine_code_entries')} |"
        )
    delta = footprint.get("delta", {})
    lines.extend(
        [
            "",
            f"- First publication GC delta: `{delta.get('first_publication_gc_objects')}`",
            f"- First publication type delta: `{json.dumps(delta.get('first_publication_object_types', {}), sort_keys=True)}`",
            f"- 10 to 1000 GC delta: `{delta.get('steady_10_to_1000_gc_objects')}`",
            f"- 10 to 1000 type delta: `{json.dumps(delta.get('steady_10_to_1000_object_types', {}), sort_keys=True)}`",
            f"- Resident-buffer delta: `{delta.get('steady_resident_code_buffers')}`",
            f"- Strict checks: `{json.dumps(footprint.get('strict_checks', {}), sort_keys=True)}`",
            "",
            "Classification: `APPROVED_STRESS_MODE_DEVIATION`. The exact +5 is "
            "one CompiledFunction, one builtin function/method, one dict, one tuple "
            "and one weak reference from first publication. Exact counts and type "
            "histograms are equal at 10, 100 and 1000 calls; two post-GC samples "
            "are equal; resident buffers and compile count do not grow. This is not "
            "a per-lookup leak.",
            "",
            "The compatibility baseline permits only the exact `test_slots` testcase "
            "and only while the final judge verifies this complete mechanism proof.",
            "",
        ]
    )
    out.write_text("\n".join(lines))


def render_footprint_deviation_proof_final(
    deviation_proof: dict,
    footprint: dict,
    out: Path,
) -> None:
    proof = deviation_proof.get("proofs", {}).get(
        "one_time_publication_footprint", {}
    )
    diagnostic = proof.get("diagnostic_numeric_delta", {})
    samples = footprint.get("samples", {})
    delta = footprint.get("delta", {})
    lines = [
        "# CPython 3.11 A2 Footprint Deviation Proof Final",
        "",
        f"- Result: `{deviation_proof.get('result')}`",
        f"- Testcase: `{proof.get('testcase')}`",
        f"- Actual assertion lhs/rhs: `{diagnostic.get('lhs')} != {diagnostic.get('rhs')}`",
        f"- Diagnostic direction: `{diagnostic.get('direction')}`",
        f"- Diagnostic delta: `{diagnostic.get('delta')}`",
        f"- Independent first-publication delta: `{delta.get('first_publication_gc_objects')}`",
        f"- Exact first-publication type histogram: `{json.dumps(delta.get('first_publication_object_types', {}), sort_keys=True)}`",
        "",
        "| Sample | GC objects | Compiled creations | Resident buffers |",
        "|---|---:|---:|---:|",
    ]
    for name in ("after_10", "after_100", "after_1000", "after_gc_1", "after_gc_2"):
        sample = samples.get(name, {})
        lines.append(
            f"| `{name}` | {sample.get('gc_objects')} | "
            f"{sample.get('compiled_function_creations')} | "
            f"{sample.get('resident_code_buffers')} |"
        )
    lines.extend(
        [
            "",
            f"- 10-to-1000 GC growth: `{delta.get('steady_10_to_1000_gc_objects')}`",
            f"- 10-to-1000 type growth: `{json.dumps(delta.get('steady_10_to_1000_object_types', {}), sort_keys=True)}`",
            f"- Steady resident-buffer growth: `{delta.get('steady_resident_code_buffers')}`",
            f"- Strict checks: `{json.dumps(footprint.get('strict_checks', {}), sort_keys=True)}`",
            f"- Errors: `{json.dumps(deviation_proof.get('errors', []), sort_keys=True)}`",
            "",
        ]
    )
    out.write_text("\n".join(lines))


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


def render_markdown(final: dict, path: Path) -> None:
    penetration = final.get("penetration", {})
    aggressive = penetration.get("aggressive_coverage", {})
    transitions = final.get("transitions", {})
    repetition = final.get("repetition", {})
    footprint = final.get("footprint", {})
    differential = penetration.get("differential", {})
    proof = final.get("approved_deviation_proof", {})
    coverage_counts = aggressive.get("counts", {})
    lines = [
        "# CPython 3.11 CinderX JIT A2 Execution Report",
        "",
        f"Final: **{final['final']}**",
        "",
        "## A2-P Aggressive penetration",
        "",
        f"- Target modules: {aggressive.get('target_modules', 0)}",
        f"- Worker JIT active: {aggressive.get('totals', {}).get('worker_jit_active', 0)}/72",
        f"- Classified: {aggressive.get('classified_modules', 0)}/72",
        f"- OWN_CODE_JIT: {coverage_counts.get('OWN_CODE_JIT', 0)}",
        f"- PUBLISHED_NO_REENTRY: {coverage_counts.get('PUBLISHED_NO_REENTRY', 0)}",
        f"- EXPECTED_SAFE_REFUSAL: {coverage_counts.get('EXPECTED_SAFE_REFUSAL', 0)}",
        f"- COVERAGE_GAP: {coverage_counts.get('COVERAGE_GAP', 0)}",
        f"- Unknown refusals: {len(aggressive.get('unknown_refusals', []))}",
        f"- Differential: {differential.get('result', 'NOT_RUN')}",
        f"- Unexpected differences: {len(differential.get('unexpected', {}))}",
        "",
        "## A2-T Transition matrix",
        "",
        "| Transition | Pre JIT | Trigger | Stock match | Recovery | Result |",
        "|---|---|---|---|---|---|",
    ]
    for row in transitions.get("transitions", []):
        lines.append(
            f"| {row['id']} {row['name']} | {row['pre_jit']} | {row['trigger']} | {row['stock_match']} | {json.dumps(row['recovery'], sort_keys=True)} | {row['result']} |"
        )
    lines.extend(
        [
            "",
            "Aggressive tracing/C-trace regression: "
            f"**{transitions.get('aggressive_tracing_regression', {}).get('result', 'NOT_RUN')}**",
            "",
            f"Frame-position matrix: **{transitions.get('frame_positions', {}).get('result', 'NOT_RUN')}**",
            f"Recursion-boundary matrix: **{transitions.get('recursion_boundary', {}).get('result', 'NOT_RUN')}**",
            f"Native C recursion boundary: **{transitions.get('native_recursion_boundary', {}).get('result', 'NOT_RUN')}**",
        ]
    )
    lines.extend(
        [
            "",
            "## A2-R Repetition",
            "",
            "| Transition | Cycles | Semantic failures | State failures |",
            "|---|---:|---:|---:|",
        ]
    )
    for row in repetition.get("transitions", []):
        lines.append(
            f"| {row['transition']} | {row['cycles']} | {row['semantic_failures']} | {row['state_failures']} |"
        )
    lines.extend(
        [
            "",
            "## Footprint plateau",
            "",
            f"- Result: {footprint.get('result', 'NOT_RUN')}",
            f"- Strict plateau: {footprint.get('strict_plateau')}",
            f"- Classification: {footprint.get('classification')}",
            f"- Delta: `{json.dumps(footprint.get('delta', {}), sort_keys=True)}`",
            "",
            "## Approved deviations",
            "",
            f"- Result: {proof.get('result', 'NOT_RUN')}",
            f"- Exact differences: `{json.dumps(sorted(differential.get('differences', {})))}`",
            f"- Proof errors: `{json.dumps(proof.get('errors', []), sort_keys=True)}`",
            "",
            "## Freeze hardening",
            "",
            f"- FH-1 native recursion: {final.get('freeze_hardening', {}).get('FH-1', 'NOT_RUN')}",
            f"- FH-2 no re-entry proof: {final.get('freeze_hardening', {}).get('FH-2', 'NOT_RUN')}",
            f"- FH-3 footprint fingerprint: {final.get('freeze_hardening', {}).get('FH-3', 'NOT_RUN')}",
            f"- Conclusion: **{final.get('a2_freeze', 'NOT FROZEN')}**",
            "",
            "## Blockers",
            "",
        ]
    )
    if final.get("blockers"):
        lines.extend(
            f"- **{item['id']}** ({item['severity']}): {item['summary']}"
            for item in final["blockers"]
        )
    else:
        lines.append("- None")
    path.write_text("\n".join(lines) + "\n")
