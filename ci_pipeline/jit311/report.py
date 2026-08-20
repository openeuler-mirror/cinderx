# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Unified trigger-proof run report for the CPython 3.11 JIT gates.

Produces the JSON document defined in docs/cp311-jit-dev-plan.md (MR-01).
Every field is either measured from the running process (runtime-derived
fields) or owned by the harness that orchestrates worker processes
(harness-owned fields).  Runtime-derived fields come from read-only
introspection only: nothing here mutates JIT state, so a report can be taken
at any point without perturbing the run.

Field sources:

  evaluator_installed        _cinderx.is_frame_evaluator_installed()
  compile_requests           observe stats: recorded scheduling events
  compile_success            trigger stats: shadow_compile_success
  compile_rejected           observe stats: events with a refusal result
  events_dropped             observe stats: requests omitted from the bounded
                             event ledger (must be zero for an accepted run)
  capability_rejects         recorded capability-gate refusals
  opcode_rejects             registered opcode-level refusals
  shape_rejects              registered code-shape refusals
  supported_opcode_failures  failures after opcode and shape eligibility
  unknown_rejects            observe stats: refusal reasons outside the
                             registered set
  specialized_opcodes_consumed
                             trigger stats: physical specialized instructions
                             consumed by successful shadow compilations
  shadow_codegen_bytes       trigger stats: discarded machine-code byte count
  peak_rss_bytes             process peak resident set size
  compile_installed          scheduling events answered by installing
                             machine code (canary; zero under shadow, which
                             compiles without installing)
  machine_code_installed     len(cinderjit.get_compiled_functions())
  machine_code_entries       trigger stats (incremented by the 3.11 entry
                             glue once machine-code execution ships; zero on
                             capability-gated builds by construction)
  executable_alloc_calls     trigger stats (supplementary, not in the minimal
  executable_alloc_bytes     schema but required by the MR-02 gate proof)
  compiled_function_creations trigger stats (supplementary, see above)
  forced_deopt_hits          0 until the site-deopt runner lands (MR-07)
  organic_deopt_hits         0 until deopt counters land (MR-07)

Harness-owned fields (filled by runners, None until then):

  target_modules_attempted, worker_crashes,
  live_compiled_functions_at_exit, resident_compiled_functions_at_exit

The refusal-reason registry starts with the capability-gate refusal; the
front-end MR extends it from the bytecode support list and the shape-refusal
registry.  Any refusal outside the registry counts into unknown_rejects,
which the blocking conditions treat as red.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SUPPORT_LIST = REPO_ROOT / "cinderx/Interpreter/3.11/bytecode_support.toml"

# Capability reasons do not describe bytecode or code-object shape, so they
# stay local to the mode gate instead of polluting the front-end fact source.
CAPABILITY_REFUSAL_REASONS: frozenset[str] = frozenset({
    "CINDERX311_JIT_EXEC_DISABLED",
})


def load_refusal_reason_registry(
    path: Path = SUPPORT_LIST,
) -> dict[str, frozenset[str]]:
    """Load the closed opcode/shape reason namespaces from the support list."""
    import tomllib

    with path.open("rb") as fp:
        doc = tomllib.load(fp)
    raw_registry = doc.get("refusal_reasons", {})
    return {
        category: frozenset(reasons)
        for category, reasons in raw_registry.items()
        if isinstance(reasons, dict)
    }


REFUSAL_REASON_REGISTRY = load_refusal_reason_registry()
KNOWN_REFUSAL_REASONS: frozenset[str] = frozenset().union(
    CAPABILITY_REFUSAL_REASONS,
    *REFUSAL_REASON_REGISTRY.values(),
)


def classify_refusal_reason(
    reason: object,
    registry: dict[str, frozenset[str]] = REFUSAL_REASON_REGISTRY,
) -> str:
    """Classify a result as capability, opcode, shape, or unknown."""
    if not isinstance(reason, str):
        return "unknown"
    if reason in CAPABILITY_REFUSAL_REASONS:
        return "capability"
    if reason in registry.get("opcode", ()):
        return "opcode"
    if reason in registry.get("shape", ()):
        return "shape"
    return "unknown"

RUNTIME_FIELDS = (
    "evaluator_installed",
    "compile_requests",
    "compile_success",
    "compile_installed",
    "compile_rejected",
    "events_dropped",
    "capability_rejects",
    "opcode_rejects",
    "shape_rejects",
    "supported_opcode_failures",
    "unknown_rejects",
    "specialized_opcodes_consumed",
    "shadow_codegen_bytes",
    "peak_rss_bytes",
    "machine_code_installed",
    "machine_code_entries",
    "executable_alloc_calls",
    "executable_alloc_bytes",
    "compiled_function_creations",
    "forced_deopt_hits",
    "organic_deopt_hits",
)

HARNESS_FIELDS = (
    "target_modules_attempted",
    "worker_crashes",
    "live_compiled_functions_at_exit",
    "resident_compiled_functions_at_exit",
)

ALL_FIELDS = RUNTIME_FIELDS + HARNESS_FIELDS


def _trigger_stats() -> dict[str, int]:
    import _cinderx

    return _cinderx._get_trigger_stats()


def _observe_stats() -> dict[str, Any] | None:
    import _cinderx

    get_stats = getattr(_cinderx, "_get_observe_stats", None)
    if get_stats is None:
        return None
    return get_stats()


def _evaluator_installed() -> bool:
    import _cinderx

    probe = getattr(_cinderx, "is_frame_evaluator_installed", None)
    if probe is None:
        return False
    return bool(probe())


def _compiled_function_count() -> int:
    try:
        from cinderjit import get_compiled_functions
    except ImportError:
        return 0
    return len(get_compiled_functions())


def _peak_rss_bytes() -> int:
    """Return this process's peak RSS in bytes when the platform exposes it."""
    try:
        import resource
    except ImportError:
        return 0

    try:
        peak = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
    except (OSError, ValueError):
        return 0
    # Darwin reports bytes; Linux and the other Unix targets used by the gate
    # report KiB.  Keep the report schema platform-neutral.
    return peak if sys.platform == "darwin" else peak * 1024


def snapshot() -> dict[str, Any]:
    """Collect the runtime-derived fields from the current process."""
    trigger = _trigger_stats()
    observe = _observe_stats()

    compile_requests = 0
    events_dropped = 0
    reject_counts = {
        "capability": 0,
        "opcode": 0,
        "shape": 0,
        "unknown": 0,
    }
    supported_opcode_failures = 0
    unknown_rejects = 0
    compile_installed = 0
    if observe is not None:
        events = observe.get("events", [])
        compile_requests = len(events)
        events_dropped = int(observe.get("events_dropped", 0))
        for event in events:
            result = event.get("result")
            if result in ("ok", "compiled", "installed"):
                if result == "installed":
                    compile_installed += 1
                continue
            category = classify_refusal_reason(result)
            reject_counts[category] += 1
            if result == "SUPPORTED_OPCODE_FAILURE":
                supported_opcode_failures += 1
            if category == "unknown":
                unknown_rejects += 1

    compile_rejected = sum(reject_counts.values())

    report: dict[str, Any] = {
        "evaluator_installed": _evaluator_installed(),
        "compile_requests": compile_requests,
        "compile_success": int(trigger["shadow_compile_success"]),
        "compile_installed": compile_installed,
        "compile_rejected": compile_rejected,
        "events_dropped": events_dropped,
        "capability_rejects": reject_counts["capability"],
        "opcode_rejects": reject_counts["opcode"],
        "shape_rejects": reject_counts["shape"],
        "supported_opcode_failures": supported_opcode_failures,
        "unknown_rejects": unknown_rejects,
        "specialized_opcodes_consumed": int(
            trigger["shadow_specialized_opcodes_consumed"]
        ),
        "shadow_codegen_bytes": int(trigger["shadow_codegen_bytes"]),
        "peak_rss_bytes": _peak_rss_bytes(),
        "machine_code_installed": _compiled_function_count(),
        "machine_code_entries": int(trigger["machine_code_entries"]),
        "executable_alloc_calls": int(trigger["executable_alloc_calls"]),
        "executable_alloc_bytes": int(trigger["executable_alloc_bytes"]),
        "compiled_function_creations": int(
            trigger["compiled_function_creations"]
        ),
        "forced_deopt_hits": 0,
        "organic_deopt_hits": 0,
    }
    for field in HARNESS_FIELDS:
        report[field] = None
    return report


def validate_schema(report: dict[str, Any], strict: bool = False) -> list[str]:
    """Structural check used by the self-tests: every field present, no
    extras, runtime fields typed.  A child snapshot leaves harness fields
    as None for its own leg to fill; the aggregated unified report passes
    strict=True, where every harness field must be a non-negative int."""
    errors = []
    for field in ALL_FIELDS:
        if field not in report:
            errors.append(f"missing field {field}")
    for field in report:
        if field not in ALL_FIELDS:
            errors.append(f"unknown field {field}")
    if not isinstance(report.get("evaluator_installed"), bool):
        errors.append("evaluator_installed must be a bool")
    for field in RUNTIME_FIELDS[1:]:
        value = report.get(field)
        if not isinstance(value, int) or isinstance(value, bool):
            errors.append(f"{field} must be an int, got {value!r}")
    if strict:
        for field in HARNESS_FIELDS:
            value = report.get(field)
            if not (isinstance(value, int) and not isinstance(value, bool)
                    and value >= 0):
                errors.append(
                    f"{field} must be a non-negative int in the unified "
                    f"report, got {value!r}"
                )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=str, default=None)
    args = parser.parse_args(argv)

    report = snapshot()
    errors = validate_schema(report)
    if errors:
        for error in errors:
            print(f"trigger-report: {error}", file=sys.stderr)
        return 1
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.out:
        with open(args.out, "w", encoding="utf8") as fp:
            fp.write(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
