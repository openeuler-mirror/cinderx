# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Exercise the CPython 3.11 materialized-frame localsplus write-through."""

import sys

from _fixtures import cinderjit
from _harness import emit, entries


events = []


class Watch:
    def __del__(self):
        caller = sys._getframe(1)
        events.append(caller.f_locals.get("slot"))


def observe_store(value):
    frame = sys._getframe()
    local = value
    return frame.f_locals.get("local")


def observe_delete():
    local = 19
    frame = sys._getframe()
    del local
    return "local" in frame.f_locals


def observe_reentrant_release():
    frame = sys._getframe()
    slot = Watch()
    slot = "new"
    return frame.f_locals.get("slot")


functions = (observe_store, observe_delete, observe_reentrant_release)
assert all(cinderjit.force_compile(fn) is True for fn in functions)

before = entries()
stored = observe_store(17)
deleted = observe_delete()
replaced = observe_reentrant_release()

emit(
    compiled={fn.__name__: cinderjit.is_jit_compiled(fn) for fn in functions},
    stored=stored,
    deleted=deleted,
    replaced=replaced,
    events=events,
    entry_delta=entries() - before,
)
