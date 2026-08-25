"""Tracing / a paused JIT never spend the attempt (TRANSIENT_CASE env)."""
import os
import sys

from _harness import T, emit, entries, events, observe
from _fixtures import cinderjit, hot

CASE = os.environ["TRANSIENT_CASE"]
# Process-wide counter: measure the window, not the total.
disabled_before = observe()["auto_jit_disabled_codes"]
if CASE == "trace":
    sys.settrace(lambda *a: None)
else:
    cinderjit.disable()
for i in range(T * 3):
    hot(i, 2)
held = [e["result"] for e in events("hot")]
disabled_during = observe()["auto_jit_disabled_codes"] - disabled_before
if CASE == "trace":
    sys.settrace(None)
else:
    cinderjit.enable()
assert hot(3, 2) == 6
after = events("hot")
before = entries()
assert hot(3, 2) == 6
emit(
    held=held,
    disabled_during=disabled_during,
    after_results=[e["result"] for e in after],
    after_count=after[0]["count"] if after else None,
    compiled=cinderjit.is_jit_compiled(hot),
    entered=entries() - before,
)
