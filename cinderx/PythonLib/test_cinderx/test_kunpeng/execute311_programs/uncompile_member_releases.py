"""Uncompiling one member retires the artifact, anchors included, for all."""
from _harness import creations, emit
from _fixtures import cinderjit, factory

resident = cinderjit._get_resident_compiled_functions

base = resident()
members = [factory(n) for n in range(3)]
for f in members:
    assert cinderjit.force_compile(f) is True
assert all(cinderjit.is_jit_compiled(f) for f in members)
anchored_before = ["__cinderx_compiled_func__" in f.__dict__ for f in members]
resident_compiled = resident() - base

assert cinderjit.force_uncompile(members[1]) is True
emit(
    anchored_before=anchored_before,
    resident_compiled=resident_compiled,
    creations=creations(),
    compiled_after=[cinderjit.is_jit_compiled(f) for f in members],
    anchored_after=["__cinderx_compiled_func__" in f.__dict__ for f in members],
    resident_after=resident() - base,
    values_ok=[f(2, 3) for f in members] == [3 * (2 + n) for n in range(3)],
)
