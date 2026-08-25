"""Explicit compilation is never budgeted, and attaches, not recompiles."""
from _harness import T, creations, emit, entries, observe
from _fixtures import cinderjit, factory

first = factory(1)
for _ in range(T + 1):
    first(2, 3)
fresh = factory(2)
fresh(2, 3)
fresh(2, 3)
auto_attached = cinderjit.is_jit_compiled(fresh)
forced = cinderjit.force_compile(fresh)
before = entries()
fresh(2, 3)
emit(
    auto_attached=auto_attached,
    forced=forced,
    compiled=cinderjit.is_jit_compiled(fresh),
    entered=entries() - before,
    creations=creations(),
    attachments=observe()["fresh_attachments"],
)
