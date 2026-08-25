"""CPython 3.11 code-swap transition scheduler policy diagnostics."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def code_a(value):
    return value + 1


def code_b(value):
    return value + 2


def _entry_count(cinderjit, code) -> int:
    return sum(
        int(row.get("entries", 0))
        for row in cinderjit._jit311_entry_ledger().get("entries", [])
        if row.get("filename") == code.co_filename
        and row.get("firstlineno") == code.co_firstlineno
        and row.get("qualname") == code.co_qualname
    )


def _own_scheduler_events(_cinderx, code) -> list[dict]:
    return [
        row
        for row in _cinderx._get_observe_stats().get("events", [])
        if row.get("filename") == code.co_filename
        and row.get("qualname") == code.co_qualname
    ]


def _state(cinderjit, function) -> dict:
    return dict(cinderjit._jit311_code_state(function))


def run(cycles: int, force_compile: bool) -> dict:
    import _cinderx
    import cinderjit
    import cinderx
    from cinderx.jit import jit_suppress

    cinderx.init()
    _cinderx.install_frame_evaluator()
    for helper in (_entry_count, _own_scheduler_events, _state, run):
        jit_suppress(helper)

    original_code = code_a.__code__
    alternate_code = code_b.__code__
    cinderjit._jit311_reset_entry_ledger()
    for _ in range(30):
        code_a(1)
        if cinderjit.is_jit_compiled(code_a):
            break
    if not cinderjit.is_jit_compiled(code_a):
        raise AssertionError("initial code_a did not compile organically")

    rows = []
    semantic_failures = 0
    stale_machine_entries = 0
    untyped_policy = 0
    force_failures = 0
    allowed_interpreter_policy = {
        "code-verdict-final",
        "automatic-attempt-spent-artifact-retired",
        "existing-member-not-fresh-attachable",
        "fresh-attach-budget-exhausted",
    }
    for cycle in range(cycles):
        old_code = code_a.__code__
        new_code = alternate_code if old_code is original_code else original_code
        before_state = _state(cinderjit, code_a)
        old_entries_before = _entry_count(cinderjit, old_code)
        new_entries_before = _entry_count(cinderjit, new_code)
        scheduler_before = len(_own_scheduler_events(_cinderx, new_code))

        code_a.__code__ = new_code
        after_swap = _state(cinderjit, code_a)
        forced_result = None
        forced_error = None
        after_force = None
        if force_compile:
            try:
                forced_result = bool(cinderjit.force_compile(code_a))
            except BaseException as exc:
                forced_error = f"{type(exc).__name__}: {exc}"
                force_failures += 1
            after_force = _state(cinderjit, code_a)

        expected = 3 if new_code is alternate_code else 2
        value = code_a(1)
        if value != expected:
            semantic_failures += 1
        after_call = _state(cinderjit, code_a)
        for _ in range(10):
            if code_a(1) != expected:
                semantic_failures += 1
        after_warm = _state(cinderjit, code_a)

        old_entries_after = _entry_count(cinderjit, old_code)
        new_entries_after = _entry_count(cinderjit, new_code)
        stale_delta = old_entries_after - old_entries_before
        machine_delta = new_entries_after - new_entries_before
        if stale_delta:
            stale_machine_entries += stale_delta
        policy_reason = after_warm.get("policy_reason")
        policy_ok = bool(after_warm.get("installed")) or (
            policy_reason in allowed_interpreter_policy and machine_delta == 0
        )
        if not policy_ok:
            untyped_policy += 1
        if force_compile and (
            forced_error is not None
            or not after_force.get("installed")
            or not after_warm.get("installed")
            or machine_delta <= 0
        ):
            force_failures += 1

        rows.append(
            {
                "cycle": cycle,
                "function_id": id(code_a),
                "old_code_id": id(old_code),
                "new_code_id": id(new_code),
                "new_code_firstlineno": new_code.co_firstlineno,
                "new_code_qualname": new_code.co_qualname,
                "is_jit_compiled_before": bool(before_state["installed"]),
                "is_jit_compiled_after_swap": bool(after_swap["installed"]),
                "is_jit_compiled_after": bool(after_warm["installed"]),
                "code_has_artifact": bool(after_warm["code_has_artifact"]),
                "artifact_member": bool(after_warm["artifact_member"]),
                "auto_jit_disabled": bool(after_warm["auto_jit_disabled"]),
                "policy_reason": policy_reason,
                "policy_ok": policy_ok,
                "scheduler_events": _own_scheduler_events(_cinderx, new_code)[
                    scheduler_before:
                ],
                "attach_count_before": after_swap["fresh_attach_count"],
                "attach_count_after": after_warm["fresh_attach_count"],
                "fresh_attach_budget": after_warm["fresh_attach_budget"],
                "machine_entry_delta": machine_delta,
                "stale_old_code_entry_delta": stale_delta,
                "force_compile_result": forced_result,
                "force_compile_error": forced_error,
                "state_after_force": after_force,
                "value": value,
                "expected": expected,
                "state_before": before_state,
                "state_after_swap": after_swap,
                "state_after_call": after_call,
                "state_after_warm": after_warm,
            }
        )

    code_a.__code__ = original_code
    ledger = cinderjit._jit311_entry_ledger()
    auto_reentry = sum(1 for row in rows if row["machine_entry_delta"] > 0)
    interpreter_policy = cycles - auto_reentry
    passed = (
        semantic_failures == 0
        and stale_machine_entries == 0
        and untyped_policy == 0
        and ledger.get("dropped", 0) == 0
        and (not force_compile or force_failures == 0)
    )
    return {
        "result": "PASS" if passed else "FAIL",
        "classification": (
            "COMPILER_RUNTIME_CAPABILITY_OK"
            if force_compile and passed
            else "APPROVED_POLICY_DECISION" if passed else "PRODUCT_BUG"
        ),
        "mode": "force-compile-diagnostic" if force_compile else "automatic",
        "cycles": cycles,
        "fresh_attach_budget": rows[0]["fresh_attach_budget"] if rows else None,
        "semantic_failures": semantic_failures,
        "stale_machine_entries": stale_machine_entries,
        "untyped_policy": untyped_policy,
        "force_failures": force_failures,
        "automatic_reentry_cycles": auto_reentry,
        "interpreter_policy_cycles": interpreter_policy,
        "entry_ledger_dropped": ledger.get("dropped", 0),
        "rows": rows,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--force-compile", action="store_true")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = run(args.cycles, args.force_compile)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(
        json.dumps(
            {
                "result": result["result"],
                "classification": result["classification"],
                "cycles": args.cycles,
            },
            sort_keys=True,
        )
    )
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
