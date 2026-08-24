"""Build sanity for the refleak leg: prove the debug build CAN run the
JIT.  This is NOT the trigger proof -- that comes from the -R processes
themselves (see refleak_attest_sitecustomize.py)."""
import sys

import _cinderx
import cinderx

assert hasattr(sys, "gettotalrefcount"), "not a Py_DEBUG interpreter"
cinderx.init()
_cinderx.install_frame_evaluator()
assert _cinderx.is_frame_evaluator_installed(), "evaluator did not install"
import cinderjit  # noqa: E402


def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total


for i in range(200):
    hot(i, 2)
assert cinderjit.is_jit_compiled(hot), "nothing compiled in the refleak arm"
stats = _cinderx._get_trigger_stats()
assert stats["machine_code_entries"] > 0, "nothing entered machine code"
print("refleak arm executes: entries=%d creations=%d"
      % (stats["machine_code_entries"], stats["compiled_function_creations"]))
