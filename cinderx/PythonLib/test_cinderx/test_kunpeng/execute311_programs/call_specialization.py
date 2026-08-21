"""The interpreted caller keeps stock CALL specialization; the compiled
callee is entered through its vectorcall entry on every call."""
import dis

from _harness import emit, entries
from _fixtures import cinderjit, hot


def caller(n, xs):
    acc = 0
    for i in range(n):
        acc = acc + hot(i, 3) + len(xs)
    return acc


assert cinderjit.force_compile(hot) is True
before = entries()
value = caller(200, [1, 2, 3])
ops = sorted({
    i.opname for i in dis.get_instructions(caller, adaptive=True)
    if i.opname.startswith(("CALL", "PRECALL"))
})
emit(
    value=value,
    entered=entries() - before,
    caller_compiled=cinderjit.is_jit_compiled(caller),
    ops=ops,
)
