"""Autocompile-coverage threshold-driven worker evidence hook and journal classifier."""

from __future__ import annotations

import argparse
import atexit
from collections import Counter
import json
import os
from pathlib import Path
import sys

from ci_pipeline.jit311.report import KNOWN_REFUSAL_REASONS


FORMAL_STATES = {
    "OWN_CODE_JIT",
    "PUBLISHED_NO_REENTRY",
    "EXPECTED_SAFE_REFUSAL",
}


def worker_target_module() -> str | None:
    for index, arg in enumerate(sys.argv):
        encoded = None
        if arg == "--worker-args" and index + 1 < len(sys.argv):
            encoded = sys.argv[index + 1]
        elif arg.startswith("--worker-args="):
            encoded = arg.partition("=")[2]
        if encoded is None:
            continue
        try:
            _namespace, test_name = json.loads(encoded)
        except Exception:
            return None
        test_name = str(test_name)
        return test_name if test_name.startswith("test.") else "test." + test_name
    return None


def install_hook() -> None:
    journal = Path(os.environ["AUTOCOMPILE_PENETRATION_JOURNAL"])
    journal.mkdir(parents=True, exist_ok=True)

    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    cinderjit._jit311_reset_entry_ledger()

    def ownership() -> dict:
        target = worker_target_module()
        module = sys.modules.get(target) if target else None
        filename = getattr(module, "__file__", None)
        if filename and filename.endswith((".pyc", ".pyo")):
            filename = filename[:-1]
        spec = getattr(module, "__spec__", None)
        origin = getattr(spec, "origin", None)
        if origin and origin.endswith((".pyc", ".pyo")):
            origin = origin[:-1]
        roots = [os.path.realpath(path) for path in getattr(module, "__path__", ())]
        return {
            "module_file": os.path.realpath(filename) if filename else None,
            "spec_origin": (
                os.path.realpath(origin)
                if origin and origin not in ("built-in", "frozen")
                else origin
            ),
            "package_roots": roots,
        }

    def emit() -> None:
        try:
            trigger = _cinderx._get_trigger_stats()
            observe = _cinderx._get_observe_stats()
            ledger = cinderjit._jit311_entry_ledger()
            rows = [
                {
                    **row,
                    "filename": os.path.realpath(row["filename"]),
                }
                for row in ledger["entries"]
            ]
            payload = {
                "pid": os.getpid(),
                "target_module": worker_target_module(),
                "trigger": trigger,
                "observe": observe,
                "entry_ledger": rows,
                "entry_ledger_dropped": ledger["dropped"],
                "ownership": ownership(),
            }
        except BaseException as exc:
            payload = {
                "pid": os.getpid(),
                "target_module": worker_target_module(),
                "summary_error": f"{type(exc).__name__}: {exc}",
            }
        (journal / f"{os.getpid()}.json").write_text(
            json.dumps(payload, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    atexit.register(emit)


def _targets(path: Path) -> list[str]:
    return [
        line.strip()
        for line in path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def _normalized_source_path(value: object) -> str | None:
    if not isinstance(value, str) or not value or value in {"built-in", "frozen"}:
        return None
    if value.endswith((".pyc", ".pyo")):
        value = value[:-1]
    return os.path.realpath(value)


def classify(
    journal: Path,
    target_path: Path,
    result_path: Path,
    *,
    stock_result_path: Path | None = None,
    jit_all_contract: bool = False,
) -> dict:
    target_modules = _targets(target_path)
    test_result = json.loads(result_path.read_text())
    stock_result = (
        json.loads(stock_result_path.read_text())
        if stock_result_path is not None
        else None
    )
    rows: dict[str, dict] = {}
    errors: list[str] = []
    totals: Counter[str] = Counter()
    unknown_refusals: list[dict] = []
    for path in sorted(journal.glob("*.json")):
        payload = json.loads(path.read_text())
        target = payload.get("target_module")
        if payload.get("summary_error"):
            errors.append(f"{path.name}: {payload['summary_error']}")
        if not target:
            continue
        short = target.removeprefix("test.")
        ownership = payload.get("ownership", {})
        owned_files = {
            path
            for path in (
                _normalized_source_path(ownership.get("module_file")),
                _normalized_source_path(ownership.get("spec_origin")),
            )
            if path is not None
        }
        package_roots = {
            path
            for path in (
                _normalized_source_path(item)
                for item in ownership.get("package_roots", ())
            )
            if path is not None
        }

        def is_owned(filename: object) -> bool:
            path = _normalized_source_path(filename)
            if path is None:
                return False
            if path in owned_files:
                return True
            return any(
                path == root or path.startswith(root + os.sep) for root in package_roots
            )

        own = [
            row
            for row in payload.get("entry_ledger", ())
            if is_owned(row.get("filename")) and int(row.get("entries", 0)) > 0
        ]
        trigger = payload.get("trigger", {})
        observe = payload.get("observe", {})
        own_scheduler = [
            event
            for event in observe.get("events", [])
            if is_owned(event.get("filename"))
        ]
        for event in observe.get("events", []):
            result = event.get("result")
            if result not in KNOWN_REFUSAL_REASONS and result not in {
                "installed",
                "ok",
                "compiled",
                "deferred",
            }:
                unknown_refusals.append(
                    {
                        "target_module": short,
                        "filename": event.get("filename"),
                        "qualname": event.get("qualname"),
                        "result": result,
                    }
                )
        dropped = int(payload.get("entry_ledger_dropped", 0))
        events_dropped = int(observe.get("events_dropped", 0))
        scheduler_threshold = observe.get("threshold")
        installed_events = [
            event for event in own_scheduler if event.get("result") == "installed"
        ]
        post_publication_evidence_complete = bool(installed_events) and all(
            "post_publication_interpreted_frames" in event
            for event in installed_events
        )
        post_publication_interpreted_frames = sum(
            int(event.get("post_publication_interpreted_frames", 0))
            for event in installed_events
        )
        refusal_events = [
            event
            for event in own_scheduler
            if event.get("result") in KNOWN_REFUSAL_REASONS
        ]
        ownership_resolved = bool(owned_files or package_roots)
        test_state = test_result.get("modules", {}).get(short)
        stock_state = (
            stock_result.get("modules", {}).get(short)
            if stock_result is not None
            else None
        )
        semantic_matches_stock = stock_result is None or test_state == stock_state
        if own:
            status = "OWN_CODE_JIT"
        elif (
            jit_all_contract
            and ownership_resolved
            and scheduler_threshold == 0
            and installed_events
            and post_publication_evidence_complete
            and post_publication_interpreted_frames == 0
            and semantic_matches_stock
        ):
            status = "PUBLISHED_NO_REENTRY"
        elif (
            jit_all_contract
            and ownership_resolved
            and scheduler_threshold == 0
            and own_scheduler
            and len(refusal_events) == len(own_scheduler)
            and semantic_matches_stock
        ):
            status = "EXPECTED_SAFE_REFUSAL"
        else:
            status = "COVERAGE_GAP" if jit_all_contract else "THRESHOLD_COVERAGE_GAP"
        machine_entries = int(trigger.get("machine_code_entries", 0))
        rows[short] = {
            "status": status,
            "worker_jit_active": machine_entries > 0,
            "worker_machine_entries": machine_entries,
            "own_code_entries": sum(int(row["entries"]) for row in own),
            "own_code_rows": own,
            "machine_entry_proven": bool(own),
            "compiled_function_creations": int(
                trigger.get("compiled_function_creations", 0)
            ),
            "organic_deopts": int(trigger.get("organic_deopt_hits", 0)),
            "forced_deopts": int(trigger.get("forced_deopt_hits", 0)),
            "scheduler_events": observe.get("events", []),
            "own_scheduler_events": own_scheduler,
            "scheduler_threshold": scheduler_threshold,
            "scheduler_threshold_source": observe.get("threshold_source"),
            "discovered_functions": len(
                {event.get("qualname") for event in own_scheduler}
            ),
            "observed_own_functions": sorted(
                {
                    str(event.get("qualname"))
                    for event in own_scheduler
                    if event.get("qualname") is not None
                }
            ),
            "observed_call_count": sum(
                int(event.get("count", 0)) for event in own_scheduler
            ),
            "compile_results": dict(
                Counter(str(event.get("result")) for event in own_scheduler)
            ),
            "artifact_installed": any(
                event.get("result") == "installed" for event in own_scheduler
            ),
            "publication_events": installed_events,
            "post_publication_evidence_complete": (
                post_publication_evidence_complete
            ),
            "post_publication_interpreted_frames": (
                post_publication_interpreted_frames
            ),
            "safe_refusal_events": refusal_events,
            "no_reentry_after_publication": bool(installed_events and not own),
            "ownership_resolved": ownership_resolved,
            "semantic_matches_stock": semantic_matches_stock,
            "test_module_state": test_state,
            "stock_module_state": stock_state,
            "events_dropped": events_dropped,
            "entry_ledger_dropped": dropped,
            "ownership": ownership,
        }
        totals["machine_entries"] += rows[short]["worker_machine_entries"]
        totals["compiled_function_creations"] += rows[short][
            "compiled_function_creations"
        ]
        totals["organic_deopts"] += rows[short]["organic_deopts"]
        totals["forced_deopts"] += rows[short]["forced_deopts"]
        totals["ledger_dropped"] += dropped
        totals["events_dropped"] += events_dropped
        totals["post_publication_interpreted_frames"] += (
            post_publication_interpreted_frames
        )
        totals["worker_jit_active"] += int(rows[short]["worker_jit_active"])
        totals["actual_own_code_machine_entry_modules"] += int(bool(own))

    missing = sorted(set(target_modules) - set(rows))
    if missing:
        errors.append(f"missing worker summaries: {missing}")
    for target in target_modules:
        if target not in rows:
            rows[target] = {
                "status": "COVERAGE_GAP" if jit_all_contract else "THRESHOLD_COVERAGE_GAP",
                "missing": True,
            }
    counts = Counter(row["status"] for row in rows.values())
    classified = sum(counts[state] for state in FORMAL_STATES)
    result = {
        "result": (
            "PASS"
            if len(target_modules) == 72
            and (classified == 72 if jit_all_contract else counts["OWN_CODE_JIT"] == 72)
            and not errors
            and totals["ledger_dropped"] == 0
            and totals["events_dropped"] == 0
            and not unknown_refusals
            else "FAIL"
        ),
        "contract": "jit-all-three-state" if jit_all_contract else "diagnostic",
        "target_modules": len(target_modules),
        "classified_modules": classified,
        "counts": dict(counts),
        "totals": dict(totals),
        "modules": rows,
        "errors": errors,
        "unknown_refusals": unknown_refusals,
        "test_modules": test_result.get("modules", {}),
    }
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--targets", type=Path, required=True)
    parser.add_argument("--test-result", type=Path, required=True)
    parser.add_argument("--stock-result", type=Path)
    parser.add_argument("--jit-all-contract", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = classify(
        args.journal,
        args.targets,
        args.test_result,
        stock_result_path=args.stock_result,
        jit_all_contract=args.jit_all_contract,
    )
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"], **report["counts"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if os.environ.get("AUTOCOMPILE_PENETRATION_JOURNAL"):
    install_hook()


if __name__ == "__main__":
    raise SystemExit(main())
