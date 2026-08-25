"""A suspended generator resumes the frame it already has: counts and the
single attempt belong to the code that actually runs."""
# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, emit, entries, observe
import cinderjit



def gen(n):
    i = 0
    while i < n:
        yield i
        i += 1


def replacement(n):
    total = n - n
    i = total
    while i < n:
        total = total + 1
        i = i + 1
    return total


# A shape the automatic surface refuses, so the second half can tell "no
# attempt yet" from "attempt already spent".
def refused(n):
    class C:
        pass
    return C


GEN_CODE = gen.__code__


def resumes_only(target):
    gen.__code__ = GEN_CODE
    g = gen(T * 6)
    next(g)               # the frame now holds gen's code
    gen.__code__ = target.__code__
    for _ in range(T + 10):
        next(g)           # resumes that frame, nothing else
    return [e for e in observe()["events"] if e["qualname"] == target.__name__]


installable = resumes_only(replacement)
compiled_from_resumes = cinderjit.is_jit_compiled(gen)

gen.__code__ = refused.__code__
refusable = resumes_only(refused)

# Only now does the replaced code actually run.
gen.__code__ = replacement.__code__
before = entries()
for _ in range(T * 2):
    assert gen(4) == 4
emit(
    installable=installable,
    refusable=refusable,
    compiled_from_resumes=compiled_from_resumes,
    compiled_after_real_calls=cinderjit.is_jit_compiled(gen),
    entered=entries() - before,
    events_after=[
        e["result"] for e in observe()["events"]
        if e["qualname"] == "replacement"
    ],
)
