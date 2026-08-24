#!/usr/bin/env python3.11
"""Minimal canary execution set, for running under AddressSanitizer.

The green-family RuntimeTests instrument the compiler but never enter
machine code.  This script exercises what only executing code reaches: the
prologue, the materialized interpreter frame, normal and exception
returns, the deopt return path that hands the entry frame back, a frame
that escapes through a traceback, and finalization with compiled functions
still live.

It reports the machine-code entry count so the caller can fail closed:
a run that never entered machine code is not sanitizer evidence for any
of the above.
"""

import gc
import sys

import _cinderx
import cinderx

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit  # noqa: E402  (import order is the point)

CALLS = 512


def hot(a, b, one):
    total = a - a
    i = total
    while i < b:
        total = total + a
        if total > b * b:
            total = total - b
        i = i + one
    return total


def raiser(x, none):
    return x + none


def interpreted_oracle(a, b, one):
    total = a - a
    i = total
    while i < b:
        total = total + a
        if total > b * b:
            total = total - b
        i = i + one
    return total


# Computed at import time, before the frame evaluator is doing anything for
# these functions and before any counting begins.
EXPECTED = [interpreted_oracle(i, 9, 1) for i in range(7)]


def main() -> int:
    assert cinderjit.is_attr_caches_enabled() is True
    assert cinderjit.force_compile(hot) is True
    assert cinderjit.force_compile(raiser) is True

    def entries() -> int:
        return _cinderx._get_trigger_stats()["machine_code_entries"]


    # The oracle is computed up front, before any entry counting starts, and
    # never called inside the measured loop: its body is also inside the
    # execute surface, so calling it there would add entries of its own and
    # let a hot() that never ran compiled still satisfy the count.
    expected = [EXPECTED[i % 7] for i in range(CALLS)]
    normal_before = entries()
    for i in range(CALLS):
        assert hot(i % 7, 9, 1) == expected[i], i
    normal_entries = entries() - normal_before

    # Exception returns, and frames that escape through a traceback and are
    # read after the machine-code call has already returned.
    escaped = []
    payload = object()
    raised = 0
    raising_before = entries()
    for i in range(CALLS):
        try:
            raiser(payload, None)
        except TypeError:
            raised += 1
            exc = sys.exc_info()[1]
            if i % 64 == 0:
                tb = exc.__traceback__
                while tb.tb_next is not None:
                    tb = tb.tb_next
                escaped.append(tb.tb_frame)
    raising_entries = entries() - raising_before
    gc.collect()
    escaped_count = len(escaped)
    for frame in escaped:
        assert frame.f_code.co_name == "raiser"
        assert frame.f_locals["x"] is payload
    del escaped

    # Each sub-path proves its own work.  Without this, a raiser() that
    # silently stopped raising -- or stopped being compiled -- would leave
    # the exception and frame-escape coverage empty while the normal path's
    # entries washed the whole run green.
    failures = []
    if normal_entries < CALLS:
        failures.append(f"normal path entered machine code {normal_entries} "
                        f"times, expected {CALLS}")
    if raised != CALLS:
        failures.append(f"raising path raised {raised} times, expected {CALLS}")
    # The deopt budget withdraws machine code after a fixed number of
    # exception exits, so the raising path is not expected to stay compiled
    # for the whole loop -- but it must have executed some of it.
    if raising_entries <= 0:
        failures.append("raising path never entered machine code")
    if escaped_count != 8:
        failures.append(f"collected {escaped_count} escaped frames, expected 8")
    if failures:
        for failure in failures:
            print(f"asan canary: {failure}", file=sys.stderr)
        return 1
    gc.collect()
    print(
        f"asan canary entries={normal_entries + raising_entries} "
        f"normal={normal_entries} raising={raising_entries} "
        f"raised={raised} escaped=8"
    )
    # Return with compiled functions still live so finalization tears them
    # down under the sanitizer.
    return 0


if __name__ == "__main__":
    sys.exit(main())
