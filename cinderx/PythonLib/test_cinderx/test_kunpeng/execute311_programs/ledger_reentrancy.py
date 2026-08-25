"""A finalizer appending to a full event ledger must not move the array
under the report that is walking it."""
import gc

from _harness import T, emit, observe

SRC = """\
def f(a, b):
    return a + b
"""


def heat(tag):
    ns = {}
    exec(compile(SRC, "<%s>" % tag, "exec"), globals(), ns)
    globals()["k_%s" % tag] = ns["f"]
    for i in range(T + 2):
        assert ns["f"](i, 2) == i + 2


# 1024 is the ledger's initial capacity; stop exactly on it.
n = 0
while len(observe()["events"]) < 1024:
    heat("fill%d" % n)
    n += 1
before = len(observe()["events"])


class Bomb:
    def __init__(self):
        self.loop = self

    def __del__(self):
        # Runs inside the report, from one of its allocations.
        heat("bomb")


gc.disable()
Bomb()
gc.set_threshold(gc.get_count()[0] + 3, 10, 10)
gc.enable()
snapshot = observe()
gc.set_threshold(700, 10, 10)

emit(
    before=before,
    reported=len(snapshot["events"]),
    malformed=sum(
        1 for e in snapshot["events"]
        if not isinstance(e.get("count"), int)
        or not isinstance(e.get("qualname"), str)
        or e.get("result") is None
    ),
    bomb_ran=any(
        e["filename"].endswith("<bomb>") for e in observe()["events"]
    ),
)
