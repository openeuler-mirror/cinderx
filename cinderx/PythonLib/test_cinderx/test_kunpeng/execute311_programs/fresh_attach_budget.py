"""Fresh instances attach within the per-code budget, then interpret."""
from _harness import T, creations, emit, entries, observe
from _fixtures import cinderjit, factory

kept = []
flags = []
for n in range(14):
    f = factory(n)
    f(2, 3)
    flags.append(cinderjit.is_jit_compiled(f))
    kept.append(f)
for _ in range(T + 1):
    kept[0](2, 3)
compiled_first = cinderjit.is_jit_compiled(kept[0])
later = []
for f in kept[1:]:
    f(2, 3)
    f(2, 3)
    later.append(cinderjit.is_jit_compiled(f))
stats = observe()
before = entries()
for f in kept:
    assert f(2, 3) == 3 * (2 + kept.index(f))
emit(
    compiled_first=compiled_first,
    later=later,
    attachments=stats["fresh_attachments"],
    creations=creations(),
    entered=entries() - before,
    same_artifact=len({
        id(f.__dict__.get("__cinderx_compiled_func__"))
        for f in kept if cinderjit.is_jit_compiled(f)
    }),
)
