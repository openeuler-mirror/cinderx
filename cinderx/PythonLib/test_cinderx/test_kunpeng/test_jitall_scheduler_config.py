import json
import os
from pathlib import Path
import subprocess
import sys


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


def _run(tmp_path, **updates):
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
    return subprocess.run(
        [sys.executable, "-c", CHILD],
        cwd=tmp_path,
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
    )


def _payload(completed):
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout.strip().splitlines()[-1])


def test_default_scheduler_does_not_arm_explicit_autojit_policy(tmp_path):
    payload = _payload(_run(tmp_path))
    assert payload["shared_threshold"] is None
    assert payload["observe_threshold"] == 50
    assert payload["threshold_source"] == "shared-jit-config"


def test_jitall_only_resolves_zero_in_shared_and_scheduler_config(tmp_path):
    payload = _payload(_run(tmp_path, PYTHONJITALL="1"))
    assert payload["shared_threshold"] == 0
    assert payload["observe_threshold"] == 0
    assert payload["threshold_source"] == "shared-jit-config"
    assert payload["machine_entry_delta"] > 0


def test_jitauto_only_resolves_same_numeric_threshold(tmp_path):
    payload = _payload(_run(tmp_path, PYTHONJITAUTO="1"))
    assert payload["shared_threshold"] == 1
    assert payload["observe_threshold"] == 1
    assert payload["threshold_source"] == "shared-jit-config"


def test_jitauto_overrides_jitall_in_flag_processor_order(tmp_path):
    payload = _payload(_run(tmp_path, PYTHONJITALL="1", PYTHONJITAUTO="7"))
    assert payload["shared_threshold"] == 7
    assert payload["observe_threshold"] == 7


def test_jit_disable_turns_execute_mode_off(tmp_path):
    payload = _payload(_run(tmp_path, PYTHONJITALL="1", PYTHONJITDISABLE="1"))
    assert payload["observe_enabled"] is False
    assert payload["observe_mode"] == "off"
    assert payload["cinderjit_loaded"] is False
    assert payload["machine_entry_delta"] == 0


def test_invalid_jitauto_fails_closed(tmp_path):
    completed = _run(tmp_path, PYTHONJITAUTO="not-a-threshold")
    assert completed.returncode != 0
    assert "invalid PYTHONJITAUTO" in completed.stderr


def test_auto_classifier_fails_closed_on_311(tmp_path):
    completed = _run(tmp_path, PYTHONJITAUTO="auto")
    assert completed.returncode != 0
    assert "classification is not supported" in completed.stderr
