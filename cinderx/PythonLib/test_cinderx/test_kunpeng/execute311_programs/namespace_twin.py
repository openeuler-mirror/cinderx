"""A twin over foreign globals never attaches to the published artifact."""
import types

from _harness import T, creations, emit, entries, observe
from _fixtures import cinderjit, hot

for i in range(T + 1):
    hot(i, 2)
assert cinderjit.is_jit_compiled(hot)
twin = types.FunctionType(hot.__code__, {"__builtins__": __builtins__}, "twin")
for _ in range(5):
    assert twin(2, 3) == 6
refused = None
try:
    cinderjit.force_compile(twin)
except RuntimeError as exc:
    refused = str(exc)
before = entries()
twin(2, 3)
emit(
    twin_compiled=cinderjit.is_jit_compiled(twin),
    refused=refused,
    attachments=observe()["fresh_attachments"],
    entered=entries() - before,
    creations=creations(),
)
