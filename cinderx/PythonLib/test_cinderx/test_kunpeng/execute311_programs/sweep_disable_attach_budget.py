"""A publication a disable() lands inside still charges the budget."""
from _harness import T, emit, entries
from _fixtures import SWEEP, charge, cinderjit, nested_factory, uncharge

landed, over = [], []
for k in range(SWEEP):
    make = nested_factory(k)
    cinderjit.enable()
    uncharge()
    seed = make()
    for i in range(T * 2):
        assert seed(i, 2) == i * 2
    if not cinderjit.is_jit_compiled(seed):
        continue
    first = make()
    charge(k)
    assert first(3, 2) == 6
    uncharge()
    if cinderjit.is_enabled():
        continue
    landed.append(k)
    cinderjit.enable()
    before = entries()
    for i in range(20):
        assert first(i, 2) == i * 2
    first_runs = entries() - before
    # The one attachment the budget allows is spent; a second instance
    # must stay interpreted.
    second = make()
    for i in range(T * 2):
        assert second(i, 2) == i * 2
    before = entries()
    for i in range(20):
        assert second(i, 2) == i * 2
    second_runs = entries() - before
    if first_runs and second_runs:
        over.append((k, first_runs, second_runs))
emit(landed=landed, over=over)
