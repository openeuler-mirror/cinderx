"""Execution-smoke product smoke and negative control."""

from __future__ import annotations

import argparse
import asyncio
import json
from pathlib import Path


def hot(value, limit):
    total = value - value
    index = total
    while index < limit:
        total += value
        index += 1
    return total


async def unsupported(value):
    return value + 1


def run(expect_mode: str) -> dict:
    import _cinderx
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()

    before = _cinderx._get_trigger_stats()
    for value in range(80 if expect_mode == "execute" else 1200):
        expected = value * 3 if expect_mode == "execute" else value * 3
        assert hot(value, 3) == expected
    after = _cinderx._get_trigger_stats()
    observe = _cinderx._get_observe_stats()
    creation_delta = after["compiled_function_creations"] - before["compiled_function_creations"]
    entry_delta = after["machine_code_entries"] - before["machine_code_entries"]
    allocation_delta = after["executable_alloc_calls"] - before["executable_alloc_calls"]

    refusal = None
    compiled = False
    if expect_mode == "execute":
        import cinderjit

        diagnostic = cinderjit._jit311_compile_diagnostic(unsupported)
        refusal = diagnostic["reason"]
        assert asyncio.run(unsupported(4)) == 5
        compiled = cinderjit.is_jit_compiled(hot)
        passed = (
            observe["mode"] == "execute"
            and observe["threshold"] == 50
            and compiled
            and creation_delta > 0
            and allocation_delta > 0
            and entry_delta > 0
            and refusal == "REFUSE_SHAPE_ASYNC_CODE"
        )
    else:
        passed = (
            observe["mode"] == "off"
            and creation_delta == 0
            and allocation_delta == 0
            and entry_delta == 0
        )
    return {
        "result": "PASS" if passed else "FAIL",
        "expected_mode": expect_mode,
        "mode": observe["mode"],
        "threshold": observe.get("threshold"),
        "compiled": compiled,
        "compiled_function_creations_delta": creation_delta,
        "executable_alloc_calls_delta": allocation_delta,
        "machine_code_entries_delta": entry_delta,
        "async_refusal": refusal,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect-mode", choices=("off", "execute"), required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run(args.expect_mode)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
