"""Churn fills the table with tombstones; growth must compact, not double.
Each round holds its code objects alive together so addresses cannot be
recycled."""
import gc

from _harness import T, emit, observe

SRC = """\
def f(a, b):
    return a + b
"""


def burn(n):
    live = []
    for k in range(n):
        ns = {}
        exec(compile(SRC, "<t%d>" % k, "exec"), ns, ns)
        live.append(ns["f"])
        for i in range(T + 5):
            assert ns["f"](i, 2) == i + 2
    return live


rounds = []
for _ in range(6):
    live = burn(700)
    peak = observe()
    del live
    gc.collect()
    rest = observe()
    rounds.append((peak["watched_codes"], peak["table_capacity"],
                   rest["watched_codes"], rest["table_capacity"],
                   rest["codes_seen"]))
emit(rounds=rounds)
