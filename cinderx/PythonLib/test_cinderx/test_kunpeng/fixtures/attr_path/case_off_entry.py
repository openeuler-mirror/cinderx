# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Qualification uses frozen capability, never a paused JIT or new env text."""
import os
import sys

mode, flag = sys.argv[1:]
for key in ("CINDERX_JIT_DISABLE", "PYTHONJITDISABLE", "ATTR_PATH_MODE"):
    os.environ.pop(key, None)
os.environ.update(CINDERX_JIT_MODE=mode, CINDERX_INTERP_OFF_ENTRY=flag,
                  CINDERX_JIT_STARTUP_GRACE_MS="0", PYTHONJITAUTO="2")
import cinderx
cinderx.init()
import _cinderx
from cinderx import jit as cinderjit
_cinderx.install_frame_evaluator()
expected = mode == "off" and flag == "1"
assert _cinderx._interp_off_entry_enabled() == expected

def callee(value):
    return value + 1

def caller():
    return sum(callee(i) for i in range(300))

for _ in range(10):
    assert caller() == 45150
if mode == "execute":
    assert cinderjit.is_jit_compiled(callee)
    assert _cinderx._get_trigger_stats()["machine_code_entries"] > 0
    cinderjit.disable()
    assert not _cinderx._interp_off_entry_enabled()
elif mode == "off":
    assert not any(_cinderx._get_trigger_stats().values())
    try:
        cinderjit.enable()
    except RuntimeError:
        pass
    assert not cinderjit.is_enabled()
    assert not cinderjit.is_jit_compiled(callee)
# Altering environment cannot change the already configured mode/qualification.
os.environ["CINDERX_JIT_MODE"] = "execute" if mode == "off" else "off"
assert _cinderx._interp_off_entry_enabled() == expected
_cinderx.remove_frame_evaluator()
assert not _cinderx._interp_off_entry_enabled()
if mode == "off":
    _cinderx.install_frame_evaluator()
    assert _cinderx._interp_off_entry_enabled() == expected
