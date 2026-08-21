"""The ledger keeps copies: holding co_filename/co_qualname would show in
sys.getrefcount() from the observed program."""
import sys

from _harness import T, emit, observe

SRC = """\
def probe_fn(a, b):
    return a + b
"""

ns = {}
exec(compile(SRC, "<ledger-probe>", "exec"), ns, ns)
fn = ns["probe_fn"]
filename = fn.__code__.co_filename
qualname = fn.__code__.co_qualname

before = (sys.getrefcount(filename), sys.getrefcount(qualname))
for i in range(T * 2):
    assert fn(i, 2) == i + 2
after = (sys.getrefcount(filename), sys.getrefcount(qualname))
emit(
    delta=[after[0] - before[0], after[1] - before[1]],
    # The names still come back, so this is not passing by recording
    # nothing.
    recorded=[
        (e["qualname"], e["filename"]) for e in observe()["events"]
        if e["qualname"] == "probe_fn"
    ],
)
