"""Observation is invisible to gc.get_objects() and weakref.getweakrefs();
the JIT-off arm is the oracle."""
import gc
import weakref

from _harness import T, emit, observe


def target(a, b):
    return a + b


for i in range(T * 2):
    assert target(i, 2) == i + 2

ref = weakref.ref(target.__code__)
emit(
    listed=weakref.getweakrefcount(target.__code__),
    tracked=gc.is_tracked(ref),
    censused=any(obj is ref for obj in gc.get_objects()),
    # CPython's cache: asking twice gives the same object.
    cached=(weakref.ref(target.__code__) is ref),
    # Only code-object weakrefs, so the death watch and type watchers
    # stay out of an observer-specific measurement.
    code_refs=sum(
        1 for obj in gc.get_objects()
        if type(obj) is weakref.ref
        and type(obj()) is type(target.__code__)
    ),
    observed=observe()["codes_seen"],
)
