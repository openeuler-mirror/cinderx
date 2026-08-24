"""Subprocess matrix for the CPython 3.11 shared/scheduler JIT threshold."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


CHILD = r"""
import json
import sys
import _cinderx
import cinderx
cinderx.init()
_cinderx.install_frame_evaluator()
def target(value):
    return value + 1
before = _cinderx._get_trigger_stats()["machine_code_entries"]
assert target(1) == 2
assert target(2) == 3
after = _cinderx._get_trigger_stats()["machine_code_entries"]
observe = _cinderx._get_observe_stats()
cinderjit = sys.modules.get("cinderjit")
print(json.dumps({
    "observe_enabled": observe["enabled"],
    "observe_mode": observe["mode"],
    "observe_threshold": observe["threshold"],
    "threshold_source": observe.get("threshold_source"),
    "shared_threshold": (
        cinderjit._jit311_config_state()["compile_after_n_calls"]
        if cinderjit is not None else None
    ),
    "cinderjit_loaded": cinderjit is not None,
    "machine_entry_delta": after - before,
}, sort_keys=True))
"""


def _environment(updates: dict[str, str]) -> dict[str, str]:
    import _cinderx
    import cinderx

    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("CINDERX_", "PYTHONJIT"))
        and key not in {"PYTHONPATH", "PYTHONHOME"}
    }
    runtime_roots = {
        str(Path(_cinderx.__file__).resolve().parent),
        str(Path(cinderx.__file__).resolve().parent.parent),
    }
    env["PYTHONPATH"] = os.pathsep.join(sorted(runtime_roots))
    env["CINDERX_JIT_MODE"] = "canary"
    env.update(updates)
    return env


def _run_case(cwd: Path, updates: dict[str, str]) -> dict:
    completed = subprocess.run(
        [sys.executable, "-c", CHILD],
        cwd=cwd,
        env=_environment(updates),
        text=True,
        capture_output=True,
        timeout=60,
    )
    payload = None
    if completed.returncode == 0 and completed.stdout.strip():
        payload = json.loads(completed.stdout.strip().splitlines()[-1])
    return {
        "environment": updates,
        "returncode": completed.returncode,
        "payload": payload,
        "stderr": completed.stderr,
    }


def run() -> dict:
    specs = {
        "default_canary": ({}, None, 50),
        "jitall_only": ({"PYTHONJITALL": "1"}, 0, 0),
        "jitauto_only": ({"PYTHONJITAUTO": "1"}, 1, 1),
        "jitauto_overrides_jitall": (
            {"PYTHONJITALL": "1", "PYTHONJITAUTO": "7"},
            7,
            7,
        ),
        "jit_disabled": (
            {"PYTHONJITALL": "1", "PYTHONJITDISABLE": "1"},
            None,
            None,
        ),
        "invalid_jitauto": (
            {"PYTHONJITAUTO": "not-a-threshold"},
            None,
            None,
        ),
        "unsupported_auto_classifier": (
            {"PYTHONJITAUTO": "auto"},
            None,
            None,
        ),
    }
    rows = {}
    errors = []
    with tempfile.TemporaryDirectory() as temporary:
        cwd = Path(temporary)
        for name, (env, expected_shared, expected_observe) in specs.items():
            row = _run_case(cwd, env)
            rows[name] = row
            payload = row["payload"] or {}
            if name in {"invalid_jitauto", "unsupported_auto_classifier"}:
                expected_message = (
                    "classification is not supported"
                    if name == "unsupported_auto_classifier"
                    else "invalid PYTHONJITAUTO"
                )
                if (
                    row["returncode"] == 0
                    or expected_message not in row["stderr"]
                ):
                    errors.append(f"{name} did not fail closed")
            elif name == "jit_disabled":
                if (
                    row["returncode"] != 0
                    or payload.get("observe_mode") != "off"
                    or payload.get("machine_entry_delta") != 0
                ):
                    errors.append("JIT disable did not dominate execute mode")
            elif (
                row["returncode"] != 0
                or payload.get("shared_threshold") != expected_shared
                or payload.get("observe_threshold") != expected_observe
                or payload.get("threshold_source") != "shared-jit-config"
            ):
                errors.append(f"{name} shared/scheduler threshold mismatch")
    return {
        "result": "PASS" if not errors else "FAIL",
        "cases": rows,
        "errors": errors,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = run()
    args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": result["result"]}, sort_keys=True))
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
