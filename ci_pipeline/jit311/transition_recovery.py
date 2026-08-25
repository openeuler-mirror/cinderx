"""100-cycle transition-recovery stress."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


GUARD_VALUE = 10


def guard_function(value):
    return GUARD_VALUE + value


def instrumentation_function(value):
    return (value + 1) * 2


class Dynamic:
    def method(self):
        return 1


def attribute_function(obj):
    return obj.method()


def code_a(value):
    return value + 1


def code_b(value):
    return value + 2


def generator_inner():
    value = yield "ready"
    return value


def generator_outer():
    return (yield from generator_inner())


def ensure_auto(cinderjit, function, invoke) -> None:
    for _ in range(30):
        invoke()
        if cinderjit.is_jit_compiled(function):
            return
    raise AssertionError(f"Auto-JIT did not compile {function.__qualname__}")


def run(cycles: int) -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()
    before = dict(_cinderx._get_trigger_stats())
    rows = []

    global GUARD_VALUE
    ensure_auto(cinderjit, guard_function, lambda: guard_function(1))
    semantic_failures = state_failures = 0
    for index in range(cycles):
        GUARD_VALUE = 10 if index % 2 == 0 else 2.5
        expected = GUARD_VALUE + 1
        if guard_function(1) != expected:
            semantic_failures += 1
        if not cinderjit.is_enabled():
            state_failures += 1
    GUARD_VALUE = 10
    rows.append(
        {
            "transition": "guard-miss",
            "cycles": cycles,
            "semantic_failures": semantic_failures,
            "state_failures": state_failures,
        }
    )

    ensure_auto(
        cinderjit,
        instrumentation_function,
        lambda: instrumentation_function(3),
    )
    for kind in ("trace", "profile"):
        semantic_failures = state_failures = 0

        def callback(frame, event, arg):
            return callback if kind == "trace" else None

        for _ in range(cycles):
            if kind == "trace":
                sys.settrace(callback)
            else:
                sys.setprofile(callback)
            if instrumentation_function(3) != 8:
                semantic_failures += 1
            if cinderjit.is_enabled():
                state_failures += 1
            if kind == "trace":
                sys.settrace(None)
            else:
                sys.setprofile(None)
            before_entry = _cinderx._get_trigger_stats()["machine_code_entries"]
            if instrumentation_function(3) != 8:
                semantic_failures += 1
            if (
                not cinderjit.is_enabled()
                or _cinderx._get_trigger_stats()["machine_code_entries"] <= before_entry
            ):
                state_failures += 1
        rows.append(
            {
                "transition": kind + "-attach-detach",
                "cycles": cycles,
                "semantic_failures": semantic_failures,
                "state_failures": state_failures,
            }
        )

    original_code = code_a.__code__
    semantic_failures = state_failures = 0
    policy_reasons: dict[str, int] = {}
    ensure_auto(cinderjit, code_a, lambda: code_a(1))
    for index in range(cycles):
        code_a.__code__ = code_b.__code__ if index % 2 else original_code
        expected = index % 2 and 3 or 2
        value = code_a(1)
        if value != expected:
            semantic_failures += 1
        for _ in range(10):
            code_a(1)
            if cinderjit.is_jit_compiled(code_a):
                break
        state = cinderjit._jit311_code_state(code_a)
        reason = str(state["policy_reason"])
        policy_reasons[reason] = policy_reasons.get(reason, 0) + 1
        if not state["installed"] and reason not in {
            "code-verdict-final",
            "automatic-attempt-spent-artifact-retired",
            "existing-member-not-fresh-attachable",
            "fresh-attach-budget-exhausted",
        }:
            state_failures += 1
    code_a.__code__ = original_code
    rows.append(
        {
            "transition": "code-swap-recompile",
            "cycles": cycles,
            "semantic_failures": semantic_failures,
            "state_failures": state_failures,
            "policy_reasons": policy_reasons,
        }
    )

    obj = Dynamic()
    original_method = Dynamic.method
    ensure_auto(cinderjit, attribute_function, lambda: attribute_function(obj))
    semantic_failures = state_failures = 0
    for index in range(cycles):
        expected = index % 2 + 1
        Dynamic.method = original_method if expected == 1 else lambda self: 2
        if attribute_function(obj) != expected:
            semantic_failures += 1
        if not cinderjit.is_enabled():
            state_failures += 1
    Dynamic.method = original_method
    rows.append(
        {
            "transition": "attribute-mutation",
            "cycles": cycles,
            "semantic_failures": semantic_failures,
            "state_failures": state_failures,
        }
    )

    cinderjit._jit311_compile_diagnostic(generator_inner)
    cinderjit._jit311_compile_diagnostic(generator_outer)
    semantic_failures = state_failures = 0

    def tracer(frame, event, arg):
        return tracer

    for index in range(cycles):
        generator = generator_outer()
        if next(generator) != "ready":
            semantic_failures += 1
        sys.settrace(tracer)
        try:
            generator.send(index)
        except StopIteration as exc:
            if exc.value != index:
                semantic_failures += 1
        else:
            semantic_failures += 1
        finally:
            sys.settrace(None)
        if not cinderjit.is_enabled():
            state_failures += 1
    rows.append(
        {
            "transition": "generator-suspend-deopt-resume",
            "cycles": cycles,
            "semantic_failures": semantic_failures,
            "state_failures": state_failures,
        }
    )

    after = dict(_cinderx._get_trigger_stats())
    final_jit_usable = bool(cinderjit.is_enabled())
    passed = (
        all(
            row["semantic_failures"] == 0 and row["state_failures"] == 0 for row in rows
        )
        and final_jit_usable
    )
    return {
        "result": "PASS" if passed else "FAIL",
        "cycles": cycles,
        "transitions": rows,
        "final_jit_usable": final_jit_usable,
        "stats_before": before,
        "stats_after": after,
        "stats_delta": {
            key: int(after[key]) - int(before.get(key, 0))
            for key in after
            if isinstance(after[key], int)
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=100)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    report = run(args.cycles)
    args.out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(
        json.dumps({"result": report["result"], "cycles": args.cycles}, sort_keys=True)
    )
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
