"""staticmethod/classmethod factories anchor their closures (FACTORY_KIND)."""
import gc
import os

# cinderjit exists only once the harness has initialized CinderX.
from _harness import T, creations, emit
import cinderjit


KIND = os.environ["FACTORY_KIND"]

if KIND == "staticmethod":
    class C:
        @staticmethod
        def factory(*args):
            k = args[-1]

            def hot(x, y):
                t = x - x
                i = t
                while i < y:
                    t = t + x + k
                    i = i + 1
                return t
            return hot
else:
    class C:
        @classmethod
        def factory(*args):
            k = args[-1]

            def hot(x, y):
                t = x - x
                i = t
                while i < y:
                    t = t + x + k
                    i = i + 1
                return t
            return hot

resident = cinderjit._get_resident_compiled_functions
base = resident()
first = C.factory(1)
for _ in range(T + 1):
    first(2, 3)
first_compiled = cinderjit.is_jit_compiled(first)
del first
gc.collect()
resident_after_death = resident() - base

second = C.factory(2)
second(2, 3)
second(2, 3)
emit(
    first_compiled=first_compiled,
    resident_after_death=resident_after_death,
    second_compiled=cinderjit.is_jit_compiled(second),
    creations=creations(),
    values_ok=second(2, 3) == 3 * (2 + 2),
)
