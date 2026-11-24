"""A refused child import must not freeze the main interpreter's first configuration."""

import os
import sys
import _xxsubinterpreters as sub

assert "_cinderx" not in sys.modules
child = sub.create()
os.environ.update(ATTR_PATH_MODE="cinderx", ATTR_PATH_DIAGNOSTICS="1")
try:
    try:
        sub.run_string(child, "import _cinderx")
    except sub.RunFailedError as error:
        assert "subinterpreter" in str(error).lower(), error
    else:
        raise AssertionError("child native import unexpectedly succeeded")
finally:
    sub.destroy(child)

os.environ["ATTR_PATH_MODE"] = "cinderx_control"
del os.environ["ATTR_PATH_DIAGNOSTICS"]
import _cinderx as cx

cx.install_frame_evaluator()
status = cx._attr_path_status()
assert cx._get_observe_stats()["mode"] == "off"
assert status["no_exception"] == status["diagnostics"] == status["specialization"] == 0
assert not any(cx._get_trigger_stats().values())
