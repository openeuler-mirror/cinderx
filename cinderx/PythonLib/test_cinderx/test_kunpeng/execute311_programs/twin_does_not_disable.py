"""The twin's refusal is about the function, not the code: same-namespace
siblings still attach."""
import types

from _harness import T, emit, entries, observe

SRC = """\
def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total
"""

import cinderjit

own = {"__builtins__": __builtins__}
exec(compile(SRC, "<twin>", "exec"), own, own)
first = own["hot"]
code = first.__code__
assert cinderjit.force_compile(first) is True

# The twin: same code, foreign globals, driven to the threshold so the
# scheduler spends the attempt on it.
foreign = {"__builtins__": __builtins__}
twin = types.FunctionType(code, foreign, "hot")
foreign["hot"] = twin
for i in range(T * 2):
    assert twin(i, 2) == i * 2
twin_verdicts = [
    e["result"] for e in observe()["events"] if e["qualname"] == "hot"
]

# An instance that shares the artifact's namespace.
sibling = types.FunctionType(code, own, "hot")
own["sibling"] = sibling
for i in range(T * 2):
    assert sibling(i, 2) == i * 2
before = entries()
for i in range(10):
    assert sibling(i, 2) == i * 2
emit(
    twin_verdicts=twin_verdicts,
    twin_compiled=cinderjit.is_jit_compiled(twin),
    sibling_attached=cinderjit.is_jit_compiled(sibling),
    sibling_ran=entries() - before,
)
