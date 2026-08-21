"""Sync generators are not auto-compiled; explicit compilation works."""
# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, emit, entries, observe
import cinderjit



def gen(n):
    i = 0
    while i < n:
        yield i
        i = i + 1


disabled_before = observe()["auto_jit_disabled_codes"]
for _ in range(T + 3):
    assert list(gen(3)) == [0, 1, 2]
disabled_after = observe()["auto_jit_disabled_codes"]
forced = cinderjit.force_compile(gen)
before = entries()
assert list(gen(3)) == [0, 1, 2]
emit(
    events=[e for e in observe()["events"] if e["qualname"] == "gen"],
    disabled=disabled_after - disabled_before,
    forced=forced,
    entered=entries() - before,
)
