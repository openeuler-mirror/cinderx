"""200 calls under whatever threshold the parent configured."""
import _cinderx

from _harness import emit, observe, trigger
from _fixtures import cinderjit, hot

# Bracket with counter reads: at a low threshold the import machinery
# compiles too and enters machine code of its own.
before = trigger()["machine_code_entries"]
values = [hot(i, 2) for i in range(200)]
hot_entries = trigger()["machine_code_entries"] - before
stats = observe()
emit(
    values_ok=values == [i * 2 for i in range(200)],
    threshold=stats["threshold"],
    events=[e for e in stats["events"] if e["qualname"] == "hot"],
    entries=hot_entries,
    installed=_cinderx.is_frame_evaluator_installed(),
    mode=stats["mode"],
    compiled=cinderjit is not None and cinderjit.is_jit_compiled(hot),
)
