# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared plumbing for the execute-mode child programs.

Importing this module initializes CinderX and installs the frame
evaluator, exactly as the first lines of every program used to.  The
helpers are bound to builtin methods so they never appear in the counters
they read.
"""

import json
import os

import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()

trigger = _cinderx._get_trigger_stats
observe = _cinderx._get_observe_stats

# The scheduling threshold the parent configured (PYTHONJITAUTO); 50 is
# the scheduler's own default for an unset variable.
T = int(os.environ.get("PYTHONJITAUTO", "50"))


def entries():
    return trigger()["machine_code_entries"]


def creations():
    return trigger()["compiled_function_creations"]


def events(name):
    return [e for e in observe()["events"] if e["qualname"].endswith(name)]


def emit(**payload):
    print("JOURNAL " + json.dumps(payload))
