"""Fail-closed A3 discovery judgment and Markdown reports."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from ci_pipeline.jit311.a3_census import CAPACITY_PATHS, CUMULATIVE_PATHS, GAUGE_PATHS, value_at


PRIMARY_CLUSTERS = {
    "C1": "B1 function-watch ownership",
    "C2": "B2 code-extra / observer tombstone",
    "C3": "B3 artifact/member/association",
    "C4": "B4 parked/deopt registry",
    "C5": "B3 artifact/member/association",
    "C6": "B6 generator lifetime",
    "C7": "B5 deferred anchor transaction",
    "C8": "B8 multithread compile lifetime",
}

SEVERITY = {
    "B1": "P0",
    "B2": "P0",
    "B3": "P0",
    "B4": "P0",
    "B5": "P1",
    "B6": "P1",
    "B7": "P0",
    "B8": "P1",
    "B9": "P0",
    "B10": "P0",
}


def _compact_sample(sample: dict) -> dict:
    snap = sample["snapshot"]
    return {
        "label": sample["label"],
        "gauges": {path: value_at(snap, path) for path in GAUGE_PATHS},
        "capacity": {path: value_at(snap, path) for path in CAPACITY_PATHS},
        "cumulative": {path: value_at(snap, path) for path in CUMULATIVE_PATHS},
        "python_liveness": sample.get("python_liveness", {}),
        "invariants": sample.get("invariants", {}),
    }


def _add_blocker(groups: dict, cluster: str, scenario: str, result: dict) -> None:
    prefix = cluster.split()[0]
    group = groups.setdefault(
        prefix,
        {
            "id": prefix,
            "title": cluster,
            "severity": SEVERITY[prefix],
            "scenarios": [],
            "errors": [],
            "evidence": {},
        },
    )
    if scenario not in group["scenarios"]:
        group["scenarios"].append(scenario)
    result_errors = list(result.get("errors", []))
    if not result_errors:
        result_errors.extend(
            error
            for failure in result.get("failures", [])
            for error in failure.get("errors", [])
        )
    group["errors"].extend(
        error for error in result_errors if error not in group["errors"]
    )
    if result.get("samples"):
        group["evidence"][scenario] = [_compact_sample(sample) for sample in result["samples"]]
    elif result.get("failures"):
        # A deterministic teardown crash fails thousands of identical
        # children; the per-state ledger plus the first few rows (which
        # carry the stdio tails, core path and gdb backtrace) is the
        # evidence, not the full repetition list.
        evidence: dict[str, Any] = {"failures": result["failures"][:5]}
        if result.get("per_state"):
            evidence["per_state"] = result["per_state"]
        group["evidence"][scenario] = evidence


def classify_blockers(c_results: dict, ownership: dict | None, finalize: dict | None) -> list[dict]:
    groups: dict[str, dict] = {}
    for scenario, result in sorted(c_results.items()):
        if result.get("result") == "PASS":
            continue
        paths = set(result.get("plateau", {}).get("gauge_drift", {}))
        errors = " ".join(result.get("errors", []))
        matched = False
        if any(path.startswith("observer.") for path in paths) or any(
            token in errors
            for token in ("observer.", "resident_code_extra_blocks")
        ):
            _add_blocker(groups, "B2 code-extra / observer tombstone", scenario, result)
            matched = True
        if any(
            path.startswith(("module.code_allocator", "runtime.resident_code"))
            for path in paths
        ) or any(
            token in errors
            for token in (
                "module.code_allocator_used_bytes",
                "runtime.resident_code_buffers",
                "jit.code_runtimes_allocated",
                "jit.code_runtimes_live",
            )
        ):
            _add_blocker(groups, "B7 code buffer residency", scenario, result)
            matched = True
        primary_signals = {
            "C1": ("weakrefs_alive", "jit.watched_functions", "I3 "),
            "C2": ("observer.", "resident_code_extra_blocks"),
            "C3": ("jit.artifact_members", "jit.associated_functions", "I2 "),
            "C4": ("jit.parked_functions", "I3 parked"),
            "C5": ("jit.artifact_members", "jit.associated_functions", "I7 "),
            "C6": ("generator", "jit_generator_gc_objects"),
            "C7": ("deferred_anchor", "active_compiles", "completed_compiles"),
            "C8": ("registered_compilation_units", "completed_compiles"),
        }
        if any(signal in errors for signal in primary_signals[scenario]):
            _add_blocker(groups, PRIMARY_CLUSTERS[scenario], scenario, result)
            matched = True
        if not matched:
            _add_blocker(groups, PRIMARY_CLUSTERS[scenario], scenario, result)
    if ownership and ownership.get("result") != "PASS":
        _add_blocker(groups, "B10 refcount/native memory safety", "O3", ownership)
    if finalize and finalize.get("result") != "PASS":
        _add_blocker(groups, "B9 finalize/shutdown", "F", finalize)
        failed_states = {row.get("state") for row in finalize.get("failures", [])}
        if "multithread-completed" in failed_states:
            _add_blocker(groups, "B8 multithread compile lifetime", "F", finalize)
    return [groups[key] for key in sorted(groups)]


def judge(
    *,
    prerequisite: dict | None,
    c_results: dict,
    ownership: dict | None,
    finalize: dict | None,
    command_failures: list[str],
    require_c: bool = True,
) -> tuple[str, list[dict]]:
    infra = list(command_failures)
    if prerequisite is None:
        infra.append("minimal A2 prerequisite result is missing")
    elif prerequisite.get("result") != "PASS":
        infra.append("minimal A2 prerequisite did not pass")
    if require_c:
        for scenario in sorted(PRIMARY_CLUSTERS):
            if scenario not in c_results:
                infra.append(f"missing {scenario} result")
            elif c_results[scenario].get("result") == "INFRA_FAIL":
                infra.append(f"{scenario} infrastructure failure")
    if infra:
        return "INFRA_FAIL", classify_blockers(c_results, ownership, finalize)
    blockers = classify_blockers(c_results, ownership, finalize)
    if any(result.get("result") != "PASS" for result in c_results.values()):
        return "DISCOVERY_FAIL", blockers
    if ownership is not None and ownership.get("result") != "PASS":
        return "DISCOVERY_FAIL", blockers
    if finalize is not None and finalize.get("result") != "PASS":
        return "DISCOVERY_FAIL", blockers
    return "DISCOVERY_PASS", blockers


def _snapshot_rows(c_results: dict) -> list[str]:
    rows = [
        "| Scenario | Checkpoint | live gauges changed from baseline | Python weakrefs alive |",
        "|---|---|---|---:|",
    ]
    for scenario, result in sorted(c_results.items()):
        samples = result.get("samples", [])
        if not samples:
            rows.append(f"| {scenario} | missing | INFRA | - |")
            continue
        baseline = samples[0]["snapshot"]
        for sample in samples:
            changed = [
                path
                for path in GAUGE_PATHS
                if value_at(sample["snapshot"], path) != value_at(baseline, path)
            ]
            alive = sample.get("python_liveness", {}).get("weakrefs_alive", "-")
            rows.append(
                f"| {scenario} | {sample['label']} | {', '.join(changed) or 'none'} | {alive} |"
            )
    return rows


def _shutdown_state_rows(per_state: dict | None) -> list[str]:
    if not per_state:
        return ["Per-state shutdown ledger: `not recorded by this lane run`."]
    rows = [
        "| Shutdown state | attempts | successes | SIGSEGV | SIGABRT | timeout | forbidden stderr |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for state, summary in per_state.items():
        rows.append(
            f"| {state} | {summary.get('attempts', 0)} | {summary.get('successes', 0)} | "
            f"{summary.get('sigsegv', 0)} | {summary.get('sigabrt', 0)} | "
            f"{summary.get('timeouts', 0)} | {summary.get('forbidden_stderr', 0)} |"
        )
    return rows


def render_discovery(document: dict, path: Path) -> None:
    c_results = document["lanes"].get("C", {})
    lines = [
        "# CPython 3.11 CinderX JIT A3 Discovery Report",
        "",
        f"Status: **{document['status']}**",
        "",
        "This is a discovery judgment, not final A3 acceptance. A3-S is intentionally separate.",
        "",
        "## Provenance and frozen prerequisite",
        "",
        f"- Source SHA: `{document['provenance'].get('source_git_sha')}`",
        f"- Wheel SHA256: `{document['provenance'].get('wheel_sha256')}`",
        f"- Wheel/source SHA match: `{document['provenance'].get('wheel_source_sha_match')}`",
        f"- A2 frozen evidence: `{document['a2_frozen'].get('result')}`",
        f"- Minimal A2 canary prerequisite: `{(document.get('prerequisite') or {}).get('result')}`",
        "",
        "## C1-C8 result",
        "",
        "| Scenario | Result | Cycles | Machine entries | Gauge drift |",
        "|---|---|---|---:|---|",
    ]
    for scenario in sorted(PRIMARY_CLUSTERS):
        result = c_results.get(scenario, {})
        drift = sorted(result.get("plateau", {}).get("gauge_drift", {}))
        lines.append(
            f"| {scenario} | {result.get('result', 'MISSING')} | "
            f"{result.get('cycles', '-')} | {result.get('machine_code_entries', '-')} | "
            f"{', '.join(drift) or 'none'} |"
        )
    lines.extend(["", "## Required checkpoint census", ""])
    lines.extend(_snapshot_rows(c_results))
    c2 = c_results.get("C2", {})
    c2_observer = []
    for sample in c2.get("samples", []):
        if sample["label"] in ("after_100", "after_1000", "after_10000"):
            observer = sample["snapshot"]["observer"]
            c2_observer.append({"label": sample["label"], **observer})
    gauge_returned = sorted(
        {
            path
            for result in c_results.values()
            if result.get("samples")
            for path in GAUGE_PATHS
            if value_at(result["samples"][0]["snapshot"], path)
            == value_at(result["samples"][-1]["snapshot"], path)
        }
    )
    capacity_growth = {
        scenario: result.get("plateau", {}).get("capacity", {})
        for scenario, result in c_results.items()
        if result.get("plateau", {}).get("capacity")
    }
    all_errors = [error for result in c_results.values() for error in result.get("errors", [])]
    lines.extend(
        [
            "",
            "## Answers to the A3 discovery questions",
            "",
            f"1. C1-C8 status is recorded above; overall C lane is `{document['lane_results'].get('C')}`.",
            f"2. The required snapshots are tabulated above; canonical numeric JSON remains under `C/*.json`.",
            f"3. C2 observer 100/1000/10000: `{json.dumps(c2_observer, sort_keys=True)}`.",
            f"4. Gauges observed back at baseline in at least one scenario: `{gauge_returned}`.",
            f"5. High-water structures: `{json.dumps(capacity_growth, sort_keys=True)}`.",
            f"6. Live ownership drift: `{any('strict live gauges drifted' in error for error in all_errors)}`.",
            f"7. Resident code-buffer drift: `{any('resident_code' in error or 'code_allocator' in error for error in all_errors)}`.",
            f"8. Watched function/code drift: `{any('watched' in error or 'observer.' in error for error in all_errors)}`.",
            f"9. Active/completed compile residue: `{any('active_compiles' in error or 'completed_compiles' in error for error in all_errors)}`.",
            f"10. Deferred-anchor residue: `{any('deferred_anchor' in error for error in all_errors)}`.",
            f"11. Generator lifetime residue: `{any('generator' in error.lower() for error in c_results.get('C6', {}).get('errors', []))}`; native generator gauge is explicitly unavailable in v0.1.",
            f"12. Unexplained capacity growth: `{any('capacity ' in error for error in all_errors)}`.",
            f"13. Crash/abort/hang evidence: C infra=`{[s for s, r in c_results.items() if r.get('result') == 'INFRA_FAIL']}`, shutdown failures=`{len((document['lanes'].get('F') or {}).get('failures', []))}`.",
            f"14. Mechanism blocker clusters: `{[blocker['id'] for blocker in document['blockers']]}`.",
            "15. Next product-fix MRs must be split by the blocker clusters in A3_BLOCKERS.md; no product lifecycle fix is included in this discovery round.",
            "",
            "## Ownership and shutdown",
            "",
            f"- A3-O: `{document['lane_results'].get('O')}`; refcount corpus details: `O/result.json`.",
            f"- A3-F: `{document['lane_results'].get('F')}`; process exits: `{(document['lanes'].get('F') or {}).get('successful_exits')}/{(document['lanes'].get('F') or {}).get('repetitions')}`.",
            f"- A3-F detector: `{json.dumps((document['lanes'].get('F') or {}).get('detector'), sort_keys=True)}`.",
            "- A3-S: `NOT_RUN_DISCOVERY_PHASE`; ASAN/LSan/refleak are not silently treated as PASS.",
            "",
            *_shutdown_state_rows((document["lanes"].get("F") or {}).get("per_state")),
            "",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def render_blockers(document: dict, path: Path) -> None:
    lines = ["# CPython 3.11 CinderX JIT A3 Blockers", ""]
    blockers = document["blockers"]
    if not blockers:
        lines.extend(["No Product discovery blocker was found.", ""])
    for blocker in blockers:
        snapshots = blocker.get("evidence", {})
        lines.extend(
            [
                f"## {blocker['id']} — {blocker['title']}",
                "",
                f"- Severity: `{blocker['severity']}`",
                f"- Scenario(s): `{blocker['scenarios']}`",
                f"- Minimal reproducer: run `a3_churn.py --scenario {blocker['scenarios'][0]} --out result.json` under the lane environment.",
                f"- Which gauge/counter moved: `{blocker['errors']}`",
                "- Python liveness evidence and native ownership snapshots:",
                "",
                "```json",
                json.dumps(snapshots, indent=2, sort_keys=True),
                "```",
                "",
                "- ASAN/refleak evidence: `NOT_RUN_IN_DISCOVERY_PHASE`",
                f"- Suspected owner structure: `{blocker['title']}` (hypothesis only; source audit required before fixing).",
                "- Suspected release path: the matching death/uncompile/finalize boundary named by the cluster; not yet proven.",
                "- Recommended fix layer: product ownership/release transaction for this mechanism, in a separate MR after review.",
                "",
            ]
        )
    path.write_text("\n".join(lines) + "\n")


def write_reports(document: dict, output: Path) -> None:
    (output / "a3_result.json").write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    render_discovery(document, output / "A3_DISCOVERY_REPORT.md")
    render_blockers(document, output / "A3_BLOCKERS.md")
