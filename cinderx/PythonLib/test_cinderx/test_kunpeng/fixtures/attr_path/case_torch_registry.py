# Copyright (c) Meta Platforms, Inc. and affiliates.
"""SC05-S1 with a mocked Torch module and the same attribute call site."""

import json
import os
from pathlib import Path
import sys

from _cinderx_auto import attr_status


arm = sys.argv[1]
expected_state = "registered" if arm == "registry-on" else "disabled"
before_import = attr_status()
assert before_import["torch_registry_state"] == (
    "pending" if arm == "registry-on" else "disabled"
)
assert "torch" not in sys.modules

mock_root = Path(__file__).resolve().with_name("torch_mock")
sys.path.insert(0, str(mock_root))
import torch

assert Path(torch.__file__).resolve().is_relative_to(mock_root)
module_status = attr_status()
assert module_status["torch_registry_state"] == expected_state
assert module_status["evaluator"] == (arm != "stock")
cx = sys.modules.get("_cinderx")
if cx is None:
    assert arm == "stock"
else:
    assert cx._get_observe_stats()["mode"] == "off"
    assert module_status["attribute"]["no_exception"] == 1
    assert bool(module_status["attribute"]["torch_registry_requested"]) == (arm == "registry-on")


class Subject(torch.nn.Module):
    pass


def read(subject):
    return subject.entry


def hits():
    return cx._attr_path_counters()["torch_registry_hits"] if cx else 0


subject = Subject()
entry = object()
subject.entry = entry
for _ in range(2_000):
    assert read(subject) is entry

trace = []
c_hits = {}


def check_value(stage, value):
    before = hits()
    assert read(subject) is value
    c_hits[stage] = hits() - before
    trace.append(stage)


def remove_and_check(stage):
    del subject.entry
    before = hits()
    try:
        read(subject)
    except AttributeError as error:
        assert error.name == "entry" and error.obj is subject
        trace.append([stage, type(error).__name__, str(error)])
    else:
        raise AssertionError((stage, "deleted attribute was still present"))
    c_hits[stage] = hits() - before
    registries = (vars(subject), subject._buffers, subject._parameters, subject._modules)
    assert not any("entry" in registry for registry in registries)


check_value("ordinary", entry)
remove_and_check("after_ordinary")
buffer = object()
subject.register_buffer("entry", buffer)
check_value("buffer", buffer)
parameter = torch.nn.Parameter()
subject.entry = parameter
assert "entry" not in subject._buffers
check_value("parameter", parameter)
remove_and_check("after_parameter")
child = torch.nn.Module()
subject.add_module("entry", child)
check_value("submodule", child)
remove_and_check("after_submodule")

for stage, count in c_hits.items():
    expected = int(arm == "registry-on" and stage in {"buffer", "parameter", "submodule"})
    assert count == expected, (arm, stage, count)
final = attr_status()
jit = final.get("jit")
assert final["torch_registry_state"] == expected_state
assert (jit is None if arm == "stock" else bool(jit) and not any(jit.values()))
result = dict(arm=arm, torch="mock", state=expected_state,
              trace=trace, c_hits=c_hits, jit=jit,
              affinity=sorted(os.sched_getaffinity(0)))
Path(os.environ["ATTR_PATH_TEST_OUTPUT"]).write_text(json.dumps(result), encoding="utf-8")
