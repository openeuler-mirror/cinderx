"""Run and judge all CPython 3.11 specialization-conformance witnesses."""

from __future__ import annotations

import argparse
import dis
import json
from pathlib import Path
import tomllib


GLOBAL_VALUE = 17


class Box:
    def __init__(self, value):
        self.value = value

    def add(self, value):
        return self.value + value


def binary_op(left, right):
    return left + right


def binary_subscr(values, index):
    return values[index]


def call_python(function, value):
    return function(value)


def compare_jump(left, right):
    if left < right:
        return 1
    return 0


def load_attr(obj):
    return obj.value


def load_global():
    return GLOBAL_VALUE


def load_method(obj, value):
    return obj.add(value)


def store_attr(obj, value):
    obj.value = value
    return obj.value


def store_subscr(values, index, value):
    values[index] = value
    return values[index]


def unpack_sequence(values):
    left, right = values
    return left + right


def loop(value):
    total = value - value
    for _ in range(value):
        total = total + 1
    return total


def const_fast(value):
    return 7 + value


_extended_namespace: dict = {}
exec(
    "def extended(value):\n"
    + "\n".join(f"    value_{index} = {index}" for index in range(300))
    + "\n    return value_299 + value\n",
    _extended_namespace,
)
extended = _extended_namespace["extended"]


def _witnesses():
    box = Box(5)
    values = [1, 2, 3]
    add_one = lambda value: value + 1
    return {
        "binary-op-int": (binary_op, lambda: binary_op(2, 3), 5),
        "binary-subscr-list": (binary_subscr, lambda: binary_subscr(values, 1), 2),
        "call-python-exact": (call_python, lambda: call_python(add_one, 4), 5),
        "compare-int-jump": (compare_jump, lambda: compare_jump(2, 3), 1),
        "extended-arg-quick": (extended, lambda: extended(1), 300),
        "jump-backward-quick": (loop, lambda: loop(5), 5),
        "load-attr-instance": (load_attr, lambda: load_attr(box), 5),
        "load-const-fast": (const_fast, lambda: const_fast(5), 12),
        "load-global-module": (load_global, load_global, 17),
        "load-method-values": (load_method, lambda: load_method(box, 4), 9),
        "store-attr-instance": (store_attr, lambda: store_attr(box, 5), 5),
        "store-subscr-list": (store_subscr, lambda: store_subscr(values, 1, 2), 2),
        "unpack-two-tuple": (unpack_sequence, lambda: unpack_sequence((2, 3)), 5),
    }


def run(manifest_path: Path) -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()

    with manifest_path.open("rb") as stream:
        manifest = tomllib.load(stream)
    families = manifest["family"]
    witnesses = _witnesses()
    results = []

    # Warm each function only once.  Several normalization families share the
    # same code object, which is exactly the relationship the manifest records.
    warmed: set[int] = set()
    for family in families:
        function, invoke, expected = witnesses[family["witness"]]
        if id(function) not in warmed:
            for _ in range(300):
                actual = invoke()
                if actual != expected:
                    raise AssertionError((family["witness"], actual, expected))
            warmed.add(id(function))

        adaptive = [
            instruction.opname
            for instruction in dis.get_instructions(function, adaptive=True)
        ]
        expected_opcodes = family["expected_specialized_opcodes"]
        specialized_proven = all(opcode in adaptive for opcode in expected_opcodes)
        before = int(_cinderx._get_trigger_stats()["machine_code_entries"])
        diagnostic = cinderjit._jit311_compile_diagnostic(function)
        semantic_result = invoke()
        after = int(_cinderx._get_trigger_stats()["machine_code_entries"])
        entry_delta = after - before

        if family["expected_outcome"] == "jit":
            passed = (
                specialized_proven
                and diagnostic["compiled"]
                and diagnostic["reason"] is None
                and entry_delta > 0
                and semantic_result == expected
            )
            outcome = "W-JIT" if passed else "UNCLASSIFIED"
        else:
            refusal_opcode = diagnostic.get("opcode")
            refusal_opcode_name = (
                dis.opname[refusal_opcode]
                if isinstance(refusal_opcode, int)
                and 0 <= refusal_opcode < len(dis.opname)
                else None
            )
            passed = (
                specialized_proven
                and not diagnostic["compiled"]
                and diagnostic["reason"] == family["expected_reason"]
                and refusal_opcode_name == family["name"]
                and entry_delta == 0
                and semantic_result == expected
            )
            outcome = "W-SAFE-REFUSE" if passed else "UNCLASSIFIED"
        results.append(
            {
                "family": family["name"],
                "witness": family["witness"],
                "expected_outcome": family["expected_outcome"],
                "expected_specialized_opcodes": expected_opcodes,
                "adaptive_opcodes": adaptive,
                "specialized_proven": specialized_proven,
                "diagnostic": diagnostic,
                "machine_entries_delta": entry_delta,
                "semantic_result": semantic_result,
                "outcome": outcome,
                "pass": passed,
            }
        )

    counts = {
        "families": len(results),
        "w_jit": sum(item["outcome"] == "W-JIT" for item in results),
        "w_safe_refuse": sum(item["outcome"] == "W-SAFE-REFUSE" for item in results),
        "unknown": sum(not item["pass"] for item in results),
    }
    return {
        "result": "PASS" if len(results) == 17 and counts["unknown"] == 0 else "FAIL",
        "counts": counts,
        "families": results,
    }


def write_markdown(report: dict, path: Path) -> None:
    lines = [
        "# Specialization Conformance",
        "",
        f"Result: **{report['result']}**",
        "",
        "| Family | Specialized | Outcome | Reason | Entry delta |",
        "|---|---|---|---|---:|",
    ]
    for item in report["families"]:
        lines.append(
            "| {family} | {specialized} | {outcome} | {reason} | {delta} |".format(
                family=item["family"],
                specialized="yes" if item["specialized_proven"] else "no",
                outcome=item["outcome"],
                reason=item["diagnostic"]["reason"] or "-",
                delta=item["machine_entries_delta"],
            )
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args(argv)
    report = run(args.manifest)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    if args.markdown:
        write_markdown(report, args.markdown)
    print(json.dumps(report["counts"], sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
