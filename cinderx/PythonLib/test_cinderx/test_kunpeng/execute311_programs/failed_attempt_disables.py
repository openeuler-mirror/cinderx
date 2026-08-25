"""A refused attempt is a permanent verdict on the code object."""
# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, emit, entries, observe
import cinderjit
from cinderx.jit import jit_suppress



def factory(k):
    @jit_suppress
    def held(x, y):
        total = x - x
        i = total
        while i < y:
            total = total + x + k
            i = i + 1
        return total
    return held


def held_events():
    return [e for e in observe()["events"]
            if e["qualname"] == "factory.<locals>.held"]


disabled_before = observe()["auto_jit_disabled_codes"]
first = factory(1)
for _ in range(T + 5):
    first(2, 3)
after_first = held_events()
disabled_after_first = observe()["auto_jit_disabled_codes"]
second = factory(2)
for _ in range(T + 5):
    second(2, 3)
stats = observe()
# The explicit path: lift the suppression and compile by hand.
cinderjit.jit_unsuppress(second)
forced = cinderjit.force_compile(second)
before = entries()
second(2, 3)
emit(
    after_first=after_first,
    after_second=held_events(),
    attachments=stats["fresh_attachments"],
    disabled=disabled_after_first - disabled_before,
    disabled_second=stats["auto_jit_disabled_codes"] - disabled_after_first,
    first_compiled=cinderjit.is_jit_compiled(first),
    forced=forced,
    forced_entered=entries() - before,
)
