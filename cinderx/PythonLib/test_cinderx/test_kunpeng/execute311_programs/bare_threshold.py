"""A threshold alone, with no mode: the interpreter-only default."""
from _harness import T, emit, observe, trigger
from _fixtures import cinderjit, hot

for i in range(T * 3):
    hot(i, 2)
emit(trigger=trigger(), stats=observe(), cinderjit=cinderjit is not None)
