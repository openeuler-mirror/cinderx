# Copyright (c) Meta Platforms, Inc. and affiliates.

"""A compiled generator that delegates with `yield from` inside a `with`
block must invoke the block's exit on the context manager, not on the
sub-iterator: 3.11's SEND replaces the receiver with the result when the
delegate finishes, so the operand stack below the delegation must shrink by
one on the finished edge."""

from _harness import T, emit
import cinderjit


class Guard:
    log = []

    def __enter__(self):
        Guard.log.append("enter")
        return self

    def __exit__(self, *exc):
        Guard.log.append("exit")
        return False


def delegate(data):
    with Guard():
        yield from iter(data)
    Guard.log.append("after")
    yield len(data)
    return len(data) * 10


def delegate_nested(data):
    with Guard():
        with Guard():
            total = yield from delegate(data)
    yield total


def chain(items):
    # The for-loop iterator sits below the delegation on the operand stack.
    for it in items:
        yield from it


results = []
chained = None
error = None
for _ in range(T + 3):
    del Guard.log[:]
    try:
        results.append(list(delegate([1, 2, 3])))
        results.append(list(delegate_nested([4, 5])))
        chained = list(chain([iter([1, 2]), (x for x in [3]), [4, 5]]))
    except Exception as exc:  # noqa: BLE001
        error = f"{type(exc).__name__}: {exc}"
        break
emit(
    error=error,
    first=results[0] if results else None,
    second=results[1] if len(results) > 1 else None,
    last=results[-1] if results else None,
    log=list(Guard.log),
    chained=chained,
    compiled=cinderjit.is_jit_compiled(delegate)
    and cinderjit.is_jit_compiled(delegate_nested)
    and cinderjit.is_jit_compiled(chain),
)
