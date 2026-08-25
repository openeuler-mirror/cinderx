"""The twin refusal lasts exactly as long as the artifact has an owner."""
import types

from _harness import T, emit, entries
from _fixtures import cinderjit, hot

for i in range(T + 1):
    hot(i, 2)
assert cinderjit.is_jit_compiled(hot)

twin = types.FunctionType(hot.__code__, {"__builtins__": __builtins__}, "twin")
refused = None
try:
    cinderjit.force_compile(twin)
except RuntimeError as exc:
    refused = str(exc)

# Retire the first owner, then the twin may compile.
assert cinderjit.force_uncompile(hot) is True
forced = cinderjit.force_compile(twin)
before = entries()
assert twin(2, 3) == 6
emit(
    refused=refused,
    forced=forced,
    compiled=cinderjit.is_jit_compiled(twin),
    entered=entries() - before,
)
