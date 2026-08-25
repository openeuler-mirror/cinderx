"""Entries are retired by the death notice; the table never has to grow."""
import gc

from _harness import T, emit, observe

SRC = """\
def f(a, b):
    return a + b
"""


def burn(n):
    ns = {}
    for k in range(n):
        exec(compile(SRC, "<burn%d>" % k, "exec"), ns, ns)
        for i in range(T + 5):
            assert ns["f"](i, 2) == i + 2


burn(50)
gc.collect()
base = observe()
rounds = []
for _ in range(4):
    burn(200)
    gc.collect()
    now = observe()
    rounds.append((now["watched_codes"], now["table_capacity"],
                   now["codes_seen"]))
emit(base=[base["watched_codes"], base["table_capacity"],
           base["codes_seen"]], rounds=rounds)
