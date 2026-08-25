"""Minimal capability prerequisite for the CPython 3.11 lifecycle runners."""

from __future__ import annotations

import argparse
import gc
import json
from pathlib import Path
import weakref


def run() -> dict:
    import _cinderx
    import cinderjit
    import cinderx

    cinderx.init()
    _cinderx.install_frame_evaluator()

    def loop(a, b, one):
        total = a - a
        index = total
        while index < b:
            total += a
            index += one
        return total

    before = _cinderx._get_trigger_stats()
    assert cinderjit.force_compile(loop) is True
    assert loop(3, 5, 1) == 15
    assert cinderjit.force_uncompile(loop) is True
    assert not cinderjit.is_jit_compiled(loop)
    assert loop(3, 5, 1) == 15
    assert cinderjit.force_compile(loop) is True
    assert loop(3, 5, 1) == 15
    after = _cinderx._get_trigger_stats()

    def make_fresh():
        namespace = {"__builtins__": __builtins__, "__name__": "__main__"}
        exec("def fresh(value):\n    return value + 1\n", namespace, namespace)
        return namespace["fresh"], namespace

    fresh, namespace = make_fresh()
    reference = weakref.ref(fresh)
    assert cinderjit.force_compile(fresh) is True
    assert fresh(1) == 2
    del fresh, namespace
    gc.collect()
    gc.collect()

    checks = {
        "frame_evaluator_installed": _cinderx.is_frame_evaluator_installed(),
        "jit_entry": after["machine_code_entries"] > before["machine_code_entries"],
        "deopt_detach_and_recompile": cinderjit.is_jit_compiled(loop),
        "function_death": reference() is None,
        "invariants": dict(cinderjit._jit311_lifecycle_invariants()),
    }
    passed = all(
        value is True
        for key, value in checks.items()
        if key != "invariants"
    ) and checks["invariants"].get("ok") is True
    return {"result": "PASS" if passed else "FAIL", "checks": checks}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = run()
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": result["result"], "checks": result["checks"]}, sort_keys=True))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
