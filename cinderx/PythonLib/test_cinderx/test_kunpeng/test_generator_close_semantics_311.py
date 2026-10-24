# Copyright (c) Meta Platforms, Inc. and affiliates.
"""Close and delegation semantics of compiled CPython 3.11 generators that
the in-place fast close and the yield-from delegation chain must keep: a
re-entry from a __del__ while the generator is being closed raises
"generator already executing"; a delegating generator cannot be consumed
again while its chain is live; a finalizer leaves the caller's in-flight
exception untouched; a sub-iterator whose close() raises finishes the
delegating generator the way gen_close does."""

import os
import subprocess
import sys
import textwrap
import unittest


PREAMBLE = """
import _cinderx
import cinderx
import sys

cinderx.init()
_cinderx.install_frame_evaluator()
import cinderjit
"""


# The live value released by close() carries a __del__ that re-enters the
# generator.  The interpreter answers ValueError("generator already
# executing") because gen_close runs with the frame marked executing; the
# body must not resume, and the second close() must be a no-op.
CLOSE_REENTRY_PROBE = PREAMBLE + """
events = []


def sink(*args):
    events.append("body resumed")


class Token:
    def __del__(self):
        try:
            next(g)
        except ValueError as exc:
            events.append("next: " + str(exc))
        except StopIteration:
            events.append("next: StopIteration")
        try:
            g.close()
        except ValueError as exc:
            events.append("close: " + str(exc))


def outer():
    sink(Token(), (yield 1))


assert cinderjit.force_compile(outer) is True
g = outer()
assert next(g) == 1
assert cinderjit.is_jit_compiled(outer)
g.close()
assert events == [
    "next: generator already executing",
    "close: generator already executing",
], events
try:
    next(g)
except StopIteration:
    pass
else:
    raise AssertionError("closed generator resumed")
g.close()

# The same window through the object a sub-iterator's close() returns.
events2 = []


class Returned:
    def __del__(self):
        try:
            next(h)
        except ValueError as exc:
            events2.append("next: " + str(exc))
        except StopIteration:
            events2.append("next: StopIteration")


class Sub:
    def __iter__(self):
        return self

    def __next__(self):
        return 7

    def close(self):
        return Returned()


def delegator():
    yield from Sub()


assert cinderjit.force_compile(delegator) is True
h = delegator()
assert next(h) == 7
h.close()
assert events2 == ["next: generator already executing"], events2
print("close reentry ok")
"""


# outer delegates to middle, middle to leaf; while the chain is live the
# leaf body iterates middle again from a compiled loop.  The interpreter
# raises ValueError("generator already executing"); the outer loop must
# then continue with the chain intact and produce the full sequence.
DELEGATION_REENTRY_PROBE = PREAMBLE + """
seen = []
middle_obj = None


def leaf():
    yield 1
    try:
        for _ in middle_obj:
            seen.append("re-entered")
    except ValueError as exc:
        seen.append(str(exc))
    yield 2
    yield 3


def middle():
    yield from leaf()
    yield 4


def outer():
    yield from middle()
    yield 5


def consume(gen):
    out = []
    for value in gen:
        out.append(value)
    return out


for fn in (leaf, middle, outer, consume):
    assert cinderjit.force_compile(fn) is True

g = outer()
# Reach the point where all three are suspended; gi_yieldfrom hands out the
# suspended delegation chain's next level.
assert next(g) == 1
middle_obj = g.gi_yieldfrom
assert middle_obj is not None
rest = consume(g)
assert rest == [2, 3, 4, 5], rest
assert seen == ["generator already executing"], seen

# Same shape again with fresh instances, consumed by the compiled loop.
seen.clear()
g2 = outer()
assert next(g2) == 1
middle_obj = g2.gi_yieldfrom
assert consume(g2) == [2, 3, 4, 5]
assert seen == ["generator already executing"], seen
print("delegation reentry ok")
"""


# A started delegating generator whose sub-iterator's close() raises is
# dropped while an IndexError is in flight (BINARY_SUBSCR releases the
# temporary tuple before checking its result).  The IndexError must
# survive the finalizer; the close error is reported as unraisable.
FINALIZER_EXCEPTION_PROBE = PREAMBLE + """
unraisable = []
sys.unraisablehook = lambda u: unraisable.append(str(u.exc_value))


class BadIter:
    def __iter__(self):
        return self

    def __next__(self):
        return 1

    def close(self):
        raise RuntimeError("close failed")


def outer():
    yield from BadIter()


def started_outer():
    g = outer()
    next(g)
    return g


assert cinderjit.force_compile(outer) is True

try:
    (started_outer(),)[1]
except IndexError as exc:
    assert str(exc) == "tuple index out of range", exc
else:
    raise AssertionError("IndexError lost across the generator finalizer")
assert unraisable == ["close failed"], unraisable
print("finalizer exception ok")
"""


# gen_close throws the sub-iterator's close() error into the delegating
# generator; with no handler around the yield-from it finishes with that
# error.  Afterwards next() raises StopIteration and a second close() does
# not call the sub-iterator's close() again.
SUBCLOSE_RAISES_PROBE = PREAMBLE + """
closes = []


class BadIter:
    def __iter__(self):
        return self

    def __next__(self):
        return 1

    def close(self):
        closes.append("close")
        raise RuntimeError("close failed")


class ExitIter(BadIter):
    def close(self):
        closes.append("exit")
        raise GeneratorExit


class StopIter(BadIter):
    def close(self):
        closes.append("stop")
        raise StopIteration


def outer():
    yield from BadIter()


def outer_exit():
    yield from ExitIter()


def outer_stop():
    yield from StopIter()


for fn in (outer, outer_exit, outer_stop):
    assert cinderjit.force_compile(fn) is True

g = outer()
assert next(g) == 1
try:
    g.close()
except RuntimeError as exc:
    assert str(exc) == "close failed", exc
else:
    raise AssertionError("close() swallowed the sub-iterator's error")
try:
    next(g)
except StopIteration:
    pass
else:
    raise AssertionError("generator still resumable after a failed close")
g.close()
assert closes == ["close"], closes

# GeneratorExit from the sub-iterator's close counts as a completed close.
g = outer_exit()
assert next(g) == 1
assert g.close() is None
try:
    next(g)
except StopIteration:
    pass
else:
    raise AssertionError("generator still resumable")

# A bare StopIteration out of the frame becomes RuntimeError (PEP 479).
g = outer_stop()
assert next(g) == 1
try:
    g.close()
except RuntimeError as exc:
    assert "generator raised StopIteration" in str(exc), exc
else:
    raise AssertionError("StopIteration not converted")
assert closes == ["close", "exit", "stop"], closes
print("subclose raises ok")
"""


@unittest.skipUnless(
    sys.version_info[:2] == (3, 11), "CPython 3.11 generator close paths"
)
class GeneratorCloseSemantics311Test(unittest.TestCase):
    def _run(self, probe, marker):
        env = {
            key: value
            for key, value in os.environ.items()
            if key != "PYTHONPATH"
            and not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
        }
        env.update(
            CINDERX_JIT_MODE="execute",
            PYTHONJITAUTO="1000000",
            PYTHONJITLIGHTWEIGHTFRAME="0",
        )
        proc = subprocess.run(
            [sys.executable, "-c", textwrap.dedent(probe)],
            capture_output=True,
            text=True,
            env=env,
            timeout=300,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr[-3000:])
        self.assertIn(marker, proc.stdout)

    def test_reentry_during_fast_close_raises_already_executing(self):
        self._run(CLOSE_REENTRY_PROBE, "close reentry ok")

    def test_reentry_into_a_live_delegation_chain_is_refused(self):
        self._run(DELEGATION_REENTRY_PROBE, "delegation reentry ok")

    def test_finalizer_keeps_the_in_flight_exception(self):
        self._run(FINALIZER_EXCEPTION_PROBE, "finalizer exception ok")

    def test_sub_iterator_close_error_finishes_the_delegator(self):
        self._run(SUBCLOSE_RAISES_PROBE, "subclose raises ok")


if __name__ == "__main__":
    unittest.main()
