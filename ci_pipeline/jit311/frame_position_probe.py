"""Stock/JIT matrix for CPython 3.11 running-frame source positions."""

from __future__ import annotations

import argparse
import inspect
import json
from pathlib import Path
import sys


def _position(code, lasti: int) -> list[int | None] | None:
    index = lasti // 2
    positions = list(code.co_positions())
    if index < 0 or index >= len(positions):
        return None
    return list(positions[index])


def _snapshot(depth=1) -> dict:
    frame = sys._getframe(depth)
    info = inspect.getframeinfo(frame)
    stack = inspect.stack(context=0)[depth]
    return {
        "function": frame.f_code.co_name,
        "f_lasti": frame.f_lasti,
        "f_lineno": frame.f_lineno,
        "co_position": _position(frame.f_code, frame.f_lasti),
        "inspect_position": list(info.positions) if info.positions else None,
        "stack_position": list(stack.positions) if stack.positions else None,
    }


def simple_expression_probe(value):
    computed = value + 1
    if computed:
        return _snapshot()
    return None


def call_probe():
    return _snapshot()


class AttrProbe:
    @property
    def value(self):
        return _snapshot(2)


def load_attr_probe(value):
    return value.value


class BinaryProbe:
    def __add__(self, other):
        return _snapshot(2)


def binary_op_probe(value):
    return value + 1


def branch_probe(flag):
    if flag:
        return _snapshot()
    return None


def loop_probe(limit):
    index = 0
    while index < limit:
        if index == 1:
            return _snapshot()
        index += 1
    return None


def exception_edge_probe():
    try:
        raise ValueError("edge")
    except ValueError:
        return _snapshot()


CASES = {
    "simple_expression": (simple_expression_probe, (1,)),
    "CALL": (call_probe, ()),
    "LOAD_ATTR": (load_attr_probe, (AttrProbe(),)),
    "BINARY_OP": (binary_op_probe, (BinaryProbe(),)),
    "branch": (branch_probe, (True,)),
    "loop": (loop_probe, (3,)),
    "exception_edge": (exception_edge_probe, ()),
}


def run(mode: str) -> dict:
    jit = mode == "jit"
    cinderjit = None
    trigger = None
    if jit:
        import _cinderx
        import cinderjit as loaded_cinderjit
        import cinderx
        from cinderx.jit import jit_suppress

        cinderx.init()
        _cinderx.install_frame_evaluator()
        jit_suppress(_snapshot)
        jit_suppress(_position)
        cinderjit = loaded_cinderjit
        trigger = _cinderx
        cinderjit._jit311_reset_entry_ledger()

    rows = []
    for name, (function, args) in CASES.items():
        function(*args)
        before = (
            int(trigger._get_trigger_stats()["machine_code_entries"])
            if trigger is not None
            else 0
        )
        observation = function(*args)
        after = (
            int(trigger._get_trigger_stats()["machine_code_entries"])
            if trigger is not None
            else 0
        )
        rows.append(
            {
                "case": name,
                "function": function.__qualname__,
                "observation": observation,
                "machine_entry_delta": after - before,
            }
        )

    entry_ledger = (
        cinderjit._jit311_entry_ledger()
        if cinderjit is not None
        else {"entries": [], "dropped": 0}
    )
    entry_by_name = {
        row["qualname"]: row
        for row in entry_ledger.get("entries", [])
        if row.get("filename") == __file__
    }
    for row in rows:
        evidence = entry_by_name.get(row["function"])
        row["entry_ledger"] = evidence
        row["machine_entry_proven"] = bool(
            evidence and int(evidence.get("entries", 0)) > 0
        )

    errors = []
    for row in rows:
        observation = row.get("observation") or {}
        positions = {
            tuple(observation[key])
            for key in ("co_position", "inspect_position", "stack_position")
            if observation.get(key) is not None
        }
        if len(positions) != 1:
            errors.append(f"{row['case']}: Python position APIs disagree")
        if jit and not row["machine_entry_proven"]:
            errors.append(f"{row['case']}: no target machine-entry proof")

    return {
        "mode": mode,
        "result": "PASS" if not errors else "FAIL",
        "rows": rows,
        "entry_ledger_dropped": entry_ledger.get("dropped", 0),
        "errors": errors,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("stock", "jit"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = run(args.mode)
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
