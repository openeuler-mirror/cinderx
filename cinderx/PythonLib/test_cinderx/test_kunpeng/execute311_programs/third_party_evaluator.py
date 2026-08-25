"""Another PEP 523 client takes the slot; the JIT degrades safely."""
import ctypes

import _cinderx

from _harness import T, emit, entries, events
from _fixtures import cinderjit, hot

assert cinderjit.force_compile(hot) is True
api = ctypes.pythonapi
api.PyInterpreterState_Get.restype = ctypes.c_void_p
api._PyInterpreterState_SetEvalFrameFunc.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p]
interp = api.PyInterpreterState_Get()
stock = ctypes.cast(api._PyEval_EvalFrameDefault, ctypes.c_void_p).value

before = entries()
assert hot(2, 3) == 6
entered_ours = entries() - before

# The stock evaluator stands in for the other client: nothing runs
# compiled, nothing crashes, and we do not write our pointer back.
api._PyInterpreterState_SetEvalFrameFunc(interp, stock)
installed_foreign = _cinderx.is_frame_evaluator_installed()
compiled_foreign = cinderjit.is_jit_compiled(hot)
before = entries()
values = [hot(i, 3) for i in range(T + 5)]
entered_foreign = entries() - before
foreign_events = len(events("hot"))
removal = None
try:
    _cinderx.remove_frame_evaluator()
except RuntimeError as exc:
    removal = str(exc)

# They give the slot back; machine code resumes from the same artifact.
_cinderx.install_frame_evaluator()
before = entries()
assert hot(2, 3) == 6
emit(
    entered_ours=entered_ours,
    installed_foreign=installed_foreign,
    compiled_foreign=compiled_foreign,
    values_ok=values == [i * 3 for i in range(T + 5)],
    entered_foreign=entered_foreign,
    foreign_events=foreign_events,
    removal=removal,
    installed_again=_cinderx.is_frame_evaluator_installed(),
    compiled_again=cinderjit.is_jit_compiled(hot),
    entered_again=entries() - before,
)
