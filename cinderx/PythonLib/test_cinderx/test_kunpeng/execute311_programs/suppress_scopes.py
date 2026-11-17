"""Inside an import/setup scope (SUPPRESS_SCOPE env) nothing dispatches,
nothing is spent and nothing is counted; after the scope closes the code needs
the full threshold of its own calls before it dispatches."""
import os

import _cinderx

from _harness import T, emit, entries, observe
from _fixtures import cinderjit, hot

SCOPE = os.environ["SUPPRESS_SCOPE"]
enter = getattr(_cinderx, "_autojit_%s_enter" % SCOPE)
leave = getattr(_cinderx, "_autojit_%s_leave" % SCOPE)

before = observe()["auto_jit_disabled_codes"]
enter()
for i in range(T * 3):
    assert hot(i, 2) == i * 2
inside = observe()
inside_compiled = cinderjit.is_jit_compiled(hot)
leave()

for _ in range(T - 1):
    assert hot(3, 2) == 6
below = observe()
assert hot(3, 2) == 6
after = observe()
b = entries()
assert hot(3, 2) == 6
emit(
    # From the snapshot taken before leave(), not the live table after.
    inside_events=[
        e["result"] for e in inside["events"] if e["qualname"] == "hot"
    ],
    inside_compiled=inside_compiled,
    spent=inside["auto_jit_disabled_codes"] - before,
    below_events=[
        e["result"] for e in below["events"] if e["qualname"] == "hot"
    ],
    after_events=[
        e["result"] for e in after["events"] if e["qualname"] == "hot"
    ],
    compiled=cinderjit.is_jit_compiled(hot),
    entered=entries() - b,
)
