"""A disable() landing inside the attach publication must keep machine
code shut: the entry predicate answers on the state NOW."""
from _harness import T, emit, entries, observe
from _fixtures import SWEEP, charge, cinderjit, factory, uncharge

seed = factory(0)
for i in range(T * 2):
    assert seed(i, 2) == i * 2
assert cinderjit.is_jit_compiled(seed)

landed, entered = [], []
attachments = observe()["fresh_attachments"]
for k in range(SWEEP):
    cinderjit.enable()
    uncharge()
    fresh = factory(0)
    charge(k)
    # First call over compiled code: the frame that attaches, with the
    # collector aimed inside it.
    assert fresh(3, 2) == 6
    uncharge()
    if cinderjit.is_enabled():
        continue
    landed.append(k)
    before = entries()
    for i in range(20):
        assert fresh(i, 2) == i * 2
    if entries() != before:
        entered.append((k, entries() - before))
cinderjit.enable()
before = entries()
fresh = factory(0)
for i in range(20):
    assert fresh(i, 2) == i * 2
emit(
    landed=landed,
    entered=entered,
    attached=observe()["fresh_attachments"] - attachments,
    resumed=entries() - before,
)
