"""A function re-created per call: one compile, budgeted attaches, no storm."""
import gc

from _harness import T, creations, emit, events, observe
from _fixtures import cinderjit, factory

compiled_flags = []
for n in range(200):
    f = factory(n)
    for _ in range(T + 2):
        assert f(2, 3) == 3 * (2 + n)
    compiled_flags.append(cinderjit.is_jit_compiled(f))
    del f
gc.collect()
stats = observe()
emit(
    events=events("adder"),
    creations=creations(),
    attachments=stats["fresh_attachments"],
    compiled_flags=compiled_flags,
    live=len(cinderjit.get_compiled_functions()),
)
