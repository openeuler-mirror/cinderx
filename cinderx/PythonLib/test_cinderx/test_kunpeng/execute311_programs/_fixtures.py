# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared workload shapes for the execute-mode child programs."""

import gc

import _harness  # noqa: F401  (initializes CinderX before anything runs)

try:
    import cinderjit
except ImportError:  # off/observe/shadow: the module does not exist
    cinderjit = None


def hot(a, b):
    total = a - a
    i = total
    while i < b:
        total = total + a
        i = i + 1
    return total


def factory(k):
    def adder(x, y):
        total = x - x
        i = total
        while i < y:
            total = total + x + k
            i = i + 1
        return total
    return adder


class Bomb:
    # An unreachable cycle carrying a finalizer: only the collector can
    # free it, so the disable() lands wherever the collector runs rather
    # than between two of the test's own statements.
    def __init__(self):
        self.loop = self

    def __del__(self):
        cinderjit.disable()


def charge(k):
    # Aim the collector at the k-th tracked allocation of whatever runs
    # next; sweeping k sweeps every point the decision can land on.
    gc.disable()
    Bomb()
    gc.set_threshold(gc.get_count()[0] + k, 10, 10)
    gc.enable()


def uncharge():
    gc.set_threshold(700, 10, 10)


# Wide enough to cover every tracked allocation the scheduler makes.
SWEEP = 64

NESTED_SRC = """\
def outer():
    def inner(a, b):
        total = a - a
        i = total
        while i < b:
            total = total + a
            i = i + 1
        return total
    return inner
"""


def nested_factory(k):
    # The shape whose scheduling registers an outer function -- the code
    # path the sweeps are aimed at.
    ns = {}
    exec(compile(NESTED_SRC, "<nested%d>" % k, "exec"), globals(), ns)
    globals()["outer%d" % k] = ns["outer"]
    return ns["outer"]
