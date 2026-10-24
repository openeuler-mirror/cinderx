# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Sync generators are auto-compiled once they cross the threshold; their
bodies then run as machine code on every resume."""

from _harness import T, emit, entries, observe
import cinderjit


def gen(n):
    i = 0
    while i < n:
        yield i
        i = i + 1


disabled_before = observe()["auto_jit_disabled_codes"]
before = entries()
for _ in range(T + 3):
    assert list(gen(3)) == [0, 1, 2]
disabled_after = observe()["auto_jit_disabled_codes"]
emit(
    events=[e for e in observe()["events"] if e["qualname"] == "gen"],
    disabled=disabled_after - disabled_before,
    compiled=cinderjit.is_jit_compiled(gen),
    entered=entries() - before,
)
