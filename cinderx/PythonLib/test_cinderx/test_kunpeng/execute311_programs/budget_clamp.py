"""An attach budget beyond the 16-bit counter is clamped, and still caps."""
from _harness import emit, observe
from _fixtures import cinderjit, factory

kept = []
for n in range(12):
    f = factory(n)
    f(2, 3)
    f(2, 3)
    kept.append(cinderjit.is_jit_compiled(f))
emit(kept=kept, attachments=observe()["fresh_attachments"])
