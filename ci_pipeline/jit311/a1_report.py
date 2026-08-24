"""A1 Compile-All classification, exact deviation matching, and reporting."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import tomllib

from ci_pipeline.libtest_diff_311 import diff_results_symmetric, load


def _lines(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def load_capabilities(path: Path) -> tuple[set[str], set[str], set[str]]:
    with path.open("rb") as stream:
        document = tomllib.load(stream)
    return (
        set(document["expected_refusal"]["reasons"]),
        set(document["runtime_fallback"]["reasons"]),
        set(document["execute_surface"]["supported_opcodes"]),
    )


def read_journal(directory: Path) -> list[dict]:
    events: list[dict] = []
    for path in sorted(directory.glob("*.jsonl")):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid journal {path}:{number}: {exc}") from exc
    return events


def validate_execute_surfaces(
    expected: set[str], observed: set[tuple[str, ...]]
) -> tuple[set[str], list[str]]:
    errors: list[str] = []
    if len(observed) != 1:
        errors.append(
            "workers did not report one identical execute surface: "
            f"{len(observed)} distinct surfaces"
        )
        return set(), errors
    actual = set(next(iter(observed)))
    missing = sorted(expected - actual)
    added = sorted(actual - expected)
    if missing or added:
        errors.append(f"execute surface drift: missing={missing}, added={added}")
    return actual, errors


def classify_compile_all(
    journal: Path, targets_path: Path, capabilities_path: Path
) -> dict:
    targets = _lines(targets_path)
    if len(targets) != 72 or len(targets) != len(set(targets)):
        raise ValueError(f"A1 target manifest must contain 72 unique modules, got {len(targets)}")
    expected_reasons, runtime_reasons, expected_surface = load_capabilities(
        capabilities_path
    )
    events = read_journal(journal)

    functions: dict[tuple[str, int, str], dict] = {}
    entered_functions: set[tuple[str, int, str]] = set()
    entries_by_filename: Counter[str] = Counter()
    ledger_dropped = 0
    ledger_errors: list[str] = []
    target_summaries: set[str] = set()
    observed_surfaces: set[tuple[str, ...]] = set()
    scans: dict[str, list[dict]] = defaultdict(list)
    entry_deltas: Counter[str] = Counter()
    for event in events:
        kind = event.get("type")
        if kind == "compile":
            key = (
                str(event.get("filename")),
                int(event.get("firstlineno", -1)),
                str(event.get("qualname")),
            )
            prior = functions.get(key)
            # Prefer successful compilation evidence when the same function is
            # rediscovered through an alias; otherwise keep the first exact
            # typed verdict.
            if prior is None or (
                prior.get("status") != "compiled" and event.get("status") == "compiled"
            ):
                functions[key] = event
        elif kind == "module-scan":
            scans[str(event.get("module"))].append(event)
        elif kind == "execute-surface":
            observed_surfaces.add(tuple(event.get("opcode_names", ())))
        elif kind == "process-summary":
            if event.get("summary_error"):
                ledger_errors.append(str(event["summary_error"]))
            if event.get("target_module"):
                target_summaries.add(str(event["target_module"]))
            ledger_dropped += int(event.get("entry_ledger_dropped", 0))
            for row in event.get("entry_ledger", ()):
                key = (
                    str(row.get("filename")),
                    int(row.get("firstlineno", -1)),
                    str(row.get("qualname")),
                )
                count = int(row.get("entries", 0))
                if count > 0:
                    entered_functions.add(key)
                    entries_by_filename[key[0]] += count
        elif kind in ("test-call", "doctest-call"):
            target = event.get("target_module")
            if target:
                entry_deltas[str(target)] += max(0, int(event.get("machine_entries_delta", 0)))

    expected_target_summaries = {
        target if target.startswith("test.") else "test." + target
        for target in targets
    }
    missing_summaries = sorted(expected_target_summaries - target_summaries)
    if missing_summaries:
        ledger_errors.append(
            f"workers missing exact entry-ledger summaries: {missing_summaries}"
        )

    observed_surface, surface_errors = validate_execute_surfaces(
        expected_surface, observed_surfaces
    )

    reason_counts: Counter[str] = Counter()
    counters = Counter()
    unexpected: list[dict] = []
    unknown: list[dict] = []
    outcomes_by_filename: dict[str, list[str]] = defaultdict(list)
    for event in functions.values():
        counters["discovered"] += 1
        counters["attempted"] += 1
        status = event.get("status")
        reason = event.get("reason")
        if status == "compiled":
            counters["compiled"] += 1
            outcome = "compiled"
        elif status == "runtime-fallback" and reason in runtime_reasons:
            counters["runtime_fallback"] += 1
            reason_counts[str(reason)] += 1
            outcome = "runtime-fallback"
        elif reason == "REFUSE_SHAPE_EXECUTE_SURFACE":
            opcode_name = event.get("opcode_name")
            if (
                not surface_errors
                and isinstance(opcode_name, str)
                and opcode_name not in expected_surface
            ):
                counters["expected_refusal"] += 1
                reason_counts[f"{reason}:{opcode_name}"] += 1
                outcome = "expected-refusal"
            else:
                counters["unexpected_refusal"] += 1
                unexpected.append(event)
                outcome = "unexpected-refusal"
        elif reason in expected_reasons:
            counters["expected_refusal"] += 1
            reason_counts[str(reason)] += 1
            outcome = "expected-refusal"
        elif reason is None or status in ("unknown-refusal", "hook-error"):
            counters["unknown_refusal"] += 1
            unknown.append(event)
            outcome = "unknown-refusal"
        else:
            counters["unexpected_refusal"] += 1
            reason_counts[str(reason)] += 1
            unexpected.append(event)
            outcome = "unexpected-refusal"
        outcomes_by_filename[str(event.get("filename"))].append(outcome)

    module_results: dict[str, dict] = {}
    for short_name in targets:
        full_name = short_name if short_name.startswith("test.") else "test." + short_name
        relevant = scans.get(full_name, []) + scans.get(short_name, [])
        discovered = sum(int(scan.get("discovered", 0)) for scan in relevant)
        statuses = Counter()
        reasons = Counter()
        for scan in relevant:
            statuses.update(scan.get("statuses", {}))
            reasons.update(scan.get("reasons", {}))
        module_files = {
            str(scan["filename"])
            for scan in relevant
            if scan.get("filename") is not None
        }
        own_entries = sum(entries_by_filename[path] for path in module_files)
        module_outcomes = [
            outcome
            for path in module_files
            for outcome in outcomes_by_filename.get(path, ())
        ]

        if own_entries > 0:
            classification = "JIT_EXECUTED"
        elif module_outcomes and all(
            outcome in ("expected-refusal", "runtime-fallback")
            for outcome in module_outcomes
        ) and any(outcome == "runtime-fallback" for outcome in module_outcomes):
            classification = "RUNTIME_FALLBACK"
        elif discovered == 0:
            classification = "EXPECTED_SAFE_REFUSAL"
            reasons["REFUSE_SHAPE_NON_FUNCTION_SCOPE"] += 1
        elif module_outcomes and all(
            outcome == "expected-refusal" for outcome in module_outcomes
        ):
            classification = "EXPECTED_SAFE_REFUSAL"
        else:
            classification = "UNCOVERED"
        module_results[short_name] = {
            "classification": classification,
            "discovered": discovered,
            "machine_entries": entry_deltas[full_name] + entry_deltas[short_name],
            "own_code_entries": own_entries,
            "module_files": sorted(module_files),
            "statuses": dict(statuses),
            "reasons": dict(reasons),
            "function_outcomes": dict(Counter(module_outcomes)),
        }

    module_counts = Counter(item["classification"] for item in module_results.values())
    counters["entered"] = len(entered_functions & set(functions))
    for name in (
        "discovered",
        "attempted",
        "compiled",
        "entered",
        "expected_refusal",
        "runtime_fallback",
        "unexpected_refusal",
        "unknown_refusal",
    ):
        counters[name] += 0
    for name in (
        "JIT_EXECUTED",
        "EXPECTED_SAFE_REFUSAL",
        "RUNTIME_FALLBACK",
        "UNCOVERED",
    ):
        module_counts[name] += 0
    result = {
        "functions": dict(counters),
        "refusals_by_reason": dict(sorted(reason_counts.items())),
        "modules": module_results,
        "module_counts": dict(module_counts),
        "unexpected_refusals": unexpected,
        "unknown_refusals": unknown,
        "journal_events": len(events),
        "entry_ledger_dropped": ledger_dropped,
        "entry_ledger_errors": ledger_errors,
        "execute_surface": sorted(observed_surface),
        "execute_surface_errors": surface_errors,
    }
    result["result"] = (
        "PASS"
        if module_counts["UNCOVERED"] == 0
        and counters["unknown_refusal"] == 0
        and counters["unexpected_refusal"] == 0
        and ledger_dropped == 0
        and not ledger_errors
        and not surface_errors
        else "FAIL"
    )
    return result


def load_deviations(path: Path, lane: str = "C") -> tuple[dict, list[dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    entries = [item for item in document.get("deviations", []) if item.get("lane") == lane]
    allowed: dict[str, dict[str, str]] = {}
    for item in entries:
        testcase = item["testcase"]
        if testcase in allowed:
            raise ValueError(f"duplicate deviation testcase: {testcase}")
        allowed[testcase] = {
            "stock": item["stock_observable"],
            "execute": item["jit_observable"],
        }
    return allowed, entries


def compare_with_deviations(
    stock_path: Path, execute_path: Path, deviations_path: Path
) -> dict:
    allowed, entries = load_deviations(deviations_path)
    stock = load(str(stock_path))
    execute = load(str(execute_path))
    raw = diff_results_symmetric(stock, execute, allowed)

    fingerprint_results: dict[str, dict] = {}
    for entry in entries:
        testcase = entry["testcase"]
        if raw["differences"].get(testcase) != allowed[testcase]:
            continue
        diagnostic = execute.get("diagnostics", {}).get(testcase, "")
        required = entry.get("execute_diagnostic", {}).get(
            "required_substrings", []
        )
        missing = [part for part in required if part not in diagnostic]
        fingerprint_results[testcase] = {
            "required_substrings": required,
            "missing_substrings": missing,
            "matched": not missing,
        }
        if missing:
            raw["unexpected"][testcase] = {
                "stock": allowed[testcase]["stock"],
                "execute": allowed[testcase]["execute"],
                "diagnostic_fingerprint_missing": missing,
            }

    # Module failure is a summary, never an approval unit.  Suppress only the
    # test_dis summary when every concrete test_dis difference is an exact,
    # approved case.  Any new case difference keeps the summary unexpected.
    dis_concrete = {
        key for key in raw["differences"] if key.startswith("test.test_dis.")
    }
    if (
        raw["differences"].get("<module> test_dis") == {"stock": "pass", "execute": "fail"}
        and dis_concrete
        and dis_concrete <= set(allowed)
    ):
        raw["unexpected"].pop("<module> test_dis", None)
        raw["derived_module_summaries"] = {
            "<module> test_dis": raw["differences"].pop("<module> test_dis")
        }
    else:
        raw["derived_module_summaries"] = {}

    raw["approved_deviations"] = entries
    raw["diagnostic_fingerprints"] = fingerprint_results
    raw["result"] = (
        "FAIL"
        if raw["unexpected"]
        else "REVIEW_REQUIRED"
        if raw["stale_baseline"]
        else "PASS_WITH_APPROVED_DEVIATIONS"
        if raw["differences"]
        else "PASS"
    )
    return raw


def write_markdown(report: dict, path: Path) -> None:
    functions = report["functions"]
    modules = report["module_counts"]
    lines = [
        "# A1 Compile-All Classification",
        "",
        f"Result: **{report['result']}**",
        "",
        "| Evidence | Count |",
        "|---|---:|",
        f"| Functions discovered | {functions.get('discovered', 0)} |",
        f"| Compile attempted | {functions.get('attempted', 0)} |",
        f"| Compiled | {functions.get('compiled', 0)} |",
        f"| Functions entered | {functions.get('entered', 0)} |",
        f"| Expected refusal | {functions.get('expected_refusal', 0)} |",
        f"| Runtime fallback | {functions.get('runtime_fallback', 0)} |",
        f"| Unexpected refusal | {functions.get('unexpected_refusal', 0)} |",
        f"| Unknown refusal | {functions.get('unknown_refusal', 0)} |",
        f"| Entry-ledger dropped | {report.get('entry_ledger_dropped', 0)} |",
        f"| Entry-ledger errors | {len(report.get('entry_ledger_errors', []))} |",
        f"| Execute-surface drift | {len(report.get('execute_surface_errors', []))} |",
        "",
        "| Module classification | Count |",
        "|---|---:|",
    ]
    for name in ("JIT_EXECUTED", "EXPECTED_SAFE_REFUSAL", "RUNTIME_FALLBACK", "UNCOVERED"):
        lines.append(f"| {name} | {modules.get(name, 0)} |")
    lines.extend(["", "## Refusals by reason", ""])
    for reason, count in report["refusals_by_reason"].items():
        lines.append(f"- `{reason}`: {count}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    classify = subparsers.add_parser("classify")
    classify.add_argument("--journal", type=Path, required=True)
    classify.add_argument("--targets", type=Path, required=True)
    classify.add_argument("--capabilities", type=Path, required=True)
    classify.add_argument("--out", type=Path, required=True)
    classify.add_argument("--markdown", type=Path)

    compare = subparsers.add_parser("compare")
    compare.add_argument("--stock", type=Path, required=True)
    compare.add_argument("--execute", type=Path, required=True)
    compare.add_argument("--deviations", type=Path, required=True)
    compare.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)

    if args.command == "classify":
        report = classify_compile_all(args.journal, args.targets, args.capabilities)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        if args.markdown:
            write_markdown(report, args.markdown)
    else:
        report = compare_with_deviations(args.stock, args.execute, args.deviations)
        args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"]}, sort_keys=True))
    return 0 if report["result"] in ("PASS", "PASS_WITH_APPROVED_DEVIATIONS") else 1


if __name__ == "__main__":
    raise SystemExit(main())
