"""Suppression must cover the attach door too, not only the first dispatch
(SUPPRESS_SCOPE env)."""
import os

import _cinderx

from _harness import T, emit, entries, observe
from _fixtures import cinderjit, factory

SCOPE = os.environ["SUPPRESS_SCOPE"]
enter = getattr(_cinderx, "_autojit_%s_enter" % SCOPE)
leave = getattr(_cinderx, "_autojit_%s_leave" % SCOPE)

# Steady state reached outside any scope, so the code object is
# dispatched and attachable.
seed = factory(0)
for i in range(T * 2):
    assert seed(i, 2) == i * 2
assert cinderjit.is_jit_compiled(seed)
charged = observe()["fresh_attachments"]

enter()
inside = factory(0)
for i in range(T):
    assert inside(i, 2) == i * 2
b = entries()
for i in range(10):
    assert inside(i, 2) == i * 2
inside_ran = entries() - b
inside_attached = cinderjit.is_jit_compiled(inside)
inside_charged = observe()["fresh_attachments"] - charged
leave()

for i in range(T):
    assert inside(i, 2) == i * 2
b = entries()
for i in range(10):
    assert inside(i, 2) == i * 2
emit(
    inside_attached=inside_attached,
    inside_ran=inside_ran,
    inside_charged=inside_charged,
    after_attached=cinderjit.is_jit_compiled(inside),
    after_ran=entries() - b,
)
