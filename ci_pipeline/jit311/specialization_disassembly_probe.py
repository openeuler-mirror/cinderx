"""Prove the four approved test_dis differences are adaptive-only."""

from __future__ import annotations

import argparse
import dis
import json
from pathlib import Path


def control_load(x, y=0):
    a, b = x, y
    return a, b


def control_loop():
    for value in [1, 2, 3] * 3:
        control_load(value)


def _opnames(function) -> list[str]:
    return [
        instruction.opname
        for instruction in dis.get_instructions(function, adaptive=True)
    ]


def _type_error(function) -> dict:
    try:
        function()
    except TypeError as exc:
        message = str(exc).replace(function.__name__, "<function>")
        return {"type": type(exc).__name__, "message": message}
    raise AssertionError("missing required-argument TypeError")


def run() -> dict:
    import _cinderx
    import cinderjit
    import cinderx
    from test import test_dis

    cinderx.init()
    _cinderx.install_frame_evaluator()
    from cinderx.jit import jit_suppress

    jit_suppress(control_load)
    jit_suppress(control_loop)

    load_diag = cinderjit._jit311_compile_diagnostic(test_dis.load_test)
    loop_diag = cinderjit._jit311_compile_diagnostic(test_dis.loop_test)
    before = _cinderx._get_trigger_stats()["machine_code_entries"]
    jit_load_result = test_dis.load_test(4, 5)
    jit_loop_result = test_dis.loop_test()
    jit_type_error = _type_error(test_dis.load_test)
    entry_delta = _cinderx._get_trigger_stats()["machine_code_entries"] - before

    # Warm equivalent interpreted functions below Auto-JIT's threshold so
    # CPython's adaptive forms appear without CinderX publication.
    for _ in range(300):
        assert control_load(4, 5) == (4, 5)
        assert control_loop() is None
    control_type_error = _type_error(control_load)

    jit_load_ops = _opnames(test_dis.load_test)
    jit_loop_ops = _opnames(test_dis.loop_test)
    control_load_ops = _opnames(control_load)
    control_loop_ops = _opnames(control_loop)
    checks = {
        "jit_machine_entry": entry_delta >= 2,
        "semantic_results_equal": jit_load_result == control_load(4, 5)
        and jit_loop_result == control_loop(),
        "exceptions_equal": jit_type_error == control_type_error,
        "jit_load_stays_generic": "LOAD_FAST__LOAD_FAST" not in jit_load_ops,
        "control_load_quickens": "LOAD_FAST__LOAD_FAST" in control_load_ops,
        "jit_loop_stays_generic": "JUMP_BACKWARD_QUICK" not in jit_loop_ops,
        "control_loop_quickens": "JUMP_BACKWARD_QUICK" in control_loop_ops,
    }
    return {
        "result": "PASS" if all(checks.values()) else "FAIL",
        "checks": checks,
        "diagnostics": {"load_test": load_diag, "loop_test": loop_diag},
        "machine_entries_delta": entry_delta,
        "semantics": {
            "jit_load": jit_load_result,
            "jit_loop": jit_loop_result,
            "jit_type_error": jit_type_error,
            "control_type_error": control_type_error,
        },
        "opcodes": {
            "jit_load": jit_load_ops,
            "jit_loop": jit_loop_ops,
            "control_load": control_load_ops,
            "control_loop": control_loop_ops,
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run()
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": report["result"], **report["checks"]}, sort_keys=True))
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
