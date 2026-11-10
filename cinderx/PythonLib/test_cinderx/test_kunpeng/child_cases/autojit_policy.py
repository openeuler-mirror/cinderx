import argparse


def run_compile_smoke():
    import cinderx.jit as jit

    def straight_compute(a, b):
        return (a + b) * 2

    # The default IC gate may defer compilation for 256 calls after auto:2.
    for value in range(512):
        assert straight_compute(value, value + 1) == 4 * value + 2
        if jit.is_jit_compiled(straight_compute):
            break
    assert jit.is_jit_compiled(straight_compute), (
        "AutoJIT smoke target did not compile",
        jit.count_interpreted_calls(straight_compute),
    )
    return jit


def asyncio_event_loop_helpers() -> None:
    import asyncio

    jit = run_compile_smoke()
    loop = asyncio.new_event_loop()
    try:
        for _ in range(128):
            loop.call_soon(lambda: None)
    finally:
        loop.close()

    assert not jit.is_jit_compiled(asyncio.BaseEventLoop.call_soon), (
        "BaseEventLoop.call_soon should stay interpreted",
        jit.count_interpreted_calls(asyncio.BaseEventLoop.call_soon),
    )
    assert not jit.is_jit_compiled(asyncio.BaseEventLoop._call_soon), (
        "BaseEventLoop._call_soon should stay interpreted",
        jit.count_interpreted_calls(asyncio.BaseEventLoop._call_soon),
    )


def deepcopy_exception_helpers() -> None:
    import copy

    jit = run_compile_smoke()
    for _ in range(2048):
        copy._deepcopy_tuple((object(),), {})
    assert not jit.is_jit_compiled(copy._deepcopy_tuple), (
        "_deepcopy_tuple should stay deferred",
        jit.count_interpreted_calls(copy._deepcopy_tuple),
    )

    memo = {}
    for _ in range(2048):
        copy._keep_alive(object(), memo)
    assert not jit.is_jit_compiled(copy._keep_alive), (
        "_keep_alive should keep the existing RiskDefer behavior",
        jit.count_interpreted_calls(copy._keep_alive),
    )


CASES = {
    "asyncio-event-loop-helpers": asyncio_event_loop_helpers,
    "deepcopy-exception-helpers": deepcopy_exception_helpers,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("case", choices=sorted(CASES))
    args = parser.parse_args()
    CASES[args.case]()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
