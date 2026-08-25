"""A nested function bound nowhere is anchored on the containing caller."""
import gc

# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, creations, emit, observe
import cinderjit



def outer():
    def make(k):
        def adder(x, y):
            total = x - x
            i = total
            while i < y:
                total = total + x + k
                i = i + 1
            return total
        return adder
    flags = []
    for n in range(6):
        f = make(n)
        for _ in range(T + 1):
            f(2, 3)
        flags.append(cinderjit.is_jit_compiled(f))
        del f
        gc.collect()
    return flags, "__cinderx_nested_compiled_funcs__" in outer.__dict__


flags, anchored = outer()
emit(flags=flags, anchored=anchored, creations=creations(),
     attachments=observe()["fresh_attachments"])
