"""The artifact is anchored by the outer; instances may die around it."""
import gc
import sys
import types

# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, creations, emit, observe
import cinderjit


FACTORY_SRC = """\
def factory(k):
    def adder(x, y):
        total = x - x
        i = total
        while i < y:
            total = total + x + k
            i = i + 1
        return total
    return adder
"""

module = types.ModuleType("fresh_owner")
exec(FACTORY_SRC, module.__dict__)
sys.modules["fresh_owner"] = module
resident = cinderjit._get_resident_compiled_functions
base = resident()
first = module.factory(1)
for _ in range(T + 1):
    first(2, 3)
assert cinderjit.is_jit_compiled(first)
artifact_id = id(first.__dict__["__cinderx_compiled_func__"])
anchors = module.factory.__dict__.get("__cinderx_nested_compiled_funcs__", [])
anchored = any(id(a) == artifact_id for a in anchors)
del anchors
del first
gc.collect()
# The compiled instance is gone; the outer keeps the machine code resident.
resident_after_death = resident() - base

second = module.factory(2)
second(2, 3)
second(2, 3)
second_compiled = cinderjit.is_jit_compiled(second)
same = id(second.__dict__.get("__cinderx_compiled_func__")) == artifact_id
del second
gc.collect()
resident_after_second = resident() - base

del module.factory
del sys.modules["fresh_owner"]
del module
gc.collect()
emit(
    anchored=anchored,
    resident_after_death=resident_after_death,
    second_compiled=second_compiled,
    same=same,
    resident_after_second=resident_after_second,
    creations=creations(),
    resident=resident() - base,
    attachments=observe()["fresh_attachments"],
)
