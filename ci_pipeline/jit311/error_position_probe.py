"""Stock/JIT matrix for CPython 3.11 propagated-error traceback positions."""

from __future__ import annotations

import argparse
import dis
import json
from pathlib import Path


class ProbeError(Exception):
    pass


def callee_raises():
    raise ProbeError("CALL")


def call_raises():
    return callee_raises()


def call_function_ex_raises(function, args):
    return function(*args)


class BinaryRaiser:
    def __add__(self, other):
        raise ProbeError("BINARY_OP")


def binary_op_raises(value):
    return value + 1


class AttrRaiser:
    @property
    def value(self):
        raise ProbeError("LOAD_ATTR")


def load_attr_raises(value):
    return value.value


class MethodRaiser:
    def method(self):
        raise ProbeError("LOAD_METHOD")


def load_method_raises(value):
    return value.method()


class CompareRaiser:
    def __lt__(self, other):
        raise ProbeError("COMPARE_OP")


def compare_op_raises(value):
    return value < 1


def raise_varargs():
    raise ProbeError("RAISE_VARARGS")


CASES = {
    "CALL": (call_raises, ()),
    "CALL_FUNCTION_EX": (call_function_ex_raises, (callee_raises, ())),
    "BINARY_OP": (binary_op_raises, (BinaryRaiser(),)),
    "LOAD_ATTR": (load_attr_raises, (AttrRaiser(),)),
    "LOAD_METHOD": (load_method_raises, (MethodRaiser(),)),
    "COMPARE_OP": (compare_op_raises, (CompareRaiser(),)),
    "RAISE_VARARGS": (raise_varargs, ()),
}


def _logical_instruction(code, lasti: int) -> dict:
    instructions = list(dis.get_instructions(code, show_caches=False))
    for index, instruction in enumerate(instructions):
        next_offset = (
            instructions[index + 1].offset
            if index + 1 < len(instructions)
            else len(code.co_code)
        )
        if instruction.offset <= lasti < next_offset:
            return {
                "opcode": instruction.opname,
                "opcode_offset": instruction.offset,
                "inline_cache_span": next_offset - instruction.offset - 2,
            }
    return {"opcode": None, "opcode_offset": None, "inline_cache_span": None}


def _position(code, lasti: int) -> list[int | None] | None:
    index = lasti // 2
    positions = list(code.co_positions())
    if index < 0 or index >= len(positions):
        return None
    return list(positions[index])


def _capture_exception(function, args) -> dict:
    try:
        function(*args)
    except ProbeError as exc:
        frames = []
        tb = exc.__traceback__
        while tb is not None:
            code = tb.tb_frame.f_code
            logical = _logical_instruction(code, tb.tb_lasti)
            frames.append(
                {
                    "function": code.co_name,
                    "tb_lasti": tb.tb_lasti,
                    "f_lasti": tb.tb_frame.f_lasti,
                    "tb_lineno": tb.tb_lineno,
                    "position": _position(code, tb.tb_lasti),
                    **logical,
                }
            )
            tb = tb.tb_next
        return {"type": type(exc).__name__, "message": str(exc), "frames": frames}
    raise AssertionError("case did not raise ProbeError")


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
        for helper in (_capture_exception, _logical_instruction, _position):
            jit_suppress(helper)
        cinderjit = loaded_cinderjit
        trigger = _cinderx
        cinderjit._jit311_reset_entry_ledger()

    rows = []
    for name, (function, args) in CASES.items():
        _capture_exception(function, args)
        if cinderjit is not None:
            cinderjit._jit311_reset_transition_ledger()
        before = (
            int(trigger._get_trigger_stats()["machine_code_entries"])
            if trigger is not None
            else 0
        )
        observation = _capture_exception(function, args)
        after = (
            int(trigger._get_trigger_stats()["machine_code_entries"])
            if trigger is not None
            else 0
        )
        transitions = (
            cinderjit._jit311_transition_ledger()
            if cinderjit is not None
            else {"transitions": [], "dropped": 0}
        )
        target_frames = [
            frame
            for frame in observation["frames"]
            if frame["function"] == function.__name__
        ]
        rows.append(
            {
                "case": name,
                "function": function.__qualname__,
                "target_frame": target_frames[-1] if target_frames else None,
                "traceback_frames": observation["frames"],
                "machine_entry_delta": after - before,
                "transitions": [
                    row
                    for row in transitions.get("rows", [])
                    if row.get("qualname") == function.__qualname__
                ],
                "transition_ledger_dropped": transitions.get("dropped", 0),
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
    errors = []
    for row in rows:
        evidence = entry_by_name.get(row["function"])
        row["entry_ledger"] = evidence
        row["machine_entry_proven"] = bool(
            evidence and int(evidence.get("entries", 0)) > 0
        )
        if row["target_frame"] is None:
            errors.append(f"{row['case']}: target traceback frame missing")
        if jit and not row["machine_entry_proven"]:
            errors.append(f"{row['case']}: no target machine-entry proof")
        if row["transition_ledger_dropped"]:
            errors.append(f"{row['case']}: transition ledger dropped evidence")

    return {
        "mode": mode,
        "result": "PASS" if not errors else "FAIL",
        "rows": rows,
        "entry_ledger_dropped": entry_ledger.get("dropped", 0),
        "errors": errors,
        "unreachable": {
            "SEND": "automatic generator compilation is outside the current CPython 3.11 execute capability"
        },
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
