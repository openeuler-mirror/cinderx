"""A __code__ move inside the attempt withholds it and never burns it,
swept over every landing point."""
import gc

from _harness import T, emit, entries
from _fixtures import SWEEP, cinderjit, nested_factory, uncharge

SWAP_SRC = """\
def swapped_in(a, b):
    return a * b + 1
"""

moved, burned, ran = [], [], []
for k in range(SWEEP):
    make = nested_factory(k)
    ns = {}
    exec(compile(SWAP_SRC, "<swap%d>" % k, "exec"), globals(), ns)
    globals()["swap%d" % k] = ns["swapped_in"]
    target = ns["swapped_in"]

    cinderjit.enable()
    uncharge()
    fn = make()
    for i in range(T - 1):
        assert fn(i, 2) == i * 2

    class Bomb:
        def __init__(self):
            self.loop = self

        def __del__(self):
            fn.__code__ = target.__code__

    gc.disable()
    Bomb()
    gc.set_threshold(gc.get_count()[0] + k, 10, 10)
    gc.enable()
    fn(3, 2)
    uncharge()
    if fn.__code__ is not target.__code__:
        continue          # the charge missed the attempt
    moved.append(k)

    # The code that never ran a frame must not have been compiled: it
    # would run machine code if it had.
    before = entries()
    assert fn(2, 3) == 7
    if entries() != before:
        ran.append(k)

    # And the attempt was withheld, not spent: put the code that earned
    # it back and let it run for real.
    fn.__code__ = make().__code__
    for i in range(T * 3):
        assert fn(i, 2) == i * 2
    if not cinderjit.is_jit_compiled(fn):
        burned.append(k)
emit(moved=moved, burned=burned, ran=ran)
