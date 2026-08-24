"""Stock/JIT CPython 3.11 recursion-boundary frame and accounting matrix."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def plain_recursive(depth):
    return 0 if depth == 0 else 1 + plain_recursive(depth - 1)


def compiled_recursive(depth):
    return 0 if depth == 0 else 1 + compiled_recursive(depth - 1)


def interpreted_caller(depth):
    return compiled_recursive_from_interpreted(depth)


def compiled_recursive_from_interpreted(depth):
    return (
        0
        if depth == 0
        else 1 + compiled_recursive_from_interpreted(depth - 1)
    )


def required_argument(value):
    return value


def defaults_kwonly_recursive(depth, value=1, *, scale=1):
    if depth == 0:
        return value * scale
    return 1 + defaults_kwonly_recursive(depth - 1, scale=scale)


def recovery_target(value):
    return value + 1


def _position(code, lasti):
    positions = list(code.co_positions())
    index = lasti // 2
    return list(positions[index]) if 0 <= index < len(positions) else None


def _exception(function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except BaseException as exc:
        frames = []
        tb = exc.__traceback__
        while tb is not None:
            code = tb.tb_frame.f_code
            frames.append(
                {
                    "function": code.co_name,
                    "tb_lasti": tb.tb_lasti,
                    "f_lasti": tb.tb_frame.f_lasti,
                    "line": tb.tb_lineno,
                    "position": _position(code, tb.tb_lasti),
                }
            )
            tb = tb.tb_next
        return {
            "type": type(exc).__name__,
            "message": str(exc),
            "frames": frames,
        }
    return {"type": None, "message": None, "frames": []}


def _stock_recursion_state():
    import _testinternalcapi

    limit = sys.getrecursionlimit()
    depth = _testinternalcapi.get_recursion_depth()
    return {
        "recursion_limit": limit,
        "recursion_remaining": limit - depth,
        "recursion_headroom": 0,
        "boundary_active": False,
        "jit_entries": 0,
    }


def run(mode: str, recursion_limit: int) -> dict:
    jit = mode == "jit"
    cinderjit = None
    _cinderx = None
    if jit:
        import _cinderx as loaded_cinderx
        import cinderjit as loaded_cinderjit
        import cinderx
        from cinderx.jit import jit_suppress

        cinderx.init()
        loaded_cinderx.install_frame_evaluator()
        for helper in (_position, _exception, _stock_recursion_state, run):
            jit_suppress(helper)
        jit_suppress(interpreted_caller)
        cinderjit = loaded_cinderjit
        _cinderx = loaded_cinderx
        cinderjit._jit311_reset_entry_ledger()

    def state():
        if cinderjit is None:
            return _stock_recursion_state()
        return {
            "recursion_limit": sys.getrecursionlimit(),
            **dict(cinderjit._jit311_recursion_state()),
        }

    if jit:
        # The observation itself must not contribute a live JIT recursion
        # entry to the state it is measuring.
        jit_suppress(state)

    def prepare(function, invoke):
        if cinderjit is None:
            invoke()
            return {"compiled": False, "machine_entry_proven": False}
        for _ in range(10):
            invoke()
            if cinderjit.is_jit_compiled(function):
                break
        return {
            "compiled": bool(cinderjit.is_jit_compiled(function)),
            "machine_entry_proven": False,
        }

    old_limit = sys.getrecursionlimit()
    rows = []
    try:
        for ident, function, invoke in (
            (
                "R1",
                plain_recursive,
                lambda: plain_recursive(3),
            ),
            (
                "R2",
                compiled_recursive,
                lambda: compiled_recursive(3),
            ),
        ):
            pre = prepare(function, invoke)
            sys.setrecursionlimit(recursion_limit)
            before = state()
            error = _exception(function, 100_000)
            after = state()
            sys.setrecursionlimit(old_limit)
            recovery = function(4)
            rows.append(
                {
                    "id": ident,
                    "pre": pre,
                    "before": before,
                    "after": after,
                    "error": error,
                    "target_frames": [
                        frame
                        for frame in error["frames"]
                        if frame["function"] == function.__name__
                    ],
                    "post_error_recovery": recovery,
                }
            )

        pre = prepare(
            compiled_recursive_from_interpreted,
            lambda: compiled_recursive_from_interpreted(3),
        )
        sys.setrecursionlimit(recursion_limit)
        before = state()
        error = _exception(interpreted_caller, 100_000)
        after = state()
        sys.setrecursionlimit(old_limit)
        rows.append(
            {
                "id": "R3",
                "pre": pre,
                "before": before,
                "after": after,
                "error": error,
                "target_frames": [
                    frame
                    for frame in error["frames"]
                    if frame["function"]
                    in {
                        "interpreted_caller",
                        "compiled_recursive_from_interpreted",
                    }
                ],
                "post_error_recovery": interpreted_caller(4),
            }
        )

        pre = prepare(required_argument, lambda: required_argument(1))
        current = state()
        current_depth = (
            current["recursion_limit"] - current["recursion_remaining"]
        )
        sys.setrecursionlimit(current_depth + 1)
        before = state()
        error = _exception(required_argument)
        after = state()
        sys.setrecursionlimit(old_limit)
        rows.append(
            {
                "id": "R4",
                "pre": pre,
                "before": before,
                "after": after,
                "error": error,
                "target_frames": [],
                "post_error_recovery": required_argument(4),
            }
        )

        pre = prepare(
            defaults_kwonly_recursive,
            lambda: defaults_kwonly_recursive(3, scale=2),
        )
        sys.setrecursionlimit(recursion_limit)
        before = state()
        error = _exception(
            defaults_kwonly_recursive,
            100_000,
            scale=2,
        )
        after = state()
        sys.setrecursionlimit(old_limit)
        rows.append(
            {
                "id": "R5",
                "pre": pre,
                "before": before,
                "after": after,
                "error": error,
                "target_frames": [
                    frame
                    for frame in error["frames"]
                    if frame["function"] == "defaults_kwonly_recursive"
                ],
                "post_error_recovery": defaults_kwonly_recursive(4, scale=2),
            }
        )

        pre = prepare(recovery_target, lambda: recovery_target(1))
        before = state()
        recovered = recovery_target(9)
        after = state()
        rows.append(
            {
                "id": "R6",
                "pre": pre,
                "before": before,
                "after": after,
                "error": None,
                "target_frames": [],
                "post_error_recovery": recovered,
            }
        )
    finally:
        sys.setrecursionlimit(old_limit)

    if cinderjit is not None:
        ledger = cinderjit._jit311_entry_ledger()
        entries = {
            row["qualname"]: row
            for row in ledger.get("entries", [])
            if row.get("filename") == __file__
        }
        for row in rows:
            names = {
                "R1": {"plain_recursive"},
                "R2": {"compiled_recursive"},
                "R3": {"compiled_recursive_from_interpreted"},
                "R4": {"required_argument"},
                "R5": {"defaults_kwonly_recursive"},
                "R6": {"recovery_target"},
            }[row["id"]]
            evidence = [entries[name] for name in names if name in entries]
            row["pre"]["entry_rows"] = evidence
            row["pre"]["machine_entry_proven"] = bool(evidence)
        dropped = ledger.get("dropped", 0)
    else:
        dropped = 0

    errors = []
    for row in rows:
        if row["before"]["recursion_remaining"] != row["after"][
            "recursion_remaining"
        ]:
            errors.append(f"{row['id']}: recursion_remaining drift")
        if row["after"].get("boundary_active"):
            errors.append(f"{row['id']}: boundary flag leaked")
        if jit and not row["pre"]["compiled"]:
            errors.append(f"{row['id']}: target did not compile")
        if row["id"] in {"R1", "R2", "R3", "R5"} and row["error"][
            "type"
        ] != "RecursionError":
            errors.append(f"{row['id']}: expected RecursionError")
        if row["id"] == "R4" and row["error"]["type"] != "TypeError":
            errors.append("R4: binding error lost precedence")
    if dropped:
        errors.append("entry ledger dropped evidence")
    return {
        "mode": mode,
        "result": "PASS" if not errors else "FAIL",
        "recursion_limit": recursion_limit,
        "rows": rows,
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
    print(json.dumps({"result": result["result"], "mode": args.mode}))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
