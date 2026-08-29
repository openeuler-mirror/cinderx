import gc
import sys
import weakref

import cinderx
import cinderx.jit
import _cinderx

try:
    import cinderjit
except ImportError:
    cinderjit = None


def minimal_jit_target() -> int:
    return 41 + 1


def run_case(force_fallback: bool = False, dump_assembly: bool = True) -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")
    if cinderjit.jit_frame_mode() != 1:
        raise RuntimeError("lightweight frame mode is not active")

    if force_fallback:
        cinderjit._test_set_thread_state_offset(-1)

    assert cinderx.jit.force_compile(minimal_jit_target)
    assert cinderx.jit.is_jit_compiled(minimal_jit_target)
    for _ in range(20):
        assert minimal_jit_target() == 42
    print("CASE_RESULT minimal_jit_target OK 42")
    if dump_assembly:
        cinderx.jit.disassemble(minimal_jit_target)


def run_localsplus_reuse_case() -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")
    if cinderjit.jit_frame_mode() != 1:
        raise RuntimeError("lightweight frame mode is not active")

    class Payload:
        pass

    def identity(value):
        return value

    assert cinderx.jit.force_compile(identity)
    assert cinderx.jit.is_jit_compiled(identity)
    before = (sys.getrefcount(identity), sys.getrefcount(identity.__code__))
    refs = []
    for _ in range(100):
        value = Payload()
        refs.append(weakref.ref(value))
        assert identity(value) is value
        del value
    gc.collect()
    assert all(ref() is None for ref in refs), "argument local leaked"
    after = (sys.getrefcount(identity), sys.getrefcount(identity.__code__))
    assert after == before, (before, after)
    print("CASE_RESULT localsplus_reuse OK 100")


def run_mode_case() -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")
    mode = cinderjit.jit_frame_mode()
    if mode != 0:
        raise RuntimeError(f"expected normal frame mode, got {mode}")
    print("CASE_RESULT frame_mode OK 0")


def run_normal_generator_case() -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")
    if cinderjit.jit_frame_mode() != 0:
        raise RuntimeError("normal frame mode is not active")

    def gen(limit):
        for value in range(limit):
            yield value * 2

    assert cinderx.jit.force_compile(gen)
    assert cinderx.jit.is_jit_compiled(gen)
    assert list(gen(5)) == [0, 2, 4, 6, 8]
    print("CASE_RESULT normal_generator OK 0 2 4 6 8")


def run_recursion_case() -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")

    def recurse(depth: int) -> int:
        if depth == 0:
            return 0
        return 1 + recurse(depth - 1)

    assert cinderx.jit.force_compile(recurse)
    assert cinderx.jit.is_jit_compiled(recurse)
    assert recurse(40) == 40

    old_limit = sys.getrecursionlimit()
    before = dict(_cinderx._native_recursion_state())
    try:
        sys.setrecursionlimit(80)
        try:
            recurse(1000)
        except RecursionError:
            pass
        else:
            raise AssertionError("recursive JIT call did not raise RecursionError")
        assert recurse(5) == 5
    finally:
        sys.setrecursionlimit(old_limit)
    after = dict(_cinderx._native_recursion_state())
    for key in (
        "recursion_remaining",
        "recursion_headroom",
        "boundary_active",
        "jit_entries",
    ):
        assert after[key] == before[key], (key, before, after)

    mode = cinderjit.jit_frame_mode()
    print(f"CASE_RESULT recursion OK mode={mode} shallow=40 recovery=5")


def require_lightweight_jit() -> None:
    if not cinderx.is_lightweight_frames_enabled():
        raise RuntimeError("LWF not compiled in")
    if not cinderx.jit.is_enabled():
        raise RuntimeError("JIT not enabled")
    if cinderjit is None:
        raise RuntimeError("cinderjit unavailable")
    if cinderjit.jit_frame_mode() != 1:
        raise RuntimeError("lightweight frame mode is not active")


def run_materialize_getframe_case() -> None:
    require_lightweight_jit()

    def f() -> tuple[bool, bool, bool, bool, bool]:
        frame = sys._getframe(0)
        builtins = __builtins__
        expected_builtins = (
            builtins.__dict__ if hasattr(builtins, "__dict__") else builtins
        )
        return (
            frame.f_globals is globals(),
            frame.f_builtins is expected_builtins,
            frame.f_code is f.__code__,
            isinstance(frame.f_lasti, int),
            isinstance(frame.f_lineno, int),
        )

    assert cinderx.jit.force_compile(f)
    assert f() == (True, True, True, True, True)
    print("CASE_RESULT materialize_getframe OK mode=1")


def run_materialize_traceback_case() -> None:
    require_lightweight_jit()

    def f() -> None:
        raise ValueError("from jit")

    assert cinderx.jit.force_compile(f)
    try:
        f()
    except ValueError as caught:
        tb = caught.__traceback__
    else:
        raise AssertionError("ValueError was not raised")

    frames = []
    while tb is not None:
        frames.append((tb.tb_frame.f_code.co_name, tb.tb_frame.f_code.co_filename))
        tb = tb.tb_next
    assert ("f", __file__) in frames, frames
    print("CASE_RESULT materialize_traceback OK mode=1")


def run_generator_return_cleanup_case() -> None:
    require_lightweight_jit()
    events = []
    holder = {}

    class ReenterOnDel:
        def __del__(self) -> None:
            gen = holder["gen"]
            events.append(("running", gen.gi_running))
            try:
                next(gen)
            except BaseException as exc:
                events.append(type(exc).__name__)

    def gen(obj):
        if obj is None:
            yield obj

    assert cinderx.jit.force_compile(gen)
    holder["gen"] = gen(ReenterOnDel())
    try:
        next(holder["gen"])
    except StopIteration:
        pass
    else:
        raise AssertionError("generator did not finish")

    assert ("running", False) in events, events
    assert "StopIteration" in events, events
    assert "ValueError" not in events, events
    print("CASE_RESULT generator_return_cleanup OK mode=1")


def run_generator_argument_lifetime_case() -> None:
    require_lightweight_jit()
    events = []

    class Marker:
        def __del__(self) -> None:
            events.append("finalized")

    def gen(obj):
        if obj is None:
            yield obj

    assert cinderx.jit.force_compile(gen)
    obj = Marker()
    suspended = gen(obj)
    del obj
    assert events == [], events
    try:
        next(suspended)
    except StopIteration:
        pass
    else:
        raise AssertionError("generator did not finish")
    assert events == ["finalized"], events
    print("CASE_RESULT generator_argument_lifetime OK mode=1")


def main() -> int:
    cases = {
        "fallback",
        "inline",
        "execute",
        "localsplus_reuse",
        "mode",
        "materialize_getframe",
        "materialize_traceback",
        "generator_return_cleanup",
        "generator_argument_lifetime",
        "normal_generator",
        "recursion",
    }
    if len(sys.argv) != 2 or sys.argv[1] not in cases:
        raise SystemExit(
            f"usage: {sys.argv[0]} "
            "<fallback|inline|execute|localsplus_reuse|mode|materialize_getframe|"
            "materialize_traceback|generator_return_cleanup|"
            "generator_argument_lifetime|normal_generator|recursion>"
        )
    if sys.argv[1] == "mode":
        run_mode_case()
    elif sys.argv[1] == "localsplus_reuse":
        run_localsplus_reuse_case()
    elif sys.argv[1] == "normal_generator":
        run_normal_generator_case()
    elif sys.argv[1] == "recursion":
        run_recursion_case()
    elif sys.argv[1] == "materialize_getframe":
        run_materialize_getframe_case()
    elif sys.argv[1] == "materialize_traceback":
        run_materialize_traceback_case()
    elif sys.argv[1] == "generator_return_cleanup":
        run_generator_return_cleanup_case()
    elif sys.argv[1] == "generator_argument_lifetime":
        run_generator_argument_lifetime_case()
    elif sys.argv[1] == "execute":
        run_case(dump_assembly=False)
    else:
        run_case(force_fallback=sys.argv[1] == "fallback")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
