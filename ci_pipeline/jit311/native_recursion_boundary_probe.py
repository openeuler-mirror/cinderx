"""Compare direct native Py_EnterRecursiveCall at the last Python frame."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def recursive(remaining, operand):
    if remaining:
        return recursive(remaining - 1, operand)
    return operand + 0


def recovery_target(value):
    return value + 1


def _capture_call(function, *args):
    try:
        return {"value": function(*args), "error": None}
    except BaseException as exc:
        frames = []
        tb = exc.__traceback__
        while tb is not None:
            code = tb.tb_frame.f_code
            frames.append(
                {
                    "function": code.co_name,
                    "tb_lasti": tb.tb_lasti,
                    "line": tb.tb_lineno,
                }
            )
            tb = tb.tb_next
        return {
            "value": None,
            "error": {
                "type": type(exc).__name__,
                "message": str(exc),
                "frames": frames,
            },
        }


def run(mode: str, recursion_limit: int) -> dict:
    import _cinderx

    jit = mode == "jit"
    cinderjit = None
    if jit:
        import cinderjit as loaded_cinderjit
        import cinderx
        from cinderx.jit import jit_suppress

        cinderx.init()
        _cinderx.install_frame_evaluator()
        cinderjit = loaded_cinderjit
        for helper in (_capture_call, run):
            jit_suppress(helper)
        cinderjit._jit311_reset_entry_ledger()
        if not cinderjit.force_compile(recursive):
            raise AssertionError("recursive target did not compile")

    old_limit = sys.getrecursionlimit()
    operand = _cinderx._native_recursion_probe_operand()
    try:
        sys.setrecursionlimit(recursion_limit)
        outer_before = dict(_cinderx._native_recursion_state())
        # The C snapshot returns its recursion slot, then _capture_call adds
        # one Python frame. This trigger admits exactly enough recursive
        # frames for the target body to run at recursion_remaining == 0.
        trigger_depth = outer_before["recursion_remaining"] - 1
        call = _capture_call(
            recursive,
            trigger_depth,
            operand,
        )
        outer_after = dict(_cinderx._native_recursion_state())
    finally:
        sys.setrecursionlimit(old_limit)

    recovery = recovery_target(9)
    entry_rows = []
    dropped = 0
    if cinderjit is not None:
        ledger = cinderjit._jit311_entry_ledger()
        entry_rows = [
            row
            for row in ledger.get("entries", [])
            if row.get("filename") == __file__
            and row.get("qualname") == "recursive"
        ]
        dropped = int(ledger.get("dropped", 0))

    errors = []
    native = call["value"]
    if native is not None and native["before"] != native["after"]:
        errors.append("native helper changed recursion accounting")
    if outer_before["recursion_remaining"] != outer_after["recursion_remaining"]:
        errors.append("outer recursion_remaining drift")
    for key in ("recursion_headroom", "jit_entries"):
        if outer_after.get(key) != 0:
            errors.append(f"outer {key} leaked")
    if outer_after.get("boundary_active") is not False:
        errors.append("outer boundary flag leaked")
    if recovery != 10:
        errors.append("post-error normal call failed")
    if jit and not entry_rows:
        errors.append("recursive target has no machine-entry proof")
    if dropped:
        errors.append("entry ledger dropped evidence")

    return {
        "mode": mode,
        "result": "PASS" if not errors else "FAIL",
        "recursion_limit": recursion_limit,
        "trigger_depth": trigger_depth,
        "outer_before": outer_before,
        "native_helper_executed": native is not None,
        "native": native,
        "call_error": call["error"],
        "outer_after": outer_after,
        "post_error_normal_call": recovery,
        "machine_entry_proven": bool(entry_rows) if jit else False,
        "entry_rows": entry_rows,
        "entry_ledger_dropped": dropped,
        "errors": errors,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("stock", "jit"), required=True)
    parser.add_argument("--recursion-limit", type=int, default=60)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = run(args.mode, args.recursion_limit)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"mode": args.mode, "result": result["result"]}))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
