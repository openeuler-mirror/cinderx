"""A refused child import must not change an installed main evaluator."""

import os
import _cinderx as cx
import _xxsubinterpreters as sub

before = cx._attr_path_status()
assert before["global_cache"] and cx.is_frame_evaluator_installed()
mode = os.environ.pop("ATTR_PATH_MODE")
try:
    child = sub.create()
    try:
        try:
            sub.run_string(child, "import _cinderx")
        except sub.RunFailedError as error:
            assert "interpreter" in str(error).lower(), error
        else:
            raise AssertionError("subinterpreter import unexpectedly succeeded")
    finally:
        sub.destroy(child)
finally:
    os.environ["ATTR_PATH_MODE"] = mode

assert cx.is_frame_evaluator_installed() and cx._attr_path_status() == before
cx.install_frame_evaluator()
assert cx._attr_path_status() == before
