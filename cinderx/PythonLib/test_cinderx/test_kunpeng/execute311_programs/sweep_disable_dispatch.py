"""A disable() landing inside the dispatch never spends the attempt; one
fresh code object per landing point sweeps the whole window."""
from _harness import T, emit, observe
from _fixtures import SWEEP, charge, cinderjit, uncharge

SRC = """\
def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total
"""

deferred, lost = [], []
seen = observe()["late_deferrals"]
for k in range(SWEEP):
    ns = {}
    exec(compile(SRC, "<hot%d>" % k, "exec"), globals(), ns)
    hot = ns["hot"]
    # Reachable from a namespace, like any real function: this is what
    # the outer-function walk looks for.
    globals()["hot%d" % k] = hot
    cinderjit.enable()
    uncharge()
    for i in range(T - 1):
        assert hot(i, 2) == i * 2
    charge(k)
    # The frame that crosses the threshold, with the collector aimed
    # inside it.
    assert hot(3, 2) == 6
    uncharge()
    cinderjit.enable()
    now = observe()["late_deferrals"]
    if now != seen:
        deferred.append(k)
    seen = now
    # The attempt has to still be there to spend.
    for i in range(T * 2):
        assert hot(i, 2) == i * 2
    if not cinderjit.is_jit_compiled(hot):
        lost.append(k)
emit(deferred=deferred, lost=lost)
