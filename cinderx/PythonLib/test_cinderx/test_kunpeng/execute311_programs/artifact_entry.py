# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Exercise the artifact-specific guarded-entry publication protocol."""

import sys

from _fixtures import cinderjit
from _harness import emit, entries


def target(callback=None):
    value = 40
    if callback is not None:
        callback()
    return value + 2


assert cinderjit.force_compile(target) is True
before = entries()
first = target()


def unpublish_from_body():
    assert cinderjit.force_uncompile(target) is True


# The body removes its own function anchor while its generated code is on the
# stack.  The static guarded entry must retain the artifact until this call has
# returned to static text.
reentrant = target(unpublish_from_body)
compiled_after_reentrant = cinderjit.is_jit_compiled(target)
interpreted_after_reentrant = target()

assert cinderjit.force_compile(target) is True


def replacement(callback=None):
    return 99


target.__code__ = replacement.__code__
swapped = target()
compiled_after_swap = cinderjit.is_jit_compiled(target)


def traced(value):
    return value + 1


assert cinderjit.force_compile(traced) is True
trace_events = []


def trace(frame, event, arg):
    if frame.f_code is traced.__code__:
        trace_events.append(event)
    return trace


trace_before = entries()
sys.settrace(trace)
try:
    traced_value = traced(4)
finally:
    sys.settrace(None)
trace_entry_delta = entries() - trace_before

emit(
    first=first,
    reentrant=reentrant,
    compiled_after_reentrant=compiled_after_reentrant,
    interpreted_after_reentrant=interpreted_after_reentrant,
    swapped=swapped,
    compiled_after_swap=compiled_after_swap,
    traced_value=traced_value,
    trace_events=trace_events,
    trace_entry_delta=trace_entry_delta,
    entry_delta=entries() - before,
)
