"""A paused JIT and an active trace both defer attachment, spending nothing."""
import sys

from _harness import T, creations, emit, observe
from _fixtures import cinderjit, factory

first = factory(1)
for _ in range(T + 1):
    first(2, 3)
assert cinderjit.is_jit_compiled(first)

cinderjit.disable()
paused = factory(2)
paused(2, 3)
paused(2, 3)
paused_attached = cinderjit.is_jit_compiled(paused)
cinderjit.enable()
paused(2, 3)
resumed_attached = cinderjit.is_jit_compiled(paused)

traced = factory(3)
sys.settrace(lambda *a: None)
traced(2, 3)
traced(2, 3)
sys.settrace(None)
traced_attached = cinderjit.is_jit_compiled(traced)
traced(2, 3)
after_trace_attached = cinderjit.is_jit_compiled(traced)
emit(
    paused_attached=paused_attached,
    resumed_attached=resumed_attached,
    traced_attached=traced_attached,
    after_trace_attached=after_trace_attached,
    attachments=observe()["fresh_attachments"],
    creations=creations(),
)
