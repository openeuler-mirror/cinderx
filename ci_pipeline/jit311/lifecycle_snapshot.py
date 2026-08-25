"""CPython 3.11 lifecycle snapshot schema and plateau judge."""

from __future__ import annotations

import gc
from typing import Any


SCHEMA = "cp311-jit-lifecycle-v1"

GAUGE_PATHS = (
    "jit.compiled_codes",
    "jit.installed_functions",
    "jit.associated_functions",
    "jit.parked_functions",
    "jit.watched_functions",
    "jit.artifact_members",
    "jit.deferred_anchor_releases",
    "jit.active_compiles",
    "jit.completed_compiles",
    "jit.deferred_finalizations",
    "jit.orphaned_compiled_codes",
    "jit.code_dedup_entries",
    "jit.code_outer_functions",
    "jit.context_references",
    "jit.code_runtimes_live",
    "module.registered_compilation_units",
    "module.perf_trampoline_worklist",
    "runtime.resident_code_buffers",
    "runtime.resident_code_extra_blocks",
    "observer.watched_codes",
)

CAPACITY_PATHS = (
    "jit.code_runtimes_allocated",
    "module.code_allocator_used_bytes",
    "observer.keyed_slots",
    "observer.table_capacity",
)

CUMULATIVE_PATHS = (
    "runtime.compiled_function_creations",
    "runtime.function_destroyed_notifications",
    "runtime.code_destroyed_notifications",
    "runtime.executable_alloc_calls",
    "runtime.executable_alloc_bytes",
    "runtime.machine_code_entries",
    "observer.events",
    "observer.post_publication_interpreted_frames",
)

BOOLEAN_PATHS = (
    "context_present",
    "module.unit_deletion_tracking_failed",
    "module.code_allocator_present",
    "generator.native_gauge_available",
)

REQUIRED_PATHS = GAUGE_PATHS + CAPACITY_PATHS + CUMULATIVE_PATHS + BOOLEAN_PATHS


def value_at(document: dict, path: str) -> Any:
    value: Any = document
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            raise KeyError(path)
        value = value[part]
    return value


def validate_snapshot(snapshot: dict) -> list[str]:
    errors = []
    if snapshot.get("schema") != SCHEMA:
        errors.append(f"snapshot schema is not {SCHEMA}")
    for path in REQUIRED_PATHS:
        try:
            value = value_at(snapshot, path)
        except KeyError:
            errors.append(f"snapshot field missing: {path}")
            continue
        expected = bool if path in BOOLEAN_PATHS else int
        if type(value) is not expected:
            errors.append(
                f"snapshot field {path} has type {type(value).__name__}, "
                f"expected {expected.__name__}"
            )
        elif expected is int and value < 0:
            errors.append(f"snapshot field {path} is negative")
    generator = snapshot.get("generator", {})
    if generator.get("native_gauge_available") is False and generator.get(
        "status"
    ) != "GENERATOR_NATIVE_GAUGE_NOT_AVAILABLE":
        errors.append("generator native-gauge unavailability has no exact status")
    return errors


def flatten(snapshot: dict) -> dict[str, int | bool]:
    return {path: value_at(snapshot, path) for path in REQUIRED_PATHS}


def snapshot(cinderjit, *, collect: bool = False) -> dict:
    if collect:
        gc.collect()
        gc.collect()
    result = dict(cinderjit._jit311_lifecycle_snapshot())
    errors = validate_snapshot(result)
    if errors:
        raise RuntimeError("; ".join(errors))
    return result


def checkpoint(cinderjit, label: str, *, python: dict | None = None) -> dict:
    snap = snapshot(cinderjit, collect=True)
    invariants = dict(cinderjit._jit311_lifecycle_invariants())
    errors = []
    if invariants.get("ok") is not True:
        errors.extend(str(item) for item in invariants.get("errors", []))
    return {
        "label": label,
        "snapshot": snap,
        "invariants": invariants,
        "python_liveness": python or {},
        "errors": errors,
    }


def judge_plateau(
    samples: list[dict],
    *,
    baseline_label: str = "baseline",
    final_label: str = "after_gc_2",
    capacity_pair: tuple[str, str] = ("after_100", "after_1000"),
) -> dict:
    by_label = {sample["label"]: sample for sample in samples}
    errors = []
    for sample in samples:
        errors.extend(f"{sample['label']}: {item}" for item in sample["errors"])
    if baseline_label not in by_label or final_label not in by_label:
        errors.append(
            f"missing strict plateau endpoints: {baseline_label}, {final_label}"
        )
        return {"result": "FAIL", "errors": errors, "gauge_drift": {}}
    baseline = by_label[baseline_label]["snapshot"]
    final = by_label[final_label]["snapshot"]
    gauge_drift = {}
    for path in GAUGE_PATHS:
        left = value_at(baseline, path)
        right = value_at(final, path)
        if left != right:
            gauge_drift[path] = {"baseline": left, "final": right}
    if gauge_drift:
        errors.append(f"strict live gauges drifted: {sorted(gauge_drift)}")
    capacity = {}
    left_label, right_label = capacity_pair
    if left_label not in by_label or right_label not in by_label:
        errors.append(f"missing capacity plateau pair: {capacity_pair}")
    else:
        for path in CAPACITY_PATHS:
            left = value_at(by_label[left_label]["snapshot"], path)
            right = value_at(by_label[right_label]["snapshot"], path)
            if path == "observer.keyed_slots":
                # keyed_slots includes live keys plus tombstones.  Exact
                # equality is not a valid open-addressing invariant: bounded
                # tombstone jitter can occur while the allocated table stays
                # fixed.  Eight slots is a structural policy, not a measured
                # baseline; linear growth still fails loudly.
                allowed_growth = 8
                plateau = right <= left + allowed_growth
                policy = "bounded-tombstone-jitter"
            else:
                allowed_growth = 0
                plateau = left == right
                policy = "exact-high-water-plateau"
            capacity[path] = {
                left_label: left,
                right_label: right,
                "allowed_growth": allowed_growth,
                "policy": policy,
                "plateau": plateau,
            }
            if not plateau:
                errors.append(
                    f"capacity {path} grows from {left_label}={left} "
                    f"to {right_label}={right}"
                )
    return {
        "result": "PASS" if not errors else "FAIL",
        "baseline_label": baseline_label,
        "final_label": final_label,
        "gauge_drift": gauge_drift,
        "capacity": capacity,
        "errors": errors,
    }


FIELD_CLASSES = {
    "gauge": list(GAUGE_PATHS),
    "capacity_high_water": list(CAPACITY_PATHS),
    "cumulative_counter": list(CUMULATIVE_PATHS),
    "boolean": list(BOOLEAN_PATHS),
}
