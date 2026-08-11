import argparse
import sys

import cinderx.jit


def make_inner():
    def inner(x):
        return x + 1

    return inner


def fast_attach() -> None:
    first = make_inner()
    assert cinderx.jit.force_compile(first)
    assert cinderx.jit.is_jit_compiled(first)

    second = make_inner()
    assert cinderx.jit.is_jit_compiled(second)
    assert second(41) == 42


def jit_list_bail() -> None:
    first = make_inner()
    assert cinderx.jit.force_compile(first)
    assert cinderx.jit.is_jit_compiled(first)

    cinderx.jit.append_jit_list("not_the_module:not_the_function")

    second = make_inner()
    assert not cinderx.jit.is_jit_compiled(second)
    assert second(41) == 42


def instrumentation_bail() -> None:
    def profiler(*args):
        return None

    first = make_inner()
    assert cinderx.jit.force_compile(first)
    assert cinderx.jit.is_jit_compiled(first)

    sys.setprofile(profiler)
    try:
        second = make_inner()
        assert not cinderx.jit.is_jit_compiled(second)
        assert second(41) == 42
    finally:
        sys.setprofile(None)


def many_recreated() -> None:
    def make_inner_with_value(value):
        def inner(offset=1):
            return value + offset

        return inner

    first = make_inner_with_value(0)
    assert cinderx.jit.force_compile(first)
    assert cinderx.jit.is_jit_compiled(first)

    for value in range(10_000):
        current = make_inner_with_value(value)
        assert current.__code__ is first.__code__
        assert cinderx.jit.is_jit_compiled(current)
        assert current() == value + 1


CASES = {
    "fast-attach": fast_attach,
    "instrumentation-bail": instrumentation_bail,
    "jit-list-bail": jit_list_bail,
    "many-recreated": many_recreated,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
