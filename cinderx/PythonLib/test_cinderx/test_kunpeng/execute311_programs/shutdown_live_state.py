"""Exit with compiled functions, attachments, a suspended generator and a
parked (uncompiled) function all alive."""
from _harness import T, emit, observe
from _fixtures import cinderjit, factory, hot

kept = [factory(n) for n in range(6)]
for f in kept:
    for _ in range(T + 1):
        f(2, 3)
for _ in range(T + 1):
    hot(2, 3)


def gen(n):
    i = 0
    while i < n:
        yield i
        i = i + 1


assert cinderjit.force_compile(gen) is True
suspended = gen(5)
next(suspended)
# A parked function: uncompiled, its artifact retired.
assert cinderjit.force_uncompile(hot) is True
emit(
    live=len(cinderjit.get_compiled_functions()),
    attachments=observe()["fresh_attachments"],
)
