"""An instance over an artifact the scheduler never dispatched must still
go through the per-code attachment budget."""
from _harness import T, emit, entries
from _fixtures import cinderjit, factory

seed = factory(1)
assert cinderjit.force_compile(seed) is True
assert cinderjit.is_jit_compiled(seed)

fresh = factory(2)
for i in range(T * 2):
    fresh(i, 2)
before = entries()
for i in range(10):
    fresh(i, 2)
emit(
    attached=cinderjit.is_jit_compiled(fresh),
    ran=entries() - before,
)
