"""The twin spends the dispatch but records no verdict on the code."""
import types

from _harness import T, emit, entries, observe
from _fixtures import cinderjit, hot

owner = hot
assert cinderjit.force_compile(owner) is True
twin = types.FunctionType(owner.__code__, {"__builtins__": __builtins__}, "twin")


def code_events():
    return [e["result"] for e in observe()["events"] if e["qualname"] == "hot"]


# Process-wide counter: measure the window, not the total.
disabled_before = observe()["auto_jit_disabled_codes"]
for _ in range(T + 1):
    twin(2, 3)
refused = code_events()
disabled = observe()["auto_jit_disabled_codes"] - disabled_before

# Remove what the refusal was about, then keep the twin hot for a long
# time: the attempt was spent, so nothing reschedules.
assert cinderjit.force_uncompile(owner) is True
before = entries()
for _ in range(T * 8):
    assert twin(2, 3) == 6
emit(
    refused=refused,
    after=code_events(),
    disabled=disabled,
    compiled=cinderjit.is_jit_compiled(twin),
    entered=entries() - before,
)
