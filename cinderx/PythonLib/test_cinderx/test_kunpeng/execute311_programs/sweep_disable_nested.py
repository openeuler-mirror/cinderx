"""The same sweep over the nested shape: the earliest landing points
precede the compile entry point."""
from _harness import T, emit, observe
from _fixtures import SWEEP, charge, cinderjit, nested_factory, uncharge

deferred, lost = [], []
seen = observe()["late_deferrals"]
for k in range(SWEEP):
    make = nested_factory(k)
    cinderjit.enable()
    uncharge()
    fn = make()
    for i in range(T - 1):
        assert fn(i, 2) == i * 2
    charge(k)
    assert fn(3, 2) == 6
    uncharge()
    cinderjit.enable()
    now = observe()["late_deferrals"]
    if now != seen:
        deferred.append(k)
    seen = now
    for i in range(T * 2):
        assert fn(i, 2) == i * 2
    if not cinderjit.is_jit_compiled(fn):
        lost.append(k)
emit(deferred=deferred, lost=lost)
